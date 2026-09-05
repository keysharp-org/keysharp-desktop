#include "wl_cosmic_windows.h"

#include "protocol.h"
#include "protocol_io.h"
#include "wl_internal.h"
#include "wl_outputs.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KSD_COSMIC_WINDOW_TIMEOUT_MS 2000
#define KSD_COSMIC_CAPABILITY(value) (UINT32_C(1) << (value))
#define KSD_COSMIC_STATE_MAXIMIZED (UINT32_C(1) << 0)
#define KSD_COSMIC_STATE_MINIMIZED (UINT32_C(1) << 1)
#define KSD_COSMIC_STATE_ACTIVATED (UINT32_C(1) << 2)
#define KSD_COSMIC_STATE_FULLSCREEN (UINT32_C(1) << 3)

static void free_geometries(ksd_cosmic_geometry *geometry)
{
    while (geometry != NULL) {
        ksd_cosmic_geometry *next = geometry->next;
        free(geometry);
        geometry = next;
    }
}

static void handle_closed(void *data,
                          struct zcosmic_toplevel_handle_v1 *handle)
{
    (void)handle;
    if (data != NULL)
        ((ksd_wl_toplevel *)data)->closed = true;
}

static void handle_done(void *data,
                        struct zcosmic_toplevel_handle_v1 *handle)
{
    (void)handle;
    if (data != NULL)
        ((ksd_wl_toplevel *)data)->cosmic_ready = true;
}

static void handle_title(void *data,
                         struct zcosmic_toplevel_handle_v1 *handle,
                         const char *title)
{
    (void)data;
    (void)handle;
    (void)title;
}

static void handle_app_id(void *data,
                          struct zcosmic_toplevel_handle_v1 *handle,
                          const char *app_id)
{
    (void)data;
    (void)handle;
    (void)app_id;
}

static void handle_output_enter(void *data,
                                struct zcosmic_toplevel_handle_v1 *handle,
                                struct wl_output *output)
{
    (void)data;
    (void)handle;
    (void)output;
}

static void remove_geometry(ksd_wl_toplevel *toplevel,
                            struct wl_output *output)
{
    ksd_cosmic_geometry **slot;

    if (toplevel == NULL)
        return;
    slot = &toplevel->cosmic_geometries;
    while (*slot != NULL) {
        ksd_cosmic_geometry *item = *slot;
        if (item->output != output) {
            slot = &item->next;
            continue;
        }
        *slot = item->next;
        free(item);
    }
}

static void handle_output_leave(void *data,
                                struct zcosmic_toplevel_handle_v1 *handle,
                                struct wl_output *output)
{
    (void)handle;
    remove_geometry(data, output);
}

static void handle_workspace_enter(void *data,
                                   struct zcosmic_toplevel_handle_v1 *handle,
                                   void *workspace)
{
    (void)data;
    (void)handle;
    (void)workspace;
}

static void handle_workspace_leave(void *data,
                                   struct zcosmic_toplevel_handle_v1 *handle,
                                   void *workspace)
{
    (void)data;
    (void)handle;
    (void)workspace;
}

static void handle_state(void *data,
                         struct zcosmic_toplevel_handle_v1 *handle,
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
    toplevel->cosmic_state = state;
}

static void handle_geometry(void *data,
                            struct zcosmic_toplevel_handle_v1 *handle,
                            struct wl_output *output, int32_t x, int32_t y,
                            int32_t width, int32_t height)
{
    ksd_wl_toplevel *toplevel = data;
    ksd_cosmic_geometry *geometry;

    (void)handle;
    if (toplevel == NULL || output == NULL || width <= 0 || height <= 0)
        return;
    for (geometry = toplevel->cosmic_geometries; geometry != NULL;
         geometry = geometry->next)
        if (geometry->output == output)
            break;
    if (geometry == NULL) {
        geometry = calloc(1u, sizeof(*geometry));
        if (geometry == NULL)
            return;
        geometry->output = output;
        geometry->next = toplevel->cosmic_geometries;
        toplevel->cosmic_geometries = geometry;
    }
    geometry->x = x;
    geometry->y = y;
    geometry->width = width;
    geometry->height = height;
}

static const struct zcosmic_toplevel_handle_v1_listener handle_listener = {
    .closed = handle_closed,
    .done = handle_done,
    .title = handle_title,
    .app_id = handle_app_id,
    .output_enter = handle_output_enter,
    .output_leave = handle_output_leave,
    .workspace_enter = handle_workspace_enter,
    .workspace_leave = handle_workspace_leave,
    .state = handle_state,
    .geometry = handle_geometry,
};

