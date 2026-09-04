#include "operation_result.h"
#include "protocol.h"
#include "protocol_io.h"
#include "x11_capture.h"
#include "x11_clipboard.h"
#include "x11_connect.h"
#include "x11_display.h"
#include "x11_query.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
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
/* Defined in x11_clipboard.c under the same define. */
extern int ksd_x11_clipboard_timeout_ms;

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

#define CLIP_TEXT "clipboard \xc3\xa4\xc3\xb6 text"
#define CLIP_CSV "a,b,c"

/* A forked helper must not outlive the test that started it. When an assertion
 * aborts the parent the child keeps running, and because it inherited the
 * stdout pipe, ctest blocks waiting for EOF on that pipe rather than reaping
 * the failure -- so a failing assertion costs the suite's entire timeout
 * instead of failing at once. Asking the kernel to kill the child with its
 * parent, and dropping the inherited pipes, removes both halves of that. */
static void detach_child(void)
{
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    /* The parent can die between the fork and the line above, in which case
     * that signal will never arrive and only this check ends the child. */
    if (getppid() == 1)
        _exit(0);
    (void)freopen("/dev/null", "w", stdout);
    (void)freopen("/dev/null", "w", stderr);
}

static xcb_atom_t intern_in(xcb_connection_t *c, const char *name)
{
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(c,
        xcb_intern_atom(c, 0, (uint16_t)strlen(name), name), NULL);
    xcb_atom_t atom = reply == NULL ? XCB_ATOM_NONE : reply->atom;

    free(reply);
    return atom;
}

/* Becomes the clipboard owner and answers conversion requests, which is the
 * only way to read a selection: there is no clipboard on an X server, only
 * whichever client currently claims to hold one. Runs in a child process
 * because the read under test blocks waiting for exactly these replies. */
static void own_clipboard(const char *canonical)
{
    xcb_connection_t *c;

    detach_child();
    c = xcb_connect(canonical, NULL);
    xcb_screen_t *screen;
    xcb_window_t window;
    xcb_atom_t clipboard;
    xcb_atom_t targets;
    xcb_atom_t utf8;
    xcb_atom_t csv;
    xcb_generic_event_t *event;

    if (c == NULL || xcb_connection_has_error(c) != 0)
        _exit(1);
    screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    clipboard = intern_in(c, "CLIPBOARD");
    targets = intern_in(c, "TARGETS");
    utf8 = intern_in(c, "UTF8_STRING");
    csv = intern_in(c, "text/csv");
    window = xcb_generate_id(c);
    xcb_create_window(c, XCB_COPY_FROM_PARENT, window, screen->root, 0, 0, 1u,
                      1u, 0, XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT,
                      0, NULL);
    xcb_set_selection_owner(c, window, clipboard, XCB_CURRENT_TIME);
    xcb_flush(c);

    while ((event = xcb_wait_for_event(c)) != NULL) {
        xcb_selection_request_event_t *request;
        xcb_selection_notify_event_t notify;

        if ((event->response_type & 0x7fu) != XCB_SELECTION_REQUEST) {
            free(event);
            continue;
        }
        request = (xcb_selection_request_event_t *)event;
        memset(&notify, 0, sizeof(notify));
        notify.response_type = XCB_SELECTION_NOTIFY;
        notify.requestor = request->requestor;
        notify.selection = request->selection;
        notify.target = request->target;
        notify.time = request->time;
        notify.property = request->property;

        if (request->target == targets) {
            xcb_atom_t offered[] = { targets, utf8, csv };
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, request->requestor,
                                request->property, XCB_ATOM_ATOM, 32, 3u,
                                offered);
        } else if (request->target == utf8) {
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, request->requestor,
                                request->property, utf8, 8,
                                (uint32_t)strlen(CLIP_TEXT), CLIP_TEXT);
        } else if (request->target == csv) {
            xcb_change_property(c, XCB_PROP_MODE_REPLACE, request->requestor,
                                request->property, csv, 8,
                                (uint32_t)strlen(CLIP_CSV), CLIP_CSV);
        } else {
            /* A target the owner will not convert to. None is how ICCCM says
             * so, and the service must report that rather than hanging. */
            notify.property = XCB_ATOM_NONE;
        }
        xcb_send_event(c, 0, request->requestor, 0, (const char *)&notify);
        xcb_flush(c);
        free(event);
    }
    _exit(0);
}

/* The reply the providers frame as a count and then the strings. */
static bool mimetypes_contain(const ksd_operation_result *result,
                              const char *wanted)
{
    const uint8_t *tail = result->tail;
    uint32_t count;
    size_t offset = 8u;

    assert(result->tail_length >= 8u);
    count = ksd_decode_u32(tail);
    assert(ksd_decode_u32(tail + 4u) == 0u);
    for (uint32_t index = 0u; index < count; index++) {
        uint32_t length;

        assert(offset + 4u <= result->tail_length);
        length = ksd_decode_u32(tail + offset);
        offset += 4u;
        assert(offset + length <= result->tail_length);
        if (strlen(wanted) == length
            && memcmp(tail + offset, wanted, length) == 0)
            return true;
        offset += length;
    }
    assert(offset == result->tail_length);
    return false;
}

