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

void ksd_wayland_replace_string(char **slot, const char *value)
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
        ksd_wayland_replace_string(&toplevel->title, title);
}

static void toplevel_app_id(void *data,
                            struct ext_foreign_toplevel_handle_v1 *handle,
                            const char *app_id)
{
    ksd_wl_toplevel *toplevel = data;

    (void)handle;
    if (toplevel != NULL)
        ksd_wayland_replace_string(&toplevel->app_id, app_id);
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
        ksd_wayland_replace_string(&toplevel->identifier, identifier);
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

static void free_toplevel(ksd_wl_toplevel *item)
{
    ksd_wayland_cosmic_detach_toplevel(item);
    if (item->handle != NULL)
        ext_foreign_toplevel_handle_v1_destroy(item->handle);
    if (item->wlr_handle != NULL)
        zwlr_foreign_toplevel_handle_v1_destroy(item->wlr_handle);
    free(item->title);
    free(item->app_id);
    free(item->identifier);
    free(item);
}

void ksd_wayland_toplevels_clear(ksd_wayland *connection)
{
    ksd_wl_toplevel *item;

    if (connection == NULL)
        return;
    item = connection->toplevels;
    while (item != NULL) {
        ksd_wl_toplevel *next = item->next;

        free_toplevel(item);
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
        free_toplevel(item);
    }
}

static bool append_window(ksd_buffer *out, ksd_wayland *connection,
                          const ksd_wl_toplevel *item)
{
    char id[32];
    const char *identifier = item->identifier == NULL ? "" : item->identifier;
    const char *title = item->title == NULL ? "" : item->title;
    const char *app_id = item->app_id == NULL ? "" : item->app_id;
    int length;

    (void)connection;
    length = snprintf(id, sizeof(id), "%llu", (unsigned long long)item->id);
    return length > 0 && (size_t)length < sizeof(id)
        && ksd_buffer_bytes(out, "{\"id\":", 6u)
        && ksd_buffer_json_string(out, id, (size_t)length, false)
        && ksd_buffer_bytes(out, ",\"compositorId\":", 16u)
        && ksd_buffer_json_string(out, identifier, strlen(identifier), false)
        && ksd_buffer_bytes(out, ",\"title\":", 9u)
        && ksd_buffer_json_string(out, title, strlen(title), false)
        && ksd_buffer_bytes(out, ",\"appId\":", 9u)
        && ksd_buffer_json_string(out, app_id, strlen(app_id), false)
        && ksd_buffer_bytes(out,
            ",\"validFields\":[\"id\",\"compositorId\",\"title\",\"appId\"]}",
            sizeof(",\"validFields\":[\"id\",\"compositorId\",\"title\",\"appId\"]}") - 1u);
}

static bool generic_usable(const ksd_wl_toplevel *item)
{
    return item->handle != NULL && !item->closed && item->identifier != NULL;
}

static uint32_t generic_state(const ksd_wl_toplevel *item)
{
    (void)item;
    return 0u;
}

static const ksd_wayland_window_view generic_view = {
    .usable = generic_usable,
    .state = generic_state,
    .append_window = append_window,
};

static const ksd_wayland_window_view *window_view(ksd_wayland *connection)
{
    if (connection == NULL)
        return NULL;
    if (connection->toplevel_manager != NULL)
        return ksd_wayland_wlr_window_view();
    if (ksd_wayland_cosmic_can_list(connection))
        return ksd_wayland_cosmic_window_view();
    return connection->toplevel_list == NULL ? NULL : &generic_view;
}

static bool refresh(ksd_wayland *connection,
                    const ksd_wayland_window_view *view,
                    ksd_operation_result *result)
{
    if (view == NULL) {
        ksd_result_error(result, KSD_STATUS_UNSUPPORTED, 0u,
                         "this compositor does not list its windows");
        return false;
    }
    if (!ksd_wayland_roundtrip(connection, KSD_WL_WINDOW_TIMEOUT_MS)) {
        ksd_result_error(result, KSD_STATUS_TIMEOUT, 0u,
                         "the compositor did not answer");
        return false;
    }
    sweep_closed(connection);
    return true;
}

static ksd_wl_toplevel *find_window(ksd_wayland *connection, uint64_t handle,
                                    const ksd_wayland_window_view *view)
{
    for (ksd_wl_toplevel *item = connection->toplevels;
         item != NULL; item = item->next)
        if (view->usable(item) && item->id == handle)
            return item;
    return NULL;
}

struct ksd_wl_toplevel *ksd_wayland_window_for_action(
    ksd_wayland *connection, uint64_t handle,
    const ksd_wayland_window_view *view, ksd_operation_result *result)
{
    ksd_wl_toplevel *window;

    if (!refresh(connection, view, result))
        return NULL;
    window = find_window(connection, handle, view);
    if (window == NULL)
        ksd_result_error(result, KSD_STATUS_NOT_FOUND, 0u,
                         "the window no longer exists");
    return window;
}

void ksd_wayland_window_query(ksd_wayland *connection, uint64_t handle,
                              ksd_operation_result *result)
{
    const ksd_wayland_window_view *view = window_view(connection);
    ksd_wl_toplevel *window = ksd_wayland_window_for_action(
        connection, handle, view, result);
    ksd_buffer out;

    if (window == NULL)
        return;
    ksd_buffer_init(&out, KSD_MAX_TEXT_BYTES);
    bool ok = ksd_buffer_bytes(&out, "{\"ok\":true,\"window\":", 20u)
        && view->append_window(&out, connection, window)
        && ksd_buffer_bytes(&out, "}", 1u);
    if (!ok)
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "the window result is too large");
    else
        (void)ksd_result_take_framed_text(&out, result,
            KSD_STATUS_RESOURCE_EXHAUSTED, "the window result is too large");
    ksd_buffer_clear(&out);
}