static void info_toplevel(void *data, struct zcosmic_toplevel_info_v1 *info,
                          struct zcosmic_toplevel_handle_v1 *handle)
{
    (void)data;
    (void)info;
    if (handle != NULL)
        zcosmic_toplevel_handle_v1_destroy(handle);
}

static void info_finished(void *data,
                          struct zcosmic_toplevel_info_v1 *info)
{
    (void)data;
    (void)info;
}

static void info_done(void *data, struct zcosmic_toplevel_info_v1 *info)
{
    ksd_wayland *connection = data;

    (void)info;
    if (connection == NULL)
        return;
    for (ksd_wl_toplevel *item = connection->toplevels;
         item != NULL; item = item->next)
        if (item->cosmic_handle != NULL)
            item->cosmic_ready = true;
}

static const struct zcosmic_toplevel_info_v1_listener info_listener = {
    .toplevel = info_toplevel,
    .finished = info_finished,
    .done = info_done,
};

static void manager_capabilities(
    void *data, struct zcosmic_toplevel_manager_v1 *manager,
    struct wl_array *capabilities)
{
    ksd_wayland *connection = data;
    uint32_t available = 0u;
    uint32_t *item;

    (void)manager;
    if (connection == NULL || capabilities == NULL)
        return;
    wl_array_for_each(item, capabilities) {
        if (*item < 32u)
            available |= KSD_COSMIC_CAPABILITY(*item);
    }
    connection->cosmic_capabilities = available;
}

static const struct zcosmic_toplevel_manager_v1_listener manager_listener = {
    .capabilities = manager_capabilities,
};

void ksd_wayland_cosmic_bind_info(ksd_wayland *connection,
                                  struct wl_registry *registry,
                                  uint32_t name, uint32_t version)
{
    if (connection == NULL || registry == NULL || version < 2u
        || connection->cosmic_toplevel_info != NULL)
        return;
    connection->cosmic_toplevel_info = wl_registry_bind(
        registry, name, &zcosmic_toplevel_info_v1_interface, 2u);
    if (connection->cosmic_toplevel_info == NULL)
        return;
    zcosmic_toplevel_info_v1_add_listener(connection->cosmic_toplevel_info,
                                          &info_listener, connection);
    for (ksd_wl_toplevel *item = connection->toplevels;
         item != NULL; item = item->next)
        ksd_wayland_cosmic_attach_toplevel(connection, item);
}

void ksd_wayland_cosmic_bind_manager(ksd_wayland *connection,
                                     struct wl_registry *registry,
                                     uint32_t name, uint32_t version)
{
    if (connection == NULL || registry == NULL || version < 1u
        || connection->cosmic_toplevel_manager != NULL)
        return;
    connection->cosmic_toplevel_manager = wl_registry_bind(
        registry, name, &zcosmic_toplevel_manager_v1_interface, 1u);
    if (connection->cosmic_toplevel_manager != NULL)
        zcosmic_toplevel_manager_v1_add_listener(
            connection->cosmic_toplevel_manager, &manager_listener,
            connection);
}

void ksd_wayland_cosmic_attach_toplevel(ksd_wayland *connection,
                                        ksd_wl_toplevel *toplevel)
{
    if (connection == NULL || toplevel == NULL || toplevel->handle == NULL
        || toplevel->cosmic_handle != NULL
        || connection->cosmic_toplevel_info == NULL)
        return;
    toplevel->cosmic_handle =
        zcosmic_toplevel_info_v1_get_cosmic_toplevel(
            connection->cosmic_toplevel_info, toplevel->handle);
    if (toplevel->cosmic_handle == NULL)
        return;
    if (zcosmic_toplevel_handle_v1_add_listener(toplevel->cosmic_handle,
                                                &handle_listener,
                                                toplevel) < 0) {
        zcosmic_toplevel_handle_v1_destroy(toplevel->cosmic_handle);
        toplevel->cosmic_handle = NULL;
    }
}

void ksd_wayland_cosmic_detach_toplevel(ksd_wl_toplevel *toplevel)
{
    if (toplevel == NULL)
        return;
    if (toplevel->cosmic_handle != NULL) {
        zcosmic_toplevel_handle_v1_destroy(toplevel->cosmic_handle);
        toplevel->cosmic_handle = NULL;
    }
    free_geometries(toplevel->cosmic_geometries);
    toplevel->cosmic_geometries = NULL;
    toplevel->cosmic_ready = false;
}

