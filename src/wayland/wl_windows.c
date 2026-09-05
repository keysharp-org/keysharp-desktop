#include "wl_windows.h"

#include "protocol_io.h"
#include "wl_internal.h"
#include "wl_cosmic_windows.h"
#include "wl_wlr_windows.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KSD_WL_WINDOW_TIMEOUT_MS 2000
#define KSD_WL_MAX_TOPLEVELS 4096u

static void replace_string(char **slot, const char *value)
{
    char *copy;

    if (value == NULL)
        return;
    /* A title can change, and the compositor resends the event when it does,
     * so the newest wins rather than the first. */
    copy = strdup(value);
    if (copy == NULL)
        return;
    free(*slot);
    *slot = copy;
}

static void toplevel_closed(void *data,
                            struct ext_foreign_toplevel_handle_v1 *handle)
{
    ksd_wl_toplevel *toplevel = data;

    (void)handle;
    /* Marked rather than unlinked. The protocol requires the handle be
     * destroyed after this, and doing that from inside its own event is how a
     * use-after-free happens; the sweep at the end of the call does it. */
    if (toplevel != NULL)
        toplevel->closed = true;
}

static void toplevel_done(void *data,
                          struct ext_foreign_toplevel_handle_v1 *handle)
{
    (void)data;
    (void)handle;
    /* Every property sent so far now applies as one update. Nothing here is
     * double-buffered, so there is nothing to commit. */
}

static void toplevel_title(void *data,
                           struct ext_foreign_toplevel_handle_v1 *handle,
                           const char *title)
{
    ksd_wl_toplevel *toplevel = data;

    (void)handle;
    if (toplevel != NULL)
        replace_string(&toplevel->title, title);
}

static void toplevel_app_id(void *data,
                            struct ext_foreign_toplevel_handle_v1 *handle,
                            const char *app_id)
{
    ksd_wl_toplevel *toplevel = data;

    (void)handle;
    if (toplevel != NULL)
        replace_string(&toplevel->app_id, app_id);
}

static void toplevel_identifier(void *data,
                                struct ext_foreign_toplevel_handle_v1 *handle,
                                const char *identifier)
{
    ksd_wl_toplevel *toplevel = data;

    (void)handle;
    /* Preserve the compositor's identifier separately from our opaque numeric
     * handle so reconnects cannot accidentally reuse a control/query target. */
    if (toplevel != NULL)
        replace_string(&toplevel->identifier, identifier);
}

static const struct ext_foreign_toplevel_handle_v1_listener handle_listener = {
    .closed = toplevel_closed,
    .done = toplevel_done,
    .title = toplevel_title,
    .app_id = toplevel_app_id,
    .identifier = toplevel_identifier,
};

static void list_toplevel(void *data, struct ext_foreign_toplevel_list_v1 *list,
                          struct ext_foreign_toplevel_handle_v1 *handle)
{
    ksd_wayland *connection = data;
    ksd_wl_toplevel *fresh;
    ksd_wl_toplevel **tail;
    size_t count = 0u;

    (void)list;
    if (connection == NULL || handle == NULL)
        return;
    tail = &connection->toplevels;
    while (*tail != NULL) {
        count++;
        tail = &(*tail)->next;
    }
    if (count >= KSD_WL_MAX_TOPLEVELS) {
        ext_foreign_toplevel_handle_v1_destroy(handle);
        return;
    }
    fresh = calloc(1u, sizeof(*fresh));
    if (fresh == NULL) {
        ext_foreign_toplevel_handle_v1_destroy(handle);
        return;
    }
    fresh->handle = handle;
    fresh->id = ksd_wayland_new_handle(connection);
    if (fresh->id == 0u) {
        ext_foreign_toplevel_handle_v1_destroy(handle);
        free(fresh);
        return;
    }
    /* Appended, so the order a caller sees is the order the compositor
     * reported, which is the closest thing to a stacking order this protocol
     * offers -- it offers none. */
    *tail = fresh;
    ext_foreign_toplevel_handle_v1_add_listener(handle, &handle_listener,
                                                fresh);
    ksd_wayland_cosmic_attach_toplevel(connection, fresh);
}

