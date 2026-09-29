#include "wl_displays.h"

#include "protocol.h"
#include "protocol_io.h"
#include "wl_hypr.h"
#include "wl_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KSD_WL_DISPLAY_TIMEOUT_MS 2000

static bool logical_bounds(const ksd_wl_output *output, int32_t *x, int32_t *y,
                           int32_t *width, int32_t *height)
{
    if (!output->logical_position || !output->logical_size
        || output->logical_width <= 0 || output->logical_height <= 0)
        return false;
    *x = output->logical_x;
    *y = output->logical_y;
    *width = output->logical_width;
    *height = output->logical_height;
    return true;
}

bool ksd_wayland_display_list_available(const ksd_wayland *connection)
{
    return connection != NULL && connection->xdg_output_manager != NULL
        && connection->outputs != NULL;
}

bool ksd_wayland_work_area_available(const ksd_wayland *connection)
{
    return ksd_wayland_display_list_available(connection)
        && connection->hypr;
}

/* Hyprland has no primary monitor, so the one at the layout origin stands in,
 * the same rule Keysharp applies to its own output list. */
static const ksd_wl_output *primary_output(const ksd_wayland *connection)
{
    const ksd_wl_output *lowest = NULL;

    for (const ksd_wl_output *output = connection->outputs; output != NULL;
         output = output->next) {
        int32_t x;
        int32_t y;
        int32_t width;
        int32_t height;

        if (!logical_bounds(output, &x, &y, &width, &height))
            continue;
        if (x == 0 && y == 0)
            return output;
        if (lowest == NULL || output->registry_name < lowest->registry_name)
            lowest = output;
    }
    return lowest;
}

/* Output events for a newly announced output arrive a round trip after the
 * registry names it, so a second one runs when the first leaves one short. */
static bool settle_outputs(ksd_wayland *connection)
{
    bool settled = true;

    if (!ksd_wayland_roundtrip(connection, KSD_WL_DISPLAY_TIMEOUT_MS))
        return false;
    for (const ksd_wl_output *output = connection->outputs; output != NULL;
         output = output->next)
        settled &= output->logical_position && output->logical_size;
    return settled
        || ksd_wayland_roundtrip(connection, KSD_WL_DISPLAY_TIMEOUT_MS);
}

bool ksd_wayland_reserved_work_area(int32_t x, int32_t y, int32_t width,
                                    int32_t height, const int32_t reserved[4],
                                    int32_t out[4])
{
    int64_t left = reserved[0] > 0 ? reserved[0] : 0;
    int64_t top = reserved[1] > 0 ? reserved[1] : 0;
    int64_t right = reserved[2] > 0 ? reserved[2] : 0;
    int64_t bottom = reserved[3] > 0 ? reserved[3] : 0;
    int64_t area_width = (int64_t)width - left - right;
    int64_t area_height = (int64_t)height - top - bottom;

    if (width <= 0 || height <= 0 || area_width <= 0 || area_height <= 0
        || (int64_t)x + left + area_width > INT32_MAX
        || (int64_t)y + top + area_height > INT32_MAX)
        return false;
    out[0] = (int32_t)(x + left);
    out[1] = (int32_t)(y + top);
    out[2] = (int32_t)area_width;
    out[3] = (int32_t)area_height;
    return true;
}

/* The Hyprland monitor behind an output, joined by name and required to sit
 * where xdg-output says, so a stale or renamed entry is never trusted. */
static const ksd_hypr_monitor *hypr_monitor(const ksd_hypr_monitor *monitors,
                                            size_t count,
                                            const ksd_wl_output *output,
                                            int32_t x, int32_t y)
{
    if (output->name[0] == 0)
        return NULL;
    for (size_t index = 0u; index < count; index++)
        if (strcmp(monitors[index].name, output->name) == 0)
            return monitors[index].x == x && monitors[index].y == y
                ? &monitors[index] : NULL;
    return NULL;
}

static int orientation(int32_t transform)
{
    return (transform & 3) * 90;
}