void ksd_wayland_cosmic_output_removed(ksd_wayland *connection,
                                       struct wl_output *output)
{
    if (connection == NULL || output == NULL)
        return;
    for (ksd_wl_toplevel *item = connection->toplevels;
         item != NULL; item = item->next)
        remove_geometry(item, output);
}

void ksd_wayland_cosmic_clear(ksd_wayland *connection)
{
    if (connection == NULL)
        return;
    if (connection->cosmic_toplevel_manager != NULL) {
        zcosmic_toplevel_manager_v1_destroy(
            connection->cosmic_toplevel_manager);
        connection->cosmic_toplevel_manager = NULL;
    }
    if (connection->cosmic_toplevel_info != NULL) {
        zcosmic_toplevel_info_v1_destroy(connection->cosmic_toplevel_info);
        connection->cosmic_toplevel_info = NULL;
    }
    connection->cosmic_capabilities = 0u;
}

bool ksd_wayland_cosmic_can_list(const ksd_wayland *connection)
{
    return connection != NULL && connection->toplevel_list != NULL
        && connection->cosmic_toplevel_info != NULL;
}

static bool has_capability(const ksd_wayland *connection, uint32_t value)
{
    return connection != NULL && value < 32u
        && (connection->cosmic_capabilities
            & KSD_COSMIC_CAPABILITY(value)) != 0u;
}

bool ksd_wayland_cosmic_can_focus(const ksd_wayland *connection)
{
    return ksd_wayland_cosmic_can_list(connection)
        && connection->cosmic_toplevel_manager != NULL
        && connection->seat != NULL
        && has_capability(connection,
            ZCOSMIC_TOPLEVEL_MANAGER_V1_CAPABILITY_ACTIVATE);
}

bool ksd_wayland_cosmic_can_close(const ksd_wayland *connection)
{
    return ksd_wayland_cosmic_can_list(connection)
        && connection->cosmic_toplevel_manager != NULL
        && has_capability(connection,
            ZCOSMIC_TOPLEVEL_MANAGER_V1_CAPABILITY_CLOSE);
}

bool ksd_wayland_cosmic_can_set_state(const ksd_wayland *connection)
{
    return ksd_wayland_cosmic_can_list(connection)
        && connection->cosmic_toplevel_manager != NULL
        && has_capability(connection,
            ZCOSMIC_TOPLEVEL_MANAGER_V1_CAPABILITY_MAXIMIZE)
        && has_capability(connection,
            ZCOSMIC_TOPLEVEL_MANAGER_V1_CAPABILITY_MINIMIZE);
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
    if (!ksd_wayland_cosmic_can_list(connection)) {
        ksd_result_error(result, KSD_STATUS_UNSUPPORTED, 0u,
                         "COSMIC window information is unavailable");
        return false;
    }
    if (!ksd_wayland_roundtrip(connection, KSD_COSMIC_WINDOW_TIMEOUT_MS)) {
        ksd_result_error(result, KSD_STATUS_TIMEOUT, 0u,
                         "the compositor did not answer");
        return false;
    }
    return true;
}

static bool resolve_geometry(ksd_wayland *connection,
                             const ksd_wl_toplevel *toplevel,
                             int32_t *x, int32_t *y,
                             int32_t *width, int32_t *height)
{
    for (ksd_cosmic_geometry *geometry = toplevel->cosmic_geometries;
         geometry != NULL; geometry = geometry->next) {
        int32_t output_x;
        int32_t output_y;
        int32_t output_width;
        int32_t output_height;
        int64_t global_x;
        int64_t global_y;

        for (ksd_wl_output *output = connection->outputs;
             output != NULL; output = output->next) {
            if (output->output != geometry->output
                || !ksd_wayland_output_bounds(output, &output_x, &output_y,
                                               &output_width,
                                               &output_height))
                continue;
            global_x = (int64_t)output_x + geometry->x;
            global_y = (int64_t)output_y + geometry->y;
            if (geometry->width <= 0 || geometry->height <= 0
                || global_x < INT32_MIN || global_x > INT32_MAX
                || global_y < INT32_MIN || global_y > INT32_MAX)
                return false;
            *x = (int32_t)global_x;
            *y = (int32_t)global_y;
            *width = geometry->width;
            *height = geometry->height;
            return true;
        }
    }
    return false;
}

