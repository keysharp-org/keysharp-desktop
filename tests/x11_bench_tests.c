#include "operation_result.h"
#include "protocol.h"
#include "protocol_io.h"
#include "x11_connect.h"
#include "x11_display.h"
#include "x11_query.h"
#include "x11_extended.h"
#include "x11_capture.h"

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

static void bench_keyboard(ksd_x11 *connection)
{
    ksd_operation_result result;
    ksd_result_init(&result);
    ksd_x11_keyboard_state_since(connection, NULL, 0u, &result);
    assert(result.status == KSD_STATUS_OK && result.tail_length > 4u);
    uint32_t full_bytes = result.tail_length;
    char *json = calloc(result.tail_length, 1u);
    assert(json != NULL);
    memcpy(json, result.tail + 4u, result.tail_length - 4u);
    const char field[] = "\"mapRevision\":\"";
    const char *revision = strstr(json, field);
    assert(revision != NULL && strlen(revision) >= sizeof(field) - 1u + 33u);
    uint8_t token[32];
    memcpy(token, revision + sizeof(field) - 1u, sizeof(token));
    free(json);
    ksd_result_clear(&result);

    for (unsigned unchanged = 0u; unchanged <= 1u; unchanged++) {
        struct timespec start;
        uint64_t bytes = 0u;
        clock_gettime(CLOCK_MONOTONIC, &start);
        for (unsigned pass = 0u; pass < 1000u; pass++) {
            ksd_result_init(&result);
            ksd_x11_keyboard_state_since(connection, unchanged ? token : NULL,
                unchanged ? sizeof(token) : 0u, &result);
            assert(result.status == KSD_STATUS_OK);
            if (unchanged) assert(result.tail_length < full_bytes);
            bytes += result.tail_length;
            ksd_result_clear(&result);
        }
        printf("keyboard_state %s: %.3f ms/call, %.0f reply bytes/call (1000 calls)\n",
            unchanged ? "unchanged" : "full map", milliseconds_since(&start) / 1000.0,
            (double)bytes / 1000.0);
    }
}

static void bench_window(ksd_x11 *connection, xcb_window_t window)
{
    struct timespec start;
    for (unsigned capture = 0u; capture <= 1u; capture++) {
        uint64_t bytes = 0u;
        clock_gettime(CLOCK_MONOTONIC, &start);
        for (unsigned pass = 0u; pass < 1000u; pass++) {
            ksd_operation_result result;
            ksd_result_init(&result);
            if (capture) ksd_x11_capture_window(connection, window, 0u, &result);
            else ksd_x11_window_query(connection, window, &result);
            assert(result.status == KSD_STATUS_OK);
            bytes += result.tail_length;
            ksd_result_clear(&result);
        }
        printf("%s on one persistent X connection: %.3f ms/call, %.0f reply bytes/call (1000 calls)\n",
            capture ? "capture_window 200x100" : "window_query", milliseconds_since(&start) / 1000.0,
            (double)bytes / 1000.0);
    }
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
    if (getenv("KSD_TEST_PROBE") != NULL) {
        if (ksd_x11_open(canonical, NULL, &connection) != KSD_STATUS_OK) return 1;
        ksd_x11_close(connection);
        return 0;
    }

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
    reply = xcb_intern_atom_reply(owner,
        xcb_intern_atom(owner, 0, 12u, "_NET_WM_NAME"), NULL);
    assert(reply != NULL);
    xcb_atom_t wm_name = reply->atom;
    free(reply);

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
                            XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, 11u,
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
    char *body = calloc(warm.tail_length, 1u);
    assert(body != NULL);
    memcpy(body, warm.tail + 4u, warm.tail_length - 4u);
    assert(strstr(body, "\"transparency\":255") != NULL);
    assert(strstr(body, "\"transparency\":1.") == NULL);
    assert(strstr(body, "\"transparency\":0.") == NULL);
    /* Titles and classes survive the round trip, so the pipelined collect is
     * still pairing each reply with the window that asked for it. */
    assert(strstr(body, "\"title\":\"bench window 0\"") != NULL);
    assert(strstr(body, "\"title\":\"bench window 39\"") != NULL);
    assert(strstr(body, "\"appId\":\"Bench\"") != NULL);
    free(body);
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
    bench_keyboard(connection);
    bench_window(connection, windows[BENCH_WINDOWS - 1u]);

    ksd_x11_close(connection);
    xcb_disconnect(owner);
    return 0;
}
