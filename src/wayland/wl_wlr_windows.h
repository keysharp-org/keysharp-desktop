#ifndef KEYSHARP_DESKTOP_WL_WLR_WINDOWS_H
#define KEYSHARP_DESKTOP_WL_WLR_WINDOWS_H

#include "operation_result.h"
#include "wl_connect.h"

void ksd_wayland_wlr_window_action(ksd_wayland *connection, uint16_t opcode,
                                   uint64_t handle, uint32_t value,
                                   ksd_operation_result *result);

#endif
