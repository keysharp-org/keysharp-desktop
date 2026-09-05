#include "kwin_jobs.h"
#include "protocol.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define GEN_A "0123456789abcdef0123456789abcdef"
#define GEN_B "fedcba9876543210fedcba9876543210"

#define FAST_OP KSD_OP_WINDOW_ACTIVE
#define SLOW_OP KSD_OP_WINDOW_LIST

static ksd_kwin_poll_outcome poll_lane(ksd_kwin_queue *host, const char *lane,
                                       uint64_t now_ms, ksd_buffer *reply)
{
    char envelope[256];
    int written = snprintf(envelope, sizeof(envelope),
                           "KSK1\ngen %s\nlane %s\nrtt 0\nlost 0\nend\n",
                           GEN_A, lane);

    assert(written > 0 && (size_t)written < sizeof(envelope));
    return ksd_kwin_queue_poll(host, (const uint8_t *)envelope,
                              (size_t)written, now_ms, reply);
}

/* A poll with nothing waiting parks rather than answering. The script has one
 * request outstanding per lane, and answering it emptily would cost a round
 * trip per idle period for no information. */
static void check_parks_when_idle(void)
{
    ksd_kwin_queue *host = ksd_kwin_queue_create(GEN_A);
    ksd_buffer reply;

    assert(host != NULL);
    ksd_buffer_init(&reply, 4096u);
    assert(poll_lane(host, "fast", 0u, &reply) == KSD_KWIN_POLL_PARKED);
    assert(reply.length == 0u);
    assert(ksd_kwin_queue_parked(host, KSD_KWIN_LANE_FAST));
    assert(!ksd_kwin_queue_parked(host, KSD_KWIN_LANE_SLOW));
    ksd_buffer_clear(&reply);
    ksd_kwin_queue_destroy(host);
}

/* The whole channel, once around: submit, poll, report, read the result. */
static void check_round_trip(void)
{
    ksd_kwin_queue *host = ksd_kwin_queue_create(GEN_A);
    ksd_buffer reply;
    char sequence[KSD_KWIN_SEQ_HEX + 1u];
    char report[512];
    uint32_t status = 0u;
    const uint8_t *body = NULL;
    uint32_t body_length = 0u;
    int written;

    assert(host != NULL);
    assert(ksd_kwin_queue_submit(host, FAST_OP, (const uint8_t *)"{}", 2u, 0u,
                                sequence));

    ksd_buffer_init(&reply, 4096u);
    assert(poll_lane(host, "fast", 0u, &reply) == KSD_KWIN_POLL_ANSWERED);
    /* The job, its body, and a budget that is what is LEFT at dispatch rather
     * than what it started with. */
    assert(memmem(reply.data, reply.length, sequence,
                  KSD_KWIN_SEQ_HEX) != NULL);
    assert(memmem(reply.data, reply.length, "2011 2000 2", 11u) != NULL);
    assert(memmem(reply.data, reply.length, "end\n{}", 6u) != NULL);
    /* Answering a poll un-parks that lane. */
    assert(!ksd_kwin_queue_parked(host, KSD_KWIN_LANE_FAST));
    ksd_buffer_clear(&reply);

    /* Still no result: the script has been given the job and has not spoken. */
    assert(!ksd_kwin_queue_result(host, sequence, &status, &body,
                                 &body_length));

    written = snprintf(report, sizeof(report),
                       "KSK1\ngen %s\ndone %s 0 5\nend\nhello",
                       GEN_A, sequence);
    assert(written > 0);
    ksd_buffer_init(&reply, 4096u);
    assert(ksd_kwin_queue_report(host, (const uint8_t *)report,
                                (size_t)written, &reply));
    assert(memmem(reply.data, reply.length, "ack", 3u) != NULL);
    ksd_buffer_clear(&reply);

    assert(ksd_kwin_queue_result(host, sequence, &status, &body,
                                &body_length));
    assert(status == 0u);
    assert(body_length == 5u);
    assert(memcmp(body, "hello", 5u) == 0);

    /* Released only once the submitter has read it, because the thread that
     * receives the report is not the one waiting on the job. */
    ksd_kwin_queue_release(host, sequence);
    assert(!ksd_kwin_queue_result(host, sequence, &status, &body,
                                 &body_length));
    ksd_kwin_queue_destroy(host);
}

