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
#include <time.h>
#include <xcb/xcb.h>

/* Creates real windows on a bare server and times the window list against them.
 * The point is not an absolute number, which means little under Xvfb on a
 * software stack; it is the shape of the cost as the window count grows. A
 * per-window cost dominated by round trips grows linearly and steeply, and that
 * is what a consumer asking for the window list in a loop would feel. */
#define BENCH_WINDOWS 40

static double milliseconds_since(const struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start->tv_sec) * 1000.0
        + (double)(now.tv_nsec - start->tv_nsec) / 1000000.0;
}

int main(void)
{
    const char *display = getenv("KSD_TEST_DISPLAY");
    char canonical[KSD_X11_DISPLAY_CAPACITY];
    ksd_x11 *connection = NULL;
    xcb_connection_t *owner;
    xcb_screen_t *screen;
    xcb_window_t windows[BENCH_WINDOWS];
    xcb_atom_t client_list;
    xcb_atom_t utf8;
    struct timespec start;
    double elapsed;

    if (display == NULL) {
        fputs("KSD_TEST_DISPLAY unset: skipping X11 benchmark\n", stderr);
        return 77;
    }
    assert(ksd_x11_display_parse(display, canonical, sizeof(canonical)));

    /* A separate connection owns the windows, because the one under test must
     * see them the way any other client would. */
    owner = xcb_connect(canonical, NULL);
    assert(xcb_connection_has_error(owner) == 0);
    screen = xcb_setup_roots_iterator(xcb_get_setup(owner)).data;
    assert(screen != NULL);

    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(owner,
        xcb_intern_atom(owner, 0, 16u, "_NET_CLIENT_LIST"), NULL);
    assert(reply != NULL);
    client_list = reply->atom;
    free(reply);
    reply = xcb_intern_atom_reply(owner,
        xcb_intern_atom(owner, 0, 11u, "UTF8_STRING"), NULL);
    assert(reply != NULL);
    utf8 = reply->atom;
    free(reply);
    xcb_atom_t wm_name = xcb_intern_atom_reply(owner,
        xcb_intern_atom(owner, 0, 12u, "_NET_WM_NAME"), NULL)->atom;

    for (int index = 0; index < BENCH_WINDOWS; index++) {
        char title[64];
        int written = snprintf(title, sizeof(title), "bench window %d", index);

        windows[index] = xcb_generate_id(owner);
        xcb_create_window(owner, XCB_COPY_FROM_PARENT, windows[index],
                          screen->root, (int16_t)(index * 3), 10, 200, 100, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                          0, NULL);
        xcb_map_window(owner, windows[index]);
        xcb_change_property(owner, XCB_PROP_MODE_REPLACE, windows[index],
                            wm_name, utf8, 8, (uint32_t)written, title);
        xcb_change_property(owner, XCB_PROP_MODE_REPLACE, windows[index],
                            XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, 10u,
                            "bench\0Bench");
    }
    /* Stand in for a window manager, so the list has something to enumerate. */
    xcb_change_property(owner, XCB_PROP_MODE_REPLACE, screen->root,
                        client_list, XCB_ATOM_WINDOW, 32,
                        (uint32_t)BENCH_WINDOWS, windows);
    xcb_flush(owner);

    assert(ksd_x11_open(canonical, NULL, &connection) == KSD_STATUS_OK);

    /* One pass to warm any caching, then the measured passes. */
    ksd_operation_result warm;
    ksd_result_init(&warm);
    ksd_x11_window_list(connection, true, &warm);
    assert(warm.status == KSD_STATUS_OK);

    /* The shape the consumer actually parses. transparency is an integer from
     * 0 to 255 on every backend, because the providers report the actor
     * opacity on that scale and the consumer reads it as an integer. Emitting
     * a fraction here would parse as zero or throw, on X11 only. */
    const char *body = (const char *)warm.tail + 4u;
    assert(strstr(body, "\"transparency\":255") != NULL);
    assert(strstr(body, "\"transparency\":1.") == NULL);
    assert(strstr(body, "\"transparency\":0.") == NULL);
    /* Titles and classes survive the round trip, so the pipelined collect is
     * still pairing each reply with the window that asked for it. */
    assert(strstr(body, "\"title\":\"bench window 0\"") != NULL);
    assert(strstr(body, "\"title\":\"bench window 39\"") != NULL);
    assert(strstr(body, "\"appId\":\"bench\"") != NULL);
    ksd_result_clear(&warm);

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int pass = 0; pass < 10; pass++) {
        ksd_operation_result result;
        ksd_result_init(&result);
        ksd_x11_window_list(connection, true, &result);
        assert(result.status == KSD_STATUS_OK);
        ksd_result_clear(&result);
    }
    elapsed = milliseconds_since(&start) / 10.0;

    printf("window_list over %d windows: %.2f ms per call (%.3f ms per window)\n",
           BENCH_WINDOWS, elapsed, elapsed / (double)BENCH_WINDOWS);

    ksd_x11_close(connection);
    xcb_disconnect(owner);
    return 0;
}
