#include "wl_capture.h"

#include "protocol.h"
#include "protocol_io.h"
#include "wl_internal.h"
#include "wl_outputs.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/memfd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define KSD_WL_CAPTURE_TIMEOUT_MS 3000
#define KSD_CAPTURE_HEADER_SIZE 20u

typedef struct frame_state {
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t flags;
    bool buffer_info;
    bool buffer_done;
    bool ready;
    bool failed;
} frame_state;

typedef struct captured_segment {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_width;
    uint32_t pixel_height;
    uint32_t source_width;
    uint32_t source_height;
    uint32_t source_x;
    uint32_t source_y;
    uint32_t stride;
    uint32_t format;
    uint32_t transform;
    uint32_t flags;
    bool swap_red_blue;
    bool opaque;
    int descriptor;
} captured_segment;

static void frame_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame,
                         uint32_t format, uint32_t width, uint32_t height,
                         uint32_t stride)
{
    frame_state *state = data;

    (void)frame;
    state->format = format;
    state->width = width;
    state->height = height;
    state->stride = stride;
    state->buffer_info = true;
}

static void frame_flags(void *data, struct zwlr_screencopy_frame_v1 *frame,
                        uint32_t flags)
{
    (void)frame;
    ((frame_state *)data)->flags = flags;
}

static void frame_ready(void *data, struct zwlr_screencopy_frame_v1 *frame,
                        uint32_t seconds_hi, uint32_t seconds_lo,
                        uint32_t nanoseconds)
{
    (void)frame;
    (void)seconds_hi;
    (void)seconds_lo;
    (void)nanoseconds;
    ((frame_state *)data)->ready = true;
}

static void frame_failed(void *data, struct zwlr_screencopy_frame_v1 *frame)
{
    (void)frame;
    ((frame_state *)data)->failed = true;
}

