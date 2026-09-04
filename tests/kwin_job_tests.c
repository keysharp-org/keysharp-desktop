#include "kwin_jobs.h"
#include "operation_scope.h"
#include "protocol.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define GEN_A "0123456789abcdef0123456789abcdef"
#define GEN_B "fedcba9876543210fedcba9876543210"

/* A bounded verb and an enumerating one, taken from the lane table rather than
 * named literally, so a change to the table shows up here as a failure rather
 * than as a test that quietly stops covering both lanes. */
#define FAST_OP KSD_OP_WINDOW_ACTIVE
#define SLOW_OP KSD_OP_WINDOW_LIST

static void check_lane_assumptions(void)
{
    assert(ksd_kwin_lane_for(FAST_OP) == KSD_KWIN_LANE_FAST);
    assert(ksd_kwin_lane_for(SLOW_OP) == KSD_KWIN_LANE_SLOW);
}

static void check_generation(void)
{
    ksd_kwin_queue queue;

    assert(ksd_kwin_queue_init(&queue, GEN_A));
    /* Exactly 32 lowercase hex digits, the same spelling the envelope parser
     * accepts. A short, long or uppercase generation is a different token, not
     * a forgiving one. */
    assert(!ksd_kwin_queue_init(&queue, "0123"));
    assert(!ksd_kwin_queue_init(&queue, GEN_A "0"));
    assert(!ksd_kwin_queue_init(&queue, "0123456789ABCDEF0123456789abcdef"));
    assert(!ksd_kwin_queue_init(&queue, NULL));
}

static void check_submit_and_take(void)
{
    ksd_kwin_queue queue;
    char sequence[KSD_KWIN_SEQ_HEX + 1u];
    char first[KSD_KWIN_SEQ_HEX + 1u];
    const ksd_kwin_job *batch[KSD_KWIN_MAX_JOBS];
    size_t taken;

    assert(ksd_kwin_queue_init(&queue, GEN_A));

    /* An opcode that never reaches the script cannot be queued. Queueing one
     * would park a caller on work nothing will ever run. */
    assert(!ksd_kwin_queue_submit(&queue, KSD_OP_CAPTURE_AREA, 0u, sequence));
    assert(!ksd_kwin_queue_submit(&queue, 0xfffeu, 0u, sequence));
    assert(ksd_kwin_queue_count(&queue, KSD_KWIN_JOB_QUEUED) == 0u);

    assert(ksd_kwin_queue_submit(&queue, FAST_OP, 0u, first));
    assert(strlen(first) == KSD_KWIN_SEQ_HEX);
    for (size_t index = 0u; index < KSD_KWIN_SEQ_HEX; index++)
        assert((first[index] >= '0' && first[index] <= '9')
               || (first[index] >= 'a' && first[index] <= 'f'));

    /* Sequences are never reused, so a stale report cannot land on a fresh
     * job that happens to sit in the same slot. */
    assert(ksd_kwin_queue_submit(&queue, FAST_OP, 0u, sequence));
    assert(strcmp(sequence, first) != 0);

    /* A fast reply carries only fast work, and never more than its cap. Ten
     * are queued against a cap of eight. */
    for (int index = 0; index < 8; index++)
        assert(ksd_kwin_queue_submit(&queue, FAST_OP, 0u, sequence));
    assert(ksd_kwin_queue_submit(&queue, SLOW_OP, 0u, sequence));

    taken = ksd_kwin_queue_take(&queue, KSD_KWIN_LANE_FAST, batch,
                                KSD_KWIN_MAX_JOBS);
    assert(taken == KSD_KWIN_JOBS_PER_REPLY_FAST);
    for (size_t index = 0u; index < taken; index++) {
        assert(batch[index]->lane == KSD_KWIN_LANE_FAST);
        assert(batch[index]->state == KSD_KWIN_JOB_DISPATCHED);
    }
    /* Oldest first: the first job submitted is the first one handed over. */
    assert(strcmp(batch[0]->sequence, first) == 0);

    /* An enumeration is pinned to one per callback whatever is waiting. */
    assert(ksd_kwin_queue_submit(&queue, SLOW_OP, 0u, sequence));
    taken = ksd_kwin_queue_take(&queue, KSD_KWIN_LANE_SLOW, batch,
                                KSD_KWIN_MAX_JOBS);
    assert(taken == KSD_KWIN_JOBS_PER_REPLY_SLOW);
    assert(taken == 1u);
    assert(batch[0]->lane == KSD_KWIN_LANE_SLOW);

    /* A lane that carries nothing takes nothing, rather than falling through
     * to the other lane's work. */
    assert(ksd_kwin_queue_take(&queue, KSD_KWIN_LANE_NONE, batch,
                               KSD_KWIN_MAX_JOBS) == 0u);
}

