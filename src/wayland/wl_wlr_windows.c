#include "wl_wlr_windows.h"

#include "protocol.h"
#include "protocol_io.h"
#include "wl_internal.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define KSD_WLR_WINDOW_TIMEOUT_MS 2000
#define KSD_WLR_MAX_TOPLEVELS 4096u

#define KSD_WLR_STATE_MAXIMIZED (1u << 0)
#define KSD_WLR_STATE_MINIMIZED (1u << 1)
#define KSD_WLR_STATE_ACTIVATED (1u << 2)

static void replace_string(char **slot, const char *value)
{
    char *copy;

    if (value == NULL)
        return;
    copy = strdup(value);
    if (copy == NULL)
        return;
    free(*slot);
    *slot = copy;
}

static void handle_title(void *data,
                         struct zwlr_foreign_toplevel_handle_v1 *handle,
                         const char *title)
{
    (void)handle;
    if (data != NULL)
        replace_string(&((ksd_wlr_toplevel *)data)->title, title);
}

static void handle_app_id(void *data,
                          struct zwlr_foreign_toplevel_handle_v1 *handle,
                          const char *app_id)
{
    (void)handle;
    if (data != NULL)
        replace_string(&((ksd_wlr_toplevel *)data)->app_id, app_id);
}

static void handle_output_enter(void *data,
                                struct zwlr_foreign_toplevel_handle_v1 *handle,
                                struct wl_output *output)
{
    (void)data;
    (void)handle;
    (void)output;
}

static void handle_output_leave(void *data,
                                struct zwlr_foreign_toplevel_handle_v1 *handle,
                                struct wl_output *output)
{
    (void)data;
    (void)handle;
    (void)output;
}

static void handle_state(void *data,
                         struct zwlr_foreign_toplevel_handle_v1 *handle,
                         struct wl_array *states)
{
    ksd_wlr_toplevel *toplevel = data;
    uint32_t state = 0u;
    uint32_t *item;

    (void)handle;
    if (toplevel == NULL || states == NULL)
        return;
    wl_array_for_each(item, states) {
        if (*item < 32u)
            state |= UINT32_C(1) << *item;
    }
    toplevel->state = state;
}

static void handle_done(void *data,
                        struct zwlr_foreign_toplevel_handle_v1 *handle)
{
    (void)handle;
    if (data != NULL)
        ((ksd_wlr_toplevel *)data)->ready = true;
}

static void handle_closed(void *data,
                          struct zwlr_foreign_toplevel_handle_v1 *handle)
{
    (void)handle;
    if (data != NULL)
        ((ksd_wlr_toplevel *)data)->closed = true;
}

static void handle_parent(void *data,
                          struct zwlr_foreign_toplevel_handle_v1 *handle,
                          struct zwlr_foreign_toplevel_handle_v1 *parent)
{
    (void)data;
    (void)handle;
    (void)parent;
}

static const struct zwlr_foreign_toplevel_handle_v1_listener handle_listener = {
    .title = handle_title,
    .app_id = handle_app_id,
    .output_enter = handle_output_enter,
    .output_leave = handle_output_leave,
    .state = handle_state,
    .done = handle_done,
    .closed = handle_closed,
    .parent = handle_parent,
};

static void manager_toplevel(
    void *data, struct zwlr_foreign_toplevel_manager_v1 *manager,
    struct zwlr_foreign_toplevel_handle_v1 *handle)
{
    ksd_wayland *connection = data;
    ksd_wlr_toplevel *fresh;
    ksd_wlr_toplevel **tail;
    size_t count = 0u;

    (void)manager;
    if (connection == NULL || handle == NULL)
        return;
    tail = &connection->wlr_toplevels;
    while (*tail != NULL) {
        count++;
        tail = &(*tail)->next;
    }
    if (count >= KSD_WLR_MAX_TOPLEVELS) {
        zwlr_foreign_toplevel_handle_v1_destroy(handle);
        return;
    }
    fresh = calloc(1u, sizeof(*fresh));
    if (fresh == NULL) {
        zwlr_foreign_toplevel_handle_v1_destroy(handle);
        return;
    }
    fresh->handle = handle;
    fresh->id = ksd_wayland_new_handle(connection);
    if (fresh->id == 0u) {
        zwlr_foreign_toplevel_handle_v1_destroy(handle);
        free(fresh);
        return;
    }
    *tail = fresh;
    zwlr_foreign_toplevel_handle_v1_add_listener(handle, &handle_listener,
                                                 fresh);
}