static void frame_damage(void *data, struct zwlr_screencopy_frame_v1 *frame,
                         uint32_t x, uint32_t y, uint32_t width,
                         uint32_t height)
{
    (void)data;
    (void)frame;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static void frame_linux_dmabuf(void *data,
                               struct zwlr_screencopy_frame_v1 *frame,
                               uint32_t format, uint32_t width,
                               uint32_t height)
{
    (void)data;
    (void)frame;
    (void)format;
    (void)width;
    (void)height;
}

static void frame_buffer_done(void *data,
                              struct zwlr_screencopy_frame_v1 *frame)
{
    (void)frame;
    ((frame_state *)data)->buffer_done = true;
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = frame_buffer,
    .flags = frame_flags,
    .ready = frame_ready,
    .failed = frame_failed,
    .damage = frame_damage,
    .linux_dmabuf = frame_linux_dmabuf,
    .buffer_done = frame_buffer_done,
};

typedef struct buffer_wait {
    frame_state *state;
    bool needs_done;
} buffer_wait;

static bool buffer_announced(void *data)
{
    buffer_wait *wait = data;

    return wait->state->failed
        || (wait->state->buffer_info
            && (!wait->needs_done || wait->state->buffer_done));
}

static bool frame_finished(void *data)
{
    frame_state *state = data;

    return state->ready || state->failed;
}

static bool rectangle_valid(int32_t x, int32_t y, uint32_t width,
                            uint32_t height)
{
    return width != 0u && height != 0u
        && width <= KSD_MAX_CAPTURE_DIMENSION
        && height <= KSD_MAX_CAPTURE_DIMENSION
        && (uint64_t)width * height <= KSD_MAX_CAPTURE_PIXELS
        && (int64_t)x + width <= INT32_MAX
        && (int64_t)y + height <= INT32_MAX;
}

static bool segment_shape_valid(const frame_state *state)
{
    uint64_t byte_length;

    if (state->format != WL_SHM_FORMAT_ARGB8888
        && state->format != WL_SHM_FORMAT_XRGB8888)
        return false;
    byte_length = (uint64_t)state->stride * state->height;
    return state->width != 0u && state->height != 0u
        && state->width <= KSD_MAX_CAPTURE_DIMENSION
        && state->height <= KSD_MAX_CAPTURE_DIMENSION
        && state->stride >= (uint64_t)state->width * 4u
        && byte_length != 0u && byte_length <= KSD_MAX_CAPTURE_BYTES
        && byte_length <= INT_MAX;
}

static void capture_error(ksd_wayland *connection,
                          ksd_operation_result *result,
                          const char *diagnostic)
{
    ksd_result_error(result,
        ksd_wayland_connection_failed(connection) ? KSD_STATUS_UNAVAILABLE
                                                  : KSD_STATUS_TIMEOUT,
        0u, diagnostic);
}

static bool capture_output(ksd_wayland *connection, ksd_wl_output *output,
                           int32_t local_x, int32_t local_y,
                           uint32_t width, uint32_t height,
                           captured_segment *segment,
                           ksd_operation_result *result)
{
    struct zwlr_screencopy_frame_v1 *frame = NULL;
    struct wl_shm_pool *pool = NULL;
    struct wl_buffer *buffer = NULL;
    frame_state state = { 0 };
    buffer_wait wait = {
        .state = &state,
        .needs_done = connection->screencopy_version >= 3u,
    };
    int descriptor = -1;
    bool success = false;

    frame = zwlr_screencopy_manager_v1_capture_output_region(
        connection->screencopy_manager, 0, output->output, local_x, local_y,
        (int32_t)width, (int32_t)height);
    if (frame == NULL
        || zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener,
                                                  &state) != 0) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "the compositor refused the capture frame");
        goto done;
    }
    if (!ksd_wayland_dispatch_until(connection, buffer_announced, &wait,
                                    KSD_WL_CAPTURE_TIMEOUT_MS)) {
        capture_error(connection, result,
                      "the compositor did not describe the capture buffer");
        goto done;
    }
    if (state.failed || !segment_shape_valid(&state)) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "the compositor did not offer a supported capture "
                         "buffer");
        goto done;
    }
    descriptor = memfd_create("keysharp-desktop-wayland-frame",
                              MFD_CLOEXEC | MFD_ALLOW_SEALING);
    uint64_t byte_length = (uint64_t)state.stride * state.height;
    if (descriptor < 0 || ftruncate(descriptor, (off_t)byte_length) != 0) {
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "could not allocate the capture buffer");
        goto done;
    }
    pool = wl_shm_create_pool(connection->shm, descriptor,
                              (int32_t)byte_length);
    if (pool != NULL)
        buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)state.width,
                                           (int32_t)state.height,
                                           (int32_t)state.stride,
                                           state.format);
    if (buffer == NULL) {
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "could not create the capture buffer");
        goto done;
    }
    zwlr_screencopy_frame_v1_copy(frame, buffer);
    if (!ksd_wayland_dispatch_until(connection, frame_finished, &state,
                                    KSD_WL_CAPTURE_TIMEOUT_MS)) {
        capture_error(connection, result,
                      "the compositor did not finish the capture");
        goto done;
    }
    if (!state.ready || state.failed) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "the compositor could not capture this output");
        goto done;
    }
    segment->pixel_width = state.width;
    segment->pixel_height = state.height;
    segment->source_width = state.width;
    segment->source_height = state.height;
    segment->source_x = 0u;
    segment->source_y = 0u;
    segment->stride = state.stride;
    segment->format = state.format;
    segment->transform = WL_OUTPUT_TRANSFORM_NORMAL;
    segment->flags = state.flags;
    segment->swap_red_blue = false;
    segment->opaque = state.format == WL_SHM_FORMAT_XRGB8888;
    segment->descriptor = descriptor;
    descriptor = -1;
    success = true;

done:
    if (buffer != NULL)
        wl_buffer_destroy(buffer);
    if (pool != NULL)
        wl_shm_pool_destroy(pool);
    if (frame != NULL)
        zwlr_screencopy_frame_v1_destroy(frame);
    if (descriptor >= 0)
        close(descriptor);
    return success;
}

static uint32_t rounded_ratio(uint64_t numerator, uint64_t denominator)
{
    uint64_t quotient = numerator / denominator;
    uint64_t remainder = numerator % denominator;

    if (remainder > denominator / 2u
        || (remainder * 2u == denominator && (quotient & 1u) != 0u))
        quotient++;
    return quotient > UINT32_MAX ? UINT32_MAX : (uint32_t)quotient;
}

static uint32_t mapped_edge(uint32_t offset, uint32_t screen_length,
                            uint32_t pixel_length)
{
    int64_t numerator;
    int64_t denominator;

    if (offset == 0u || screen_length == 0u || pixel_length == 0u)
        return 0u;
    numerator = (int64_t)2 * offset * pixel_length - screen_length;
    denominator = (int64_t)2 * screen_length;
    if (numerator <= 0)
        return 0u;
    return (uint32_t)((numerator + denominator - 1) / denominator);
}

