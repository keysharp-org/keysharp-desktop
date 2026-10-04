#ifndef KEYSHARP_DESKTOP_WL_WATCH_H
#define KEYSHARP_DESKTOP_WL_WATCH_H

#include "wl_connect.h"
#include "keysharp_desktop/client.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Streams the compositor's available window state until the consumer disconnects. */
bool ksd_wayland_watch_run(ksd_wayland *connection, int stream_fd,
                           uint64_t request_id);

/* Emits full-record changes before removals. Exposed for tests. */
void ksd_wayland_watch_diff(const ksd_window_record *previous,
                            size_t previous_count,
                            const ksd_window_record *next, size_t next_count,
                            void (*emit)(void *context, uint16_t kind,
                                         uint64_t id),
                            void *context);

#endif
