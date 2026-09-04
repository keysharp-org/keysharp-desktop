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

/* Several variables out of ONE read of the same file.
 *
 * The single-value form opens and reads /proc/<pid>/environ per variable, and
 * X11 needs two of them, so it read a 64 KiB file twice per request. Worse
 * than the cost, the two reads can see different processes: nothing stops the
 * daemon exiting and its pid being reused between them, and the display would
 * then come from one process and its authority file from another.
 *
 * names and destinations are parallel arrays of count entries. Each
 * destination holds at most capacity bytes. A name that is absent leaves its
 * destination an empty string; the return value is whether the file could be
 * read at all, not whether every name was found. */
bool ksd_session_environ_values(pid_t pid, const char *const *names,
                                char **destinations, size_t capacity,
                                size_t count);

#endif
