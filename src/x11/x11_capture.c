#include "x11_capture.h"

#include "protocol.h"
#include "x11_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xcb/shm.h>

/* The pixels land at this offset inside the descriptor, behind the same
 * 20-byte header ksd_capture_tail_valid parses, so a consumer reads one shape
 * whether the bytes arrived from a provider or from the X server. */
#define KSD_X11_CAPTURE_HEADER 20u

/* A drawable the server can render, kept separate from the request so the two
 * verbs share one body. */
typedef struct capture_target {
    xcb_drawable_t drawable;
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;
} capture_target;

/* The server describes its own memory layout per depth, and the answer is not
 * width * 4: a scanline is padded to scanline_pad bits. Computing the stride
 * instead of reading it is the defect the odd test width exists to catch. */
static const xcb_format_t *format_for_depth(xcb_connection_t *connection,
                                            uint8_t depth)
{
    const xcb_setup_t *setup = xcb_get_setup(connection);
    const xcb_format_t *format = xcb_setup_pixmap_formats(setup);
    int count = xcb_setup_pixmap_formats_length(setup);

    for (int index = 0; index < count; index++) {
        if (format[index].depth == depth)
            return format + index;
    }
    return NULL;
}

/* This backend emits one pixel format and refuses every other rather than
 * reinterpreting bytes it did not verify. A server storing pixels any other
 * way would need a conversion pass, and passing the bytes through unconverted
 * would hand the consumer wrong colours under a correct-looking header. */
static bool server_is_bgra8(xcb_connection_t *connection,
                            const xcb_screen_t *screen, uint8_t depth,
                            const xcb_format_t *format)
{
    const xcb_setup_t *setup = xcb_get_setup(connection);
    xcb_depth_iterator_t depths = xcb_screen_allowed_depths_iterator(screen);

    if (format == NULL || format->bits_per_pixel != 32u
        || (depth != 24u && depth != 32u)
        || setup->image_byte_order != XCB_IMAGE_ORDER_LSB_FIRST)
        return false;
    for (; depths.rem; xcb_depth_next(&depths)) {
        xcb_visualtype_t *visual = xcb_depth_visuals(depths.data);
        int count = xcb_depth_visuals_length(depths.data);

        for (int index = 0; index < count; index++) {
            if (visual[index].visual_id != screen->root_visual)
                continue;
            /* Byte order is LSB first, so a 0x00FF0000 red mask puts red in
             * the third byte: B, G, R, A in memory. */
            return visual[index].red_mask == 0x00ff0000u
                && visual[index].green_mask == 0x0000ff00u
                && visual[index].blue_mask == 0x000000ffu;
        }
    }
    return false;
}

/* Rounds one scanline of pixels up to the server pad, in bytes. */
static uint64_t scanline_bytes(uint64_t width, const xcb_format_t *format)
{
    uint64_t bits = width * format->bits_per_pixel;
    uint64_t pad = format->scanline_pad;

    return ((bits + pad - 1u) / pad) * pad / 8u;
}

/* The alpha byte is undefined on a depth-24 visual: the server never wrote it,
 * so it holds whatever the last tenant of that memory left behind. The
 * declared format is premultiplied, and at full opacity premultiplication
 * leaves the colour bytes alone, so forcing the byte to 255 is the whole
 * conversion. Skipped at depth 32, where the alpha the server wrote is real. */
static void force_opaque(uint8_t *pixels, uint64_t stride, uint32_t height,
                         uint32_t width)
{
    for (uint32_t row = 0u; row < height; row++) {
        uint8_t *line = pixels + (uint64_t)row * stride;

        for (uint32_t column = 0u; column < width; column++)
            line[(size_t)column * 4u + 3u] = 0xffu;
    }
}

/* Asks the server to write the image into the descriptor itself. Returns false
 * when the extension is absent or refuses, which is not an error: the caller
 * falls back to the core request, whose only cost is that the bytes come back
 * through the connection buffer. */
#ifdef KSD_X11_TESTING
/* Set by the tests to force the fallback. A server that supports MIT-SHM would
 * otherwise never exercise core_into_map, and the path that runs against every
 * remote display would ship untested. */
bool ksd_x11_capture_disable_shm = false;
/* Counts captures the shared-memory path actually completed. A test that runs
 * both transports proves nothing unless this shows the first one really took
 * the transport it was meant to take. */
unsigned ksd_x11_capture_shm_count = 0u;
#endif

