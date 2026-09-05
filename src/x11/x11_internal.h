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
#endif
