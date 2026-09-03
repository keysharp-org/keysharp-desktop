#ifndef KEYSHARP_DESKTOP_BACKEND_H
#define KEYSHARP_DESKTOP_BACKEND_H

#include "keysharp_desktop/client.h"

#include <sys/types.h>

ksd_backend ksd_backend_resolve(void);
ksd_backend ksd_backend_resolve_process(pid_t pid);
/* Whether the process sits in a real X11 session. DISPLAY is deliberately not
 * consulted: a Wayland session almost always has one, because XWayland sets
 * it, so a rule that looked at DISPLAY would call nearly every Wayland session
 * X11. The session type must say x11 as a whole value and no Wayland display
 * may be present. */
bool ksd_session_is_x11_process(pid_t pid);
bool ksd_backend_session_unsupported(void);
uint64_t ksd_backend_operations(ksd_backend backend);
/* Which operations the X11 worker serves for a backend in a session type.
 * Never more than that backend advertises, and always zero off an X11 session:
 * "nothing changes on Wayland" is a property this function makes checkable
 * rather than a promise made in prose. */
uint64_t ksd_backend_x11_route(ksd_backend backend, bool x11_session);

#endif
