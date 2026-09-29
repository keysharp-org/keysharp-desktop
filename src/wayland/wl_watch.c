#include "wl_watch.h"

#include "protocol.h"
#include "protocol_io.h"
#include "transport.h"
#include "wl_hypr.h"
#include "wl_hypr_windows.h"
#include "wl_internal.h"
#include "wl_windows.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Hyprland reports no event for a move or resize, so the geometry is sampled
 * on this period; events that it does report refresh sooner, coalesced so a
 * burst costs one refresh. */
#define KSD_WATCH_TICK_MS 250u
#define KSD_WATCH_COALESCE_MS 30u
#define KSD_WATCH_REFRESH_FAILURES 3u

static const ksd_watch_window *find_window(const ksd_watch_window *windows,
                                           size_t count, uint64_t id)
{
    for (size_t index = 0u; index < count; index++)
        if (windows[index].id == id)
            return &windows[index];
    return NULL;
}

static bool same_title(const char *a, const char *b)
{
    return strcmp(a == NULL ? "" : a, b == NULL ? "" : b) == 0;
}

void ksd_wayland_watch_diff(const ksd_watch_window *previous,
                            size_t previous_count, uint64_t previous_active,
                            const ksd_watch_window *next, size_t next_count,
                            uint64_t next_active,
                            void (*emit)(void *context, uint16_t kind,
                                         uint64_t id),
                            void *context)
{
    for (size_t index = 0u; index < next_count; index++) {
        const ksd_watch_window *now = &next[index];
        const ksd_watch_window *was = find_window(previous, previous_count,
                                                  now->id);

        if (was == NULL) {
            emit(context, KSD_WINDOW_EVENT_CREATE, now->id);
            continue;
        }
        if (!same_title(was->title, now->title))
            emit(context, KSD_WINDOW_EVENT_TITLE, now->id);
        if (was->minimized != now->minimized)
            emit(context, now->minimized ? KSD_WINDOW_EVENT_MINIMIZE
                                         : KSD_WINDOW_EVENT_RESTORE, now->id);
        if (was->x != now->x || was->y != now->y
            || was->width != now->width || was->height != now->height)
            emit(context, KSD_WINDOW_EVENT_MOVE, now->id);
    }
    for (size_t index = 0u; index < previous_count; index++)
        if (find_window(next, next_count, previous[index].id) == NULL)
            emit(context, KSD_WINDOW_EVENT_CLOSE, previous[index].id);
    if (previous_active == next_active)
        return;
    if (previous_active != 0u
        && find_window(next, next_count, previous_active) != NULL)
        emit(context, KSD_WINDOW_EVENT_ACTIVE_STATE, previous_active);
    if (next_active != 0u)
        emit(context, KSD_WINDOW_EVENT_ACTIVE, next_active);
}

typedef struct window_watch {
    ksd_wayland *connection;
    const ksd_wayland_window_view *view;
    int stream_fd;
    ksd_watch_window *windows;
    size_t count;
    uint64_t active;
    bool failed;
    bool dirty;
} window_watch;

static void clear_windows(ksd_watch_window *windows, size_t count)
{
    for (size_t index = 0u; index < count; index++)
        free(windows[index].title);
    free(windows);
}

/* The usable windows of the refreshed view, and the active one. */
static bool snapshot(window_watch *watch, ksd_watch_window **windows,
                     size_t *count, uint64_t *active)
{
    ksd_watch_window *items;
    size_t capacity = 0u;
    size_t used = 0u;

    *windows = NULL;
    *count = 0u;
    *active = 0u;
    for (ksd_wl_toplevel *item = watch->connection->toplevels; item != NULL;
         item = item->next)
        capacity++;
    items = calloc(capacity == 0u ? 1u : capacity, sizeof(*items));
    if (items == NULL)
        return false;
    for (ksd_wl_toplevel *item = watch->connection->toplevels; item != NULL;
         item = item->next) {
        uint32_t state;

        if (!watch->view->usable(item))
            continue;
        state = watch->view->state(item);
        items[used].id = item->id;
        items[used].title = strdup(item->title == NULL ? "" : item->title);
        if (items[used].title == NULL) {
            clear_windows(items, used);
            return false;
        }
        items[used].minimized =
            (state & KSD_WL_TOPLEVEL_STATE_MINIMIZED) != 0u;
        items[used].x = item->hypr.x;
        items[used].y = item->hypr.y;
        items[used].width = item->hypr.width;
        items[used].height = item->hypr.height;
        if ((state & KSD_WL_TOPLEVEL_STATE_ACTIVATED) != 0u && *active == 0u)
            *active = item->id;
        used++;
    }
    *windows = items;
    *count = used;
    return true;
}