static bool append_window(ksd_buffer *out, ksd_wayland *connection,
                          const ksd_wl_toplevel *item)
{
    char id[32];
    char geometry[320];
    int id_length = snprintf(id, sizeof(id), "%llu",
                             (unsigned long long)item->id);
    bool active = (item->cosmic_state
        & KSD_COSMIC_STATE_ACTIVATED) != 0u;
    bool minimized = (item->cosmic_state
        & KSD_COSMIC_STATE_MINIMIZED) != 0u;
    bool maximized = (item->cosmic_state
        & (KSD_COSMIC_STATE_MAXIMIZED | KSD_COSMIC_STATE_FULLSCREEN)) != 0u;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    bool has_geometry;

    if (id_length <= 0 || (size_t)id_length >= sizeof(id)
        || !ksd_buffer_bytes(out, "{\"id\":\"", 7u)
        || !ksd_buffer_bytes(out, id, (size_t)id_length)
        || !ksd_buffer_bytes(out, "\",\"title\":", 10u)
        || !append_json_string(out, item->title)
        || !ksd_buffer_bytes(out, ",\"appId\":", 9u)
        || !append_json_string(out, item->app_id))
        return false;
    has_geometry = resolve_geometry(connection, item, &x, &y, &width, &height);
    if (has_geometry) {
        int length = snprintf(geometry, sizeof(geometry),
            ",\"frame\":{\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d}"
            ",\"client\":{\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d}",
            x, y, width, height, x, y, width, height);
        if (length <= 0 || (size_t)length >= sizeof(geometry)
            || !ksd_buffer_bytes(out, geometry, (size_t)length))
            return false;
    }
    return ksd_buffer_bytes(out, active
            ? ",\"active\":true" : ",\"active\":false",
            active ? sizeof(",\"active\":true") - 1u
                   : sizeof(",\"active\":false") - 1u)
        && ksd_buffer_bytes(out, minimized
            ? ",\"minimized\":true" : ",\"minimized\":false",
            minimized ? sizeof(",\"minimized\":true") - 1u
                      : sizeof(",\"minimized\":false") - 1u)
        && ksd_buffer_bytes(out, maximized
            ? ",\"maximized\":true" : ",\"maximized\":false",
            maximized ? sizeof(",\"maximized\":true") - 1u
                      : sizeof(",\"maximized\":false") - 1u)
        && ksd_buffer_bytes(out,
            ",\"validFields\":[\"id\",\"title\",\"appId\",\"active\","
            "\"minimized\",\"maximized\"",
            sizeof(",\"validFields\":[\"id\",\"title\",\"appId\",\"active\","
                   "\"minimized\",\"maximized\"") - 1u)
        && (!has_geometry || ksd_buffer_bytes(out, ",\"frame\"",
                                    sizeof(",\"frame\"") - 1u))
        && ksd_buffer_bytes(out, "]}", 2u);
}

static bool usable(const ksd_wl_toplevel *item)
{
    return item != NULL && !item->closed && item->cosmic_handle != NULL
        && item->cosmic_ready && item->identifier != NULL && item->id != 0u;
}

void ksd_wayland_cosmic_window_handles(ksd_wayland *connection,
                                       ksd_operation_result *result)
{
    ksd_buffer out;
    bool ok;
    bool first = true;

    if (!refresh(connection, result))
        return;
    ksd_buffer_init(&out, KSD_MAX_TEXT_BYTES);
    ok = ksd_buffer_bytes(&out, "{\"ok\":true,\"handles\":[", 22u);
    for (ksd_wl_toplevel *item = connection->toplevels;
         ok && item != NULL; item = item->next) {
        char id[32];
        int length;
        if (!usable(item))
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

void ksd_wayland_cosmic_window_list(ksd_wayland *connection,
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
    for (ksd_wl_toplevel *item = connection->toplevels;
         ok && item != NULL; item = item->next) {
        if (!usable(item)
            || (!include_hidden
                && (item->cosmic_state & KSD_COSMIC_STATE_MINIMIZED) != 0u))
            continue;
        if (!first)
            ok = ksd_buffer_bytes(&out, ",", 1u);
        first = false;
        ok = ok && append_window(&out, connection, item);
    }
    ok = ok && ksd_buffer_bytes(&out, "]}", 2u);
    if (!ok)
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "the window list is too large");
    else
        (void)finish_json(&out, result);
    ksd_buffer_clear(&out);
}

void ksd_wayland_cosmic_active_window(ksd_wayland *connection,
                                      ksd_operation_result *result)
{
    ksd_buffer out;
    bool ok;
    ksd_wl_toplevel *active = NULL;

    if (!refresh(connection, result))
        return;
    for (ksd_wl_toplevel *item = connection->toplevels;
         item != NULL; item = item->next)
        if (usable(item)
            && (item->cosmic_state & KSD_COSMIC_STATE_ACTIVATED) != 0u) {
            active = item;
            break;
        }
    ksd_buffer_init(&out, KSD_MAX_TEXT_BYTES);
    ok = ksd_buffer_bytes(&out, "{\"ok\":true,\"window\":", 20u)
        && (active == NULL
            ? ksd_buffer_bytes(&out, "null", 4u)
            : append_window(&out, connection, active))
        && ksd_buffer_bytes(&out, "}", 1u);
    if (!ok)
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "the active window result is too large");
    else
        (void)finish_json(&out, result);
    ksd_buffer_clear(&out);
}

