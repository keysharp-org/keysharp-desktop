#ifndef KEYSHARP_DESKTOP_WL_HYPR_WINDOWS_H
#define KEYSHARP_DESKTOP_WL_HYPR_WINDOWS_H

#include "operation_result.h"
#include "wl_connect.h"

#include <stdbool.h>
#include <stdint.h>

/* Hyprland's portable toplevel list joined with its IPC client facts. The
 * list supplies handles and capture identifiers; the IPC supplies geometry,
 * process, workspace and stacking facts and carries the window actions. */
bool ksd_wayland_hypr_windows_available(const ksd_wayland *connection);
bool ksd_wayland_hypr_windows_refresh(ksd_wayland *connection,
                                      ksd_operation_result *result);
void ksd_wayland_hypr_window_action(ksd_wayland *connection, uint16_t opcode,
                                    uint64_t handle, uint32_t value,
                                    ksd_operation_result *result);
void ksd_wayland_hypr_window_move_resize(ksd_wayland *connection,
                                         uint64_t handle, int32_t x,
                                         int32_t y, uint32_t width,
                                         uint32_t height,
                                         ksd_operation_result *result);
void ksd_wayland_hypr_window_at_point(ksd_wayland *connection, int32_t x,
                                      int32_t y, ksd_operation_result *result);

#endif
