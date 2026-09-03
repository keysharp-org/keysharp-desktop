#ifndef KEYSHARP_DESKTOP_X11_QUERY_H
#define KEYSHARP_DESKTOP_X11_QUERY_H

#include "operation_result.h"
#include "x11_connect.h"

#include <stdint.h>

/* The coordinate group. Each fills result with exactly the payload the
 * compositor providers produce for the same opcode, so a client cannot tell
 * which backend answered except by reading the backend value. */
void ksd_x11_cursor_position(ksd_x11 *connection, ksd_operation_result *result);
void ksd_x11_work_area(ksd_x11 *connection, ksd_operation_result *result);
void ksd_x11_window_list(ksd_x11 *connection, bool include_hidden,
                         ksd_operation_result *result);
void ksd_x11_window_active(ksd_x11 *connection, ksd_operation_result *result);

#endif