static void manager_finished(
    void *data, struct zwlr_foreign_toplevel_manager_v1 *manager)
{
    (void)data;
    (void)manager;
}

static const struct zwlr_foreign_toplevel_manager_v1_listener manager_listener = {
    .toplevel = manager_toplevel,
    .finished = manager_finished,
};

void ksd_wayland_wlr_toplevels_attach(ksd_wayland *connection)
{
    if (connection != NULL && connection->toplevel_manager != NULL)
        zwlr_foreign_toplevel_manager_v1_add_listener(
            connection->toplevel_manager, &manager_listener, connection);
}

static void free_toplevel(ksd_wlr_toplevel *item)
{
    if (item == NULL)
        return;
    if (item->handle != NULL)
        zwlr_foreign_toplevel_handle_v1_destroy(item->handle);
    free(item->title);
    free(item->app_id);
    free(item);
}

void ksd_wayland_wlr_toplevels_clear(ksd_wayland *connection)
{
    ksd_wlr_toplevel *item;

    if (connection == NULL)
        return;
    item = connection->wlr_toplevels;
    while (item != NULL) {
        ksd_wlr_toplevel *next = item->next;
        free_toplevel(item);
        item = next;
    }
    connection->wlr_toplevels = NULL;
}

static void sweep_closed(ksd_wayland *connection)
{
    ksd_wlr_toplevel **slot = &connection->wlr_toplevels;

    while (*slot != NULL) {
        ksd_wlr_toplevel *item = *slot;

        if (!item->closed) {
            slot = &item->next;
            continue;
        }
        *slot = item->next;
        free_toplevel(item);
    }
}

