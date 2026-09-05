#ifndef KEYSHARP_DESKTOP_X11_CONNECT_INTERNAL_H
#define KEYSHARP_DESKTOP_X11_CONNECT_INTERNAL_H

#include <xcb/xcb.h>

typedef struct x11_atoms {
    xcb_atom_t client_list;
    xcb_atom_t client_list_stacking;
    xcb_atom_t active_window;
    xcb_atom_t close_window;
    xcb_atom_t moveresize_window;
    xcb_atom_t work_area;
    xcb_atom_t current_desktop;
    xcb_atom_t wm_desktop;
    xcb_atom_t wm_name;
    xcb_atom_t wm_pid;
    xcb_atom_t icccm_wm_state;
    xcb_atom_t wm_state;
    xcb_atom_t state_hidden;
    xcb_atom_t state_above;
    xcb_atom_t state_max_vert;
    xcb_atom_t state_max_horz;
    xcb_atom_t change_state;
    xcb_atom_t frame_extents;
    xcb_atom_t opacity;
    xcb_atom_t motif_hints;
    xcb_atom_t utf8_string;
} x11_atoms;

/* Shared between the X11 sources only. Nothing outside src/x11 sees an xcb
 * type, so no other target ever needs the xcb headers. */
struct ksd_x11 {
    xcb_connection_t *connection;
    xcb_screen_t *screen;
    x11_atoms atoms;
    struct ksd_x11_keyboard_cache *keyboard;
};

void ksd_x11_load_atoms(xcb_connection_t *connection, x11_atoms *atoms);

#endif
