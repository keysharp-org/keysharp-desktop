#ifndef KEYSHARP_DESKTOP_X11_INTERNAL_H
#define KEYSHARP_DESKTOP_X11_INTERNAL_H

#include "operation_result.h"
#include "protocol_io.h"
#include "x11_connect_internal.h"
#include "x11_query.h"

#include <stdbool.h>
#include <stdint.h>
#include <xcb/xcb.h>

#define KSD_X11_MAX_WINDOWS 4096u
#define KSD_X11_MAX_TEXT 4096u

/* Interned once per operation. A bare window manager sets very few of these,
 * so an atom that does not exist is XCB_ATOM_NONE and every read of it is
 * absent rather than an error. */
typedef struct x11_atoms {
    xcb_atom_t client_list;
    xcb_atom_t active_window;
    xcb_atom_t work_area;
    xcb_atom_t current_desktop;
    xcb_atom_t wm_desktop;
    xcb_atom_t wm_name;
    xcb_atom_t wm_pid;
    xcb_atom_t wm_state;
    xcb_atom_t state_hidden;
    xcb_atom_t state_above;
    xcb_atom_t state_max_vert;
    xcb_atom_t state_max_horz;
    xcb_atom_t frame_extents;
    xcb_atom_t opacity;
    xcb_atom_t utf8_string;
} x11_atoms;

void ksd_x11_load_atoms(xcb_connection_t *connection, x11_atoms *atoms);
xcb_get_property_reply_t *ksd_x11_property(xcb_connection_t *connection,
                                           xcb_window_t window,
                                           xcb_atom_t name, xcb_atom_t type,
                                           uint32_t words);
bool ksd_x11_cardinal(xcb_connection_t *connection, xcb_window_t window,
                      xcb_atom_t name, uint32_t *value);
/* Issues a property request without waiting, and collects one. Splitting the
 * two is the whole point of xcb: a caller can put every request it needs on
 * the wire and then read the answers, instead of paying a round trip each. */
xcb_get_property_cookie_t ksd_x11_property_cookie(xcb_connection_t *connection,
                                                  xcb_window_t window,
                                                  xcb_atom_t name,
                                                  xcb_atom_t type,
                                                  uint32_t words);
xcb_get_property_reply_t *ksd_x11_take_property(xcb_connection_t *connection,
                                                xcb_get_property_cookie_t cookie);
/* Whether a collected _NET_WM_STATE reply carries an atom. */
bool ksd_x11_state_has(const xcb_get_property_reply_t *state,
                       xcb_atom_t wanted);
/* Appends a JSON string from a collected reply, taking ownership of it. */
bool ksd_x11_append_text_reply(ksd_buffer *out,
                               xcb_get_property_reply_t *reply);
bool ksd_x11_has_state(xcb_connection_t *connection, const x11_atoms *atoms,
                       xcb_window_t window, xcb_atom_t wanted);
/* Appends a JSON string. Every byte JSON cannot carry raw is escaped,
 * including the C0 range, because a window title is arbitrary text chosen by
 * another application and it is being pasted into a document the caller will
 * parse. Invalid UTF-8 is truncated away rather than passed through. */
bool ksd_x11_append_text(ksd_buffer *out, xcb_connection_t *connection,
                         xcb_window_t window, xcb_atom_t name,
                         xcb_atom_t type);

#endif
