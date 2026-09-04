#include "operation_result.h"
#include "protocol.h"
#include "protocol_io.h"
#include "x11_capture.h"
#include "x11_connect.h"
#include "x11_display.h"
#include "x11_query.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xcb/xcb.h>

/* The screen the wrapper starts. The width is odd on purpose: a scanline pad
 * of zero hides a stride computed as width * 4 rather than read from the
 * server, and that matters once capture lands. */
#define TEST_WIDTH 1279u
#define TEST_HEIGHT 1024u

static uint32_t tail_u32(const ksd_operation_result *result, size_t index)
{
    assert(result->tail != NULL);
    assert(result->tail_length >= (index + 1u) * 4u);
    return ksd_decode_u32(result->tail + index * 4u);
}

/* A JSON reply is framed as a length and then the bytes, which is what the
 * compositor providers emit for the same opcodes. */
static const char *json_body(const ksd_operation_result *result,
                             uint32_t *length)
{
    assert(result->tail != NULL && result->tail_length >= 4u);
    *length = ksd_decode_u32(result->tail);
    assert(*length == result->tail_length - 4u);
    return (const char *)result->tail + 4u;
}

static void check_cursor_position(ksd_x11 *connection)
{
    ksd_operation_result result;

    ksd_result_init(&result);
    ksd_x11_cursor_position(connection, &result);
    assert(result.status == KSD_STATUS_OK);
    assert(result.tail_length == 8u);
    /* The pointer is somewhere on the screen, which is all a headless server
     * guarantees. The point is that the reply is well formed and in range. */
    assert((int32_t)tail_u32(&result, 0u) >= 0);
    assert((int32_t)tail_u32(&result, 0u) < (int32_t)TEST_WIDTH);
    assert((int32_t)tail_u32(&result, 1u) >= 0);
    assert((int32_t)tail_u32(&result, 1u) < (int32_t)TEST_HEIGHT);
    ksd_result_clear(&result);
}

static void check_work_area(ksd_x11 *connection)
{
    ksd_operation_result result;

    ksd_result_init(&result);
    ksd_x11_work_area(connection, &result);
    assert(result.status == KSD_STATUS_OK);
    assert(result.tail_length == 16u);
    /* No window manager runs under the bare server, so nothing publishes
     * _NET_WORKAREA and the screen itself is the answer. That is true rather
     * than a failure, and it is the fallback this asserts. */
    assert(tail_u32(&result, 0u) == 0u);
    assert(tail_u32(&result, 1u) == 0u);
    assert(tail_u32(&result, 2u) == TEST_WIDTH);
    assert(tail_u32(&result, 3u) == TEST_HEIGHT);
    ksd_result_clear(&result);
}

static void check_window_list(ksd_x11 *connection)
{
    ksd_operation_result result;

    ksd_result_init(&result);
    ksd_x11_window_list(connection, false, &result);
    /* With no window manager there is no _NET_CLIENT_LIST, and the honest
     * answer is that the operation cannot be served here -- not an empty list,
     * which would claim the session has no windows. */
    assert(result.status == KSD_STATUS_UNAVAILABLE);
    assert(result.tail == NULL);
    ksd_result_clear(&result);
}

static void check_window_active(ksd_x11 *connection)
{
    ksd_operation_result result;
    uint32_t length = 0u;
    const char *body;

    ksd_result_init(&result);
    ksd_x11_window_active(connection, &result);
    assert(result.status == KSD_STATUS_OK);
    body = json_body(&result, &length);
    /* No active window is an answer, and it has to be the same shape the
     * compositor providers use so a consumer parses one format. */
    assert(length == strlen("{\"ok\":true,\"window\":null}"));
    assert(memcmp(body, "{\"ok\":true,\"window\":null}", length) == 0);
    ksd_result_clear(&result);
}

/* A capture answers with a sealed descriptor, never with a byte tail, so the
 * pixels are never copied into a response buffer. Maps it read-only, which is
 * how the consumer reads it. */