static void inverse_transform(uint32_t x, uint32_t y, uint32_t width,
                              uint32_t height, uint32_t transform,
                              uint32_t *source_x, uint32_t *source_y)
{
    switch (transform) {
        case WL_OUTPUT_TRANSFORM_90:
            *source_x = y;
            *source_y = height - 1u - x;
            break;
        case WL_OUTPUT_TRANSFORM_180:
            *source_x = width - 1u - x;
            *source_y = height - 1u - y;
            break;
        case WL_OUTPUT_TRANSFORM_270:
            *source_x = width - 1u - y;
            *source_y = x;
            break;
        case WL_OUTPUT_TRANSFORM_FLIPPED:
            *source_x = width - 1u - x;
            *source_y = y;
            break;
        case WL_OUTPUT_TRANSFORM_FLIPPED_90:
            *source_x = y;
            *source_y = x;
            break;
        case WL_OUTPUT_TRANSFORM_FLIPPED_180:
            *source_x = x;
            *source_y = height - 1u - y;
            break;
        case WL_OUTPUT_TRANSFORM_FLIPPED_270:
            *source_x = width - 1u - y;
            *source_y = height - 1u - x;
            break;
        default:
            *source_x = x;
            *source_y = y;
            break;
    }
}

typedef struct image_copy_state {
    uint32_t width;
    uint32_t height;
    uint32_t transform;
    uint32_t failure_reason;
    bool argb;
    bool xrgb;
    bool abgr;
    bool xbgr;
    bool constraints_done;
    bool stopped;
    bool ready;
    bool failed;
} image_copy_state;

static void image_buffer_size(
    void *data, struct ext_image_copy_capture_session_v1 *session,
    uint32_t width, uint32_t height)
{
    image_copy_state *state = data;
    (void)session;
    state->width = width;
    state->height = height;
}

static void image_shm_format(
    void *data, struct ext_image_copy_capture_session_v1 *session,
    uint32_t format)
{
    image_copy_state *state = data;
    (void)session;
    if (format == WL_SHM_FORMAT_ARGB8888)
        state->argb = true;
    else if (format == WL_SHM_FORMAT_XRGB8888)
        state->xrgb = true;
    else if (format == WL_SHM_FORMAT_ABGR8888)
        state->abgr = true;
    else if (format == WL_SHM_FORMAT_XBGR8888)
        state->xbgr = true;
}

static void image_dmabuf_device(
    void *data, struct ext_image_copy_capture_session_v1 *session,
    struct wl_array *device)
{
    (void)data;
    (void)session;
    (void)device;
}

static void image_dmabuf_format(
    void *data, struct ext_image_copy_capture_session_v1 *session,
    uint32_t format, struct wl_array *modifiers)
{
    (void)data;
    (void)session;
    (void)format;
    (void)modifiers;
}

static void image_constraints_done(
    void *data, struct ext_image_copy_capture_session_v1 *session)
{
    (void)session;
    ((image_copy_state *)data)->constraints_done = true;
}

static void image_session_stopped(
    void *data, struct ext_image_copy_capture_session_v1 *session)
{
    (void)session;
    ((image_copy_state *)data)->stopped = true;
}

static const struct ext_image_copy_capture_session_v1_listener
image_session_listener = {
    .buffer_size = image_buffer_size,
    .shm_format = image_shm_format,
    .dmabuf_device = image_dmabuf_device,
    .dmabuf_format = image_dmabuf_format,
    .done = image_constraints_done,
    .stopped = image_session_stopped,
};

static void image_transform(
    void *data, struct ext_image_copy_capture_frame_v1 *frame,
    uint32_t transform)
{
    (void)frame;
    ((image_copy_state *)data)->transform = transform;
}