static bool shm_into_fd(xcb_connection_t *connection,
                        const capture_target *target, int payload_fd)
{
    const xcb_query_extension_reply_t *extension;

#ifdef KSD_X11_TESTING
    if (ksd_x11_capture_disable_shm)
        return false;
#endif
    extension = xcb_get_extension_data(connection, &xcb_shm_id);
    xcb_shm_query_version_reply_t *version;
    xcb_shm_seg_t segment;
    xcb_shm_get_image_reply_t *image;
    xcb_generic_error_t *error = NULL;
    int duplicate;

    if (extension == NULL || !extension->present)
        return false;
    /* Only the descriptor-passing form is used. The older shmget form would
     * put the pixels in a System V segment any process on the host can attach
     * to, which is the exposure the sealed descriptor exists to avoid. */
    version = xcb_shm_query_version_reply(connection,
        xcb_shm_query_version(connection), NULL);
    if (version == NULL)
        return false;
    free(version);

    /* attach_fd takes ownership of the descriptor it is handed, and the caller
     * still needs its own. */
    duplicate = fcntl(payload_fd, F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0)
        return false;
    segment = xcb_generate_id(connection);
    xcb_shm_attach_fd(connection, segment, duplicate, 0);
    image = xcb_shm_get_image_reply(connection,
        xcb_shm_get_image(connection, target->drawable, target->x, target->y,
                          target->width, target->height, ~0u,
                          XCB_IMAGE_FORMAT_Z_PIXMAP, segment,
                          KSD_X11_CAPTURE_HEADER), &error);
    xcb_shm_detach(connection, segment);
    /* The seal below is refused while any writable mapping of the descriptor
     * exists, and the server holds one until it processes that detach. This
     * round trip is what makes the detach have happened. */
    free(xcb_get_input_focus_reply(connection,
        xcb_get_input_focus(connection), NULL));
    free(error);
    if (image == NULL)
        return false;
    free(image);
#ifdef KSD_X11_TESTING
    ksd_x11_capture_shm_count++;
#endif
    return true;
}

/* The fallback: the reply carries the pixels, so they are copied once into the
 * mapping. Correct everywhere, including against a server reached over TCP
 * where no descriptor can be shared at all. */
static bool core_into_map(xcb_connection_t *connection,
                          const capture_target *target, uint8_t *pixels,
                          uint64_t length)
{
    xcb_get_image_reply_t *image = xcb_get_image_reply(connection,
        xcb_get_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, target->drawable,
                      target->x, target->y, target->width, target->height,
                      ~0u), NULL);
    int data_length;

    if (image == NULL)
        return false;
    data_length = xcb_get_image_data_length(image);
    if (data_length < 0 || (uint64_t)data_length != length) {
        free(image);
        return false;
    }
    memcpy(pixels, xcb_get_image_data(image), (size_t)length);
    free(image);
    return true;
}

static void capture_drawable(ksd_x11 *connection, const capture_target *target,
                             uint8_t depth, ksd_operation_result *result)
{
    xcb_connection_t *c = connection->connection;
    const xcb_format_t *format = format_for_depth(c, depth);
    uint64_t stride;
    uint64_t length;
    uint64_t total;
    int payload_fd;
    uint8_t *map;
    int seals = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;

    if (!server_is_bgra8(c, connection->screen, depth, format)) {
        ksd_result_error(result, KSD_STATUS_UNSUPPORTED, 0u,
                         "this display stores pixels in a format this service "
                         "does not convert");
        return;
    }
    stride = scanline_bytes(target->width, format);
    length = stride * target->height;
    /* Checked before anything is allocated, and against the ceilings
     * ksd_capture_tail_valid enforces, so a request that could never produce a
     * valid answer is refused rather than served and then rejected. */
    if (length == 0u || length > KSD_MAX_CAPTURE_BYTES
        || stride > UINT32_MAX
        || (uint64_t)target->width * target->height > KSD_MAX_CAPTURE_PIXELS) {
        ksd_result_error(result, KSD_STATUS_INVALID_REQUEST, 0u,
                         "the requested area is larger than this service will "
                         "capture");
        return;
    }
    total = KSD_X11_CAPTURE_HEADER + length;

    payload_fd = memfd_create("keysharp-desktop-capture",
                              MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (payload_fd < 0) {
        ksd_result_error(result, KSD_STATUS_INTERNAL, 0u,
                         "could not allocate the capture buffer");
        return;
    }
    if (ftruncate(payload_fd, (off_t)total) != 0) {
        close(payload_fd);
        ksd_result_error(result, KSD_STATUS_INTERNAL, 0u,
                         "could not size the capture buffer");
        return;
    }
    map = mmap(NULL, (size_t)total, PROT_READ | PROT_WRITE, MAP_SHARED,
               payload_fd, 0);
    if (map == MAP_FAILED) {
        close(payload_fd);
        ksd_result_error(result, KSD_STATUS_INTERNAL, 0u,
                         "could not map the capture buffer");
        return;
    }
    if (!shm_into_fd(c, target, payload_fd)
        && !core_into_map(c, target, map + KSD_X11_CAPTURE_HEADER, length)) {
        munmap(map, (size_t)total);
        close(payload_fd);
        /* A drawable that went away between the geometry read and the image
         * request lands here, which is the ordinary race and not a fault of
         * this service. */
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "the display would not produce an image for this "
                         "request");
        return;
    }
    if (depth == 24u)
        force_opaque(map + KSD_X11_CAPTURE_HEADER, stride, target->height,
                     target->width);
    ksd_encode_u16(map, KSD_CAPTURE_FORMAT_BGRA8_PREMULTIPLIED);
    ksd_encode_u16(map + 2u, 0u);
    ksd_encode_u32(map + 4u, target->width);
    ksd_encode_u32(map + 8u, target->height);
    ksd_encode_u32(map + 12u, (uint32_t)stride);
    ksd_encode_u32(map + 16u, (uint32_t)length);
    /* Unmapped before sealing, not after: F_SEAL_WRITE is refused while a
     * writable mapping of the file exists anywhere on the host. */
    if (munmap(map, (size_t)total) != 0
        || fcntl(payload_fd, F_ADD_SEALS, seals) != 0
        || !ksd_result_take_fd(result, payload_fd, (uint32_t)total)) {
        close(payload_fd);
        ksd_result_error(result, KSD_STATUS_INTERNAL, 0u,
                         "could not seal the capture buffer");
    }
}