static const uint8_t *map_capture(const ksd_operation_result *result,
                                  size_t *length)
{
    const uint8_t *pixels;
    int seals;

    assert(result->status == KSD_STATUS_OK);
    assert(result->tail == NULL);
    assert(result->payload_fd >= 0);
    /* Sealed against every kind of change. Unsealed, the sender could rewrite
     * the pixels after their length was agreed and the consumer maps it
     * expecting not to have to re-check. */
    seals = fcntl(result->payload_fd, F_GET_SEALS);
    assert(seals >= 0);
    assert((seals & F_SEAL_SEAL) != 0);
    assert((seals & F_SEAL_SHRINK) != 0);
    assert((seals & F_SEAL_GROW) != 0);
    assert((seals & F_SEAL_WRITE) != 0);

    *length = result->tail_length;
    /* MAP_PRIVATE, exactly as the shipped client maps it. A sealed
     * descriptor cannot be mapped MAP_SHARED at all: F_SEAL_WRITE makes the
     * kernel refuse a shared mapping even at PROT_READ, because such a
     * mapping keeps VM_MAYWRITE and could be made writable later. Reading it
     * the way the consumer does is the point of doing it here. */
    pixels = mmap(NULL, *length, PROT_READ, MAP_PRIVATE, result->payload_fd,
                  0);
    assert(pixels != MAP_FAILED);
    /* The same header the provider backends emit, so one parser reads both. */
    assert(ksd_decode_u16(pixels) == KSD_CAPTURE_FORMAT_BGRA8_PREMULTIPLIED);
    assert(ksd_decode_u16(pixels + 2u) == 0u);
    return pixels;
}

/* Reads one pixel out of a mapped capture. */
static uint32_t pixel_at(const uint8_t *capture, uint32_t x, uint32_t y)
{
    uint32_t stride = ksd_decode_u32(capture + 12u);
    const uint8_t *pixel = capture + 20u + (size_t)y * stride + (size_t)x * 4u;

    return ((uint32_t)pixel[0]) | ((uint32_t)pixel[1] << 8)
        | ((uint32_t)pixel[2] << 16) | ((uint32_t)pixel[3] << 24);
}

/* Defined in x11_capture.c under KSD_X11_TESTING, which is why this test
 * compiles that source rather than taking it from the library. */
extern bool ksd_x11_capture_disable_shm;
extern unsigned ksd_x11_capture_shm_count;

#define PATCH 64u
/* Blue 0x40, green 0x80, red 0xff. Three different bytes, so a channel swap
 * cannot pass: a grey or a primary would let BGRA and RGBA agree. */
#define PATCH_COLOUR 0x00ff8040u
/* What that colour is in memory once alpha is forced opaque. */
#define PATCH_EXPECTED 0xffff8040u