static void list_finished(void *data,
                          struct ext_foreign_toplevel_list_v1 *list)
{
    (void)data;
    (void)list;
}

static const struct ext_foreign_toplevel_list_v1_listener list_listener = {
    .toplevel = list_toplevel,
    .finished = list_finished,
};

void ksd_wayland_toplevels_attach(ksd_wayland *connection)
{
    if (connection == NULL || connection->toplevel_list == NULL)
        return;
    ext_foreign_toplevel_list_v1_add_listener(connection->toplevel_list,
                                              &list_listener, connection);
}

void ksd_wayland_toplevels_clear(ksd_wayland *connection)
{
    ksd_wl_toplevel *item;

    if (connection == NULL)
        return;
    item = connection->toplevels;
    while (item != NULL) {
        ksd_wl_toplevel *next = item->next;

        ksd_wayland_cosmic_detach_toplevel(item);
        if (item->handle != NULL)
            ext_foreign_toplevel_handle_v1_destroy(item->handle);
        free(item->title);
        free(item->app_id);
        free(item->identifier);
        free(item);
        item = next;
    }
    connection->toplevels = NULL;
}

/* Drops the toplevels the compositor has said are gone, and destroys their
 * handles. Done between calls rather than inside an event, because the
 * protocol requires the destroy and doing it from the closed event itself
 * would free the object the event is still running on. */
static void sweep_closed(ksd_wayland *connection)
{
    ksd_wl_toplevel **slot = &connection->toplevels;

    while (*slot != NULL) {
        ksd_wl_toplevel *item = *slot;

        if (!item->closed) {
            slot = &item->next;
            continue;
        }
        *slot = item->next;
        ksd_wayland_cosmic_detach_toplevel(item);
        if (item->handle != NULL)
            ext_foreign_toplevel_handle_v1_destroy(item->handle);
        free(item->title);
        free(item->app_id);
        free(item->identifier);
        free(item);
    }
}

static bool append_json_string(ksd_buffer *out, const char *value)
{
    if (!ksd_buffer_bytes(out, "\"", 1u))
        return false;
    for (const char *at = value == NULL ? "" : value; *at != '\0'; at++) {
        unsigned char byte = (unsigned char)*at;
        char escape[8];

        /* Every byte JSON cannot carry raw is escaped, the C0 range included.
         * A window title is arbitrary text chosen by another application and
         * it is being pasted into a document the caller will parse. */
        if (byte == '"' || byte == '\\') {
            escape[0] = '\\';
            escape[1] = (char)byte;
            if (!ksd_buffer_bytes(out, escape, 2u))
                return false;
        } else if (byte < 0x20u) {
            int written = snprintf(escape, sizeof(escape), "\\u%04x",
                                   (unsigned)byte);
            if (written != 6 || !ksd_buffer_bytes(out, escape, 6u))
                return false;
        } else if (!ksd_buffer_bytes(out, at, 1u)) {
            return false;
        }
    }
    return ksd_buffer_bytes(out, "\"", 1u);
}

static bool append_window(ksd_buffer *out, const ksd_wl_toplevel *item)
{
    char id[32];
    int length = snprintf(id, sizeof(id), "%llu", (unsigned long long)item->id);
    return length > 0 && (size_t)length < sizeof(id)
        && ksd_buffer_bytes(out, "{\"id\":", 6u)
        && append_json_string(out, id)
        && ksd_buffer_bytes(out, ",\"compositorId\":", 16u)
        && append_json_string(out, item->identifier)
        && ksd_buffer_bytes(out, ",\"title\":", 9u)
        && append_json_string(out, item->title)
        && ksd_buffer_bytes(out, ",\"appId\":", 9u)
        && append_json_string(out, item->app_id)
        && ksd_buffer_bytes(out,
            ",\"validFields\":[\"id\",\"compositorId\",\"title\",\"appId\"]}",
            sizeof(",\"validFields\":[\"id\",\"compositorId\",\"title\",\"appId\"]}") - 1u);
}