void ksd_x11_capture_area(ksd_x11 *connection, int32_t x, int32_t y,
                          uint32_t width, uint32_t height,
                          ksd_operation_result *result)
{
    capture_target target;

    /* The root window is the composited output on X11, so an area capture is
     * correct by construction: what is on screen in that rectangle is what the
     * server returns, occlusion included. */
    if (width == 0u || height == 0u || width > INT16_MAX
        || height > INT16_MAX || x < INT16_MIN || x > INT16_MAX
        || y < INT16_MIN || y > INT16_MAX) {
        ksd_result_error(result, KSD_STATUS_INVALID_REQUEST, 0u,
                         "the requested area is not one this display can "
                         "address");
        return;
    }
    target.drawable = connection->screen->root;
    target.x = (int16_t)x;
    target.y = (int16_t)y;
    target.width = (uint16_t)width;
    target.height = (uint16_t)height;
    capture_drawable(connection, &target, connection->screen->root_depth,
                     result);
}

void ksd_x11_capture_window(ksd_x11 *connection, uint32_t window,
                            bool include_decoration,
                            ksd_operation_result *result)
{
    xcb_connection_t *c = connection->connection;
    xcb_drawable_t drawable = window;
    xcb_get_window_attributes_reply_t *attributes;
    xcb_get_geometry_reply_t *geometry;
    capture_target target;

    if (include_decoration) {
        /* A reparenting window manager makes the frame an ancestor of the
         * client window, so the decorated image is the parent one. Falling
         * back to the client window when the parent is the root is deliberate:
         * there is no frame in that case, and capturing the root would hand
         * back the whole desktop for a window request. */
        xcb_query_tree_reply_t *tree = xcb_query_tree_reply(c,
            xcb_query_tree(c, window), NULL);

        if (tree != NULL) {
            if (tree->parent != XCB_WINDOW_NONE
                && tree->parent != connection->screen->root)
                drawable = tree->parent;
            free(tree);
        }
    }
    attributes = xcb_get_window_attributes_reply(c,
        xcb_get_window_attributes(c, drawable), NULL);
    geometry = xcb_get_geometry_reply(c, xcb_get_geometry(c, drawable), NULL);
    /* An unmapped window has no pixels on the server at all, so a capture of
     * one would be undefined content rather than the window. Saying so is the
     * honest answer; a black rectangle would not be. */
    if (attributes == NULL || geometry == NULL
        || attributes->map_state != XCB_MAP_STATE_VIEWABLE) {
        free(attributes);
        free(geometry);
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "that window is not on screen");
        return;
    }
    target.drawable = drawable;
    target.x = 0;
    target.y = 0;
    target.width = geometry->width;
    target.height = geometry->height;
    if (target.width == 0u || target.height == 0u) {
        free(attributes);
        free(geometry);
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "that window has no area to capture");
        return;
    }
    capture_drawable(connection, &target, geometry->depth, result);
    free(attributes);
    free(geometry);
}
