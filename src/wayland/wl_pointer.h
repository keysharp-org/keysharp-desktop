#ifndef KEYSHARP_DESKTOP_WL_POINTER_H
#define KEYSHARP_DESKTOP_WL_POINTER_H

#include "operation_result.h"
#include "wl_connect.h"

#include <stdint.h>

void ksd_wayland_pointer_create(ksd_wayland *connection);
void ksd_wayland_pointer_clear(ksd_wayland *connection);
void ksd_wayland_move_absolute(ksd_wayland *connection, int32_t x, int32_t y,
                               ksd_operation_result *result);

#endif
