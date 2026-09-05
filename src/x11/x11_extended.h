#ifndef KEYSHARP_DESKTOP_X11_EXTENDED_H
#define KEYSHARP_DESKTOP_X11_EXTENDED_H

#include "x11_connect.h"
#include "operation_result.h"

void ksd_x11_window_query(ksd_x11 *, uint32_t, ksd_operation_result *);
void ksd_x11_window_children(ksd_x11 *, uint32_t, ksd_operation_result *);
void ksd_x11_window_at_point(ksd_x11 *, int32_t, int32_t, bool, ksd_operation_result *);
void ksd_x11_display_list(ksd_x11 *, ksd_operation_result *);
void ksd_x11_window_set_title(ksd_x11 *, uint32_t, const uint8_t *, uint32_t, ksd_operation_result *);
void ksd_x11_window_set_visible(ksd_x11 *, uint32_t, bool, ksd_operation_result *);
void ksd_x11_window_redraw(ksd_x11 *, uint32_t, ksd_operation_result *);
void ksd_x11_window_click(ksd_x11 *, uint32_t, int32_t, int32_t, uint32_t, uint32_t, ksd_operation_result *);
void ksd_x11_mouse_move_absolute(ksd_x11 *, int32_t, int32_t, ksd_operation_result *);

void ksd_x11_window_button(ksd_x11 *, uint32_t, int32_t, int32_t, uint32_t, bool, ksd_operation_result *);
void ksd_x11_window_focus_child(ksd_x11 *, uint32_t, ksd_operation_result *);
void ksd_x11_keyboard_clear(ksd_x11 *);
void ksd_x11_keyboard_state_since(ksd_x11 *, const uint8_t *, uint32_t, ksd_operation_result *);
#endif