static bool append_json_string(ksd_buffer *out, const char *value)
{
    if (!ksd_buffer_bytes(out, "\"", 1u))
        return false;
    for (const char *at = value == NULL ? "" : value; *at != '\0'; at++) {
        unsigned char byte = (unsigned char)*at;
        char escape[8];

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

static bool append_window(ksd_buffer *out, const ksd_wlr_toplevel *item)
{
    char id[32];
    int length = snprintf(id, sizeof(id), "%llu",
                          (unsigned long long)item->id);

    return length > 0 && (size_t)length < sizeof(id)
        && ksd_buffer_bytes(out, "{\"id\":\"", 7u)
        && ksd_buffer_bytes(out, id, (size_t)length)
        && ksd_buffer_bytes(out, "\",\"title\":", 10u)
        && append_json_string(out, item->title)
        && ksd_buffer_bytes(out, ",\"appId\":", 9u)
        && append_json_string(out, item->app_id)
        && ksd_buffer_bytes(out, ",\"active\":", 10u)
        && ksd_buffer_bytes(out,
            (item->state & KSD_WLR_STATE_ACTIVATED) != 0u ? "true" : "false",
            (item->state & KSD_WLR_STATE_ACTIVATED) != 0u ? 4u : 5u)
        && ksd_buffer_bytes(out, ",\"minimized\":", 13u)
        && ksd_buffer_bytes(out,
            (item->state & KSD_WLR_STATE_MINIMIZED) != 0u ? "true" : "false",
            (item->state & KSD_WLR_STATE_MINIMIZED) != 0u ? 4u : 5u)
        && ksd_buffer_bytes(out, ",\"maximized\":", 13u)
        && ksd_buffer_bytes(out,
            (item->state & KSD_WLR_STATE_MAXIMIZED) != 0u ? "true" : "false",
            (item->state & KSD_WLR_STATE_MAXIMIZED) != 0u ? 4u : 5u)
        && ksd_buffer_bytes(out,
            ",\"validFields\":[\"id\",\"title\",\"appId\",\"active\","
            "\"minimized\",\"maximized\"]}",
            sizeof(",\"validFields\":[\"id\",\"title\",\"appId\",\"active\","
                   "\"minimized\",\"maximized\"]}") - 1u);
}

static bool finish_json(ksd_buffer *json, ksd_operation_result *result)
{
    ksd_buffer framed;
    bool ok;

    ksd_buffer_init(&framed, KSD_MAX_TEXT_BYTES + 4u);
    ok = json->length <= KSD_MAX_TEXT_BYTES
        && ksd_buffer_u32(&framed, (uint32_t)json->length)
        && ksd_buffer_bytes(&framed, json->data, json->length)
        && ksd_result_copy(result, framed.data, (uint32_t)framed.length);
    ksd_buffer_clear(&framed);
    if (!ok)
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "the window result is too large");
    return ok;
}

static bool refresh(ksd_wayland *connection, ksd_operation_result *result)
{
    if (connection == NULL || connection->toplevel_manager == NULL) {
        ksd_result_error(result, KSD_STATUS_UNSUPPORTED, 0u,
                         "this compositor does not manage foreign windows");
        return false;
    }
    if (!ksd_wayland_roundtrip(connection, KSD_WLR_WINDOW_TIMEOUT_MS)) {
        ksd_result_error(result, KSD_STATUS_TIMEOUT, 0u,
                         "the compositor did not answer");
        return false;
    }
    sweep_closed(connection);
    return true;
}

void ksd_wayland_wlr_window_handles(ksd_wayland *connection,
                                    ksd_operation_result *result)
{
    ksd_buffer out;
    bool ok;
    bool first = true;

    if (!refresh(connection, result))
        return;
    ksd_buffer_init(&out, KSD_MAX_TEXT_BYTES);
    ok = ksd_buffer_bytes(&out, "{\"ok\":true,\"handles\":[", 22u);
    for (ksd_wlr_toplevel *item = connection->wlr_toplevels;
         ok && item != NULL; item = item->next) {
        char id[32];
        int length;

        if (!item->ready)
            continue;
        length = snprintf(id, sizeof(id), "\"%llu\"",
                          (unsigned long long)item->id);
        if (!first)
            ok = ksd_buffer_bytes(&out, ",", 1u);
        first = false;
        ok = ok && length > 0 && (size_t)length < sizeof(id)
            && ksd_buffer_bytes(&out, id, (size_t)length);
    }
    ok = ok && ksd_buffer_bytes(&out, "]}", 2u);
    if (!ok)
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "the handle list is too large");
    else
        (void)finish_json(&out, result);
    ksd_buffer_clear(&out);
}

void ksd_wayland_wlr_window_list(ksd_wayland *connection,
                                 bool include_hidden,
                                 ksd_operation_result *result)
{
    ksd_buffer out;
    bool ok;
    bool first = true;

    if (!refresh(connection, result))
        return;
    ksd_buffer_init(&out, KSD_MAX_TEXT_BYTES);
    ok = ksd_buffer_bytes(&out, "{\"ok\":true,\"windows\":[", 22u);
    for (ksd_wlr_toplevel *item = connection->wlr_toplevels;
         ok && item != NULL; item = item->next) {
        if (!item->ready
            || (!include_hidden
                && (item->state & KSD_WLR_STATE_MINIMIZED) != 0u))
            continue;
        if (!first)
            ok = ksd_buffer_bytes(&out, ",", 1u);
        first = false;
        ok = ok && append_window(&out, item);
    }
    ok = ok && ksd_buffer_bytes(&out, "]}", 2u);
    if (!ok)
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "the window list is too large");
    else
        (void)finish_json(&out, result);
    ksd_buffer_clear(&out);
}