static bool write_frame(int descriptor, uint16_t opcode, uint16_t flags,
                        uint64_t request_id, uint8_t *payload, size_t length)
{
    ksd_frame frame = {
        .magic = { KSD_FRAME_MAGIC_0, KSD_FRAME_MAGIC_1,
                   KSD_FRAME_MAGIC_2, KSD_FRAME_MAGIC_3 },
        .major = KSD_PROTOCOL_MAJOR, .minor = KSD_PROTOCOL_MINOR,
        .opcode = opcode, .flags = flags, .request_id = request_id,
        .payload = payload, .payload_length = (uint32_t)length,
    };
    return ksd_frame_write(descriptor, &frame);
}

static const ksd_wl_toplevel *toplevel_for(window_watch *watch, uint64_t id)
{
    for (const ksd_wl_toplevel *item = watch->connection->toplevels;
         item != NULL; item = item->next)
        if (item->id == id && watch->view->usable(item))
            return item;
    return NULL;
}

static void emit_event(void *context, uint16_t kind, uint64_t id)
{
    window_watch *watch = context;
    const ksd_wl_toplevel *item;
    ksd_buffer json;
    ksd_buffer payload;
    char closed[48];
    bool ok;

    if (watch->failed)
        return;
    ksd_buffer_init(&json, KSD_MAX_TEXT_BYTES);
    if (kind == KSD_WINDOW_EVENT_CLOSE) {
        int written = snprintf(closed, sizeof(closed), "{\"id\":\"%llu\"}",
                               (unsigned long long)id);
        ok = written > 0 && (size_t)written < sizeof(closed)
            && ksd_buffer_bytes(&json, closed, (size_t)written);
    } else {
        item = toplevel_for(watch, id);
        /* Every other kind names a window of the snapshot just taken. */
        ok = item != NULL
            && watch->view->append_window(&json, watch->connection, item);
    }
    ksd_buffer_init(&payload, KSD_MAX_TEXT_BYTES + 8u);
    ok = ok && ksd_buffer_u16(&payload, kind)
        && ksd_buffer_u16(&payload, 0u)
        && ksd_buffer_u32(&payload, (uint32_t)json.length)
        && ksd_buffer_bytes(&payload, json.data, json.length)
        && write_frame(watch->stream_fd, KSD_OP_WINDOW_EVENT, KSD_FLAG_EVENT,
                       0u, payload.data, payload.length);
    ksd_buffer_clear(&payload);
    ksd_buffer_clear(&json);
    if (!ok)
        watch->failed = true;
}

/* A refresh that fails is not diffed: its empty list would read as every
 * window closing. */
static bool refresh(window_watch *watch, bool emit)
{
    ksd_operation_result result;
    ksd_watch_window *windows;
    size_t count;
    uint64_t active;
    bool refreshed;

    ksd_result_init(&result);
    alarm(10u);
    refreshed = ksd_wayland_windows_refresh(watch->connection, watch->view,
                                            &result)
        && snapshot(watch, &windows, &count, &active);
    if (refreshed && emit)
        ksd_wayland_watch_diff(watch->windows, watch->count, watch->active,
                               windows, count, active, emit_event, watch);
    alarm(0u);
    ksd_result_clear(&result);
    if (!refreshed)
        return false;
    clear_windows(watch->windows, watch->count);
    watch->windows = windows;
    watch->count = count;
    watch->active = active;
    return true;
}