static void image_damage(
    void *data, struct ext_image_copy_capture_frame_v1 *frame,
    int32_t x, int32_t y, int32_t width, int32_t height)
{
    (void)data;
    (void)frame;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static void image_presentation(
    void *data, struct ext_image_copy_capture_frame_v1 *frame,
    uint32_t seconds_hi, uint32_t seconds_lo, uint32_t nanoseconds)
{
    (void)data;
    (void)frame;
    (void)seconds_hi;
    (void)seconds_lo;
    (void)nanoseconds;
}

static void image_ready(void *data,
                        struct ext_image_copy_capture_frame_v1 *frame)
{
    (void)frame;
    ((image_copy_state *)data)->ready = true;
}

static void image_failed(void *data,
                         struct ext_image_copy_capture_frame_v1 *frame,
                         uint32_t reason)
{
    image_copy_state *state = data;
    (void)frame;
    state->failure_reason = reason;
    state->failed = true;
}

static const struct ext_image_copy_capture_frame_v1_listener
image_frame_listener = {
    .transform = image_transform,
    .damage = image_damage,
    .presentation_time = image_presentation,
    .ready = image_ready,
    .failed = image_failed,
};

static bool image_constraints_complete(void *data)
{
    image_copy_state *state = data;
    return state->constraints_done || state->stopped;
}

static bool image_frame_complete(void *data)
{
    image_copy_state *state = data;
    return state->ready || state->failed || state->stopped;
}

static bool image_format(const image_copy_state *state, uint32_t *format,
                         bool *swap_red_blue, bool *opaque)
{
    if (state->argb) {
        *format = WL_SHM_FORMAT_ARGB8888;
        *swap_red_blue = false;
        *opaque = false;
    } else if (state->xrgb) {
        *format = WL_SHM_FORMAT_XRGB8888;
        *swap_red_blue = false;
        *opaque = true;
    } else if (state->abgr) {
        *format = WL_SHM_FORMAT_ABGR8888;
        *swap_red_blue = true;
        *opaque = false;
    } else if (state->xbgr) {
        *format = WL_SHM_FORMAT_XBGR8888;
        *swap_red_blue = true;
        *opaque = true;
    } else {
        return false;
    }
    return true;
}

static bool capture_output_image_copy(
    ksd_wayland *connection, ksd_wl_output *output,
    int32_t local_x, int32_t local_y, uint32_t logical_width,
    uint32_t logical_height, uint32_t output_width, uint32_t output_height,
    captured_segment *segment, ksd_operation_result *result)
{
    struct ext_image_capture_source_v1 *source = NULL;
    struct ext_image_copy_capture_session_v1 *session = NULL;
    struct ext_image_copy_capture_frame_v1 *frame = NULL;
    struct wl_shm_pool *pool = NULL;
    struct wl_buffer *buffer = NULL;
    image_copy_state state = { 0 };
    uint32_t format = 0u;
    uint32_t oriented_width;
    uint32_t oriented_height;
    uint32_t crop_right;
    uint32_t crop_bottom;
    uint32_t stride;
    uint64_t byte_length;
    bool swap_red_blue = false;
    bool opaque = false;
    bool success = false;
    int descriptor = -1;

    source = ext_output_image_capture_source_manager_v1_create_source(
        connection->output_source_manager, output->output);
    if (source != NULL)
        session = ext_image_copy_capture_manager_v1_create_session(
            connection->image_copy_manager, source, 0u);
    if (session == NULL
        || ext_image_copy_capture_session_v1_add_listener(
            session, &image_session_listener, &state) != 0) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "the compositor refused the image-copy session");
        goto done;
    }
    if (!ksd_wayland_dispatch_until(connection, image_constraints_complete,
                                    &state, KSD_WL_CAPTURE_TIMEOUT_MS)) {
        capture_error(connection, result,
                      "the compositor did not describe the image-copy buffer");
        goto done;
    }
    if (state.stopped) {
        ksd_result_error(result, KSD_STATUS_CANCELLED, 0u,
                         "the compositor stopped the capture session");
        goto done;
    }
    if (!image_format(&state, &format, &swap_red_blue, &opaque)
        || state.width == 0u || state.height == 0u
        || state.width > KSD_MAX_CAPTURE_DIMENSION
        || state.height > KSD_MAX_CAPTURE_DIMENSION) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "the compositor offered no supported image-copy buffer");
        goto done;
    }
    stride = state.width * 4u;
    byte_length = (uint64_t)stride * state.height;
    if (byte_length == 0u || byte_length > KSD_MAX_CAPTURE_BYTES
        || byte_length > INT_MAX) {
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "the image-copy buffer exceeds the service limit");
        goto done;
    }
    descriptor = memfd_create("keysharp-desktop-image-copy-frame",
                              MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (descriptor < 0 || ftruncate(descriptor, (off_t)byte_length) != 0) {
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "could not allocate the image-copy buffer");
        goto done;
    }
    pool = wl_shm_create_pool(connection->shm, descriptor,
                              (int32_t)byte_length);
    if (pool != NULL)
        buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)state.width,
                                           (int32_t)state.height,
                                           (int32_t)stride, format);
    if (buffer == NULL) {
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "could not create the image-copy buffer");
        goto done;
    }
    frame = ext_image_copy_capture_session_v1_create_frame(session);
    if (frame == NULL
        || ext_image_copy_capture_frame_v1_add_listener(
            frame, &image_frame_listener, &state) != 0) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "the compositor refused the image-copy frame");
        goto done;
    }
    ext_image_copy_capture_frame_v1_attach_buffer(frame, buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(
        frame, 0, 0, (int32_t)state.width, (int32_t)state.height);
    ext_image_copy_capture_frame_v1_capture(frame);
    if (!ksd_wayland_dispatch_until(connection, image_frame_complete,
                                    &state, KSD_WL_CAPTURE_TIMEOUT_MS)) {
        capture_error(connection, result,
                      "the compositor did not finish the image-copy frame");
        goto done;
    }
    if (state.stopped || (state.failed
        && state.failure_reason
            == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED)) {
        ksd_result_error(result, KSD_STATUS_CANCELLED, 0u,
                         "the compositor stopped the capture session");
        goto done;
    }
    if (!state.ready || state.failed || state.transform > 7u) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "the compositor could not capture this output");
        goto done;
    }
    if (state.transform == WL_OUTPUT_TRANSFORM_90
        || state.transform == WL_OUTPUT_TRANSFORM_270
        || state.transform == WL_OUTPUT_TRANSFORM_FLIPPED_90
        || state.transform == WL_OUTPUT_TRANSFORM_FLIPPED_270) {
        oriented_width = state.height;
        oriented_height = state.width;
    } else {
        oriented_width = state.width;
        oriented_height = state.height;
    }
    segment->source_x = mapped_edge((uint32_t)local_x, output_width,
                                    oriented_width);
    segment->source_y = mapped_edge((uint32_t)local_y, output_height,
                                    oriented_height);
    crop_right = mapped_edge((uint32_t)local_x + logical_width,
                             output_width, oriented_width);
    crop_bottom = mapped_edge((uint32_t)local_y + logical_height,
                              output_height, oriented_height);
    if (crop_right <= segment->source_x
        || crop_bottom <= segment->source_y
        || crop_right > oriented_width || crop_bottom > oriented_height) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "the image-copy frame does not cover the requested area");
        goto done;
    }
    segment->pixel_width = crop_right - segment->source_x;
    segment->pixel_height = crop_bottom - segment->source_y;
    segment->source_width = state.width;
    segment->source_height = state.height;
    segment->stride = stride;
    segment->format = format;
    segment->transform = state.transform;
    segment->flags = 0u;
    segment->swap_red_blue = swap_red_blue;
    segment->opaque = opaque;
    segment->descriptor = descriptor;
    descriptor = -1;
    success = true;