void ksd_wayland_window_handles(ksd_wayland *connection,
                                ksd_operation_result *result)
{
    const ksd_wayland_window_view *view = window_view(connection);
    ksd_buffer out;
    bool ok;
    bool first = true;

    if (!refresh(connection, view, result))
        return;
    ksd_buffer_init(&out, KSD_MAX_TEXT_BYTES);
    ok = ksd_buffer_bytes(&out, "{\"ok\":true,\"handles\":[", 22u);
    for (ksd_wl_toplevel *item = connection->toplevels;
         ok && item != NULL; item = item->next) {
        char id[32];
        int length;

        if (!view->usable(item))
            continue;
        length = snprintf(id, sizeof(id), "%llu",
                          (unsigned long long)item->id);
        if (!first)
            ok = ksd_buffer_bytes(&out, ",", 1u);
        first = false;
        ok = ok && length > 0 && (size_t)length < sizeof(id)
            && ksd_buffer_json_string(&out, id, (size_t)length, false);
    }
    ok = ok && ksd_buffer_bytes(&out, "]}", 2u);
    if (!ok)
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "the handle list is too large");
    else
        (void)ksd_result_take_framed_text(&out, result,
            KSD_STATUS_RESOURCE_EXHAUSTED, "the handle list is too large");
    ksd_buffer_clear(&out);
}

void ksd_wayland_window_list(ksd_wayland *connection, bool include_hidden,
                             ksd_operation_result *result)
{
    const ksd_wayland_window_view *view = window_view(connection);
    ksd_buffer out;
    bool ok;
    bool first = true;

    if (!refresh(connection, view, result))
        return;
    ksd_buffer_init(&out, KSD_MAX_TEXT_BYTES);
    ok = ksd_buffer_bytes(&out, "{\"ok\":true,\"windows\":[", 22u);
    for (ksd_wl_toplevel *item = connection->toplevels;
         ok && item != NULL; item = item->next) {
        if (!view->usable(item)
            || (!include_hidden && (view->state(item)
                & KSD_WL_TOPLEVEL_STATE_MINIMIZED) != 0u))
            continue;
        if (!first)
            ok = ksd_buffer_bytes(&out, ",", 1u);
        first = false;
        ok = ok && view->append_window(&out, connection, item);
    }
    ok = ok && ksd_buffer_bytes(&out, "]}", 2u);
    if (!ok)
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "the window list is too large");
    else
        (void)ksd_result_take_framed_text(&out, result,
            KSD_STATUS_RESOURCE_EXHAUSTED, "the window list is too large");
    ksd_buffer_clear(&out);
}

void ksd_wayland_active_window(ksd_wayland *connection,
                               ksd_operation_result *result)
{
    const ksd_wayland_window_view *view = window_view(connection);
    ksd_wl_toplevel *active = NULL;
    ksd_buffer out;
    bool ok;

    if (view == &generic_view) {
        ksd_result_error(result, KSD_STATUS_UNSUPPORTED, 0u,
                         "this compositor does not expose an active window");
        return;
    }
    if (!refresh(connection, view, result))
        return;
    for (ksd_wl_toplevel *item = connection->toplevels;
         item != NULL; item = item->next) {
        if (view->usable(item) && (view->state(item)
            & KSD_WL_TOPLEVEL_STATE_ACTIVATED) != 0u) {
            active = item;
            break;
        }
    }
    ksd_buffer_init(&out, KSD_MAX_TEXT_BYTES);
    ok = ksd_buffer_bytes(&out, "{\"ok\":true,\"window\":", 20u)
        && (active == NULL ? ksd_buffer_bytes(&out, "null", 4u)
            : view->append_window(&out, connection, active))
        && ksd_buffer_bytes(&out, "}", 1u);
    if (!ok)
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "the active window result is too large");
    else
        (void)ksd_result_take_framed_text(&out, result,
            KSD_STATUS_RESOURCE_EXHAUSTED,
            "the active window result is too large");
    ksd_buffer_clear(&out);
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
