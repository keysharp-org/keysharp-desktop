#include "kwin_jobs.h"

#include <string.h>

/* The per-reply cap for a lane. The two differ on purpose: a fast batch drains
 * the structural ceiling in a few replies, while enumerations are pinned to
 * one per callback because two of them double the hole they punch in the
 * event stream to save a single message. */
static size_t lane_capacity(ksd_kwin_lane lane)
{
    if (lane == KSD_KWIN_LANE_FAST)
        return KSD_KWIN_JOBS_PER_REPLY_FAST;
    if (lane == KSD_KWIN_LANE_SLOW)
        return KSD_KWIN_JOBS_PER_REPLY_SLOW;
    return 0u;
}

static bool generation_valid(const char *generation)
{
    if (generation == NULL)
        return false;
    for (size_t index = 0u; index < KSD_KWIN_GENERATION_HEX; index++) {
        char digit = generation[index];
        bool hex = (digit >= '0' && digit <= '9')
            || (digit >= 'a' && digit <= 'f');

        if (!hex)
            return false;
    }
    return generation[KSD_KWIN_GENERATION_HEX] == '\0';
}

/* Sequences are fixed-width lowercase hex, the same spelling the envelope
 * parser accepts. Formatted here rather than with snprintf so there is one
 * definition of the width and no chance of a %llx that disagrees with it. */
static void format_sequence(uint64_t value,
                            char out[KSD_KWIN_SEQ_HEX + 1u])
{
    static const char digits[] = "0123456789abcdef";

    for (size_t index = 0u; index < KSD_KWIN_SEQ_HEX; index++) {
        unsigned shift = (unsigned)((KSD_KWIN_SEQ_HEX - 1u - index) * 4u);
        out[index] = digits[(value >> shift) & 0xfu];
    }
    out[KSD_KWIN_SEQ_HEX] = '\0';
}

bool ksd_kwin_queue_init(ksd_kwin_queue *queue, const char *generation)
{
    if (queue == NULL || !generation_valid(generation))
        return false;
    memset(queue, 0, sizeof(*queue));
    memcpy(queue->generation, generation, KSD_KWIN_GENERATION_HEX);
    queue->generation[KSD_KWIN_GENERATION_HEX] = '\0';
    /* Sequence zero is never issued, so a zeroed sequence field cannot be
     * mistaken for a real one by anything that reads the array directly. */
    queue->next_sequence = 1u;
    queue->next_ticket = 1u;
    return true;
}

bool ksd_kwin_queue_submit(ksd_kwin_queue *queue, uint16_t opcode,
                           uint64_t now_ms,
                           char sequence[KSD_KWIN_SEQ_HEX + 1u])
{
    ksd_kwin_lane lane;

    if (queue == NULL || sequence == NULL)
        return false;
    lane = ksd_kwin_lane_for(opcode);
    /* An opcode that takes no lane never reaches the script -- a capture runs
     * in the forked worker, a reservation is answered from a daemon table --
     * so queueing one would park a caller on work nothing will ever run. */
    if (lane != KSD_KWIN_LANE_FAST && lane != KSD_KWIN_LANE_SLOW)
        return false;
    for (size_t index = 0u; index < KSD_KWIN_MAX_JOBS; index++) {
        ksd_kwin_job *job = queue->jobs + index;

        if (job->state != KSD_KWIN_JOB_FREE)
            continue;
        format_sequence(queue->next_sequence, job->sequence);
        queue->next_sequence++;
        job->opcode = opcode;
        job->lane = lane;
        job->state = KSD_KWIN_JOB_QUEUED;
        job->deadline_ms = now_ms + KSD_KWIN_OP_DEADLINE_MS;
        job->ticket = queue->next_ticket++;
        job->status = 0u;
        memcpy(sequence, job->sequence, KSD_KWIN_SEQ_HEX + 1u);
        return true;
    }
    return false;
}

size_t ksd_kwin_queue_take(ksd_kwin_queue *queue, ksd_kwin_lane lane,
                           const ksd_kwin_job **batch, size_t capacity)
{
    size_t limit = lane_capacity(lane);
    size_t taken = 0u;

    if (queue == NULL || batch == NULL)
        return 0u;
    if (capacity < limit)
        limit = capacity;
    while (taken < limit) {
        ksd_kwin_job *oldest = NULL;

        /* Oldest queued job of this lane, by ticket. Scanning rather than
         * keeping a list keeps the array stable: a job completing in the
         * middle does not have to be compacted out of anything. */
        for (size_t index = 0u; index < KSD_KWIN_MAX_JOBS; index++) {
            ksd_kwin_job *job = queue->jobs + index;

            if (job->state != KSD_KWIN_JOB_QUEUED || job->lane != lane)
                continue;
            if (oldest == NULL || job->ticket < oldest->ticket)
                oldest = job;
        }
        if (oldest == NULL)
            break;
        oldest->state = KSD_KWIN_JOB_DISPATCHED;
        batch[taken] = oldest;
        taken++;
    }
    return taken;
}

bool ksd_kwin_queue_complete(ksd_kwin_queue *queue, const char *generation,
                             const char *sequence, uint32_t status)
{
    if (queue == NULL || generation == NULL || sequence == NULL)
        return false;
    if (!generation_valid(generation)
        || memcmp(generation, queue->generation, KSD_KWIN_GENERATION_HEX) != 0)
        return false;
    for (size_t index = 0u; index < KSD_KWIN_MAX_JOBS; index++) {
        ksd_kwin_job *job = queue->jobs + index;

        if (job->state == KSD_KWIN_JOB_FREE)
            continue;
        if (memcmp(job->sequence, sequence, KSD_KWIN_SEQ_HEX + 1u) != 0)
            continue;
        /* Only a dispatched job may complete. A queued one completing is the
         * script reporting work it was never handed, and a done one
         * completing again is a replay; both would hand a caller a result
         * that no run of the script produced. */
        if (job->state != KSD_KWIN_JOB_DISPATCHED)
            return false;
        job->state = KSD_KWIN_JOB_DONE;
        job->status = status;
        return true;
    }
    return false;
}

size_t ksd_kwin_queue_expire(ksd_kwin_queue *queue, uint64_t now_ms)
{
    size_t expired = 0u;

    if (queue == NULL)
        return 0u;
    for (size_t index = 0u; index < KSD_KWIN_MAX_JOBS; index++) {
        ksd_kwin_job *job = queue->jobs + index;

        /* Queued only. A dispatched job is inside a callback that cannot be
         * interrupted, so its outcome is unknown rather than known not to have
         * happened, and freeing it here would let a caller retry an operation
         * that may well have run. */
        if (job->state != KSD_KWIN_JOB_QUEUED || job->deadline_ms > now_ms)
            continue;
        memset(job, 0, sizeof(*job));
        job->state = KSD_KWIN_JOB_FREE;
        expired++;
    }
    return expired;
}

size_t ksd_kwin_queue_count(const ksd_kwin_queue *queue,
                            ksd_kwin_job_state state)
{
    size_t count = 0u;

    if (queue == NULL)
        return 0u;
    for (size_t index = 0u; index < KSD_KWIN_MAX_JOBS; index++) {
        if (queue->jobs[index].state == state)
            count++;
    }
    return count;
}
