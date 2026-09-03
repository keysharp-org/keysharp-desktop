#include "x11_connect.h"
#include "x11_display.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Runs against a real X server, which CI supplies as Xvfb. A skip is reported
 * as 77 so a run without one is visibly skipped rather than quietly passing:
 * a gate that passes when it did not run is not a gate. */
int main(void)
{
    const char *display = getenv("KSD_TEST_DISPLAY");
    char canonical[KSD_X11_DISPLAY_CAPACITY];
    ksd_x11 *connection = NULL;

    /* Probe mode: connect, report, exit. The wrapper polls with this rather
     * than waiting for a socket file to appear, because a server may be
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
    /* The worker never passes an unparsed value through, so neither does the
     * test: the display it connects with is one the grammar rebuilt. */
    assert(ksd_x11_display_parse(display, canonical, sizeof(canonical)));

    assert(ksd_x11_open(canonical, NULL, &connection) == KSD_STATUS_OK);
    assert(connection != NULL);

    /* Xvfb is not XWayland, so the veto must not fire here. The opposite case
     * cannot be built in CI, which is why the refusal is a runtime diagnostic
     * and not a gate of its own. */
    assert(!ksd_x11_server_is_xwayland(connection));
    ksd_x11_close(connection);

    /* A display number nothing is listening on fails as unavailable rather
     * than hanging or aborting the process, which is the whole reason this
     * uses xcb instead of Xlib. */
    connection = NULL;
    assert(ksd_x11_open(":247", NULL, &connection) != KSD_STATUS_OK);
    assert(connection == NULL);

    ksd_x11_close(NULL);
    return 0;
}