void ksd_wayland_wlr_active_window(ksd_wayland *connection,
                                   ksd_operation_result *result)
{
    ksd_buffer out;
    bool ok;
    ksd_wlr_toplevel *active = NULL;

    if (!refresh(connection, result))
        return;
    for (ksd_wlr_toplevel *item = connection->wlr_toplevels;
         item != NULL; item = item->next)
        if (item->ready
            && (item->state & KSD_WLR_STATE_ACTIVATED) != 0u) {
            active = item;
            break;
        }
    ksd_buffer_init(&out, KSD_MAX_TEXT_BYTES);
    ok = ksd_buffer_bytes(&out, "{\"ok\":true,\"window\":", 20u)
        && (active == NULL
            ? ksd_buffer_bytes(&out, "null", 4u)
            : append_window(&out, active))
        && ksd_buffer_bytes(&out, "}", 1u);
    if (!ok)
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "the active window result is too large");
    else
        (void)finish_json(&out, result);
    ksd_buffer_clear(&out);
}

static ksd_wlr_toplevel *find_window(ksd_wayland *connection, uint64_t id)
{
    for (ksd_wlr_toplevel *item = connection->wlr_toplevels;
         item != NULL; item = item->next)
        if (!item->closed && item->id == id)
            return item;
    return NULL;
}

void ksd_wayland_wlr_window_query(ksd_wayland *connection,
    uint64_t handle, ksd_operation_result *result)
{
    if (!refresh(connection, result))
        return;
    ksd_wlr_toplevel *window = find_window(connection, handle);
    if (window == NULL || !window->ready) {
        ksd_result_error(result, KSD_STATUS_NOT_FOUND, 0u,
                         "the window no longer exists");
        return;
    }
    ksd_buffer out;
    ksd_buffer_init(&out, KSD_MAX_TEXT_BYTES);
    bool ok = ksd_buffer_bytes(&out, "{\"ok\":true,\"window\":", 20u)
        && append_window(&out, window) && ksd_buffer_bytes(&out, "}", 1u);
    if (ok)
        (void)finish_json(&out, result);
    else
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "the window result is too large");
    ksd_buffer_clear(&out);
}

void ksd_wayland_wlr_window_action(ksd_wayland *connection, uint16_t opcode,
                                   uint64_t handle, uint32_t value,
                                   ksd_operation_result *result)
{
    ksd_wlr_toplevel *window;

    if (!refresh(connection, result))
        return;
    window = find_window(connection, handle);
    if (window == NULL) {
        ksd_result_error(result, KSD_STATUS_NOT_FOUND, 0u,
                         "the window no longer exists");
        return;
    }
    if (opcode == KSD_OP_WINDOW_FOCUS) {
        if (connection->seat == NULL) {
            ksd_result_error(result, KSD_STATUS_UNSUPPORTED, 0u,
                             "the compositor exposes no seat");
            return;
        }
        zwlr_foreign_toplevel_handle_v1_activate(window->handle,
                                                 connection->seat);
    } else if (opcode == KSD_OP_WINDOW_CLOSE) {
        zwlr_foreign_toplevel_handle_v1_close(window->handle);
    } else if (opcode == KSD_OP_WINDOW_SET_STATE) {
        if (value == 1u) {
            zwlr_foreign_toplevel_handle_v1_set_minimized(window->handle);
        } else if (value == 2u) {
            zwlr_foreign_toplevel_handle_v1_set_maximized(window->handle);
        } else {
            if ((window->state & KSD_WLR_STATE_MINIMIZED) != 0u)
                zwlr_foreign_toplevel_handle_v1_unset_minimized(window->handle);
            if ((window->state & KSD_WLR_STATE_MAXIMIZED) != 0u)
                zwlr_foreign_toplevel_handle_v1_unset_maximized(window->handle);
        }
    } else {
        ksd_result_error(result, KSD_STATUS_INVALID_REQUEST, 0u,
                         "invalid Wayland window operation");
        return;
    }
    if (!ksd_wayland_roundtrip(connection, KSD_WLR_WINDOW_TIMEOUT_MS)) {
        ksd_result_error(result, KSD_STATUS_TIMEOUT, 0u,
                         "the compositor did not acknowledge the request");
        return;
    }
    (void)ksd_result_copy(result, NULL, 0u);
}
