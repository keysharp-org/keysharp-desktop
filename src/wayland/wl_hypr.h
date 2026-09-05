#ifndef KEYSHARP_DESKTOP_WL_HYPR_H
#define KEYSHARP_DESKTOP_WL_HYPR_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

bool ksd_wayland_hypr_available(pid_t session_pid);
bool ksd_wayland_hypr_cursor(pid_t session_pid, int32_t *x, int32_t *y);
bool ksd_wayland_hypr_move(pid_t session_pid, int32_t x, int32_t y);

#endif
