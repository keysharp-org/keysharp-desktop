#ifndef KEYSHARP_DESKTOP_X11_DISPLAY_H
#define KEYSHARP_DESKTOP_X11_DISPLAY_H

#include <stdbool.h>
#include <stddef.h>

/* The longest display this accepts is ":255.255" plus a terminator. */
#define KSD_X11_DISPLAY_CAPACITY 16u

/* Parses a local DISPLAY value and rebuilds it from the numbers it read, so
 * no byte of the input ever reaches the X library. Only the local form is
 * accepted: a host part would name a remote server, and a transport prefix
 * would choose a transport, neither of which the broker will do on a
 * caller's behalf. Returns false and leaves canonical untouched otherwise. */
bool ksd_x11_display_parse(const char *value, char *canonical,
                           size_t capacity);

#endif