static void check_capture(ksd_x11 *connection, xcb_connection_t *owner,
                          xcb_screen_t *screen)
{
    ksd_operation_result result;
    const uint8_t *capture;
    size_t length;
    uint32_t stride;
    uint32_t values[] = { PATCH_COLOUR };
    xcb_window_t patch = xcb_generate_id(owner);
    xcb_window_t unmapped = xcb_generate_id(owner);

    xcb_create_window(owner, XCB_COPY_FROM_PARENT, patch, screen->root, 0, 0,
                      (uint16_t)PATCH, (uint16_t)PATCH, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                      XCB_CW_BACK_PIXEL, values);
    xcb_map_window(owner, patch);
    /* Never mapped, so the server holds no pixels for it at all. */
    xcb_create_window(owner, XCB_COPY_FROM_PARENT, unmapped, screen->root, 0,
                      0, (uint16_t)PATCH, (uint16_t)PATCH, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                      XCB_CW_BACK_PIXEL, values);
    xcb_flush(owner);
    /* A round trip on the owner connection, so the map has been processed
     * before the connection under test asks for an image. Without it the two
     * connections race and the patch is sometimes not yet on screen. */
    free(xcb_get_input_focus_reply(owner, xcb_get_input_focus(owner), NULL));

    /* An area capture of the patch reads back the colour that was put there,
     * in the channel order the declared format promises. */
    ksd_result_init(&result);
    ksd_x11_capture_area(connection, 0, 0, PATCH, PATCH, &result);
    capture = map_capture(&result, &length);
    assert(ksd_decode_u32(capture + 4u) == PATCH);
    assert(ksd_decode_u32(capture + 8u) == PATCH);
    assert(pixel_at(capture, 0u, 0u) == PATCH_EXPECTED);
    assert(pixel_at(capture, PATCH - 1u, PATCH - 1u) == PATCH_EXPECTED);
    munmap((void *)capture, length);
    ksd_result_clear(&result);

    /* The window verb answers with the same pixels for the same area. */
    ksd_result_init(&result);
    ksd_x11_capture_window(connection, patch, false, &result);
    capture = map_capture(&result, &length);
    assert(ksd_decode_u32(capture + 4u) == PATCH);
    assert(pixel_at(capture, 1u, 1u) == PATCH_EXPECTED);
    munmap((void *)capture, length);
    ksd_result_clear(&result);

    /* A full-width capture on a screen whose width is not a round number. What
     * this gates is that the declared stride, the declared length and the
     * descriptor size agree, and that rows are indexed by that stride rather
     * than running into each other.
     *
     * It does NOT gate the scanline-pad arithmetic, and the odd width does not
     * make it: this backend only accepts 32 bits per pixel, and at 32 bits a
     * scanline is already pad-aligned, so a stride hardcoded to width * 4
     * would agree with the server here. Catching that needs a server at fewer
     * bits per pixel, which is not something this suite can start. */
    ksd_result_init(&result);
    ksd_x11_capture_area(connection, 0, 0, TEST_WIDTH, PATCH, &result);
    capture = map_capture(&result, &length);
    stride = ksd_decode_u32(capture + 12u);
    assert(stride >= TEST_WIDTH * 4u);
    assert(ksd_decode_u32(capture + 16u) == stride * PATCH);
    assert(length == 20u + (size_t)stride * PATCH);
    assert(pixel_at(capture, 0u, 0u) == PATCH_EXPECTED);
    /* Just past the patch on the same row is root background, not the patch,
     * which is what proves rows are indexed by the declared stride. */
    assert(pixel_at(capture, PATCH, 0u) != PATCH_EXPECTED);
    munmap((void *)capture, length);
    ksd_result_clear(&result);

    /* The two transports must be indistinguishable. One asks the server to
     * write into the descriptor, the other copies from a reply; a consumer
     * cannot tell which ran, so the bytes must be identical including the
     * header. Without this the fallback ships untested wherever MIT-SHM is
     * available, which is every local server. */
    uint8_t *shared;
    size_t shared_length;

    unsigned shm_before = ksd_x11_capture_shm_count;

    ksd_result_init(&result);
    ksd_x11_capture_area(connection, 0, 0, PATCH, PATCH, &result);
    capture = map_capture(&result, &length);
    /* The comparison below is worth nothing unless this capture really used
     * shared memory. If the server does not offer MIT-SHM both halves take the
     * same path and the test would agree with itself while proving nothing. */
    assert(ksd_x11_capture_shm_count == shm_before + 1u);
    shared_length = length;
    shared = malloc(shared_length);
    assert(shared != NULL);
    memcpy(shared, capture, shared_length);
    munmap((void *)capture, length);
    ksd_result_clear(&result);

    ksd_x11_capture_disable_shm = true;
    ksd_result_init(&result);
    ksd_x11_capture_area(connection, 0, 0, PATCH, PATCH, &result);
    capture = map_capture(&result, &length);
    /* And this one really did not. */
    assert(ksd_x11_capture_shm_count == shm_before + 1u);
    assert(length == shared_length);
    assert(memcmp(capture, shared, shared_length) == 0);
    assert(pixel_at(capture, 0u, 0u) == PATCH_EXPECTED);
    munmap((void *)capture, length);
    ksd_result_clear(&result);
    ksd_x11_capture_disable_shm = false;
    free(shared);

    /* An unmapped window has no pixels on the server, so the honest answer is
     * that it cannot be served. A black rectangle would be a lie, and
     * undefined memory would be worse than a lie. */
    ksd_result_init(&result);
    ksd_x11_capture_window(connection, unmapped, false, &result);
    assert(result.status == KSD_STATUS_UNAVAILABLE);
    assert(result.payload_fd < 0);
    assert(result.tail == NULL);
    /* The status alone does not gate the check that produced it: the server
     * refuses an image request on an unmapped drawable too, so deleting the
     * map_state test would still yield UNAVAILABLE, by accident and with a
     * diagnostic that says only that the request failed. What is pinned here
     * is that the service knows why and says so. */
    assert(strstr(result.diagnostic, "not on screen") != NULL);
    ksd_result_clear(&result);

    /* A window id nothing ever created is refused the same way rather than
     * faulting the worker. */
    ksd_result_init(&result);
    ksd_x11_capture_window(connection, 0x7ffffffeu, false, &result);
    assert(result.status == KSD_STATUS_UNAVAILABLE);
    assert(result.payload_fd < 0);
    ksd_result_clear(&result);

    /* A zero-sized request is refused before anything is allocated. */
    ksd_result_init(&result);
    ksd_x11_capture_area(connection, 0, 0, 0u, PATCH, &result);
    assert(result.status == KSD_STATUS_INVALID_REQUEST);
    assert(result.payload_fd < 0);
    ksd_result_clear(&result);

    xcb_destroy_window(owner, patch);
    xcb_destroy_window(owner, unmapped);
    xcb_flush(owner);
}

