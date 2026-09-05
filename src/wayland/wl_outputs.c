#include "wl_outputs.h"

#include "wl_internal.h"
#include "wl_cosmic_windows.h"

#include <limits.h>
#include <stdlib.h>

static void output_geometry(void *data, struct wl_output *proxy, int32_t x,
                            int32_t y, int32_t physical_width,
                            int32_t physical_height, int32_t subpixel,
                            const char *make, const char *model,
                            int32_t transform)
{
    ksd_wl_output *output = data;

    (void)proxy;
    (void)physical_width;
    (void)physical_height;
    (void)subpixel;
    (void)make;
    (void)model;
    output->x = x;
    output->y = y;
    output->transform = transform;
    if (wl_output_get_version(proxy) < 2u)
        output->done = true;
}

static void output_mode(void *data, struct wl_output *proxy, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh)
{
    ksd_wl_output *output = data;

    (void)proxy;
    (void)refresh;
    if ((flags & WL_OUTPUT_MODE_CURRENT) == 0u)
        return;
    output->mode_width = width;
    output->mode_height = height;
    output->current_mode = width > 0 && height > 0;
    if (wl_output_get_version(proxy) < 2u)
        output->done = true;
}

static void output_done(void *data, struct wl_output *proxy)
{
    (void)proxy;
    ((ksd_wl_output *)data)->done = true;
}

static void output_scale(void *data, struct wl_output *proxy, int32_t factor)
{
    (void)proxy;
    ((ksd_wl_output *)data)->scale = factor > 0 ? factor : 1;
}

static void output_name(void *data, struct wl_output *proxy, const char *name)
{
    (void)data;
    (void)proxy;
    (void)name;
}

static void output_description(void *data, struct wl_output *proxy,
                               const char *description)
{
    (void)data;
    (void)proxy;
    (void)description;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

static void xdg_logical_position(void *data, struct zxdg_output_v1 *proxy,
                                 int32_t x, int32_t y)
{
    ksd_wl_output *output = data;

    (void)proxy;
    output->logical_x = x;
    output->logical_y = y;
    output->logical_position = true;
}

static void xdg_logical_size(void *data, struct zxdg_output_v1 *proxy,
                             int32_t width, int32_t height)
{
    ksd_wl_output *output = data;

    (void)proxy;
    output->logical_width = width;
    output->logical_height = height;
    output->logical_size = width > 0 && height > 0;
}

static void xdg_done(void *data, struct zxdg_output_v1 *proxy)
{
    (void)proxy;
    ((ksd_wl_output *)data)->done = true;
}

static void xdg_name(void *data, struct zxdg_output_v1 *proxy,
                     const char *name)
{
    (void)data;
    (void)proxy;
    (void)name;
}

static void xdg_description(void *data, struct zxdg_output_v1 *proxy,
                            const char *description)
{
    (void)data;
    (void)proxy;
    (void)description;
}

static const struct zxdg_output_v1_listener xdg_listener = {
    .logical_position = xdg_logical_position,
    .logical_size = xdg_logical_size,
    .done = xdg_done,
    .name = xdg_name,
    .description = xdg_description,
};

static void bind_xdg(ksd_wayland *connection, ksd_wl_output *output)
{
    if (connection->xdg_output_manager == NULL || output->xdg_output != NULL)
        return;
    output->xdg_output = zxdg_output_manager_v1_get_xdg_output(
        connection->xdg_output_manager, output->output);
    if (output->xdg_output != NULL)
        zxdg_output_v1_add_listener(output->xdg_output, &xdg_listener,
                                    output);
}

void ksd_wayland_output_add(ksd_wayland *connection,
                            struct wl_registry *registry, uint32_t name,
                            uint32_t version)
{
    ksd_wl_output *output;
    ksd_wl_output **tail;
    uint32_t bind_version;

    if (connection == NULL || registry == NULL)
        return;
    for (output = connection->outputs; output != NULL; output = output->next)
        if (output->registry_name == name)
            return;
    output = calloc(1u, sizeof(*output));
    if (output == NULL)
        return;
    bind_version = version < 4u ? version : 4u;
    output->output = wl_registry_bind(registry, name, &wl_output_interface,
                                      bind_version);
    if (output->output == NULL) {
        free(output);
        return;
    }
    output->registry_name = name;
    output->scale = 1;
    wl_output_add_listener(output->output, &output_listener, output);
    tail = &connection->outputs;
    while (*tail != NULL)
        tail = &(*tail)->next;
    *tail = output;
    bind_xdg(connection, output);
}

static void free_output(ksd_wl_output *output)
{
    if (output->xdg_output != NULL)
        zxdg_output_v1_destroy(output->xdg_output);
    if (output->output != NULL)
        wl_output_destroy(output->output);
    free(output);
}

void ksd_wayland_output_remove(ksd_wayland *connection, uint32_t name)
{
    ksd_wl_output **slot;

    if (connection == NULL)
        return;
    slot = &connection->outputs;
    while (*slot != NULL) {
        ksd_wl_output *output = *slot;

        if (output->registry_name != name) {
            slot = &output->next;
            continue;
        }
        *slot = output->next;
        ksd_wayland_cosmic_output_removed(connection, output->output);
        free_output(output);
        return;
    }
}

void ksd_wayland_outputs_bind_xdg(ksd_wayland *connection)
{
    if (connection == NULL)
        return;
    for (ksd_wl_output *output = connection->outputs; output != NULL;
         output = output->next)
        bind_xdg(connection, output);
}

void ksd_wayland_outputs_clear(ksd_wayland *connection)
{
    ksd_wl_output *output;

    if (connection == NULL)
        return;
    output = connection->outputs;
    while (output != NULL) {
        ksd_wl_output *next = output->next;
        free_output(output);
        output = next;
    }
    connection->outputs = NULL;
}

bool ksd_wayland_output_bounds(const ksd_wl_output *output, int32_t *x,
                               int32_t *y, int32_t *width, int32_t *height)
{
    int64_t logical_width;
    int64_t logical_height;
    int32_t scale;

    if (output == NULL || x == NULL || y == NULL || width == NULL
        || height == NULL)
        return false;
    if (output->logical_position && output->logical_size) {
        *x = output->logical_x;
        *y = output->logical_y;
        *width = output->logical_width;
        *height = output->logical_height;
        return *width > 0 && *height > 0;
    }
    if (!output->current_mode)
        return false;
    scale = output->scale > 0 ? output->scale : 1;
    logical_width = (output->mode_width + scale / 2) / scale;
    logical_height = (output->mode_height + scale / 2) / scale;
    if (output->transform == WL_OUTPUT_TRANSFORM_90
        || output->transform == WL_OUTPUT_TRANSFORM_270
        || output->transform == WL_OUTPUT_TRANSFORM_FLIPPED_90
        || output->transform == WL_OUTPUT_TRANSFORM_FLIPPED_270) {
        int64_t swap = logical_width;
        logical_width = logical_height;
        logical_height = swap;
    }
    if (logical_width <= 0 || logical_height <= 0
        || logical_width > INT32_MAX || logical_height > INT32_MAX)
        return false;
    *x = output->x;
    *y = output->y;
    *width = (int32_t)logical_width;
    *height = (int32_t)logical_height;
    return true;
}