void ksd_wayland_window_query(ksd_wayland *connection,
    uint64_t handle, ksd_operation_result *result)
{
    if (connection != NULL && connection->toplevel_manager != NULL) {
        ksd_wayland_wlr_window_query(connection, handle, result);
        return;
    }
    if (ksd_wayland_cosmic_can_list(connection)) {
        ksd_wayland_cosmic_window_query(connection, handle, result);
        return;
    }
    if (!ksd_wayland_supported(connection).toplevel_list) {
        ksd_result_error(result, KSD_STATUS_UNSUPPORTED, 0u,
                         "this compositor does not list its windows");
        return;
    }
    if (!ksd_wayland_roundtrip(connection, KSD_WL_WINDOW_TIMEOUT_MS)) {
        ksd_result_error(result, KSD_STATUS_TIMEOUT, 0u,
                         "the compositor did not answer");
        return;
    }
    sweep_closed(connection);
    ksd_wl_toplevel *window = connection->toplevels;
    while (window != NULL && (window->id != handle || window->identifier == NULL))
        window = window->next;
    if (window == NULL) {
        ksd_result_error(result, KSD_STATUS_NOT_FOUND, 0u,
                         "the window no longer exists");
        return;
    }
    ksd_buffer out;
    ksd_buffer framed;
    ksd_buffer_init(&out, KSD_MAX_TEXT_BYTES);
    ksd_buffer_init(&framed, KSD_MAX_TEXT_BYTES + 4u);
    if (!ksd_buffer_bytes(&out, "{\"ok\":true,\"window\":", 20u)
        || !append_window(&out, window) || !ksd_buffer_bytes(&out, "}", 1u)
        || !ksd_buffer_u32(&framed, (uint32_t)out.length)
        || !ksd_buffer_bytes(&framed, out.data, out.length)
        || !ksd_result_copy(result, framed.data, (uint32_t)framed.length))
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "the window result is too large");
    ksd_buffer_clear(&framed);
    ksd_buffer_clear(&out);
}

/* Handles only. On this backend the difference from the window list is
 * smaller than on X11 -- the protocol carries so little that the list is
 * nearly handles already -- but the permission difference is the same, and a
 * caller that only wants to know what exists should not need a grant for it. */
void ksd_wayland_window_handles(ksd_wayland *connection,
                                ksd_operation_result *result)
{
    ksd_buffer out;
    ksd_buffer framed;
    bool ok;
    bool first = true;

    if (connection != NULL && connection->toplevel_manager != NULL) {
        ksd_wayland_wlr_window_handles(connection, result);
        return;
    }
    if (ksd_wayland_cosmic_can_list(connection)) {
        ksd_wayland_cosmic_window_handles(connection, result);
        return;
    }

    if (!ksd_wayland_supported(connection).toplevel_list) {
        ksd_result_error(result, KSD_STATUS_UNSUPPORTED, 0u,
                         "this compositor does not list its windows");
        return;
    }
    if (!ksd_wayland_roundtrip(connection, KSD_WL_WINDOW_TIMEOUT_MS)) {
        ksd_result_error(result, KSD_STATUS_TIMEOUT, 0u,
                         "the compositor did not answer");
        return;
    }
    sweep_closed(connection);

    ksd_buffer_init(&out, KSD_MAX_TEXT_BYTES);
    ok = ksd_buffer_bytes(&out, "{\"ok\":true,\"handles\":[", 22u);
    for (ksd_wl_toplevel *item = connection->toplevels;
         ok && item != NULL; item = item->next) {
        if (item->identifier == NULL)
            continue;
        if (!first)
            ok = ksd_buffer_bytes(&out, ",", 1u);
        first = false;
        char id[32];
        int length = snprintf(id, sizeof(id), "%llu", (unsigned long long)item->id);
        ok = ok && length > 0 && (size_t)length < sizeof(id)
            && append_json_string(&out, id);
    }
    ok = ok && ksd_buffer_bytes(&out, "]}", 2u);

    ksd_buffer_init(&framed, KSD_MAX_TEXT_BYTES + 4u);
    if (!ok || out.length > KSD_MAX_TEXT_BYTES
        || !ksd_buffer_u32(&framed, (uint32_t)out.length)
        || !ksd_buffer_bytes(&framed, out.data, out.length)
        || !ksd_result_copy(result, framed.data, (uint32_t)framed.length))
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "the handle list is too large");
    ksd_buffer_clear(&framed);
    ksd_buffer_clear(&out);
}