static ksd_wl_toplevel *find_window(ksd_wayland *connection, uint64_t id)
{
    for (ksd_wl_toplevel *item = connection->toplevels;
         item != NULL; item = item->next)
        if (usable(item) && item->id == id)
            return item;
    return NULL;
}

void ksd_wayland_cosmic_window_query(ksd_wayland *connection,
    uint64_t handle, ksd_operation_result *result)
{
    if (!refresh(connection, result))
        return;
    ksd_wl_toplevel *window = find_window(connection, handle);
    if (window == NULL) {
        ksd_result_error(result, KSD_STATUS_NOT_FOUND, 0u,
                         "the window no longer exists");
        return;
    }
    ksd_buffer out;
    ksd_buffer_init(&out, KSD_MAX_TEXT_BYTES);
    bool ok = ksd_buffer_bytes(&out, "{\"ok\":true,\"window\":", 20u)
        && append_window(&out, connection, window) && ksd_buffer_bytes(&out, "}", 1u);
    if (ok)
        (void)finish_json(&out, result);
    else
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "the window result is too large");
    ksd_buffer_clear(&out);
}

void ksd_wayland_cosmic_window_action(ksd_wayland *connection,
                                      uint16_t opcode, uint64_t handle,
                                      uint32_t value,
                                      ksd_operation_result *result)
{
    ksd_wl_toplevel *window;

    if (!refresh(connection, result))
        return;
    window = find_window(connection, handle);
    if (window == NULL) {
        ksd_result_error(result, KSD_STATUS_NOT_FOUND, 0u,
                         "the window no longer exists");
        return;
    }
    if (opcode == KSD_OP_WINDOW_FOCUS
        && ksd_wayland_cosmic_can_focus(connection)) {
        zcosmic_toplevel_manager_v1_activate(
            connection->cosmic_toplevel_manager, window->cosmic_handle,
            connection->seat);
    } else if (opcode == KSD_OP_WINDOW_CLOSE
               && ksd_wayland_cosmic_can_close(connection)) {
        zcosmic_toplevel_manager_v1_close(
            connection->cosmic_toplevel_manager, window->cosmic_handle);
    } else if (opcode == KSD_OP_WINDOW_SET_STATE
               && ksd_wayland_cosmic_can_set_state(connection)) {
        if (value == 1u) {
            zcosmic_toplevel_manager_v1_set_minimized(
                connection->cosmic_toplevel_manager,
                window->cosmic_handle);
        } else if (value == 2u) {
            zcosmic_toplevel_manager_v1_set_maximized(
                connection->cosmic_toplevel_manager,
                window->cosmic_handle);
        } else {
            if ((window->cosmic_state & KSD_COSMIC_STATE_MINIMIZED) != 0u)
                zcosmic_toplevel_manager_v1_unset_minimized(
                    connection->cosmic_toplevel_manager,
                    window->cosmic_handle);
            if ((window->cosmic_state
                 & KSD_COSMIC_STATE_MAXIMIZED) != 0u)
                zcosmic_toplevel_manager_v1_unset_maximized(
                    connection->cosmic_toplevel_manager,
                    window->cosmic_handle);
        }
    } else {
        ksd_result_error(result, KSD_STATUS_UNSUPPORTED, 0u,
                         "COSMIC does not advertise this window action");
        return;
    }
    if (!ksd_wayland_roundtrip(connection, KSD_COSMIC_WINDOW_TIMEOUT_MS)) {
        ksd_result_error(result, KSD_STATUS_TIMEOUT, 0u,
                         "the compositor did not acknowledge the request");
        return;
    }
    (void)ksd_result_copy(result, NULL, 0u);
}