/* The budget shrinks as the job waits, and reaches zero rather than wrapping.
 * A job dispatched with a spent budget is skipped by the script, which is the
 * point: the caller has already been told BUSY. */
static void check_budget_is_what_remains(void)
{
    ksd_kwin_queue *host = ksd_kwin_queue_create(GEN_A);
    ksd_buffer reply;
    char sequence[KSD_KWIN_SEQ_HEX + 1u];

    assert(host != NULL);
    assert(ksd_kwin_queue_submit(host, FAST_OP, NULL, 0u, 0u, sequence));
    ksd_buffer_init(&reply, 4096u);
    assert(poll_lane(host, "fast", 500u, &reply) == KSD_KWIN_POLL_ANSWERED);
    assert(memmem(reply.data, reply.length, " 1500 0\n", 8u) != NULL);
    ksd_buffer_clear(&reply);
    ksd_kwin_queue_destroy(host);
}

static void check_generation_is_load_bearing(void)
{
    ksd_kwin_queue *host = ksd_kwin_queue_create(GEN_A);
    ksd_buffer reply;
    char sequence[KSD_KWIN_SEQ_HEX + 1u];
    char envelope[256];
    char report[512];
    int written;

    assert(host != NULL);
    assert(ksd_kwin_queue_submit(host, FAST_OP, NULL, 0u, 0u, sequence));

    /* A poll from the previous run of the script. Answering it would hand
     * this run's work to a script on its way out, and those jobs would never
     * be reported by anyone. */
    written = snprintf(envelope, sizeof(envelope),
                       "KSK1\ngen %s\nlane fast\nrtt 0\nlost 0\nend\n", GEN_B);
    assert(written > 0);
    ksd_buffer_init(&reply, 4096u);
    assert(ksd_kwin_queue_poll(host, (const uint8_t *)envelope,
                              (size_t)written, 0u, &reply)
           == KSD_KWIN_POLL_REFUSED);
    ksd_buffer_clear(&reply);

    /* And the job is still queued, not consumed by the refusal. */
    ksd_buffer_init(&reply, 4096u);
    assert(poll_lane(host, "fast", 0u, &reply) == KSD_KWIN_POLL_ANSWERED);
    ksd_buffer_clear(&reply);

    /* A report from another generation names sequences that mean nothing. */
    written = snprintf(report, sizeof(report),
                       "KSK1\ngen %s\ndone %s 0 0\nend\n", GEN_B, sequence);
    assert(written > 0);
    ksd_buffer_init(&reply, 4096u);
    assert(!ksd_kwin_queue_report(host, (const uint8_t *)report,
                                 (size_t)written, &reply));
    ksd_buffer_clear(&reply);
    ksd_kwin_queue_destroy(host);
}

/* A report is one message from one script. A mixture of records this daemon
 * dispatched and records it did not is not partially valid: believing the half
 * that matches would hand a caller a result from a job never run. */
