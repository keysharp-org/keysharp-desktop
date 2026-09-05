#include "wl_pointer.h"

#include "wl_internal.h"
#include "wl_outputs.h"
#include "wl_hypr.h"

#include <errno.h>
#include <limits.h>
#include <time.h>
#include <wayland-client.h>

void ksd_wayland_pointer_create(ksd_wayland *connection)
{
    if (connection == NULL || connection->virtual_pointer != NULL
        || connection->pointer_manager == NULL)
        return;
    connection->virtual_pointer =
        zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
            connection->pointer_manager, connection->seat);
}

void ksd_wayland_pointer_clear(ksd_wayland *connection)
{
    if (connection == NULL)
        return;
    if (connection->virtual_pointer != NULL) {
        zwlr_virtual_pointer_v1_destroy(connection->virtual_pointer);
        connection->virtual_pointer = NULL;
    }
    if (connection->pointer_manager != NULL) {
        zwlr_virtual_pointer_manager_v1_destroy(connection->pointer_manager);
        connection->pointer_manager = NULL;
    }
}

static uint32_t timestamp_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0u;
    return (uint32_t)((uint64_t)now.tv_sec * 1000u
        + (uint64_t)now.tv_nsec / 1000000u);
}

static bool desktop_bounds(ksd_wayland *connection, int32_t *left,
                           int32_t *top, uint32_t *width, uint32_t *height)
{
    int64_t minimum_x = INT64_MAX;
    int64_t minimum_y = INT64_MAX;
    int64_t maximum_x = INT64_MIN;
    int64_t maximum_y = INT64_MIN;

    for (ksd_wl_output *output = connection->outputs; output != NULL;
         output = output->next) {
        int32_t x;
        int32_t y;
        int32_t output_width;
        int32_t output_height;

        if (!ksd_wayland_output_bounds(output, &x, &y, &output_width,
                                       &output_height))
            continue;
        if (x < minimum_x)
            minimum_x = x;
        if (y < minimum_y)
            minimum_y = y;
        if ((int64_t)x + output_width > maximum_x)
            maximum_x = (int64_t)x + output_width;
        if ((int64_t)y + output_height > maximum_y)
            maximum_y = (int64_t)y + output_height;
    }
    uint64_t span_x = maximum_x > minimum_x
        ? (uint64_t)(maximum_x - minimum_x) : 0u;
    uint64_t span_y = maximum_y > minimum_y
        ? (uint64_t)(maximum_y - minimum_y) : 0u;
    if (minimum_x < INT32_MIN || minimum_x > INT32_MAX
        || minimum_y < INT32_MIN || minimum_y > INT32_MAX
        || span_x == 0u || span_x > UINT32_MAX
        || span_y == 0u || span_y > UINT32_MAX)
        return false;
    *left = (int32_t)minimum_x;
    *top = (int32_t)minimum_y;
    *width = (uint32_t)span_x;
    *height = (uint32_t)span_y;
    return true;
}

void ksd_wayland_move_absolute(ksd_wayland *connection, int32_t x, int32_t y,
                               ksd_operation_result *result)
{
    int32_t left;
    int32_t top;
    uint32_t width;
    uint32_t height;
    int64_t local_x;
    int64_t local_y;

    if (connection == NULL || result == NULL) {
        ksd_result_error(result, KSD_STATUS_INVALID_REQUEST, 0u,
                         "invalid absolute pointer request");
        return;
    }
    if (ksd_wayland_hypr_move(connection->session_pid, x, y)) {
        (void)ksd_result_take(result, NULL, 0u);
        return;
    }
    if (connection->virtual_pointer == NULL
        || !desktop_bounds(connection, &left, &top, &width, &height)) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "the compositor does not support absolute pointer "
                         "motion");
        return;
    }
    local_x = (int64_t)x - left;
    local_y = (int64_t)y - top;
    if (local_x < 0)
        local_x = 0;
    else if ((uint64_t)local_x > width)
        local_x = width;
    if (local_y < 0)
        local_y = 0;
    else if ((uint64_t)local_y > height)
        local_y = height;
    zwlr_virtual_pointer_v1_motion_absolute(
        connection->virtual_pointer, timestamp_ms(), (uint32_t)local_x,
        (uint32_t)local_y, width, height);
    zwlr_virtual_pointer_v1_frame(connection->virtual_pointer);
    if (wl_display_flush(connection->display) < 0 && errno != EAGAIN) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "the compositor rejected absolute pointer motion");
        return;
    }
    (void)ksd_result_take(result, NULL, 0u);
}
