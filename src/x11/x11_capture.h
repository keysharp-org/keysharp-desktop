#ifndef KEYSHARP_DESKTOP_X11_CAPTURE_H
#define KEYSHARP_DESKTOP_X11_CAPTURE_H

#include "operation_result.h"
#include "x11_connect.h"

#include <stdbool.h>
#include <stdint.h>

/* Both capture verbs answer with a sealed memfd on result->payload_fd rather
 * than with a byte tail. The design note in the plan that said X11 capture had
 * to keep its bytes on the public wire predates the descriptor transport and
 * does not apply: the pixels are never copied into a response buffer, and on
 * the fast path the X server writes them straight into the descriptor that
 * gets sealed and handed on. */
void ksd_x11_capture_area(ksd_x11 *connection, int32_t x, int32_t y,
                          uint32_t width, uint32_t height,
                          ksd_operation_result *result);
/* include_decoration selects the frame the window manager reparented the
 * client into rather than the client window itself. A client whose frame is
 * its own window answers the same either way. */
void ksd_x11_capture_window(ksd_x11 *connection, uint32_t window,
                            bool include_decoration,
                            ksd_operation_result *result);

#endif
