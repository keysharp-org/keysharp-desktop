#include "wl_wlr_windows.h"

#include "protocol.h"
#include "protocol_io.h"
#include "wl_internal.h"
#include "wl_windows.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define KSD_WLR_WINDOW_TIMEOUT_MS 2000
#define KSD_WLR_MAX_TOPLEVELS 4096u

static void handle_title(void *data,
                         struct zwlr_foreign_toplevel_handle_v1 *handle,
                         const char *title)
{
    (void)handle;
    if (data != NULL)
        ksd_wayland_replace_string(&((ksd_wl_toplevel *)data)->title, title);
}

static void handle_app_id(void *data,
                          struct zwlr_foreign_toplevel_handle_v1 *handle,
                          const char *app_id)
{
    (void)handle;
    if (data != NULL)
        ksd_wayland_replace_string(&((ksd_wl_toplevel *)data)->app_id, app_id);
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
    ksd_wl_toplevel *toplevel = data;
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
        ((ksd_wl_toplevel *)data)->ready = true;
}

static void handle_closed(void *data,
                          struct zwlr_foreign_toplevel_handle_v1 *handle)
{
    (void)handle;
    if (data != NULL)
        ((ksd_wl_toplevel *)data)->closed = true;
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
    ksd_wl_toplevel *fresh;
    ksd_wl_toplevel **tail;
    size_t count = 0u;

    (void)manager;
    if (connection == NULL || handle == NULL)
        return;
    tail = &connection->toplevels;
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
    fresh->wlr_handle = handle;
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

static bool append_window(ksd_buffer *out, ksd_wayland *connection,
                          const ksd_wl_toplevel *item)
{
    char id[32];
    const char *title = item->title == NULL ? "" : item->title;
    const char *app_id = item->app_id == NULL ? "" : item->app_id;
    int length;

    (void)connection;
    length = snprintf(id, sizeof(id), "%llu",
                          (unsigned long long)item->id);

    return length > 0 && (size_t)length < sizeof(id)
        && ksd_buffer_bytes(out, "{\"id\":\"", 7u)
        && ksd_buffer_bytes(out, id, (size_t)length)
        && ksd_buffer_bytes(out, "\",\"title\":", 10u)
        && ksd_buffer_json_string(out, title, strlen(title), false)
        && ksd_buffer_bytes(out, ",\"appId\":", 9u)
        && ksd_buffer_json_string(out, app_id, strlen(app_id), false)
        && ksd_buffer_bytes(out, ",\"active\":", 10u)
        && ksd_buffer_bytes(out,
            (item->state & KSD_WL_TOPLEVEL_STATE_ACTIVATED) != 0u ? "true" : "false",
            (item->state & KSD_WL_TOPLEVEL_STATE_ACTIVATED) != 0u ? 4u : 5u)
        && ksd_buffer_bytes(out, ",\"minimized\":", 13u)
        && ksd_buffer_bytes(out,
            (item->state & KSD_WL_TOPLEVEL_STATE_MINIMIZED) != 0u ? "true" : "false",
            (item->state & KSD_WL_TOPLEVEL_STATE_MINIMIZED) != 0u ? 4u : 5u)
        && ksd_buffer_bytes(out, ",\"maximized\":", 13u)
        && ksd_buffer_bytes(out,
            (item->state & KSD_WL_TOPLEVEL_STATE_MAXIMIZED) != 0u ? "true" : "false",
            (item->state & KSD_WL_TOPLEVEL_STATE_MAXIMIZED) != 0u ? 4u : 5u)
        && ksd_buffer_bytes(out,
            ",\"validFields\":[\"id\",\"title\",\"appId\",\"active\","
            "\"minimized\",\"maximized\"]}",
            sizeof(",\"validFields\":[\"id\",\"title\",\"appId\",\"active\","
                   "\"minimized\",\"maximized\"]}") - 1u);
}

static bool usable(const ksd_wl_toplevel *item)
{
    return item->wlr_handle != NULL && item->ready;
}

static uint32_t state(const ksd_wl_toplevel *item)
{
    return item->state;
}

const ksd_wayland_window_view *ksd_wayland_wlr_window_view(void)
{
    static const ksd_wayland_window_view view = {
        .usable = usable,
        .state = state,
        .append_window = append_window,
    };

    return &view;
}

void ksd_wayland_wlr_window_action(ksd_wayland *connection, uint16_t opcode,
                                   uint64_t handle, uint32_t value,
                                   ksd_operation_result *result)
{
    ksd_wl_toplevel *window;

    window = ksd_wayland_window_for_action(
        connection, handle, ksd_wayland_wlr_window_view(), result);
    if (window == NULL)
        return;
    if (opcode == KSD_OP_WINDOW_FOCUS) {
        if (connection->seat == NULL) {
            ksd_result_error(result, KSD_STATUS_UNSUPPORTED, 0u,
                             "the compositor exposes no seat");
            return;
        }
        zwlr_foreign_toplevel_handle_v1_activate(window->wlr_handle,
                                                 connection->seat);
    } else if (opcode == KSD_OP_WINDOW_CLOSE) {
        zwlr_foreign_toplevel_handle_v1_close(window->wlr_handle);
    } else if (opcode == KSD_OP_WINDOW_SET_STATE) {
        if (value == 1u) {
            zwlr_foreign_toplevel_handle_v1_set_minimized(window->wlr_handle);
        } else if (value == 2u) {
            zwlr_foreign_toplevel_handle_v1_set_maximized(window->wlr_handle);
        } else {
            if ((window->state & KSD_WL_TOPLEVEL_STATE_MINIMIZED) != 0u)
                zwlr_foreign_toplevel_handle_v1_unset_minimized(window->wlr_handle);
            if ((window->state & KSD_WL_TOPLEVEL_STATE_MAXIMIZED) != 0u)
                zwlr_foreign_toplevel_handle_v1_unset_maximized(window->wlr_handle);
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
