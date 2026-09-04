#include "kwin_host.h"

#include <stdlib.h>
#include <string.h>

struct ksd_kwin_host {
    char generation[KSD_KWIN_GENERATION_HEX + 1u];
    ksd_kwin_queue queue;
    /* One parked poll per lane. Indexed by lane so the two are independent,
     * which is the whole point of the split: a bounded verb is never behind a
     * queue of enumerations, only ever behind at most one executing job. */
    bool parked[KSD_KWIN_LANES];
};

static size_t lane_index(ksd_kwin_lane lane)
{
    return lane == KSD_KWIN_LANE_FAST ? 0u : 1u;
}

ksd_kwin_host *ksd_kwin_host_create(const char *generation)
{
    ksd_kwin_host *host = calloc(1u, sizeof(*host));

    if (host == NULL)
        return NULL;
    if (!ksd_kwin_queue_init(&host->queue, generation)) {
        free(host);
        return NULL;
    }
    memcpy(host->generation, generation, KSD_KWIN_GENERATION_HEX);
    host->generation[KSD_KWIN_GENERATION_HEX] = '\0';
    return host;
}

void ksd_kwin_host_destroy(ksd_kwin_host *host)
{
    if (host == NULL)
        return;
    /* Every slot the queue still owns is released, including the bodies and
     * replies of jobs nobody read. */
    (void)ksd_kwin_queue_expire(&host->queue, UINT64_MAX);
    free(host);
}

bool ksd_kwin_host_hello(ksd_kwin_host *host, ksd_buffer *reply)
{
    if (host == NULL || reply == NULL)
        return false;
    /* The same shape as a report acknowledgement: the script needs only the
     * generation out of it, and one shape is one parser. */
    return ksd_kwin_format_report_ack(host->generation, reply);
}

ksd_kwin_poll_outcome ksd_kwin_host_poll(ksd_kwin_host *host,
                                         const uint8_t *envelope,
                                         size_t length, uint64_t now_ms,
                                         ksd_buffer *reply)
{
    ksd_kwin_poll poll;
    const ksd_kwin_job *batch[KSD_KWIN_JOBS_PER_REPLY_FAST];
    ksd_kwin_dispatch dispatch[KSD_KWIN_JOBS_PER_REPLY_FAST];
    size_t taken;

    if (host == NULL || reply == NULL)
        return KSD_KWIN_POLL_REFUSED;
    if (!ksd_kwin_parse_poll(envelope, length, &poll))
        return KSD_KWIN_POLL_REFUSED;
    /* A poll from another generation is the previous run of the script still
     * talking. Answering it would hand this run's work to a script that is on
     * its way out, and those jobs would never be reported. */
    if (memcmp(poll.generation, host->generation,
               KSD_KWIN_GENERATION_HEX) != 0)
        return KSD_KWIN_POLL_REFUSED;

    (void)now_ms;
    taken = ksd_kwin_queue_take(&host->queue, poll.lane, batch,
                                KSD_KWIN_JOBS_PER_REPLY_FAST);
    if (taken == 0u) {
        /* Nothing for this lane. The caller parks the invocation; it is
         * completed when work arrives or the idle timer fires, never dropped. */
        host->parked[lane_index(poll.lane)] = true;
        return KSD_KWIN_POLL_PARKED;
    }
    for (size_t index = 0u; index < taken; index++) {
        dispatch[index].sequence = batch[index]->sequence;
        dispatch[index].opcode = batch[index]->opcode;
        /* What is left of this job's budget at the moment of dispatch, not
         * what it started with. The script skips a job whose budget has run
         * out rather than running work the caller was already told is BUSY. */
        dispatch[index].budget_ms =
            batch[index]->deadline_ms > now_ms
                ? (uint32_t)(batch[index]->deadline_ms - now_ms) : 0u;
        dispatch[index].body = batch[index]->body;
        dispatch[index].body_length = batch[index]->body_length;
    }
    host->parked[lane_index(poll.lane)] = false;
    if (!ksd_kwin_format_poll_reply(host->generation, poll.lane, 0u, dispatch,
                                    taken, reply))
        return KSD_KWIN_POLL_REFUSED;
    return KSD_KWIN_POLL_ANSWERED;
}

/* Answers a lane whose poll is already parked. The transport calls this both
 * when work has just been queued and when the idle timer fires, and the two
 * cases differ only in what the queue happens to hold -- which is why it asks
 * rather than assuming. A version that always wrote an idle reply would accept
 * work and then sit on it until the next timer. */