done:
    if (frame != NULL)
        ext_image_copy_capture_frame_v1_destroy(frame);
    if (session != NULL)
        ext_image_copy_capture_session_v1_destroy(session);
    if (source != NULL)
        ext_image_capture_source_v1_destroy(source);
    if (buffer != NULL)
        wl_buffer_destroy(buffer);
    if (pool != NULL)
        wl_shm_pool_destroy(pool);
    if (descriptor >= 0)
        close(descriptor);
    return success;
}

static bool compose_segment(uint8_t *target, uint32_t target_width,
                            uint32_t target_height,
                            int32_t request_x, int32_t request_y,
                            uint32_t request_width, uint32_t request_height,
                            const captured_segment *segment)
{
    uint32_t left = mapped_edge((uint32_t)(segment->x - request_x),
                                request_width, target_width);
    uint32_t top = mapped_edge((uint32_t)(segment->y - request_y),
                               request_height, target_height);
    uint32_t right = mapped_edge(
        (uint32_t)((int64_t)segment->x + segment->width - request_x),
        request_width, target_width);
    uint32_t bottom = mapped_edge(
        (uint32_t)((int64_t)segment->y + segment->height - request_y),
        request_height, target_height);
    uint32_t destination_width;
    uint32_t destination_height;
    uint8_t *source;
    size_t source_length;

    if (left > target_width)
        left = target_width;
    if (right > target_width)
        right = target_width;
    if (top > target_height)
        top = target_height;
    if (bottom > target_height)
        bottom = target_height;
    if (right <= left || bottom <= top)
        return true;
    destination_width = right - left;
    destination_height = bottom - top;
    source_length = (size_t)segment->stride * segment->source_height;
    source = mmap(NULL, source_length, PROT_READ, MAP_SHARED,
                  segment->descriptor, 0);
    if (source == MAP_FAILED)
        return false;
    for (uint32_t row = 0u; row < destination_height; row++) {
        uint8_t *destination_row = target + KSD_CAPTURE_HEADER_SIZE
            + (size_t)(top + row) * target_width * 4u
            + (size_t)left * 4u;
        uint32_t oriented_y = segment->source_y
            + (uint32_t)(((uint64_t)row * 2u + 1u)
                * segment->pixel_height
                / ((uint64_t)destination_height * 2u));
        for (uint32_t column = 0u; column < destination_width; column++) {
            uint32_t oriented_x = segment->source_x
                + (uint32_t)(((uint64_t)column * 2u + 1u)
                    * segment->pixel_width
                    / ((uint64_t)destination_width * 2u));
            uint32_t source_x;
            uint32_t source_y;
            uint8_t *input;
            uint8_t *output = destination_row + (size_t)column * 4u;

            inverse_transform(oriented_x, oriented_y,
                              segment->source_width,
                              segment->source_height, segment->transform,
                              &source_x, &source_y);
            if ((segment->flags
                 & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) != 0u)
                source_y = segment->source_height - 1u - source_y;
            input = source + (size_t)source_y * segment->stride
                + (size_t)source_x * 4u;
            if (segment->swap_red_blue) {
                output[0] = input[2];
                output[1] = input[1];
                output[2] = input[0];
                output[3] = input[3];
            } else {
                memcpy(output, input, 4u);
            }
            if (segment->opaque)
                output[3] = 0xffu;
        }
    }
    munmap(source, source_length);
    return true;
}

