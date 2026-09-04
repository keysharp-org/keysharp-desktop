#ifndef KEYSHARP_DESKTOP_X11_CONTROL_H
#define KEYSHARP_DESKTOP_X11_CONTROL_H

#include "operation_result.h"
#include "x11_connect.h"

#include <stdbool.h>
#include <stdint.h>

/* The window control verbs, as EWMH asks for them.
 *
 * Almost all of these are requests to the window manager rather than
 * operations on the server: a client does not move or focus a managed window
 * itself, it asks the manager to, by sending a client message to the root
 * window. That has a consequence worth stating plainly rather than discovering
 * later -- with no window manager running, these calls are correctly formed
 * and nothing happens, because there is nobody listening. The two exceptions
 * are the ones that really are server operations: killing a client, and
 * setting a property.
 *
 * Each fills result the way the compositor providers do for the same opcode:
 * OK with an empty tail when applied, NOT_FOUND when the window is gone. */
void ksd_x11_window_focus(ksd_x11 *connection, uint32_t window,
                          ksd_operation_result *result);
void ksd_x11_window_raise(ksd_x11 *connection, uint32_t window, bool raise,
                          ksd_operation_result *result);
void ksd_x11_window_close(ksd_x11 *connection, uint32_t window,
                          ksd_operation_result *result);
void ksd_x11_window_kill(ksd_x11 *connection, uint32_t window,
                         ksd_operation_result *result);
void ksd_x11_window_move_resize(ksd_x11 *connection, uint32_t window,
                                int32_t x, int32_t y, uint32_t width,
                                uint32_t height,
                                ksd_operation_result *result);
/* 0 restores, 1 minimizes, 2 maximizes. */
void ksd_x11_window_set_state(ksd_x11 *connection, uint32_t window,
                              uint32_t state, ksd_operation_result *result);
/* 0 to 255, the scale every backend reports and the consumer parses. */
void ksd_x11_window_set_opacity(ksd_x11 *connection, uint32_t window,
                                uint32_t opacity,
                                ksd_operation_result *result);
void ksd_x11_window_set_above(ksd_x11 *connection, uint32_t window,
                              bool above, ksd_operation_result *result);
void ksd_x11_window_set_decorated(ksd_x11 *connection, uint32_t window,
                                  bool decorated,
                                  ksd_operation_result *result);

#endif