bool ksd_kwin_host_poll_parked(ksd_kwin_host *host, ksd_kwin_lane lane,
                               uint64_t now_ms, ksd_buffer *reply)
{
    const ksd_kwin_job *batch[KSD_KWIN_JOBS_PER_REPLY_FAST];
    ksd_kwin_dispatch dispatch[KSD_KWIN_JOBS_PER_REPLY_FAST];
    size_t taken;

    if (host == NULL || reply == NULL)
        return false;
    if (lane != KSD_KWIN_LANE_FAST && lane != KSD_KWIN_LANE_SLOW)
        return false;
    host->parked[lane_index(lane)] = false;
    taken = ksd_kwin_queue_take(&host->queue, lane, batch,
                                KSD_KWIN_JOBS_PER_REPLY_FAST);
    if (taken == 0u)
        return ksd_kwin_format_poll_reply(host->generation, lane,
                                          KSD_KWIN_IDLE_REPLY_MS, NULL, 0u,
                                          reply);
    for (size_t index = 0u; index < taken; index++) {
        dispatch[index].sequence = batch[index]->sequence;
        dispatch[index].opcode = batch[index]->opcode;
        dispatch[index].budget_ms = batch[index]->deadline_ms > now_ms
            ? (uint32_t)(batch[index]->deadline_ms - now_ms) : 0u;
        dispatch[index].body = batch[index]->body;
        dispatch[index].body_length = batch[index]->body_length;
    }
    return ksd_kwin_format_poll_reply(host->generation, lane, 0u, dispatch,
                                      taken, reply);
}

bool ksd_kwin_host_report(ksd_kwin_host *host, const uint8_t *envelope,
                          size_t length, ksd_buffer *reply)
{
    ksd_kwin_report report;

    if (host == NULL || reply == NULL)
        return false;
    if (!ksd_kwin_parse_report(envelope, length, &report))
        return false;
    if (memcmp(report.generation, host->generation,
               KSD_KWIN_GENERATION_HEX) != 0)
        return false;
    /* Checked in full before anything is applied. A report is one message from
     * one script, and a mixture of records this daemon dispatched and records
     * it did not is not a partially valid report -- it is a script saying
     * something untrue, and believing the half that happens to match would
     * hand a caller a result from a job that was never run. */
    for (size_t index = 0u; index < report.count; index++) {
        const ksd_kwin_job *job = ksd_kwin_queue_find(&host->queue,
                                                      report.done[index].sequence);

        if (job == NULL || job->state != KSD_KWIN_JOB_DISPATCHED)
            return false;
    }
    for (size_t index = 0u; index < report.count; index++) {
        if (!ksd_kwin_queue_complete(&host->queue, host->generation,
                                     report.done[index].sequence,
                                     report.done[index].status,
                                     report.done[index].body,
                                     report.done[index].body_length))
            return false;
    }
    return ksd_kwin_format_report_ack(host->generation, reply);
}

bool ksd_kwin_host_submit(ksd_kwin_host *host, uint16_t opcode,
                          const uint8_t *body, uint32_t body_length,
                          uint64_t now_ms,
                          char sequence[KSD_KWIN_SEQ_HEX + 1u])
{
    if (host == NULL)
        return false;
    return ksd_kwin_queue_submit(&host->queue, opcode, body, body_length,
                                 now_ms, sequence);
}

bool ksd_kwin_host_parked(const ksd_kwin_host *host, ksd_kwin_lane lane)
{
    if (host == NULL || (lane != KSD_KWIN_LANE_FAST
                         && lane != KSD_KWIN_LANE_SLOW))
        return false;
    return host->parked[lane_index(lane)];
}

bool ksd_kwin_host_result(ksd_kwin_host *host, const char *sequence,
                          uint32_t *status, const uint8_t **body,
                          uint32_t *body_length)
{
    const ksd_kwin_job *job;

    if (host == NULL || status == NULL || body == NULL
        || body_length == NULL)
        return false;
    job = ksd_kwin_queue_find(&host->queue, sequence);
    /* Only a completed job has a result. A dispatched one has an unknown
     * outcome rather than an empty one, and reporting empty would be a lie
     * the caller cannot tell from a real empty answer. */
    if (job == NULL || job->state != KSD_KWIN_JOB_DONE)
        return false;
    *status = job->status;
    *body = job->reply;
    *body_length = job->reply_length;
    return true;
}

void ksd_kwin_host_release(ksd_kwin_host *host, const char *sequence)
{
    if (host == NULL)
        return;
    ksd_kwin_queue_release(&host->queue, sequence);
}

size_t ksd_kwin_host_expire(ksd_kwin_host *host, uint64_t now_ms)
{
    if (host == NULL)
        return 0u;
    return ksd_kwin_queue_expire(&host->queue, now_ms);
}

const char *ksd_kwin_host_generation(const ksd_kwin_host *host)
{
    return host == NULL ? NULL : host->generation;
}
