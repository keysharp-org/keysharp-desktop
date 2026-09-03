#include "operation_result.h"
#include "protocol.h"
#include "protocol_io.h"
#include "x11_connect.h"
#include "x11_display.h"
#include "x11_query.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
