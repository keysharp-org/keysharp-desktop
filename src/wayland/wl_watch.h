#ifndef KEYSHARP_DESKTOP_WL_WATCH_H
#define KEYSHARP_DESKTOP_WL_WATCH_H

#include "wl_connect.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Takes over a dedicated worker socket and streams window events until its
 * consumer disconnects. Served where Hyprland IPC supplies the window facts
 * and an event socket; anything else answers the subscription UNAVAILABLE. */
bool ksd_wayland_watch_run(ksd_wayland *connection, int stream_fd,
                           uint64_t request_id);

/* One window as the last snapshot saw it. */
typedef struct ksd_watch_window {
    uint64_t id;
    char *title;
    bool minimized;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} ksd_watch_window;

/* Emits the events that turn the previous snapshot into the next, in the
 * order the X11 watch uses: per window create, title, minimize or restore and
 * move; then closes; then the old active window's deactivation and the new
 * one's activation. Exposed for tests. */
void ksd_wayland_watch_diff(const ksd_watch_window *previous,
                            size_t previous_count, uint64_t previous_active,
                            const ksd_watch_window *next, size_t next_count,
                            uint64_t next_active,
                            void (*emit)(void *context, uint16_t kind,
                                         uint64_t id),
                            void *context);

#endif