static void check_report_is_all_or_nothing(void)
{
    ksd_kwin_queue *host = ksd_kwin_queue_create(GEN_A);
    ksd_buffer reply;
    char sequence[KSD_KWIN_SEQ_HEX + 1u];
    char report[512];
    uint32_t status = 0u;
    const uint8_t *body = NULL;
    uint32_t body_length = 0u;
    int written;

    assert(host != NULL);
    assert(ksd_kwin_queue_submit(host, FAST_OP, NULL, 0u, 0u, sequence));
    ksd_buffer_init(&reply, 4096u);
    assert(poll_lane(host, "fast", 0u, &reply) == KSD_KWIN_POLL_ANSWERED);
    ksd_buffer_clear(&reply);

    /* One real record and one invented one, the real one first so that a
     * daemon applying as it parsed would have committed it before noticing. */
    written = snprintf(report, sizeof(report),
                       "KSK1\ngen %s\ndone %s 0 0\ndone ffffffffffffffff 0 0\n"
                       "end\n", GEN_A, sequence);
    assert(written > 0);
    ksd_buffer_init(&reply, 4096u);
    assert(!ksd_kwin_queue_report(host, (const uint8_t *)report,
                                 (size_t)written, &reply));
    ksd_buffer_clear(&reply);
    /* And the real record was NOT applied. */
    assert(!ksd_kwin_queue_result(host, sequence, &status, &body,
                                 &body_length));
    ksd_kwin_queue_destroy(host);
}

/* A job the script was never given cannot be completed, however well formed
 * the record is: that is a script claiming work it never received. */
static void check_undispatched_cannot_complete(void)
{
    ksd_kwin_queue *host = ksd_kwin_queue_create(GEN_A);
    ksd_buffer reply;
    char sequence[KSD_KWIN_SEQ_HEX + 1u];
    char report[512];
    int written;

    assert(host != NULL);
    assert(ksd_kwin_queue_submit(host, SLOW_OP, NULL, 0u, 0u, sequence));
    written = snprintf(report, sizeof(report),
                       "KSK1\ngen %s\ndone %s 0 0\nend\n", GEN_A, sequence);
    assert(written > 0);
    ksd_buffer_init(&reply, 4096u);
    assert(!ksd_kwin_queue_report(host, (const uint8_t *)report,
                                 (size_t)written, &reply));
    ksd_buffer_clear(&reply);
    ksd_kwin_queue_destroy(host);
}

/* The lanes are independent. Work in one does not answer a poll in the other,
 * which is the property the whole split exists to provide. */
static void check_lanes_are_independent(void)
{
    ksd_kwin_queue *host = ksd_kwin_queue_create(GEN_A);
    ksd_buffer reply;
    char sequence[KSD_KWIN_SEQ_HEX + 1u];

    assert(host != NULL);
    assert(ksd_kwin_queue_submit(host, SLOW_OP, NULL, 0u, 0u, sequence));

    ksd_buffer_init(&reply, 4096u);
    assert(poll_lane(host, "fast", 0u, &reply) == KSD_KWIN_POLL_PARKED);
    ksd_buffer_clear(&reply);

    ksd_buffer_init(&reply, 4096u);
    assert(poll_lane(host, "slow", 0u, &reply) == KSD_KWIN_POLL_ANSWERED);
    ksd_buffer_clear(&reply);
    ksd_kwin_queue_destroy(host);
}

static void check_malformed_is_refused(void)
{
    ksd_kwin_queue *host = ksd_kwin_queue_create(GEN_A);
    ksd_buffer reply;

    assert(host != NULL);
    ksd_buffer_init(&reply, 4096u);
    assert(ksd_kwin_queue_poll(host, (const uint8_t *)"not an envelope", 15u,
                              0u, &reply) == KSD_KWIN_POLL_REFUSED);
    assert(!ksd_kwin_queue_report(host, (const uint8_t *)"nor this", 8u,
                                 &reply));
    ksd_buffer_clear(&reply);
    /* A generation that is not 32 lowercase hex digits is not a generation. */
    assert(ksd_kwin_queue_create("short") == NULL);
    assert(ksd_kwin_queue_create(NULL) == NULL);
    ksd_kwin_queue_destroy(host);
    ksd_kwin_queue_destroy(NULL);
}

int main(void)
{
    check_parks_when_idle();
    check_round_trip();
    check_budget_is_what_remains();
    check_generation_is_load_bearing();
    check_report_is_all_or_nothing();
    check_undispatched_cannot_complete();
    check_lanes_are_independent();
    check_malformed_is_refused();
    return 0;
}
