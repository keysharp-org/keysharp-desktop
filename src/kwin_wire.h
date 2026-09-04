#ifndef KEYSHARP_DESKTOP_KWIN_WIRE_H
#define KEYSHARP_DESKTOP_KWIN_WIRE_H

#include "protocol.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

/* A KWin script cannot be called. Its only outbound path is callDBus, so the
 * daemon parks a poll the script issues and holds the reply until there is
 * work. That gives one channel, and one channel serialises consumers: a cheap
 * query waits behind an enumeration that has nothing to do with it.
 *
 * Two lanes fix the ordering. Work inside KWin is still strictly serial,
 * because a script runs on the compositor main thread and nothing here changes
 * that; what the split buys is that a trivial query is never behind a *queue*
 * of enumerations, only ever behind at most one executing job. */
typedef enum ksd_kwin_lane {
    KSD_KWIN_LANE_FAST,
    KSD_KWIN_LANE_SLOW,
    /* Served without a script round trip at all: reservations from a daemon
     * table, kill(2) from the daemon, captures from the forked worker. */
    KSD_KWIN_LANE_NONE,
} ksd_kwin_lane;

/* Whether the work a verb causes inside the script is bounded by a constant or
 * by the number of windows. The fast lane admits only bounded verbs, and that
 * is the property that stops this design eroding: without it, someone adds an
 * enumerating verb to the fast lane later and the lane split silently stops
 * meaning anything. */
typedef enum ksd_kwin_cost {
    KSD_KWIN_COST_BOUNDED,
    KSD_KWIN_COST_UNBOUNDED,
    KSD_KWIN_COST_NONE,
} ksd_kwin_cost;

/* Whether a message arriving on the provider connection came from the script
 * this daemon started, in the run it is currently talking to.
 *
 * The connection is a private socket the session daemon handed over, not the
 * session bus, so this is not a general authorization question: exactly one
 * peer is ever legitimate. Three things must agree, and each rules out a
 * different way of being wrong rather than being belt and braces.
 *
 * The unique name pins WHICH connection. A unique bus name is never reused
 * within a bus lifetime, so a reconnecting script gets a new one and cannot
 * inherit the standing of the connection it replaced.
 *
 * The uid pins WHOSE. A script runs as the session user; anything arriving
 * under another uid is not the compositor of this session whatever it says.
 *
 * The generation pins WHEN. A script that was unloaded and restarted is a new
 * run with its own numbering, and a reply from the previous run names
 * sequences that no longer mean anything -- the case the job queue refuses
 * per-job, refused here for the whole peer.
 *
 * Generations must be well formed, not merely equal: two malformed strings
 * that happen to match would otherwise pass, and "equal" is not the property
 * being checked, "is the generation I issued" is. */
bool ksd_kwin_peer_allowed(const char *expected_unique,
                           const char *sender_unique,
                           uid_t expected_uid, uid_t sender_uid,
                           const char *expected_generation,
                           const char *sender_generation);

ksd_kwin_lane ksd_kwin_lane_for(uint16_t opcode);
ksd_kwin_cost ksd_kwin_script_cost(uint16_t opcode);

#define KSD_KWIN_BUS_PROBE_MS 500u
#define KSD_KWIN_SCRIPTING_PROBE_MS 500u
#define KSD_KWIN_HELLO_DEADLINE_MS 4000u
/* The only KWin timeout. Two names for one bound, with nothing tying them
 * together, is how they drift apart. */
#define KSD_KWIN_OP_DEADLINE_MS 2000u
#define KSD_KWIN_IDLE_REPLY_MS 8000u
/* The two fast polls idle on offset timers, so an idle desktop does not
 * produce a two-fold poll storm every eight seconds. */
#define KSD_KWIN_IDLE_STAGGER_MS 1500u
#define KSD_KWIN_POLL_WATCHDOG_MS 13000u
#define KSD_KWIN_POLL_GRACE_MS 1000u
#define KSD_KWIN_LIVENESS_MS 20000u
#define KSD_KWIN_SUSPECT_MS 2000u
#define KSD_KWIN_SCRIPT_RESTART_FLOOR_MS 30000u
#define KSD_KWIN_SCRIPT_RESTART_BUDGET 8u
#define KSD_KWIN_LANES 2u
/* Two, not one. With re-park-before-execute one is nominally enough, but the
 * parked poll's round trip and the batch it overlaps are the same order, so at
 * one a batch that finishes first pays a full round trip -- which is the
 * per-job round trip the pipeline exists to remove. */
#define KSD_KWIN_PARKED_POLLS_FAST 2u
#define KSD_KWIN_PARKED_POLLS_SLOW 1u
/* Eight drains the structural ceiling of 31 in four replies. Not larger: a
 * batch runs as one uninterruptible callback, and a dispatched job can no
 * longer be dropped when its deadline passes. */
#define KSD_KWIN_JOBS_PER_REPLY_FAST 8u
/* Pinned to one. Two enumerations in a single callback double the hole in the
 * event stream to save one message, which is not a trade worth making. */
#define KSD_KWIN_JOBS_PER_REPLY_SLOW 1u
#define KSD_KWIN_MAX_DONE_PER_REPORT 9u
#define KSD_KWIN_MAX_OUTSTANDING_CALLS 5u
#define KSD_KWIN_EVENTS_PER_SECOND 128u
#define KSD_KWIN_MAX_EVENTS_PER_POLL 64u
#define KSD_KWIN_MAX_EVENT_BYTES 65536u
#define KSD_KWIN_MAX_HEADER_BYTES 8192u
#define KSD_KWIN_HANDLE_CACHE_ENTRIES 4096u

/* A job must be able to fail on its own deadline before the authority gives up
 * on the provider, or the caller is told the provider is gone when in fact one
 * operation was slow. */
_Static_assert(KSD_KWIN_OP_DEADLINE_MS < 5000u,
               "a job deadline must expire before the provider timeout");
_Static_assert(KSD_KWIN_IDLE_REPLY_MS < KSD_KWIN_POLL_WATCHDOG_MS,
               "an idle reply must arrive before the watchdog fires");
_Static_assert(KSD_KWIN_POLL_WATCHDOG_MS < 15000u,
               "the watchdog must fire before the callDBus reply ceiling");
_Static_assert(KSD_KWIN_IDLE_STAGGER_MS < KSD_KWIN_IDLE_REPLY_MS,
               "a stagger longer than the idle reply would reorder the lanes");
_Static_assert(KSD_KWIN_PARKED_POLLS_FAST >= 2u,
               "one parked fast poll reintroduces a round trip per batch");
_Static_assert(KSD_KWIN_JOBS_PER_REPLY_SLOW == 1u,
               "batching enumerations doubles the hole in the event stream");
_Static_assert(KSD_KWIN_MAX_DONE_PER_REPORT
                   == KSD_KWIN_JOBS_PER_REPLY_FAST
                      + KSD_KWIN_JOBS_PER_REPLY_SLOW,
               "a report must be able to carry both lanes at full batch");
_Static_assert(KSD_KWIN_MAX_OUTSTANDING_CALLS
                   == KSD_KWIN_PARKED_POLLS_FAST
                      + KSD_KWIN_PARKED_POLLS_SLOW + 2u,
               "outstanding calls are the parked polls plus Report and Event");

#endif