void ksd_wayland_window_list(ksd_wayland *connection, bool include_hidden,
                             ksd_operation_result *result)
{
    ksd_buffer out;
    ksd_buffer framed;
    bool ok;
    bool first = true;

    if (connection != NULL && connection->toplevel_manager != NULL) {
        ksd_wayland_wlr_window_list(connection, include_hidden, result);
        return;
    }
    if (ksd_wayland_cosmic_can_list(connection)) {
        ksd_wayland_cosmic_window_list(connection, include_hidden, result);
        return;
    }

    if (!ksd_wayland_supported(connection).toplevel_list) {
        ksd_result_error(result, KSD_STATUS_UNSUPPORTED, 0u,
                         "this compositor does not list its windows");
        return;
    }
    /* One round trip to collect whatever the compositor has to say since the
     * last call: new toplevels, retitles, and closures. */
    if (!ksd_wayland_roundtrip(connection, KSD_WL_WINDOW_TIMEOUT_MS)) {
        ksd_result_error(result, KSD_STATUS_TIMEOUT, 0u,
                         "the compositor did not answer");
        return;
    }
    sweep_closed(connection);

    ksd_buffer_init(&out, KSD_MAX_TEXT_BYTES);
    ok = ksd_buffer_bytes(&out, "{\"ok\":true,\"windows\":[", 22u);
    for (ksd_wl_toplevel *item = connection->toplevels;
         ok && item != NULL; item = item->next) {
        /* A toplevel with no identifier yet cannot be named by a caller, so
         * reporting it would hand back a window that cannot be used. */
        if (item->identifier == NULL)
            continue;
        if (!first)
            ok = ksd_buffer_bytes(&out, ",", 1u);
        first = false;
        ok = ok && append_window(&out, item);
        /* pid, frame, client, minimized and transparency are deliberately
         * ABSENT rather than zero. This protocol reports none of them, and a
         * zero would be read as a fact -- a window at the origin with no size,
         * owned by process zero. The consumer already treats an absent field
         * as unknown, which is exactly what it is. */
    }
    ok = ok && ksd_buffer_bytes(&out, "]}", 2u);

    /* The providers frame a JSON reply as a length and then the bytes. */
    ksd_buffer_init(&framed, KSD_MAX_TEXT_BYTES + 4u);
    if (!ok || out.length > KSD_MAX_TEXT_BYTES
        || !ksd_buffer_u32(&framed, (uint32_t)out.length)
        || !ksd_buffer_bytes(&framed, out.data, out.length)
        || !ksd_result_copy(result, framed.data, (uint32_t)framed.length))
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "the window list is too large");
    ksd_buffer_clear(&framed);
    ksd_buffer_clear(&out);
}

void ksd_wayland_active_window(ksd_wayland *connection,
                               ksd_operation_result *result)
{
    if (connection != NULL && connection->toplevel_manager != NULL)
        ksd_wayland_wlr_active_window(connection, result);
    else if (ksd_wayland_cosmic_can_list(connection))
        ksd_wayland_cosmic_active_window(connection, result);
    else
        ksd_result_error(result, KSD_STATUS_UNSUPPORTED, 0u,
                         "this compositor does not expose an active window");
}

void ksd_wayland_window_action(ksd_wayland *connection, uint16_t opcode,
                               uint64_t handle, uint32_t value,
                               ksd_operation_result *result)
{
    if (connection != NULL && connection->toplevel_manager != NULL)
        ksd_wayland_wlr_window_action(connection, opcode, handle, value,
                                      result);
    else if (ksd_wayland_cosmic_can_list(connection))
        ksd_wayland_cosmic_window_action(connection, opcode, handle, value,
                                         result);
    else
        ksd_result_error(result, KSD_STATUS_UNSUPPORTED, 0u,
                         "this compositor does not control foreign windows");
}
