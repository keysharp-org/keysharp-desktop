#ifndef KEYSHARP_DESKTOP_SESSION_ENVIRON_H
#define KEYSHARP_DESKTOP_SESSION_ENVIRON_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* Reads one variable out of the environment of the registered session daemon.
 *
 * The daemon is the party the authority authenticated and revalidates on every
 * operation, which is why its environment is the one entitled to name a
 * display. Taking that from the calling client would let a client point the
 * broker at a server or compositor it started itself.
 *
 * Called after privileges are dropped, so the open happens as the user and
 * root never touches a path a user named. Both display backends need exactly
 * this -- DISPLAY and XAUTHORITY on X11, WAYLAND_DISPLAY on Wayland -- and a
 * second copy of a rule this load-bearing is a second place for it to drift. */
bool ksd_session_environ_value(pid_t pid, const char *name, char *destination,
                               size_t capacity);

#endif
