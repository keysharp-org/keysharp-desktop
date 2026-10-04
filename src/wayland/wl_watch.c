#include "wl_watch.h"

#include "protocol.h"
#include "protocol_io.h"
#include "transport.h"
#include "wl_hypr.h"
#include "wl_hypr_windows.h"
#include "wl_internal.h"
#include "wl_windows.h"
#include "wl_keyboard.h"
#include "wl_displays.h"
#include "state_wire.h"

#include <errno.h>
#include <poll.h>
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

static const ksd_window_record *find_window(const ksd_window_record *windows,
                                           size_t count, uint64_t id)
{
    for (size_t index = 0u; index < count; index++)
        if (windows[index].handle == id)
            return &windows[index];
    return NULL;
}

void ksd_wayland_watch_diff(const ksd_window_record *previous,
                            size_t previous_count,
                            const ksd_window_record *next, size_t next_count,
                            void (*emit)(void *context, uint16_t kind,
                                         uint64_t id),
                            void *context)
{
    for (size_t index = 0u; index < next_count; index++) {
        const ksd_window_record *now = &next[index];
        const ksd_window_record *was = find_window(previous, previous_count,
                                                  now->handle);

        if (was == NULL) {
            emit(context, KSD_WINDOW_EVENT_CREATE, now->handle);
            continue;
        }
        if (!ksd_state_window_equal(was, now))
            emit(context, KSD_WINDOW_EVENT_CHANGED, now->handle);
    }
    for (size_t index = 0u; index < previous_count; index++)
        if (find_window(next, next_count, previous[index].handle) == NULL)
            emit(context, KSD_WINDOW_EVENT_CLOSE, previous[index].handle);
}

typedef struct window_watch {
    ksd_wayland *connection;
    const ksd_wayland_window_view *view;
    int stream_fd;
    ksd_window_record *windows;
    size_t count;
    bool failed;
    bool dirty;
    ksd_buffer keyboard_state;
    ksd_buffer display_state;
    char revision[65];
} window_watch;

static void clear_windows(ksd_window_record *windows, size_t count)
{
    for (size_t index = 0u; index < count; index++)
        ksd_window_record_clear(&windows[index]);
    free(windows);
}

