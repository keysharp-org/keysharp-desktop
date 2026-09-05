#ifndef KEYSHARP_DESKTOP_X11_WATCH_H
#define KEYSHARP_DESKTOP_X11_WATCH_H

#include "x11_connect.h"
#include <stdint.h>

/* Takes over a dedicated worker socket until its consumer disconnects. */
bool ksd_x11_watch_run(ksd_x11 *connection, int stream_fd, uint64_t request_id);

#endif
