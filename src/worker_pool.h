#ifndef KEYSHARP_DESKTOP_WORKER_POOL_H
#define KEYSHARP_DESKTOP_WORKER_POOL_H

#include <stdbool.h>
#include <stddef.h>

/* The authority runs one thread per connection, and a connection's kind is not
 * known when it is accepted: the magic that distinguishes a session daemon
 * registering from an application making calls is only peeked afterwards. So
 * every connection is admitted probationally and classified later.
 *
 * That ordering creates the problem these two functions exist to solve. A
 * uid's applications can fill the pool with ordinary RPC connections, and the
 * session daemon of that same desktop then cannot register -- which takes the
 * whole backend down for every consumer, including the ones that were not
 * flooding. A few slots are therefore held back for registrations only.
 *
 * The per-uid cap is deliberately not applied to a registration. Every
 * consumer of one desktop shares a uid, so a uid at its RPC limit is the
 * ordinary busy case, not an abusive one, and refusing its daemon a
 * registration would punish the desktop for being in use. */

#define KSD_MAX_AUTHORITY_WORKERS 128u
#define KSD_MAX_AUTHORITY_WORKERS_PER_UID 32u
/* Enough for several desktops to register at once while a pool that size is
 * saturated, and small enough that holding them back cannot itself exhaust the
 * general pool. */
#define KSD_AUTHORITY_REGISTRATION_RESERVE 8u

/* Whether to admit a connection at accept time, before its kind is known.
 * from_reserve reports that the slot came out of the registration reserve, in
 * which case only a registration may keep it. */
bool ksd_authority_admit_worker(size_t workers, size_t uid_workers,
                                bool *from_reserve);

/* Whether a connection that has now been classified may keep the slot it was
 * admitted into. A reserved slot serving an ordinary connection is exactly the
 * starvation the reserve exists to prevent, so it is refused and the
 * connection is closed before it can say anything. */
bool ksd_authority_worker_keeps_slot(bool from_reserve, bool is_registration);

#endif