static void check_clipboard(ksd_x11 *connection, xcb_connection_t *owner,
                            const char *canonical)
{
    ksd_operation_result result;
    xcb_atom_t clipboard = intern_in(owner, "CLIPBOARD");
    pid_t child;
    uint32_t length;

    /* Nothing owns the clipboard yet, so every read is an empty answer rather
     * than a failure: an unowned selection is a legitimate state. */
    ksd_result_init(&result);
    ksd_x11_clipboard_text(connection, &result);
    assert(result.status == KSD_STATUS_OK);
    assert(result.tail_length == 4u);
    assert(ksd_decode_u32(result.tail) == 0u);
    ksd_result_clear(&result);

    ksd_result_init(&result);
    ksd_x11_clipboard_mimetypes(connection, &result);
    assert(result.status == KSD_STATUS_OK);
    assert(ksd_decode_u32(result.tail) == 0u);
    ksd_result_clear(&result);

    child = fork();
    assert(child >= 0);
    if (child == 0)
        own_clipboard(canonical);

    /* Wait for the child to actually hold the selection. Reading before it
     * does would test the unowned path again and pass for the wrong reason. */
    for (int attempt = 0; attempt < 500; attempt++) {
        xcb_get_selection_owner_reply_t *reply =
            xcb_get_selection_owner_reply(owner,
                xcb_get_selection_owner(owner, clipboard), NULL);
        bool owned = reply != NULL && reply->owner != XCB_WINDOW_NONE;

        free(reply);
        if (owned)
            break;
        assert(attempt < 499);
        usleep(10000);
    }

    /* Text comes back byte for byte, including the non-ASCII in it: a
     * transport that mangled encoding would show here and not on plain ASCII. */
    ksd_result_init(&result);
    ksd_x11_clipboard_text(connection, &result);
    assert(result.status == KSD_STATUS_OK);
    length = ksd_decode_u32(result.tail);
    assert(length == strlen(CLIP_TEXT));
    assert(memcmp(result.tail + 4u, CLIP_TEXT, length) == 0);
    ksd_result_clear(&result);

    /* The format list reports mimetypes. UTF8_STRING is an X11 target name,
     * not a mimetype, so it is reported as the mimetype every other backend
     * uses for text; TARGETS is not a format at all and must not appear. */
    ksd_result_init(&result);
    ksd_x11_clipboard_mimetypes(connection, &result);
    assert(result.status == KSD_STATUS_OK);
    assert(mimetypes_contain(&result, KSD_CLIPBOARD_TEXT_MIMETYPE));
    assert(mimetypes_contain(&result, "text/csv"));
    assert(!mimetypes_contain(&result, "TARGETS"));
    assert(!mimetypes_contain(&result, "UTF8_STRING"));
    ksd_result_clear(&result);

    /* One named format, fetched by its mimetype. */
    ksd_result_init(&result);
    ksd_x11_clipboard_content(connection, (const uint8_t *)"text/csv", 8u,
                              &result);
    assert(result.status == KSD_STATUS_OK);
    length = ksd_decode_u32(result.tail);
    assert(length == strlen(CLIP_CSV));
    assert(memcmp(result.tail + 4u, CLIP_CSV, length) == 0);
    ksd_result_clear(&result);

    /* The canonical text mimetype is spelled UTF8_STRING on the wire, so
     * asking for it by mimetype has to reach the same bytes as the text verb. */
    ksd_result_init(&result);
    ksd_x11_clipboard_content(connection,
                              (const uint8_t *)KSD_CLIPBOARD_TEXT_MIMETYPE,
                              (uint32_t)strlen(KSD_CLIPBOARD_TEXT_MIMETYPE),
                              &result);
    assert(result.status == KSD_STATUS_OK);
    assert(ksd_decode_u32(result.tail) == strlen(CLIP_TEXT));
    ksd_result_clear(&result);

    /* A format the owner will not convert to is refused, not waited on. The
     * owner answers with None, and reporting that promptly is what keeps a
     * hostile or merely unhelpful owner from parking the worker. */
    ksd_result_init(&result);
    ksd_x11_clipboard_content(connection, (const uint8_t *)"application/x-no",
                              16u, &result);
    assert(result.status == KSD_STATUS_UNSUPPORTED);
    assert(result.tail == NULL);
    ksd_result_clear(&result);

    kill(child, SIGKILL);
    waitpid(child, NULL, 0);

    /* An owner that takes the selection and then never answers. Nothing in the
     * protocol obliges it to reply, so without a deadline any client could
     * park this worker for as long as it liked simply by claiming the
     * clipboard and going quiet -- and the worker holds a slot while it waits.
     * The budget is lowered here so proving that costs a fraction of a second
     * rather than the whole production timeout. */
    ksd_x11_clipboard_timeout_ms = 250;
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        xcb_connection_t *mute;

        detach_child();
        mute = xcb_connect(canonical, NULL);
        xcb_screen_t *mute_screen =
            xcb_setup_roots_iterator(xcb_get_setup(mute)).data;
        xcb_window_t window = xcb_generate_id(mute);

        xcb_create_window(mute, XCB_COPY_FROM_PARENT, window,
                          mute_screen->root, 0, 0, 1u, 1u, 0,
                          XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT,
                          0, NULL);
        xcb_set_selection_owner(mute, window, intern_in(mute, "CLIPBOARD"),
                                XCB_CURRENT_TIME);
        xcb_flush(mute);
        for (;;)
            pause();
    }
    for (int attempt = 0; attempt < 500; attempt++) {
        xcb_get_selection_owner_reply_t *reply =
            xcb_get_selection_owner_reply(owner,
                xcb_get_selection_owner(owner, clipboard), NULL);
        bool owned = reply != NULL && reply->owner != XCB_WINDOW_NONE;

        free(reply);
        if (owned)
            break;
        assert(attempt < 499);
        usleep(10000);
    }

    ksd_result_init(&result);
    ksd_x11_clipboard_text(connection, &result);
    assert(result.status == KSD_STATUS_TIMEOUT);
    assert(result.tail == NULL);
    ksd_result_clear(&result);

    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    ksd_x11_clipboard_timeout_ms = 5000;
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
    check_clipboard(connection, owner, canonical);
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
