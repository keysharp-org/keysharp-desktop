#ifndef KEYSHARP_DESKTOP_WL_KEYBOARD_H
#define KEYSHARP_DESKTOP_WL_KEYBOARD_H

#include "wl_connect.h"
#include "operation_result.h"

void ksd_wayland_keyboard_attach(ksd_wayland *connection);
void ksd_wayland_keyboard_clear(ksd_wayland *connection);
void ksd_wayland_keyboard_state(ksd_wayland *connection,
                                ksd_operation_result *result);
void ksd_wayland_keyboard_state_since(ksd_wayland *connection,
    const uint8_t *revision, uint32_t length, ksd_operation_result *result);

#endif
