#ifndef KEYSHARP_DESKTOP_X11_CONNECT_INTERNAL_H
#define KEYSHARP_DESKTOP_X11_CONNECT_INTERNAL_H

#include <xcb/xcb.h>

/* Shared between the X11 sources only. Nothing outside src/x11 sees an xcb
 * type, so no other target ever needs the xcb headers. */
struct ksd_x11 {
    xcb_connection_t *connection;
    xcb_screen_t *screen;
    struct ksd_x11_keyboard_cache *keyboard;
};

#endif
