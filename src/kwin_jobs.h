#ifndef KEYSHARP_DESKTOP_KWIN_JOBS_H
#define KEYSHARP_DESKTOP_KWIN_JOBS_H

#include "kwin_envelope.h"
#include "kwin_wire.h"
#include "protocol_io.h"
#include "worker_pool.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The daemon side of the KWin channel: what is waiting, what has been handed
 * to the script, and what came back.
 *
 * A script cannot be called, so the daemon never pushes work. It parks the
 * polls the script issues and answers one when there is something to run. This
 * queue is what a parked poll is answered from, and what a report is matched
 * against on the way back.
 *
 * One job per connection thread is the structural ceiling: a thread blocks on
 * the operation it submitted, so it cannot have two outstanding, and there are
 * KSD_MAX_AUTHORITY_WORKERS threads. A queue larger than that could only hold
 * jobs nobody is waiting for. */
#define KSD_KWIN_MAX_JOBS KSD_MAX_AUTHORITY_WORKERS

typedef enum ksd_kwin_job_state {
    KSD_KWIN_JOB_FREE,
    /* Submitted, not yet handed to the script. Only in this state may a job
     * be dropped for missing its deadline: nothing has run, so the caller can
     * be told BUSY and retry safely. */
    KSD_KWIN_JOB_QUEUED,
    /* Handed to the script inside a batch. A batch runs as one uninterruptible
     * callback, so from here the job cannot be dropped on its deadline: its
     * outcome is unknown rather than known-not-to-have-happened, and telling a
     * caller otherwise would make close and move-resize unsafe to retry. */
    KSD_KWIN_JOB_DISPATCHED,
    KSD_KWIN_JOB_DONE,
} ksd_kwin_job_state;

typedef enum ksd_kwin_poll_outcome {
    KSD_KWIN_POLL_ANSWERED,
    KSD_KWIN_POLL_PARKED,
    KSD_KWIN_POLL_REFUSED,
} ksd_kwin_poll_outcome;

typedef struct ksd_kwin_job {
    char sequence[KSD_KWIN_SEQ_HEX + 1u];
    uint16_t opcode;
    ksd_kwin_lane lane;
    ksd_kwin_job_state state;
    uint64_t deadline_ms;
    /* Submission order, so a lane is drained oldest first without having to
     * compact the array when a job in the middle completes. */
    uint64_t ticket;
    uint32_t status;
    /* The request body, owned by the queue and freed with the slot. A job is
     * dispatched long after the frame it came from is gone, so the bytes have
     * to be copied rather than pointed at. */
    uint8_t *body;
    uint32_t body_length;
    /* What the script sent back. Also owned here, and read once by whoever is
     * waiting on this sequence. */
    uint8_t *reply;
    uint32_t reply_length;
} ksd_kwin_job;

typedef struct ksd_kwin_queue {
    ksd_kwin_job jobs[KSD_KWIN_MAX_JOBS];
    /* The script generation these jobs belong to. A report carrying any other
     * generation is refused: after a script restart the sequences of the old
     * script mean nothing to the new one, and completing one against the other
     * would hand a caller a result from a job that no longer exists. */
    char generation[KSD_KWIN_GENERATION_HEX + 1u];
    uint64_t next_sequence;
    uint64_t next_ticket;
    bool parked[KSD_KWIN_LANES];
} ksd_kwin_queue;

/* generation must be exactly KSD_KWIN_GENERATION_HEX lowercase hex digits. */
bool ksd_kwin_queue_init(ksd_kwin_queue *queue, const char *generation);
ksd_kwin_queue *ksd_kwin_queue_create(const char *generation);
void ksd_kwin_queue_destroy(ksd_kwin_queue *queue);

bool ksd_kwin_queue_hello(ksd_kwin_queue *queue, ksd_buffer *reply);
ksd_kwin_poll_outcome ksd_kwin_queue_poll(ksd_kwin_queue *queue,
                                           const uint8_t *envelope,
                                           size_t length, uint64_t now_ms,
                                           ksd_buffer *reply);
bool ksd_kwin_queue_poll_parked(ksd_kwin_queue *queue, ksd_kwin_lane lane,
                                uint64_t now_ms, ksd_buffer *reply);
bool ksd_kwin_queue_report(ksd_kwin_queue *queue, const uint8_t *envelope,
                           size_t length, ksd_buffer *reply);
bool ksd_kwin_queue_parked(const ksd_kwin_queue *queue, ksd_kwin_lane lane);
bool ksd_kwin_queue_result(ksd_kwin_queue *queue, const char *sequence,
                           uint32_t *status, const uint8_t **body,
                           uint32_t *body_length);
const char *ksd_kwin_queue_generation(const ksd_kwin_queue *queue);

/* Submits one operation and writes its sequence. Returns false when the queue
 * is full or the opcode takes no lane, in which case nothing is stored. */
bool ksd_kwin_queue_submit(ksd_kwin_queue *queue, uint16_t opcode,
                           const uint8_t *body, uint32_t body_length,
                           uint64_t now_ms,
                           char sequence[KSD_KWIN_SEQ_HEX + 1u]);

/* Fills a reply batch for one lane, oldest first, and marks what it takes as
 * dispatched. Never returns more than that lane's per-reply cap, and never a
 * job belonging to the other lane. Returns how many it wrote. */
size_t ksd_kwin_queue_take(ksd_kwin_queue *queue, ksd_kwin_lane lane,
                           const ksd_kwin_job **batch, size_t capacity);

/* Matches one entry of a report. Refuses, without changing anything:
 *   - a generation other than the queue's,
 *   - a sequence that names no job,
 *   - a job that was never dispatched, which is a script claiming to have run
 *     work it was never given,
 *   - a job already completed, which is a replay. */
bool ksd_kwin_queue_complete(ksd_kwin_queue *queue, const char *generation,
                             const char *sequence, uint32_t status,
                             const uint8_t *reply, uint32_t reply_length);

/* The job a sequence names, or NULL. Used by whoever is waiting on it to read
 * the result once the script has reported. */
const ksd_kwin_job *ksd_kwin_queue_find(const ksd_kwin_queue *queue,
                                        const char *sequence);

/* Releases a completed job's slot. Separate from completing it because the
 * thread that submitted the job is not the one that received the report, and
 * the result has to survive until the submitter has read it. */
void ksd_kwin_queue_release(ksd_kwin_queue *queue, const char *sequence);

/* Drops queued jobs whose deadline has passed and reports how many. Dispatched
 * jobs are deliberately untouched; see KSD_KWIN_JOB_DISPATCHED. */
size_t ksd_kwin_queue_expire(ksd_kwin_queue *queue, uint64_t now_ms);

/* How many jobs are in a given state, which is what the callers and the gates
 * ask about rather than reaching into the array. */
size_t ksd_kwin_queue_count(const ksd_kwin_queue *queue,
                            ksd_kwin_job_state state);

#endif