static bool compose_capture(int32_t x, int32_t y, uint32_t width,
                            uint32_t height, captured_segment *segments,
                            size_t count, ksd_operation_result *result)
{
    uint32_t scale_x_numerator = 1u;
    uint32_t scale_x_denominator = 1u;
    uint32_t scale_y_numerator = 1u;
    uint32_t scale_y_denominator = 1u;
    uint32_t pixel_width;
    uint32_t pixel_height;
    uint32_t stride;
    uint64_t pixel_length;
    uint64_t total;
    uint8_t header[KSD_CAPTURE_HEADER_SIZE] = { 0 };
    uint8_t *mapping;
    int descriptor = -1;
    int seals = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;

    for (size_t index = 0u; index < count; index++) {
        captured_segment *segment = segments + index;
        if ((uint64_t)segment->pixel_width * scale_x_denominator
            > (uint64_t)scale_x_numerator * segment->width) {
            scale_x_numerator = segment->pixel_width;
            scale_x_denominator = segment->width;
        }
        if ((uint64_t)segment->pixel_height * scale_y_denominator
            > (uint64_t)scale_y_numerator * segment->height) {
            scale_y_numerator = segment->pixel_height;
            scale_y_denominator = segment->height;
        }
    }
    pixel_width = rounded_ratio((uint64_t)width * scale_x_numerator,
                                scale_x_denominator);
    pixel_height = rounded_ratio((uint64_t)height * scale_y_numerator,
                                 scale_y_denominator);
    pixel_length = (uint64_t)pixel_width * pixel_height * 4u;
    total = KSD_CAPTURE_HEADER_SIZE + pixel_length;
    if (pixel_width == 0u || pixel_height == 0u
        || pixel_width > KSD_MAX_CAPTURE_DIMENSION
        || pixel_height > KSD_MAX_CAPTURE_DIMENSION
        || (uint64_t)pixel_width * pixel_height > KSD_MAX_CAPTURE_PIXELS
        || pixel_length > KSD_MAX_CAPTURE_BYTES || total > UINT32_MAX) {
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "the scaled capture exceeds the service limit");
        return false;
    }
    stride = pixel_width * 4u;
    descriptor = memfd_create("keysharp-desktop-capture",
                              MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (descriptor < 0 || ftruncate(descriptor, (off_t)total) != 0) {
        if (descriptor >= 0)
            close(descriptor);
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "could not allocate the composed capture");
        return false;
    }
    mapping = mmap(NULL, (size_t)total, PROT_READ | PROT_WRITE, MAP_SHARED,
                   descriptor, 0);
    if (mapping == MAP_FAILED) {
        close(descriptor);
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "could not map the composed capture");
        return false;
    }
    for (size_t index = 0u; index < count; index++)
        if (!compose_segment(mapping, pixel_width, pixel_height, x, y,
                             width, height, segments + index)) {
            munmap(mapping, (size_t)total);
            close(descriptor);
            ksd_result_error(result, KSD_STATUS_INTERNAL, 0u,
                             "could not compose the captured outputs");
            return false;
        }
    ksd_encode_u16(header, KSD_CAPTURE_FORMAT_BGRA8_PREMULTIPLIED);
    ksd_encode_u32(header + 4u, pixel_width);
    ksd_encode_u32(header + 8u, pixel_height);
    ksd_encode_u32(header + 12u, stride);
    ksd_encode_u32(header + 16u, (uint32_t)pixel_length);
    memcpy(mapping, header, sizeof(header));
    if (munmap(mapping, (size_t)total) != 0
        || fcntl(descriptor, F_ADD_SEALS, seals) != 0) {
        close(descriptor);
        ksd_result_error(result, KSD_STATUS_INTERNAL, 0u,
                         "could not seal the composed capture");
        return false;
    }
    if (!ksd_result_take_fd(result, descriptor, (uint32_t)total)) {
        ksd_result_error(result, KSD_STATUS_INTERNAL, 0u,
                         "could not return the composed capture");
        return false;
    }
    return true;
}