static bool append_display(ksd_buffer *out, const ksd_wl_output *output,
                           bool primary, const ksd_hypr_monitor *monitor)
{
    char text[512];
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    int32_t area[4];
    bool rotated = (output->transform & 1) != 0;
    int32_t mode_width = rotated ? output->mode_height : output->mode_width;
    bool has_scale = output->current_mode && mode_width > 0;
    bool has_physical = output->physical_width_mm > 0
        && output->physical_height_mm > 0;
    bool has_refresh = output->refresh_mhz > 0;
    bool has_area;
    int length;

    if (!logical_bounds(output, &x, &y, &width, &height))
        return true;
    has_area = monitor != NULL
        && ksd_wayland_reserved_work_area(x, y, width, height,
                                          monitor->reserved, area);
    if (!ksd_buffer_bytes(out, "{\"name\":", 8u)
        || !ksd_buffer_json_string(out, output->name, strlen(output->name),
                                   false))
        return false;
    length = snprintf(text, sizeof(text),
        ",\"output\":%u,\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d"
        ",\"primary\":%s,\"physicalWidth\":%d,\"physicalHeight\":%d"
        ",\"refreshRate\":%.6f,\"orientation\":%d,\"scale\":%.6f",
        output->registry_name, x, y, width, height,
        primary ? "true" : "false", output->physical_width_mm,
        output->physical_height_mm, output->refresh_mhz / 1000.0,
        orientation(output->transform),
        has_scale ? (double)mode_width / width : 1.0);
    if (length <= 0 || (size_t)length >= sizeof(text)
        || !ksd_buffer_bytes(out, text, (size_t)length))
        return false;
    if (has_area) {
        length = snprintf(text, sizeof(text),
            ",\"workArea\":{\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d}",
            area[0], area[1], area[2], area[3]);
        if (length <= 0 || (size_t)length >= sizeof(text)
            || !ksd_buffer_bytes(out, text, (size_t)length))
            return false;
    }
    if (monitor != NULL) {
        length = snprintf(text, sizeof(text), ",\"focused\":%s",
                          monitor->focused ? "true" : "false");
        if (length <= 0 || (size_t)length >= sizeof(text)
            || !ksd_buffer_bytes(out, text, (size_t)length))
            return false;
    }
    length = snprintf(text, sizeof(text),
        ",\"validFields\":[\"output\",\"x\",\"y\",\"width\",\"height\","
        "\"primary\",\"orientation\"%s%s%s%s%s%s]}",
        output->name[0] != 0 ? ",\"name\"" : "",
        has_physical ? ",\"physicalWidth\",\"physicalHeight\"" : "",
        has_refresh ? ",\"refreshRate\"" : "",
        has_scale ? ",\"scale\"" : "",
        has_area ? ",\"workArea\"" : "",
        monitor != NULL ? ",\"focused\"" : "");
    return length > 0 && (size_t)length < sizeof(text)
        && ksd_buffer_bytes(out, text, (size_t)length);
}

void ksd_wayland_display_list(ksd_wayland *connection,
                              ksd_operation_result *result)
{
    ksd_hypr_monitor *monitors = NULL;
    size_t count = 0u;
    const ksd_wl_output *primary;
    ksd_buffer out;
    bool ok;
    bool first = true;

    if (!ksd_wayland_display_list_available(connection)) {
        ksd_result_error(result, KSD_STATUS_UNSUPPORTED, 0u,
                         "the compositor does not report logical outputs");
        return;
    }
    if (!settle_outputs(connection)) {
        ksd_result_error(result, KSD_STATUS_TIMEOUT, 0u,
                         "the compositor did not answer");
        return;
    }
    /* Without Hyprland the list simply carries no work areas. */
    if (connection->hypr)
        (void)ksd_wayland_hypr_monitors(connection->session_pid, &monitors,
                                        &count);
    primary = primary_output(connection);
    ksd_buffer_init(&out, KSD_MAX_TEXT_BYTES);
    ok = ksd_buffer_bytes(&out, "{\"ok\":true,\"displays\":[", 23u);
    for (const ksd_wl_output *output = connection->outputs;
         ok && output != NULL; output = output->next) {
        int32_t x;
        int32_t y;
        int32_t width;
        int32_t height;

        if (!logical_bounds(output, &x, &y, &width, &height))
            continue;
        if (!first)
            ok = ksd_buffer_bytes(&out, ",", 1u);
        first = false;
        ok = ok && append_display(&out, output, output == primary,
                                  hypr_monitor(monitors, count, output, x, y));
    }
    ok = ok && ksd_buffer_bytes(&out, "]}", 2u);
    free(monitors);
    if (!ok)
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "the display list is too large");
    else
        (void)ksd_result_take_framed_text(&out, result,
            KSD_STATUS_RESOURCE_EXHAUSTED, "the display list is too large");
    ksd_buffer_clear(&out);
}

void ksd_wayland_work_area(ksd_wayland *connection,
                           ksd_operation_result *result)
{
    ksd_hypr_monitor *monitors = NULL;
    size_t count = 0u;
    const ksd_wl_output *primary;
    const ksd_hypr_monitor *monitor = NULL;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    int32_t area[4];
    uint8_t tail[16];
    bool found;

    if (!ksd_wayland_work_area_available(connection)) {
        ksd_result_error(result, KSD_STATUS_UNSUPPORTED, 0u,
                         "the compositor does not report a work area");
        return;
    }
    if (!settle_outputs(connection)) {
        ksd_result_error(result, KSD_STATUS_TIMEOUT, 0u,
                         "the compositor did not answer");
        return;
    }
    if (!ksd_wayland_hypr_monitors(connection->session_pid, &monitors,
                                   &count)) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "the compositor did not report its monitors");
        return;
    }
    primary = primary_output(connection);
    found = primary != NULL
        && logical_bounds(primary, &x, &y, &width, &height)
        && (monitor = hypr_monitor(monitors, count, primary, x, y)) != NULL
        && ksd_wayland_reserved_work_area(x, y, width, height,
                                          monitor->reserved, area);
    free(monitors);
    if (!found) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "the compositor reported no work area for the"
                         " primary output");
        return;
    }
    ksd_encode_u32(tail, (uint32_t)area[0]);
    ksd_encode_u32(tail + 4u, (uint32_t)area[1]);
    ksd_encode_u32(tail + 8u, (uint32_t)area[2]);
    ksd_encode_u32(tail + 12u, (uint32_t)area[3]);
    if (!ksd_result_copy(result, tail, sizeof(tail)))
        ksd_result_error(result, KSD_STATUS_INTERNAL, 0u,
                         "could not return the work area");
}
