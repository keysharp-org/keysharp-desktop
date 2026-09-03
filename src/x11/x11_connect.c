#include "x11_connect.h"

#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

struct ksd_x11 {
    xcb_connection_t *connection;
    xcb_screen_t *screen;
};

/* xcb latches a connection error and no-ops every later call, which is the
 * shape this service already wants: one unavailable answer rather than a
 * process that exits under the caller. Xlib would have called exit() here. */
static ksd_status connection_status(xcb_connection_t *connection)
{
    switch (xcb_connection_has_error(connection)) {
        case 0:
            return KSD_STATUS_OK;
        case XCB_CONN_CLOSED_INVALID_SCREEN:
            return KSD_STATUS_NOT_FOUND;
        case XCB_CONN_CLOSED_MEM_INSUFFICIENT:
            return KSD_STATUS_RESOURCE_EXHAUSTED;
        default:
            return KSD_STATUS_UNAVAILABLE;
    }
}

bool ksd_x11_server_is_xwayland(ksd_x11 *connection)
{
    static const char name[] = "XWAYLAND";
    xcb_query_extension_cookie_t cookie;
    xcb_query_extension_reply_t *reply;
    bool present;

    if (connection == NULL || connection->connection == NULL)
        return false;
    cookie = xcb_query_extension(connection->connection,
                                 (uint16_t)(sizeof(name) - 1u), name);
    reply = xcb_query_extension_reply(connection->connection, cookie, NULL);
    if (reply == NULL)
        return false;
    present = reply->present != 0;
    free(reply);
    return present;
}

ksd_status ksd_x11_open(const char *display, const char *authority,
                        ksd_x11 **connection)
{
    int screen_number = 0;
    ksd_status status;

    if (display == NULL || connection == NULL)
        return KSD_STATUS_INVALID_REQUEST;
    *connection = NULL;
    if (authority != NULL && setenv("XAUTHORITY", authority, 1) != 0)
        return KSD_STATUS_INTERNAL;

    ksd_x11 *opened = calloc(1u, sizeof(*opened));
    if (opened == NULL)
        return KSD_STATUS_RESOURCE_EXHAUSTED;
    opened->connection = xcb_connect(display, &screen_number);
    status = connection_status(opened->connection);
    if (status != KSD_STATUS_OK) {
        ksd_x11_close(opened);
        return status;
    }
    if (ksd_x11_server_is_xwayland(opened)) {
        ksd_x11_close(opened);
        return KSD_STATUS_UNAVAILABLE;
    }

    const xcb_setup_t *setup = xcb_get_setup(opened->connection);
    xcb_screen_iterator_t screens = xcb_setup_roots_iterator(setup);
    for (int index = 0; index < screen_number && screens.rem != 0; index++)
        xcb_screen_next(&screens);
    if (screens.rem == 0) {
        ksd_x11_close(opened);
        return KSD_STATUS_NOT_FOUND;
    }
    opened->screen = screens.data;
    *connection = opened;
    return KSD_STATUS_OK;
}

void ksd_x11_close(ksd_x11 *connection)
{
    if (connection == NULL)
        return;
    if (connection->connection != NULL)
        xcb_disconnect(connection->connection);
    free(connection);
}