int main(void)
{
    const char *display = getenv("KSD_TEST_DISPLAY");
    char canonical[KSD_X11_DISPLAY_CAPACITY];
    ksd_x11 *connection = NULL;

    /* Probe mode: connect, report, exit. The wrapper polls with this rather
     * than waiting for a socket file to appear, because a server can be
     * reachable without one -- Xvfb falls back to TCP when it cannot bind a
     * UNIX listener, which happens whenever /tmp/.X11-unix is not sticky. */
    if (display != NULL && getenv("KSD_TEST_PROBE") != NULL) {
        ksd_x11 *probe = NULL;
        if (!ksd_x11_display_parse(display, canonical, sizeof(canonical)))
            return 1;
        if (ksd_x11_open(canonical, NULL, &probe) != KSD_STATUS_OK)
            return 1;
        ksd_x11_close(probe);
        return 0;
    }
    if (display == NULL) {
        fputs("KSD_TEST_DISPLAY unset: skipping X server tests\n", stderr);
        return 77;
    }
    /* The worker never hands an unparsed value to the X library, so neither
     * does the test: the display it connects with is one the grammar rebuilt. */
    assert(ksd_x11_display_parse(display, canonical, sizeof(canonical)));

    assert(ksd_x11_open(canonical, NULL, &connection) == KSD_STATUS_OK);
    assert(connection != NULL);

    /* Xvfb is not XWayland, so the veto must not fire here. The opposite case
     * cannot be built in CI, which is why the refusal ships as a runtime
     * diagnostic and not as a gate of its own. */
    assert(!ksd_x11_server_is_xwayland(connection));

    check_cursor_position(connection);
    check_work_area(connection);
    check_window_list(connection);
    check_window_active(connection);

    /* A second connection owns the windows, because the one under test has to
     * see them the way any other client would. */
    xcb_connection_t *owner = xcb_connect(canonical, NULL);
    assert(xcb_connection_has_error(owner) == 0);
    xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(owner)).data;
    assert(screen != NULL);
    check_capture(connection, owner, screen);
    xcb_disconnect(owner);

    ksd_x11_close(connection);

    /* A display nothing is listening on fails as unavailable rather than
     * hanging or aborting the process, which is the whole reason this uses xcb
     * instead of Xlib. */
    connection = NULL;
    assert(ksd_x11_open(":247", NULL, &connection) != KSD_STATUS_OK);
    assert(connection == NULL);

    ksd_x11_close(NULL);
    return 0;
}
