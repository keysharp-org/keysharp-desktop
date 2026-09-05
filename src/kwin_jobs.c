#include "kwin_jobs.h"

#include <stdlib.h>
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

static size_t lane_index(ksd_kwin_lane lane)
{
    return lane == KSD_KWIN_LANE_FAST ? 0u : 1u;
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

/* Frees what a slot owns and returns it to the free list. */
static void release_slot(ksd_kwin_job *job)
{
    free(job->body);
    free(job->reply);
    memset(job, 0, sizeof(*job));
    job->state = KSD_KWIN_JOB_FREE;
}

ksd_kwin_queue *ksd_kwin_queue_create(const char *generation)
{
    ksd_kwin_queue *queue = malloc(sizeof(*queue));

    if (queue == NULL)
        return NULL;
    if (!ksd_kwin_queue_init(queue, generation)) {
        free(queue);
        return NULL;
    }
    return queue;
}

void ksd_kwin_queue_destroy(ksd_kwin_queue *queue)
{
    if (queue == NULL)
        return;
    for (size_t index = 0u; index < KSD_KWIN_MAX_JOBS; index++) {
        if (queue->jobs[index].state != KSD_KWIN_JOB_FREE)
            release_slot(queue->jobs + index);
    }
    free(queue);
}

bool ksd_kwin_queue_submit(ksd_kwin_queue *queue, uint16_t opcode,
                           const uint8_t *body, uint32_t body_length,
                           uint64_t now_ms,
                           char sequence[KSD_KWIN_SEQ_HEX + 1u])
{
    ksd_kwin_lane lane;

    if (queue == NULL || sequence == NULL)
        return false;
    if (body_length != 0u && body == NULL)
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
        job->reply = NULL;
        job->reply_length = 0u;
        job->body = NULL;
        job->body_length = 0u;
        if (body_length != 0u) {
            /* Copied, not referenced: the frame this came from is gone long
             * before the job is dispatched. */
            job->body = malloc(body_length);
            if (job->body == NULL) {
                release_slot(job);
                return false;
            }
            memcpy(job->body, body, body_length);
            job->body_length = body_length;
        }
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

static size_t dispatch_jobs(ksd_kwin_queue *queue, ksd_kwin_lane lane,
                            uint64_t now_ms, ksd_kwin_dispatch *dispatch)
{
    const ksd_kwin_job *batch[KSD_KWIN_JOBS_PER_REPLY_FAST];
    size_t taken = ksd_kwin_queue_take(queue, lane, batch,
                                       KSD_KWIN_JOBS_PER_REPLY_FAST);

    for (size_t index = 0u; index < taken; index++) {
        dispatch[index].sequence = batch[index]->sequence;
        dispatch[index].opcode = batch[index]->opcode;
        dispatch[index].budget_ms = batch[index]->deadline_ms > now_ms
            ? (uint32_t)(batch[index]->deadline_ms - now_ms) : 0u;
        dispatch[index].body = batch[index]->body;
        dispatch[index].body_length = batch[index]->body_length;
    }
    return taken;
}

bool ksd_kwin_queue_hello(ksd_kwin_queue *queue, ksd_buffer *reply)
{
    return queue != NULL && reply != NULL
        && ksd_kwin_format_report_ack(queue->generation, reply);
}

ksd_kwin_poll_outcome ksd_kwin_queue_poll(ksd_kwin_queue *queue,
                                           const uint8_t *envelope,
                                           size_t length, uint64_t now_ms,
                                           ksd_buffer *reply)
{
    ksd_kwin_poll poll;
    ksd_kwin_dispatch dispatch[KSD_KWIN_JOBS_PER_REPLY_FAST];
    size_t taken;

    if (queue == NULL || reply == NULL
        || !ksd_kwin_parse_poll(envelope, length, &poll)
        || memcmp(poll.generation, queue->generation,
                  KSD_KWIN_GENERATION_HEX) != 0)
        return KSD_KWIN_POLL_REFUSED;
    taken = dispatch_jobs(queue, poll.lane, now_ms, dispatch);
    if (taken == 0u) {
        queue->parked[lane_index(poll.lane)] = true;
        return KSD_KWIN_POLL_PARKED;
    }
    queue->parked[lane_index(poll.lane)] = false;
    if (!ksd_kwin_format_poll_reply(queue->generation, poll.lane, 0u,
                                    dispatch, taken, reply))
        return KSD_KWIN_POLL_REFUSED;
    return KSD_KWIN_POLL_ANSWERED;
}

bool ksd_kwin_queue_poll_parked(ksd_kwin_queue *queue, ksd_kwin_lane lane,
                                uint64_t now_ms, ksd_buffer *reply)
{
    ksd_kwin_dispatch dispatch[KSD_KWIN_JOBS_PER_REPLY_FAST];
    size_t taken;

    if (queue == NULL || reply == NULL
        || (lane != KSD_KWIN_LANE_FAST && lane != KSD_KWIN_LANE_SLOW))
        return false;
    queue->parked[lane_index(lane)] = false;
    taken = dispatch_jobs(queue, lane, now_ms, dispatch);
    return ksd_kwin_format_poll_reply(
        queue->generation, lane, taken == 0u ? KSD_KWIN_IDLE_REPLY_MS : 0u,
        taken == 0u ? NULL : dispatch, taken, reply);
}

bool ksd_kwin_queue_report(ksd_kwin_queue *queue, const uint8_t *envelope,
                           size_t length, ksd_buffer *reply)
{
    ksd_kwin_report report;

    if (queue == NULL || reply == NULL
        || !ksd_kwin_parse_report(envelope, length, &report)
        || memcmp(report.generation, queue->generation,
                  KSD_KWIN_GENERATION_HEX) != 0)
        return false;
    for (size_t index = 0u; index < report.count; index++) {
        const ksd_kwin_job *job =
            ksd_kwin_queue_find(queue, report.done[index].sequence);

        if (job == NULL || job->state != KSD_KWIN_JOB_DISPATCHED)
            return false;
    }
    for (size_t index = 0u; index < report.count; index++) {
        if (!ksd_kwin_queue_complete(queue, queue->generation,
                                     report.done[index].sequence,
                                     report.done[index].status,
                                     report.done[index].body,
                                     report.done[index].body_length))
            return false;
    }
    return ksd_kwin_format_report_ack(queue->generation, reply);
}

bool ksd_kwin_queue_parked(const ksd_kwin_queue *queue, ksd_kwin_lane lane)
{
    if (queue == NULL || (lane != KSD_KWIN_LANE_FAST
                          && lane != KSD_KWIN_LANE_SLOW))
        return false;
    return queue->parked[lane_index(lane)];
}

bool ksd_kwin_queue_result(ksd_kwin_queue *queue, const char *sequence,
                           uint32_t *status, const uint8_t **body,
                           uint32_t *body_length)
{
    const ksd_kwin_job *job;

    if (queue == NULL || status == NULL || body == NULL
        || body_length == NULL)
        return false;
    job = ksd_kwin_queue_find(queue, sequence);
    if (job == NULL || job->state != KSD_KWIN_JOB_DONE)
        return false;
    *status = job->status;
    *body = job->reply;
    *body_length = job->reply_length;
    return true;
}

const char *ksd_kwin_queue_generation(const ksd_kwin_queue *queue)
{
    return queue == NULL ? NULL : queue->generation;
}

bool ksd_kwin_queue_complete(ksd_kwin_queue *queue, const char *generation,
                             const char *sequence, uint32_t status,
                             const uint8_t *reply, uint32_t reply_length)
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
        if (reply_length != 0u && reply == NULL)
            return false;
        if (reply_length != 0u) {
            uint8_t *copy = malloc(reply_length);

            if (copy == NULL)
                return false;
            memcpy(copy, reply, reply_length);
            free(job->reply);
            job->reply = copy;
            job->reply_length = reply_length;
        }
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
        release_slot(job);
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

const ksd_kwin_job *ksd_kwin_queue_find(const ksd_kwin_queue *queue,
                                        const char *sequence)
{
    if (queue == NULL || sequence == NULL)
        return NULL;
    for (size_t index = 0u; index < KSD_KWIN_MAX_JOBS; index++) {
        const ksd_kwin_job *job = queue->jobs + index;

        if (job->state == KSD_KWIN_JOB_FREE)
            continue;
        if (memcmp(job->sequence, sequence, KSD_KWIN_SEQ_HEX + 1u) == 0)
            return job;
    }
    return NULL;
}

void ksd_kwin_queue_release(ksd_kwin_queue *queue, const char *sequence)
{
    if (queue == NULL || sequence == NULL)
        return;
    for (size_t index = 0u; index < KSD_KWIN_MAX_JOBS; index++) {
        ksd_kwin_job *job = queue->jobs + index;

        if (job->state == KSD_KWIN_JOB_FREE)
            continue;
        if (memcmp(job->sequence, sequence, KSD_KWIN_SEQ_HEX + 1u) == 0) {
            release_slot(job);
            return;
        }
    }
}
