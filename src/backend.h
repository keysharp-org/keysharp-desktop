#ifndef KEYSHARP_DESKTOP_BACKEND_H
#define KEYSHARP_DESKTOP_BACKEND_H

#include "keysharp_desktop/client.h"

#include <stdint.h>
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
/* The registration acknowledgement, written once and read once, so the two
 * ends cannot drift on where the accepted mask lives. It sits at the same
 * offset the request carries its own mask: the offset itself means "mask".
 *
 * Telling the daemon what was actually stored closes a gap the withhold-only
 * rule leaves open. The authority may narrow what a daemon asked to advertise,
 * and without this the daemon would carry on believing it serves the wider
 * set -- answering for operations the authority will refuse on its behalf, and
 * with no way to notice. */
void ksd_backend_ack_encode(uint8_t *reply, uint16_t status, uint32_t backend,
                            uint64_t accepted);
/* Checks an acknowledgement against what was asked for. Refuses a mask wider
 * than the request: withhold-only is a rule the daemon can enforce too, and a
 * daemon that accepted a widened mask would advertise a capability it never
 * claimed on the say-so of the other end. */
bool ksd_backend_ack_parse(const uint8_t *reply, uint32_t expected_backend,
                           uint64_t requested, uint64_t *accepted);

/* The mask to store for a registration, given what the daemon asked to
 * advertise. Withhold-only: the result is always a subset of what the backend
 * statically supports. Returns false when the record itself is unacceptable,
 * which rejects the registration rather than silently clamping it. */
bool ksd_backend_registration_mask(ksd_backend backend, uint16_t version,
                                   uint16_t flags, uint64_t requested,
                                   uint64_t *stored);
/* What to report for a backend, given whether a registration was found and
 * what it advertised. A registration narrows; its absence falls back to the
 * static table. Narrowed again on the way out, because staying a subset is the
 * invariant and asserting it costs nothing. */
uint64_t ksd_backend_reported_operations(ksd_backend backend, bool registered,
                                         uint64_t advertised);

#endif
