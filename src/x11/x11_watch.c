#include "x11_watch.h"
#include "x11_extended.h"
#include "x11_internal.h"
#include "protocol.h"
#include "state_wire.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/randr.h>
#include <xcb/xkb.h>
#include <xkbcommon/xkbcommon-x11.h>

typedef struct watched_window {
    xcb_window_t id;
    xcb_window_t frame;
    bool minimized;
    xcb_window_t above_sibling;
} watched_window;

typedef struct window_watch {
    ksd_x11 *display;
    int stream_fd;
    x11_atoms atoms;
    watched_window *windows;
    size_t count;
    xcb_window_t active;
    ksd_x11 *query_display;
    uint8_t xkb_event_base, randr_event_base;
} window_watch;

static bool emit_query_state(window_watch *, uint32_t, bool);
static bool initial_snapshot(window_watch *);

static xcb_atom_t intern(xcb_connection_t *connection, const char *name)
{
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(connection,
        xcb_intern_atom(connection, 0u, (uint16_t)strlen(name), name), NULL);
    xcb_atom_t result = reply != NULL ? reply->atom : XCB_ATOM_NONE;
    free(reply);
    return result;
}

static int compare_ids(const void *left, const void *right)
{
    xcb_window_t a = *(const xcb_window_t *)left;
    xcb_window_t b = *(const xcb_window_t *)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

static watched_window *find_window(window_watch *watch, xcb_window_t id)
{
    size_t first = 0u, last = watch->count;
    while (first < last) {
        size_t middle = first + (last - first) / 2u;
        if (watch->windows[middle].id < id) first = middle + 1u;
        else last = middle;
    }
    return first < watch->count && watch->windows[first].id == id
        ? &watch->windows[first] : NULL;
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

static bool emit_window(window_watch *watch, xcb_window_t id, uint16_t kind)
{
    ksd_operation_result result;
    ksd_result_init(&result);
    char closed[40];
    const uint8_t *json;
    size_t length;
    if (kind == KSD_WINDOW_EVENT_CLOSE) {
        int written = snprintf(closed, sizeof(closed), "{\"id\":\"%u\"}", id);
        if (written < 0 || (size_t)written >= sizeof(closed)) return false;
        json = (const uint8_t *)closed;
        length = (size_t)written;
    } else {
        ksd_x11_window_query(watch->display, id, &result);
        /* A client can disappear between its notification and its query. Its
         * DestroyNotify or the next client-list change supplies the close. */
        if (result.status != KSD_STATUS_OK) {
            ksd_result_clear(&result);
            return !ksd_x11_connection_failed(watch->display);
        }
        const char prefix[] = "{\"ok\":true,\"window\":";
        size_t prefix_length = sizeof(prefix) - 1u;
        if (result.tail_length <= 4u + prefix_length
            || ksd_decode_u32(result.tail) != result.tail_length - 4u
            || memcmp(result.tail + 4u, prefix, prefix_length) != 0
            || result.tail[result.tail_length - 1u] != '}') {
            ksd_result_clear(&result);
            return false;
        }
        json = result.tail + 4u + prefix_length;
        length = result.tail_length - 4u - prefix_length - 1u;
    }
    ksd_buffer payload;
    ksd_buffer_init(&payload, KSD_MAX_TEXT_BYTES + 8u);
    bool ok = ksd_buffer_u16(&payload, kind)
        && ksd_buffer_u16(&payload, 0u)
        && ksd_buffer_u32(&payload, (uint32_t)length)
        && ksd_buffer_bytes(&payload, json, length)
        && write_frame(watch->stream_fd, KSD_OP_WINDOW_EVENT,
                        KSD_FLAG_EVENT, 0u, payload.data, payload.length);
    ksd_buffer_clear(&payload);
    ksd_result_clear(&result);
    return ok;
}

static xcb_window_t frame_for(window_watch *watch, xcb_window_t id)
{
    xcb_connection_t *c = watch->display->connection;
    for (unsigned depth = 0u; depth < 64u; depth++) {
        xcb_query_tree_reply_t *reply = xcb_query_tree_reply(c,
            xcb_query_tree(c, id), NULL);
        if (reply == NULL) break;
        xcb_window_t parent = reply->parent;
        bool done = parent == reply->root || parent == XCB_WINDOW_NONE || parent == id;
        free(reply);
        if (done) break;
        id = parent;
    }
    return id;
}

static bool minimized(window_watch *watch, xcb_window_t id)
{
    return ksd_x11_has_state(watch->display->connection, &watch->atoms,
                             id, watch->atoms.state_hidden);
}

/* Only a client-list notification reads this list. Idle subscriptions issue
 * no X requests, and property/configure events query just their window. */
static bool refresh_clients(window_watch *watch, bool initial)
{
    xcb_connection_t *c = watch->display->connection;
    xcb_window_t ids[KSD_X11_MAX_WINDOWS];
    size_t count = 0u;
    if (!ksd_x11_enumerate_windows(watch->display, ids, &count)) return false;
    qsort(ids, count, sizeof(*ids), compare_ids);
    watched_window *next = calloc(count + watch->count + 1u, sizeof(*next));
    if (next == NULL) return false;
    size_t used = 0u;
    bool ok = true;
    for (size_t i = 0u; ok && i < count; i++) {
        if (ids[i] == XCB_WINDOW_NONE || (i != 0u && ids[i] == ids[i - 1u])) continue;
        watched_window *old = find_window(watch, ids[i]);
        if (old != NULL) next[used++] = *old;
        else {
            uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
            xcb_change_window_attributes(c, ids[i], XCB_CW_EVENT_MASK, &mask);
            next[used++] = (watched_window) {
                .id = ids[i], .frame = frame_for(watch, ids[i]),
                .minimized = minimized(watch, ids[i]),
            };
            if (!initial) ok = emit_window(watch, ids[i], KSD_WINDOW_EVENT_CREATE);
        }
    }
    for (size_t i = 0u; ok && i < watch->count; i++) {
        xcb_window_t id = watch->windows[i].id;
        if (bsearch(&id, ids, count, sizeof(*ids), compare_ids) == NULL) {
            xcb_generic_error_t *error = NULL;
            xcb_get_window_attributes_reply_t *attributes = xcb_get_window_attributes_reply(c,
                xcb_get_window_attributes(c, id), &error);
            if (attributes != NULL) {
                next[used++] = watch->windows[i];
                if (!initial) ok = emit_window(watch, id, KSD_WINDOW_EVENT_HIDE);
            } else if (error != NULL && error->error_code == XCB_WINDOW) {
                if (!initial) ok = emit_window(watch, id, KSD_WINDOW_EVENT_CLOSE);
            } else ok = false;
            free(attributes); free(error);
        }
    }
    free(watch->windows);
    watch->windows = next;
    watch->count = used;
    qsort(watch->windows, watch->count, sizeof(*watch->windows), compare_ids);
    xcb_flush(c);
    return ok && !ksd_x11_connection_failed(watch->display);
}

static bool handle_event(window_watch *watch, xcb_generic_event_t *event)
{
    uint8_t type = event->response_type & 0x7fu;
    if (watch->xkb_event_base != 0u && type == watch->xkb_event_base) {
        uint8_t kind = ((uint8_t *)event)[1];
        if (kind != XCB_XKB_STATE_NOTIFY) ksd_x11_keyboard_clear(watch->query_display);
        return emit_query_state(watch, KSD_STATE_KEYBOARD, false);
    }
    if (watch->randr_event_base != 0u && (type == watch->randr_event_base || type == watch->randr_event_base + 1u))
        return emit_query_state(watch, KSD_STATE_DISPLAYS, false);
    xcb_window_t root = watch->display->screen->root;
    if (type == XCB_CREATE_NOTIFY && ((xcb_create_notify_event_t *)event)->parent == root)
        return refresh_clients(watch, false);
    if (type == XCB_PROPERTY_NOTIFY) {
        xcb_property_notify_event_t *property = (xcb_property_notify_event_t *)event;
        if (property->window == root) {
            if (property->atom == watch->atoms.client_list)
                return refresh_clients(watch, false);
            if (property->atom == watch->atoms.client_list_stacking)
                return initial_snapshot(watch);
            if (property->atom == watch->atoms.current_desktop || property->atom == watch->atoms.work_area)
                return emit_query_state(watch, KSD_STATE_DISPLAYS, false);
            if (property->atom == watch->atoms.active_window) {
                uint32_t active = XCB_WINDOW_NONE;
                (void)ksd_x11_cardinal(watch->display->connection, root,
                                       watch->atoms.active_window, &active);
                if (active == watch->active) return true;
                xcb_window_t old = watch->active;
                watch->active = active;
                return (find_window(watch, old) == NULL
                    || emit_window(watch, old, KSD_WINDOW_EVENT_ACTIVE_STATE))
                    && (active == XCB_WINDOW_NONE
                        || emit_window(watch, active, KSD_WINDOW_EVENT_ACTIVE));
            }
            return true;
        }
        watched_window *window = find_window(watch, property->window);
        if (window == NULL) return true;
        if (property->atom == watch->atoms.wm_name || property->atom == XCB_ATOM_WM_NAME)
            return emit_window(watch, window->id, KSD_WINDOW_EVENT_TITLE);
        if (property->atom == watch->atoms.frame_extents)
            return emit_window(watch, window->id, KSD_WINDOW_EVENT_MOVE);
        if (property->atom == watch->atoms.wm_state) {
            bool state = minimized(watch, window->id);
            if (state != window->minimized) {
                window->minimized = state;
                return emit_window(watch, window->id, state
                    ? KSD_WINDOW_EVENT_MINIMIZE : KSD_WINDOW_EVENT_RESTORE);
            }
        }
        return emit_window(watch, window->id, KSD_WINDOW_EVENT_CHANGED);
    } else if (type == XCB_MAP_NOTIFY || type == XCB_UNMAP_NOTIFY) {
        xcb_window_t id = type == XCB_MAP_NOTIFY
            ? ((xcb_map_notify_event_t *)event)->window : ((xcb_unmap_notify_event_t *)event)->window;
        if (find_window(watch, id) != NULL)
            return emit_window(watch, id, type == XCB_MAP_NOTIFY ? KSD_WINDOW_EVENT_SHOW : KSD_WINDOW_EVENT_HIDE);
        if (type == XCB_MAP_NOTIFY) return refresh_clients(watch, false);
    } else if (type == XCB_DESTROY_NOTIFY) {
        xcb_destroy_notify_event_t *destroy = (xcb_destroy_notify_event_t *)event;
        watched_window *window = find_window(watch, destroy->window);
        if (window != NULL) {
            size_t index = (size_t)(window - watch->windows);
            bool ok = emit_window(watch, window->id, KSD_WINDOW_EVENT_CLOSE);
            memmove(window, window + 1u, (watch->count - index - 1u) * sizeof(*window));
            watch->count--;
            return ok;
        }
    } else if (type == XCB_CONFIGURE_NOTIFY) {
        xcb_configure_notify_event_t *configure = (xcb_configure_notify_event_t *)event;
        bool restacked = false;
        for (size_t i = 0u; i < watch->count; i++) {
            if (watch->windows[i].id != configure->window && watch->windows[i].frame != configure->window) continue;
            if (!emit_window(watch, watch->windows[i].id, KSD_WINDOW_EVENT_MOVE)) return false;
            restacked |= watch->windows[i].above_sibling != configure->above_sibling;
            watch->windows[i].above_sibling = configure->above_sibling;
        }
        if (restacked) return initial_snapshot(watch);
    } else if (type == XCB_REPARENT_NOTIFY) {
        xcb_reparent_notify_event_t *reparent = (xcb_reparent_notify_event_t *)event;
        watched_window *window = find_window(watch, reparent->window);
        if (window != NULL) window->frame = frame_for(watch, window->id);
    }
    return true;
}

static bool emit_state(window_watch *watch, uint32_t kind, uint32_t domain, ksd_window_record *window,
                       const uint8_t *data, size_t length)
{
    ksd_state_event event; ksd_state_event_init(&event); event.kind = kind; event.domain = domain;
    if (window != NULL) event.window = *window;
    event.data.data = (char *)data; event.data.length = length;
    ksd_buffer payload; ksd_buffer_init(&payload, KSD_MAX_TEXT_BYTES + 256u);
    bool ok = ksd_state_event_encode(&event, &payload)
        && write_frame(watch->stream_fd, KSD_OP_STATE_EVENT, KSD_FLAG_EVENT, 0u, payload.data, payload.length);
    ksd_buffer_clear(&payload); return ok;
}

static bool initial_window(ksd_window_record *window, void *context)
{
    window_watch *watch = context;
    if (find_window(watch, (xcb_window_t)window->handle) == NULL) {
        watched_window *items = realloc(watch->windows, (watch->count + 1u) * sizeof(*items));
        if (items == NULL) return false;
        watch->windows = items;
        watch->windows[watch->count++] = (watched_window) { .id = (xcb_window_t)window->handle,
            .frame = frame_for(watch, (xcb_window_t)window->handle), .minimized = (window->flags & KSD_WINDOW_MINIMIZED) != 0u };
        qsort(watch->windows, watch->count, sizeof(*items), compare_ids);
        uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
        xcb_change_window_attributes(watch->display->connection, (xcb_window_t)window->handle, XCB_CW_EVENT_MASK, &mask);
    }
    return emit_state(watch, KSD_STATE_SNAPSHOT_ITEM, KSD_STATE_WINDOWS, window, NULL, 0u);
}

static bool initial_snapshot(window_watch *watch)
{
    ksd_operation_result result; ksd_result_init(&result);
    ksd_x11_window_list(watch->display, true, &result);
    bool ok = result.status == KSD_STATUS_OK && result.tail_length >= 4u
        && emit_state(watch, KSD_STATE_SNAPSHOT_BEGIN, KSD_STATE_WINDOWS, NULL, NULL, 0u)
        && ksd_state_windows_json(result.tail + 4u, result.tail_length - 4u, initial_window, watch)
        && emit_state(watch, KSD_STATE_SNAPSHOT_END, KSD_STATE_WINDOWS, NULL, NULL, 0u);
    ksd_result_clear(&result); return ok;
}

static bool emit_query_state(window_watch *watch, uint32_t domain, bool initial)
{
    if (watch->query_display == NULL) return true;
    ksd_operation_result result; ksd_result_init(&result);
    if (domain == KSD_STATE_KEYBOARD) {
        const char *revision = initial ? NULL : ksd_x11_keyboard_revision(watch->query_display);
        char known[33] = { 0 }; if (revision != NULL) memcpy(known, revision, 32u);
        ksd_x11_keyboard_state_since(watch->query_display, (const uint8_t *)known, known[0] == '\0' ? 0u : 32u, &result);
    } else ksd_x11_display_list(watch->query_display, &result);
    bool ok = result.status != KSD_STATUS_OK || result.tail_length < 4u
        || ((!initial || emit_state(watch, KSD_STATE_SNAPSHOT_BEGIN, domain, NULL, NULL, 0u))
            && emit_state(watch, initial ? KSD_STATE_SNAPSHOT_ITEM : KSD_STATE_CHANGED, domain,
                NULL, result.tail + 4u, result.tail_length - 4u)
            && (!initial || emit_state(watch, KSD_STATE_SNAPSHOT_END, domain, NULL, NULL, 0u)));
    ksd_result_clear(&result); return ok;
}

bool ksd_x11_watch_run(ksd_x11 *connection, int stream_fd, uint64_t request_id)
{
    window_watch watch = { .display = connection, .stream_fd = stream_fd };
    xcb_connection_t *c = connection->connection;
    /* This runs only in a dedicated worker process. A stalled X server must
     * not retain that process after its authority connection disappears. */
    alarm(10u);
    watch.atoms = connection->atoms;
    watch.atoms.client_list = intern(c, "_NET_CLIENT_LIST");
    watch.atoms.active_window = intern(c, "_NET_ACTIVE_WINDOW");
    watch.atoms.wm_name = intern(c, "_NET_WM_NAME");
    watch.atoms.wm_state = intern(c, "_NET_WM_STATE");
    watch.atoms.state_hidden = intern(c, "_NET_WM_STATE_HIDDEN");
    watch.atoms.frame_extents = intern(c, "_NET_FRAME_EXTENTS");
    uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
    xcb_change_window_attributes(c, connection->screen->root, XCB_CW_EVENT_MASK, &mask);
    bool ok = refresh_clients(&watch, true);
    (void)ksd_x11_open(connection->display_name, NULL, &watch.query_display);
    if (watch.query_display != NULL) {
        if (xkb_x11_setup_xkb_extension(c, XKB_X11_MIN_MAJOR_XKB_VERSION, XKB_X11_MIN_MINOR_XKB_VERSION,
            XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS, NULL, NULL, &watch.xkb_event_base, NULL)) {
            int32_t device = xkb_x11_get_core_keyboard_device_id(c);
            uint16_t selected = XCB_XKB_EVENT_TYPE_STATE_NOTIFY | XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY
                | XCB_XKB_EVENT_TYPE_MAP_NOTIFY | XCB_XKB_EVENT_TYPE_NAMES_NOTIFY;
            if (device >= 0) xcb_xkb_select_events(c, (uint16_t)device, selected, 0u, selected, 0xffu, 0xffu, NULL);
        }
        const xcb_query_extension_reply_t *randr = xcb_get_extension_data(c, &xcb_randr_id);
        if (randr != NULL && randr->present) {
            watch.randr_event_base = randr->first_event;
            xcb_randr_select_input(c, connection->screen->root, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE
                | XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE | XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE);
        }
    }
    (void)ksd_x11_cardinal(c, connection->screen->root,
                           watch.atoms.active_window, &watch.active);
    uint8_t answer[8] = { 0u };
    ksd_encode_u32(answer, ok ? KSD_STATUS_OK : KSD_STATUS_UNAVAILABLE);
    ok = write_frame(stream_fd, 0u, 0u, request_id, answer, sizeof(answer)) && ok;
    if (ok) ok = initial_snapshot(&watch);
    if (ok) ok = emit_query_state(&watch, KSD_STATE_KEYBOARD, true) && emit_query_state(&watch, KSD_STATE_DISPLAYS, true);
    alarm(0u);
    while (ok) {
        struct pollfd descriptors[2] = {
            { .fd = stream_fd, .events = POLLIN | POLLHUP | POLLERR },
            { .fd = xcb_get_file_descriptor(c), .events = POLLIN | POLLHUP | POLLERR },
        };
        unsigned handled = 0u;
        xcb_generic_event_t *event;
        while (handled < 256u && (event = xcb_poll_for_event(c)) != NULL) {
            alarm(10u);
            ok = handle_event(&watch, event);
            alarm(0u);
            free(event);
            handled++;
            if (!ok) break;
        }
        if (!ok || ksd_x11_connection_failed(connection)) break;
        int ready = poll(descriptors, 2u, handled == 256u ? 0 : -1);
        if (ready < 0) { if (errno == EINTR) continue; ok = false; break; }
        if (descriptors[0].revents != 0) break;
        if ((descriptors[1].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) { ok = false; break; }
    }
    free(watch.windows);
    ksd_x11_close(watch.query_display);
    return ok;
}
