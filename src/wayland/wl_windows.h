#ifndef KEYSHARP_DESKTOP_WL_WINDOWS_H
#define KEYSHARP_DESKTOP_WL_WINDOWS_H

#include "operation_result.h"
#include "protocol_io.h"
#include "wl_connect.h"

struct ksd_wl_toplevel;

typedef struct ksd_wayland_window_view {
    bool (*usable)(const struct ksd_wl_toplevel *toplevel);
    uint32_t (*state)(const struct ksd_wl_toplevel *toplevel);
    bool (*append_window)(ksd_buffer *out, ksd_wayland *connection,
                          const struct ksd_wl_toplevel *toplevel);
} ksd_wayland_window_view;

const ksd_wayland_window_view *ksd_wayland_wlr_window_view(void);
const ksd_wayland_window_view *ksd_wayland_cosmic_window_view(void);
struct ksd_wl_toplevel *ksd_wayland_window_for_action(
    ksd_wayland *connection, uint64_t handle,
    const ksd_wayland_window_view *view, ksd_operation_result *result);

/* ext-foreign-toplevel-list is the portable enumeration floor. Optional
 * wlroots and COSMIC protocols add state, active-window lookup and the actions
 * they advertise; COSMIC also adds geometry. Facts absent from the selected
 * protocol are omitted rather than filled with zeros. */
/* Handles only, carrying no properties and needing no grant. */
void ksd_wayland_window_handles(ksd_wayland *connection,
                                ksd_operation_result *result);
void ksd_wayland_window_list(ksd_wayland *connection, bool include_hidden,
                             ksd_operation_result *result);
void ksd_wayland_window_query(ksd_wayland *connection,
    uint64_t handle, ksd_operation_result *result);
void ksd_wayland_active_window(ksd_wayland *connection,
                               ksd_operation_result *result);
void ksd_wayland_window_action(ksd_wayland *connection, uint16_t opcode,
                               uint64_t handle, uint32_t value,
                               ksd_operation_result *result);

#endif