static void check_no_job_is_dispatched_twice(void)
{
    ksd_kwin_queue queue;
    char sequence[KSD_KWIN_SEQ_HEX + 1u];
    const ksd_kwin_job *batch[KSD_KWIN_MAX_JOBS];

    assert(ksd_kwin_queue_init(&queue, GEN_A));
    assert(ksd_kwin_queue_submit(&queue, FAST_OP, 0u, sequence));
    assert(ksd_kwin_queue_take(&queue, KSD_KWIN_LANE_FAST, batch,
                               KSD_KWIN_MAX_JOBS) == 1u);
    /* The second poll must not be answered with the same job. Running an
     * operation twice is not something a caller can be protected from after
     * the fact -- a window closed twice is closed once and then refused, but a
     * scroll applied twice has simply happened twice. */
    assert(ksd_kwin_queue_take(&queue, KSD_KWIN_LANE_FAST, batch,
                               KSD_KWIN_MAX_JOBS) == 0u);
}

static void check_completion_rules(void)
{
    ksd_kwin_queue queue;
    char dispatched[KSD_KWIN_SEQ_HEX + 1u];
    char queued[KSD_KWIN_SEQ_HEX + 1u];
    const ksd_kwin_job *batch[KSD_KWIN_MAX_JOBS];

    assert(ksd_kwin_queue_init(&queue, GEN_A));
    assert(ksd_kwin_queue_submit(&queue, FAST_OP, 0u, dispatched));
    assert(ksd_kwin_queue_take(&queue, KSD_KWIN_LANE_FAST, batch,
                               KSD_KWIN_MAX_JOBS) == 1u);
    assert(ksd_kwin_queue_submit(&queue, SLOW_OP, 0u, queued));

    /* A report from another generation names sequences that mean nothing
     * here. After a script restart the old script's numbering is gone, and
     * completing one against the other hands a caller a result from a job
     * that no longer exists. */
    assert(!ksd_kwin_queue_complete(&queue, GEN_B, dispatched, 0u));
    assert(ksd_kwin_queue_count(&queue, KSD_KWIN_JOB_DONE) == 0u);

    /* A sequence that names no job at all. */
    assert(!ksd_kwin_queue_complete(&queue, GEN_A, "ffffffffffffffff", 0u));

    /* A job the script was never given. A wedged or replaced script claiming
     * to have run work it never received would otherwise return a fabricated
     * result to whoever is waiting on it. */
    assert(!ksd_kwin_queue_complete(&queue, GEN_A, queued, 0u));
    assert(ksd_kwin_queue_count(&queue, KSD_KWIN_JOB_DONE) == 0u);

    assert(ksd_kwin_queue_complete(&queue, GEN_A, dispatched, 7u));
    assert(ksd_kwin_queue_count(&queue, KSD_KWIN_JOB_DONE) == 1u);

    /* Replay. The same result arriving twice must not complete anything a
     * second time. */
    assert(!ksd_kwin_queue_complete(&queue, GEN_A, dispatched, 0u));
    assert(ksd_kwin_queue_count(&queue, KSD_KWIN_JOB_DONE) == 1u);
}

static void check_expiry_spares_dispatched_work(void)
{
    ksd_kwin_queue queue;
    char sequence[KSD_KWIN_SEQ_HEX + 1u];
    const ksd_kwin_job *batch[KSD_KWIN_MAX_JOBS];

    assert(ksd_kwin_queue_init(&queue, GEN_A));
    assert(ksd_kwin_queue_submit(&queue, FAST_OP, 0u, sequence));
    assert(ksd_kwin_queue_take(&queue, KSD_KWIN_LANE_FAST, batch,
                               KSD_KWIN_MAX_JOBS) == 1u);
    assert(ksd_kwin_queue_submit(&queue, FAST_OP, 0u, sequence));

    /* Before the deadline nothing moves. */
    assert(ksd_kwin_queue_expire(&queue, KSD_KWIN_OP_DEADLINE_MS - 1u) == 0u);
    assert(ksd_kwin_queue_count(&queue, KSD_KWIN_JOB_QUEUED) == 1u);

    /* After it, the queued job is dropped and the dispatched one is not. This
     * is the whole distinction: a queued job provably never reached the
     * compositor, so its caller gets BUSY and may retry; a dispatched one is
     * inside an uninterruptible callback and its outcome is unknown, which is
     * not safe to retry for close or move-resize. */
    assert(ksd_kwin_queue_expire(&queue, KSD_KWIN_OP_DEADLINE_MS) == 1u);
    assert(ksd_kwin_queue_count(&queue, KSD_KWIN_JOB_QUEUED) == 0u);
    assert(ksd_kwin_queue_count(&queue, KSD_KWIN_JOB_DISPATCHED) == 1u);

    /* Expiring again finds nothing left to drop. */
    assert(ksd_kwin_queue_expire(&queue, KSD_KWIN_OP_DEADLINE_MS * 4u) == 0u);
    assert(ksd_kwin_queue_count(&queue, KSD_KWIN_JOB_DISPATCHED) == 1u);
}

