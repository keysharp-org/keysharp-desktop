#ifndef KEYSHARP_DESKTOP_WL_DISPLAYS_H
#define KEYSHARP_DESKTOP_WL_DISPLAYS_H

#include "operation_result.h"
#include "wl_connect.h"

#include <stdbool.h>
#include <stdint.h>

/* Display topology in the logical coordinates every generic operation uses.
 * xdg-output supplies the geometry, since wl_output alone reports neither a
 * position nor a fractional scale on compositors such as Hyprland. The work
 * area needs a compositor channel that reports reserved edges, which today is
 * Hyprland IPC. */
bool ksd_wayland_display_list_available(const ksd_wayland *connection);
bool ksd_wayland_work_area_available(const ksd_wayland *connection);
void ksd_wayland_display_list(ksd_wayland *connection,
                              ksd_operation_result *result);
void ksd_wayland_work_area(ksd_wayland *connection,
                           ksd_operation_result *result);

/* The bounds minus reserved left, top, right and bottom edges, into out as
 * x, y, width and height. False when nothing is left. Exposed for tests. */
bool ksd_wayland_reserved_work_area(int32_t x, int32_t y, int32_t width,
                                    int32_t height, const int32_t reserved[4],
                                    int32_t out[4]);

#endif