static bool snapshot(window_watch *watch, ksd_window_record **windows,
                     size_t *count)
{
    ksd_window_record *items;
    size_t capacity = 0u;
    size_t used = 0u;

    *windows = NULL;
    *count = 0u;
    for (ksd_wl_toplevel *item = watch->connection->toplevels; item != NULL;
         item = item->next)
        capacity++;
    items = calloc(capacity == 0u ? 1u : capacity, sizeof(*items));
    if (items == NULL)
        return false;
    for (ksd_wl_toplevel *item = watch->connection->toplevels; item != NULL;
         item = item->next) {
        if (!watch->view->usable(item))
            continue;
        ksd_buffer json;
        ksd_buffer_init(&json, KSD_MAX_TEXT_BYTES);
        bool ok = watch->view->append_window(&json, watch->connection, item)
            && ksd_state_window_json(json.data, json.length, &items[used]);
        ksd_buffer_clear(&json);
        if (!ok) {
            clear_windows(items, used);
            return false;
        }
        items[used].stacking_order = used;
        items[used].valid_fields |= KSD_FIELD_STACKING_ORDER;
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

static bool emit_state(window_watch *watch, uint32_t domain, uint32_t kind,
                       const ksd_window_record *window, const uint8_t *data, size_t length)
{
    ksd_state_event event; ksd_state_event_init(&event); event.domain = domain; event.kind = kind;
    if (window != NULL) event.window = *window;
    event.data.data = (char *)data; event.data.length = length;
    ksd_buffer payload; ksd_buffer_init(&payload, KSD_MAX_TEXT_BYTES + 256u);
    bool ok = ksd_state_event_encode(&event, &payload)
        && write_frame(watch->stream_fd, KSD_OP_STATE_EVENT, KSD_FLAG_EVENT, 0u, payload.data, payload.length);
    ksd_buffer_clear(&payload); return ok;
}

static bool initial_windows(window_watch *watch)
{
    if (watch->view == NULL) return true;
    bool ok = emit_state(watch, KSD_STATE_WINDOWS, KSD_STATE_SNAPSHOT_BEGIN, NULL, NULL, 0u);
    for (size_t i = 0u; ok && i < watch->count; i++)
        ok = emit_state(watch, KSD_STATE_WINDOWS, KSD_STATE_SNAPSHOT_ITEM, &watch->windows[i], NULL, 0u);
    return ok && emit_state(watch, KSD_STATE_WINDOWS, KSD_STATE_SNAPSHOT_END, NULL, NULL, 0u);
}

static bool auxiliary_state(window_watch *watch, uint32_t domain, bool initial)
{
    ksd_operation_result result; ksd_result_init(&result);
    ksd_buffer *previous = domain == KSD_STATE_KEYBOARD ? &watch->keyboard_state : &watch->display_state;
    if (domain == KSD_STATE_KEYBOARD) {
        ksd_wayland_keyboard_state_since(watch->connection, (const uint8_t *)watch->revision,
            watch->revision[0] == '\0' ? 0u : 64u, &result);
        if (watch->connection->keymap_revision != NULL) memcpy(watch->revision, watch->connection->keymap_revision, 65u);
    } else ksd_wayland_display_list(watch->connection, &result);
    bool ok = true;
    if (result.status == KSD_STATUS_OK && result.tail_length >= 4u) {
        size_t length = result.tail_length - 4u; const uint8_t *json = result.tail + 4u;
        if (initial || previous->length != length || (length != 0u && memcmp(previous->data, json, length) != 0)) {
            ok = (!initial || emit_state(watch, domain, KSD_STATE_SNAPSHOT_BEGIN, NULL, NULL, 0u))
                && emit_state(watch, domain, initial ? KSD_STATE_SNAPSHOT_ITEM : KSD_STATE_CHANGED, NULL, json, length)
                && (!initial || emit_state(watch, domain, KSD_STATE_SNAPSHOT_END, NULL, NULL, 0u));
            previous->length = 0u; ok = ksd_buffer_bytes(previous, json, length) && ok;
        }
    }
    ksd_result_clear(&result); return ok;
}

static void emit_event(void *context, uint16_t kind, uint64_t id)
{
    window_watch *watch = context;
    if (watch->failed) return;
    ksd_window_record closed; ksd_window_record_init(&closed);
    closed.handle = id; closed.valid_fields = KSD_FIELD_ID;
    const ksd_window_record *window = kind == KSD_WINDOW_EVENT_CLOSE
        ? &closed : find_window(watch->windows, watch->count, id);
    uint32_t event = kind == KSD_WINDOW_EVENT_CLOSE ? KSD_STATE_DESTROY : KSD_STATE_CHANGED;
    watch->failed = window == NULL || !emit_state(watch, KSD_STATE_WINDOWS, event, window, NULL, 0u);
}

/* A refresh that fails is not diffed: its empty list would read as every
 * window closing. */
static bool refresh(window_watch *watch, bool emit)
{
    ksd_operation_result result;
    ksd_window_record *windows;
    size_t count;
    bool refreshed;

    ksd_result_init(&result);
    alarm(10u);
    refreshed = ksd_wayland_windows_refresh(watch->connection, watch->view,
                                            &result)
        && snapshot(watch, &windows, &count);
    alarm(0u);
    ksd_result_clear(&result);
    if (!refreshed)
        return false;
    ksd_window_record *previous = watch->windows;
    size_t previous_count = watch->count;
    watch->windows = windows;
    watch->count = count;
    if (emit)
        ksd_wayland_watch_diff(previous, previous_count, windows, count, emit_event, watch);
    clear_windows(previous, previous_count);
    return true;
}

static void on_event_line(void *context, const char *name)
{
    window_watch *watch = context;

    (void)name;
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
        .view = connection->handle_salt_ready ? ksd_wayland_current_window_view(connection) : NULL,
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
    ksd_buffer_init(&watch.keyboard_state, KSD_MAX_TEXT_BYTES);
    ksd_buffer_init(&watch.display_state, KSD_MAX_TEXT_BYTES);
    /* This runs only in a dedicated worker process; a stalled compositor
     * must not keep it once its authority connection is gone. */
    alarm(10u);
    /* Handles must name windows the way the query worker does, which only a
     * derived handle guarantees across processes. */
    bool hypr = ksd_wayland_hypr_windows_available(connection);
    ok = true;
    if (hypr)
        events_fd = ksd_wayland_hypr_events_open(connection->session_pid);
    ok = (!hypr || events_fd >= 0) && (watch.view == NULL || refresh(&watch, false));
    ksd_encode_u32(answer, ok ? KSD_STATUS_OK : KSD_STATUS_UNAVAILABLE);
    ok = write_frame(stream_fd, 0u, 0u, request_id, answer, sizeof(answer))
        && ok;
    if (ok) ok = initial_windows(&watch) && auxiliary_state(&watch, KSD_STATE_KEYBOARD, true)
        && auxiliary_state(&watch, KSD_STATE_DISPLAYS, true);
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
        int ready = poll(descriptors, 3u, hypr ? until(deadline, now) : -1);

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
        if (hypr && now < next_tick
            && !(watch.dirty && now >= last_refresh + KSD_WATCH_COALESCE_MS))
            continue;
        watch.dirty = false;
        if ((watch.view == NULL || refresh(&watch, true))
            && auxiliary_state(&watch, KSD_STATE_KEYBOARD, false) && auxiliary_state(&watch, KSD_STATE_DISPLAYS, false))
            failures = 0u;
        else if (++failures >= KSD_WATCH_REFRESH_FAILURES)
            ok = false;
        last_refresh = ksd_monotonic_milliseconds();
        next_tick = last_refresh + KSD_WATCH_TICK_MS;
    }
    if (events_fd >= 0)
        close(events_fd);
    clear_windows(watch.windows, watch.count);
    ksd_buffer_clear(&watch.keyboard_state); ksd_buffer_clear(&watch.display_state);
    return ok && !watch.failed;
}