static void on_event_line(void *context, const char *name)
{
    window_watch *watch = context;

    if (ksd_wayland_hypr_event_affects_windows(name))
        watch->dirty = true;
}

/* Reads the event socket to empty. False when Hyprland closed it. */
static bool read_events(window_watch *watch, int events_fd,
                        ksd_hypr_event_reader *reader)
{
    char bytes[4096];

    for (;;) {
        ssize_t count = recv(events_fd, bytes, sizeof(bytes), MSG_DONTWAIT);

        if (count > 0) {
            ksd_wayland_hypr_event_feed(reader, bytes, (size_t)count,
                                        on_event_line, watch);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        return count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
    }
}

static int until(uint64_t deadline, uint64_t now)
{
    return deadline <= now ? 0
        : deadline - now > (uint64_t)INT32_MAX ? INT32_MAX
        : (int)(deadline - now);
}

bool ksd_wayland_watch_run(ksd_wayland *connection, int stream_fd,
                           uint64_t request_id)
{
    window_watch watch = {
        .connection = connection,
        .view = ksd_wayland_hypr_window_view(),
        .stream_fd = stream_fd,
    };
    ksd_hypr_event_reader reader;
    uint8_t answer[8] = { 0u };
    int events_fd = -1;
    unsigned failures = 0u;
    uint64_t last_refresh;
    uint64_t next_tick;
    bool ok;

    ksd_wayland_hypr_event_reader_init(&reader);
    /* This runs only in a dedicated worker process; a stalled compositor
     * must not keep it once its authority connection is gone. */
    alarm(10u);
    /* Handles must name windows the way the query worker does, which only a
     * derived handle guarantees across processes. */
    ok = ksd_wayland_hypr_windows_available(connection)
        && connection->handle_salt_ready;
    if (ok)
        events_fd = ksd_wayland_hypr_events_open(connection->session_pid);
    ok = ok && events_fd >= 0 && refresh(&watch, false);
    ksd_encode_u32(answer, ok ? KSD_STATUS_OK : KSD_STATUS_UNAVAILABLE);
    ok = write_frame(stream_fd, 0u, 0u, request_id, answer, sizeof(answer))
        && ok;
    alarm(0u);
    last_refresh = ksd_monotonic_milliseconds();
    next_tick = last_refresh + KSD_WATCH_TICK_MS;
    while (ok && !watch.failed) {
        struct pollfd descriptors[3] = {
            { .fd = stream_fd, .events = POLLIN | POLLHUP | POLLERR },
            { .fd = wl_display_get_fd(connection->display),
              .events = POLLIN },
            { .fd = events_fd, .events = POLLIN },
        };
        uint64_t now = ksd_monotonic_milliseconds();
        uint64_t deadline = watch.dirty
            && last_refresh + KSD_WATCH_COALESCE_MS < next_tick
            ? last_refresh + KSD_WATCH_COALESCE_MS : next_tick;
        int ready = poll(descriptors, 3u, until(deadline, now));

        if (ready < 0) {
            if (errno == EINTR)
                continue;
            ok = false;
            break;
        }
        /* Anything from the authority, including its hangup, ends the watch. */
        if (descriptors[0].revents != 0)
            break;
        if (descriptors[1].revents != 0) {
            if ((descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0
                || !ksd_wayland_dispatch_ready(connection)) {
                ok = false;
                break;
            }
            watch.dirty = true;
        }
        if (descriptors[2].revents != 0
            && !read_events(&watch, events_fd, &reader)) {
            ok = false;
            break;
        }
        now = ksd_monotonic_milliseconds();
        if (now < next_tick
            && !(watch.dirty && now >= last_refresh + KSD_WATCH_COALESCE_MS))
            continue;
        watch.dirty = false;
        if (refresh(&watch, true))
            failures = 0u;
        else if (++failures >= KSD_WATCH_REFRESH_FAILURES)
            ok = false;
        last_refresh = ksd_monotonic_milliseconds();
        next_tick = last_refresh + KSD_WATCH_TICK_MS;
    }
    if (events_fd >= 0)
        close(events_fd);
    clear_windows(watch.windows, watch.count);
    return ok && !watch.failed;
}
