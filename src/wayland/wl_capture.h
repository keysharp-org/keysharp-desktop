#ifndef KEYSHARP_DESKTOP_WL_CAPTURE_H
#define KEYSHARP_DESKTOP_WL_CAPTURE_H

#include "operation_result.h"
#include "wl_connect.h"

#include <stdint.h>

void ksd_wayland_capture_area(ksd_wayland *connection, int32_t x, int32_t y,
                              uint32_t width, uint32_t height,
                              ksd_operation_result *result);

#endif
