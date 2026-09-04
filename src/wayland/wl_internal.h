#ifndef KEYSHARP_DESKTOP_WL_INTERNAL_H
#define KEYSHARP_DESKTOP_WL_INTERNAL_H

#include "ext-data-control-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "wl_connect.h"

#include <wayland-client.h>

/* Shared between the Wayland sources only. Nothing outside src/wayland sees a
 * wl_ type, so no other target needs the Wayland headers. */
struct ksd_wayland {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_seat *seat;
    struct ext_data_control_manager_v1 *data_control;
    struct ext_foreign_toplevel_list_v1 *toplevel_list;
    /* The name and version the registry advertised for the seat, kept so a
     * seat arriving after the first round trip can still be bound. */
    uint32_t seat_name;
};

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