static void check_queue_is_bounded(void)
{
    ksd_kwin_queue queue;
    char sequence[KSD_KWIN_SEQ_HEX + 1u];

    assert(ksd_kwin_queue_init(&queue, GEN_A));
    for (size_t index = 0u; index < KSD_KWIN_MAX_JOBS; index++)
        assert(ksd_kwin_queue_submit(&queue, FAST_OP, 0u, sequence));
    /* One job per connection thread is the ceiling, so a further job is one
     * nobody is waiting for. Refusing is what turns an overrun into BUSY at
     * the caller rather than into a write past the array. */
    assert(!ksd_kwin_queue_submit(&queue, FAST_OP, 0u, sequence));
    assert(ksd_kwin_queue_count(&queue, KSD_KWIN_JOB_QUEUED)
           == KSD_KWIN_MAX_JOBS);

    /* A freed slot is reusable, or a long-lived daemon would run out. */
    assert(ksd_kwin_queue_expire(&queue, KSD_KWIN_OP_DEADLINE_MS)
           == KSD_KWIN_MAX_JOBS);
    assert(ksd_kwin_queue_submit(&queue, FAST_OP, 0u, sequence));
}

/* A batch may never exceed what one report can carry back, or a result would
 * have nowhere to be reported. Checked as arithmetic over the constants
 * rather than by running a batch, so it holds for every future edit to them. */
static void check_batch_fits_a_report(void)
{
    assert(KSD_KWIN_JOBS_PER_REPLY_FAST + KSD_KWIN_JOBS_PER_REPLY_SLOW
           <= KSD_KWIN_MAX_DONE_PER_REPORT);
    assert(KSD_KWIN_JOBS_PER_REPLY_FAST <= KSD_KWIN_MAX_JOBS);
}


/* G6 and G7. The provider connection has exactly one legitimate peer, so this
 * is not a general authorization question: it is whether the message came from
 * the script this daemon started, in the run it is currently talking to. */
static void check_peer_allowed(void)
{
    const uid_t me = (uid_t)1000;
    const uid_t other = (uid_t)1001;

    assert(ksd_kwin_peer_allowed(":1.5", ":1.5", me, me, GEN_A, GEN_A));

    /* G6. A different connection. A unique name is never reused within a bus
     * lifetime, so a reconnecting script gets a new one and must not inherit
     * the standing of the connection it replaced. */
    assert(!ksd_kwin_peer_allowed(":1.99", ":1.5", me, me, GEN_A, GEN_A));

    /* G7. The previous run of the script. Its sequences name jobs that no
     * longer exist, which the queue refuses per job and this refuses for the
     * whole peer. */
    assert(!ksd_kwin_peer_allowed(":1.5", ":1.5", me, me, GEN_A, GEN_B));

    /* Another user is not this session compositor whatever it claims. */
    assert(!ksd_kwin_peer_allowed(":1.5", ":1.5", me, other, GEN_A, GEN_A));

    /* A well-known name is refused even when both sides present it. Comparing
     * unique names only works because a unique name cannot be handed from one
     * connection to another, and a well-known name can. */
    assert(!ksd_kwin_peer_allowed("org.kde.KWin", "org.kde.KWin", me, me,
                                  GEN_A, GEN_A));
    assert(!ksd_kwin_peer_allowed(":1.5", "org.kde.KWin", me, me,
                                  GEN_A, GEN_A));

    /* Malformed unique names. */
    assert(!ksd_kwin_peer_allowed(":1.5", ":", me, me, GEN_A, GEN_A));
    assert(!ksd_kwin_peer_allowed(":1.5", ":1", me, me, GEN_A, GEN_A));
    assert(!ksd_kwin_peer_allowed(":1.5", ":1.", me, me, GEN_A, GEN_A));
    assert(!ksd_kwin_peer_allowed(":1.5", ":1.5x", me, me, GEN_A, GEN_A));
    assert(!ksd_kwin_peer_allowed(":1.5", "", me, me, GEN_A, GEN_A));
    assert(!ksd_kwin_peer_allowed(":1.5", NULL, me, me, GEN_A, GEN_A));

    /* Two malformed generations that match each other still fail: the property
     * is not that they are equal, it is that they are the one issued. */
    assert(!ksd_kwin_peer_allowed(":1.5", ":1.5", me, me, "zz", "zz"));
    assert(!ksd_kwin_peer_allowed(":1.5", ":1.5", me, me, NULL, NULL));
}

int main(void)
{
    check_lane_assumptions();
    check_generation();
    check_submit_and_take();
    check_no_job_is_dispatched_twice();
    check_completion_rules();
    check_expiry_spares_dispatched_work();
    check_queue_is_bounded();
    check_batch_fits_a_report();
    check_peer_allowed();
    return 0;
}
