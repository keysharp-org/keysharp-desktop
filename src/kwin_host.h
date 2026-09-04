#ifndef KEYSHARP_DESKTOP_KWIN_HOST_H
#define KEYSHARP_DESKTOP_KWIN_HOST_H

#include "kwin_envelope.h"
#include "kwin_jobs.h"
#include "kwin_wire.h"
#include "protocol_io.h"

#include <stdbool.h>
#include <stdint.h>

/* The daemon end of the KWin channel.
 *
 * Deliberately free of GLib and of any I/O. Everything here is a decision --
 * what to answer a poll with, whether a report may be believed, when a parked
 * invocation must be completed -- and every one of those is a rule worth
 * testing without standing up a bus. The D-Bus surface on top of this is meant
 * to be thin enough to have nothing left to get wrong.
 *
 * The inversion is the thing to keep in mind while reading it: a KWin script
 * cannot be called, so this never pushes work. It holds the script's own
 * request until it has something to send back. */

typedef enum ksd_kwin_poll_outcome {
    /* Work is waiting: the reply carries a batch and must be sent at once. */
    KSD_KWIN_POLL_ANSWERED,
    /* Nothing to do. The caller parks the invocation and answers it later,
     * either when work arrives or when the idle timer fires. A parked poll is
     * always completed eventually and never dropped: the script has exactly
     * this one request outstanding per lane, and dropping it ends the lane. */
    KSD_KWIN_POLL_PARKED,
    /* The envelope was not understood, or came from another generation. */
    KSD_KWIN_POLL_REFUSED,
} ksd_kwin_poll_outcome;

typedef struct ksd_kwin_host ksd_kwin_host;

/* generation must be 32 lowercase hex digits, and identifies this run of the
 * script. It is issued by the daemon rather than chosen by the script, because
 * a script that named its own run could name the previous one. */
ksd_kwin_host *ksd_kwin_host_create(const char *generation);
void ksd_kwin_host_destroy(ksd_kwin_host *host);

/* The reply to Hello: which generation the script is now part of. */
bool ksd_kwin_host_hello(ksd_kwin_host *host, ksd_buffer *reply);

/* Answers or parks one poll. On ANSWERED the reply holds a batch for that
 * lane and the jobs in it are dispatched; on PARKED nothing has changed. */
ksd_kwin_poll_outcome ksd_kwin_host_poll(ksd_kwin_host *host,
                                         const uint8_t *envelope,
                                         size_t length, uint64_t now_ms,
                                         ksd_buffer *reply);

/* Matches a report against dispatched jobs. Refuses the whole report if any
 * record in it is one this daemon did not dispatch, rather than believing the
 * part it recognises: a report is one message from one script, and a mixture
 * of real and invented records is not a partially valid report. */
bool ksd_kwin_host_report(ksd_kwin_host *host, const uint8_t *envelope,
                          size_t length, ksd_buffer *reply);

/* Queues one operation for the script. Returns false when the queue is full or
 * the opcode never reaches the script, which the caller answers with BUSY --
 * a refusal that provably never reached the compositor and is safe to retry. */
bool ksd_kwin_host_submit(ksd_kwin_host *host, uint16_t opcode,
                          const uint8_t *body, uint32_t body_length,
                          uint64_t now_ms,
                          char sequence[KSD_KWIN_SEQ_HEX + 1u]);

/* Whether a lane has a poll parked, so the caller knows there is an
 * invocation to complete when work arrives. */
bool ksd_kwin_host_parked(const ksd_kwin_host *host, ksd_kwin_lane lane);

/* The result for a sequence, once the script has reported it. Returns false
 * while the job is still queued or dispatched. */
bool ksd_kwin_host_result(ksd_kwin_host *host, const char *sequence,
                          uint32_t *status, const uint8_t **body,
                          uint32_t *body_length);
void ksd_kwin_host_release(ksd_kwin_host *host, const char *sequence);

/* The generation this host issued. The transport needs it to write an idle
 * reply, which carries no jobs but must still name the run it belongs to. */
const char *ksd_kwin_host_generation(const ksd_kwin_host *host);

/* Drops queued work whose deadline has passed, and reports how many. */
size_t ksd_kwin_host_expire(ksd_kwin_host *host, uint64_t now_ms);

#endif