void ksd_wayland_capture_area(ksd_wayland *connection, int32_t x, int32_t y,
                              uint32_t width, uint32_t height,
                              ksd_operation_result *result)
{
    captured_segment *segments = NULL;
    size_t capacity = 0u;
    size_t count = 0u;

    if (connection == NULL || result == NULL
        || !rectangle_valid(x, y, width, height)) {
        ksd_result_error(result, KSD_STATUS_INVALID_REQUEST, 0u,
                         "invalid Wayland capture request");
        return;
    }
    if (connection->shm == NULL
        || (connection->screencopy_manager == NULL
            && (connection->output_source_manager == NULL
                || connection->image_copy_manager == NULL))) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "the compositor does not support screen capture");
        return;
    }
    for (ksd_wl_output *output = connection->outputs; output != NULL;
         output = output->next)
        capacity++;
    segments = calloc(capacity, sizeof(*segments));
    if (segments == NULL) {
        ksd_result_error(result, KSD_STATUS_RESOURCE_EXHAUSTED, 0u,
                         "could not allocate the output list");
        return;
    }
    for (ksd_wl_output *output = connection->outputs; output != NULL;
         output = output->next) {
        int32_t output_x;
        int32_t output_y;
        int32_t output_width;
        int32_t output_height;
        int64_t left;
        int64_t top;
        int64_t right;
        int64_t bottom;
        captured_segment *segment;

        if (!ksd_wayland_output_bounds(output, &output_x, &output_y,
                                       &output_width, &output_height))
            continue;
        left = x > output_x ? x : output_x;
        top = y > output_y ? y : output_y;
        right = (int64_t)x + width < (int64_t)output_x + output_width
            ? (int64_t)x + width : (int64_t)output_x + output_width;
        bottom = (int64_t)y + height < (int64_t)output_y + output_height
            ? (int64_t)y + height : (int64_t)output_y + output_height;
        if (right <= left || bottom <= top)
            continue;
        segment = segments + count;
        segment->x = (int32_t)left;
        segment->y = (int32_t)top;
        segment->width = (uint32_t)(right - left);
        segment->height = (uint32_t)(bottom - top);
        segment->descriptor = -1;
        bool captured = connection->screencopy_manager != NULL
            ? capture_output(connection, output,
                (int32_t)(left - output_x),
                (int32_t)(top - output_y), segment->width,
                segment->height, segment, result)
            : capture_output_image_copy(connection, output,
                (int32_t)(left - output_x),
                (int32_t)(top - output_y), segment->width,
                segment->height, (uint32_t)output_width,
                (uint32_t)output_height, segment, result);
        if (!captured)
            goto done;
        count++;
    }
    if (count == 0u) {
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "the capture does not intersect an output");
        goto done;
    }
    (void)compose_capture(x, y, width, height, segments, count, result);

done:
    for (size_t index = 0u; index < count; index++)
        if (segments[index].descriptor >= 0)
            close(segments[index].descriptor);
    free(segments);
}
