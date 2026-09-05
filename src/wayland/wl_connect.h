#ifndef KEYSHARP_DESKTOP_WL_CONNECT_H
#define KEYSHARP_DESKTOP_WL_CONNECT_H

#include "protocol.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

/* A Wayland connection to the session compositor, for the backend that has no
 * extension of its own: wlroots compositors, COSMIC, sway, Hyprland, niri and
 * anything else that implements the shared protocols.
 *
 * This is a different position from every other backend here. GNOME and
 * Cinnamon run code INSIDE the compositor, so they sit on the privileged side
 * of every Wayland restriction. This is an ordinary client, on the outside,
 * and can only do what a compositor has chosen to expose to one. A good deal
 * is therefore not merely unimplemented but impossible, and the backend says
 * so through its mask rather than by failing at call time. */
typedef struct ksd_wayland ksd_wayland;

/* Which optional protocols this compositor turned out to advertise. Bound once
 * at connect, because a registry walk is a round trip and every operation
 * would otherwise pay it. */
typedef struct ksd_wayland_features {
    bool data_control;
    bool toplevel_list;
    bool toplevel_control;
    bool toplevel_active;
    bool toplevel_focus;
    bool toplevel_close;
    bool toplevel_state;
    bool screencopy;
    bool absolute_pointer;
    bool cursor_position;
    bool keyboard_keymap;
} ksd_wayland_features;

/* display may be NULL, in which case WAYLAND_DISPLAY is used the way any
 * Wayland client would. Returns UNAVAILABLE when there is no compositor to
 * talk to, which is the ordinary case off a Wayland session and not an error
 * worth a diagnostic of its own. */
ksd_status ksd_wayland_open(const char *display, ksd_wayland **connection);
void ksd_wayland_set_session_pid(ksd_wayland *connection, pid_t session_pid);
pid_t ksd_wayland_session_pid(const ksd_wayland *connection);
void ksd_wayland_close(ksd_wayland *connection);
ksd_wayland_features ksd_wayland_supported(const ksd_wayland *connection);

/* Whether the compositor connection has failed. A worker that keeps one must
 * notice and reopen rather than answering every later request from a dead
 * connection. */
bool ksd_wayland_connection_failed(const ksd_wayland *connection);

#endif
