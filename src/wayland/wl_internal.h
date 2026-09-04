#ifndef KEYSHARP_DESKTOP_WL_INTERNAL_H
#define KEYSHARP_DESKTOP_WL_INTERNAL_H

#include "ext-data-control-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "wl_connect.h"

#include <wayland-client.h>

/* Shared between the Wayland sources only. Nothing outside src/wayland sees a
 * wl_ type, so no other target needs the Wayland headers. */
/* One toplevel the compositor has told us about. The protocol delivers a
 * handle and then its properties as separate events, so these accumulate as
 * they arrive and are read once the compositor says it is done. */
typedef struct ksd_wl_toplevel {
    struct ext_foreign_toplevel_handle_v1 *handle;
    char *title;
    char *app_id;
    char *identifier;
    bool closed;
    struct ksd_wl_toplevel *next;
} ksd_wl_toplevel;

struct ksd_wayland {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_seat *seat;
    struct ext_data_control_manager_v1 *data_control;
    struct ext_foreign_toplevel_list_v1 *toplevel_list;
    /* The name and version the registry advertised for the seat, kept so a
     * seat arriving after the first round trip can still be bound. */
    uint32_t seat_name;
    /* Every toplevel seen on this connection, oldest first. The list is
     * attached at connect rather than per call, because the compositor sends
     * one toplevel event per existing window as soon as the global is bound
     * and a listener added later would miss them all. */
    ksd_wl_toplevel *toplevels;
};

/* Attaches the toplevel listener. Called once, immediately after the global is
 * bound, for the reason above. */
void ksd_wayland_toplevels_attach(ksd_wayland *connection);
void ksd_wayland_toplevels_clear(ksd_wayland *connection);

/* One round trip, bounded. wl_display_roundtrip blocks with no deadline of its
 * own, and a compositor that stops answering would park the worker for as long
 * as it liked -- the same hazard the X11 clipboard has, answered the same way. */
bool ksd_wayland_roundtrip(ksd_wayland *connection, int timeout_ms);

/* Reads everything a peer writes into a pipe, to a ceiling, with a deadline.
 * Both the clipboard read path and anything else that takes a descriptor from
 * the compositor need exactly this. */
bool ksd_wayland_drain(int descriptor, int timeout_ms, uint8_t **data,
                       size_t *length);

#endif
