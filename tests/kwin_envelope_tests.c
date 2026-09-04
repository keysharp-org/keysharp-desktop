#include "kwin_envelope.h"

#include <assert.h>
#include <string.h>

#define GEN "0123456789abcdef0123456789abcdef"
#define SEQ "00000000000000ff"

static bool poll_ok(const char *text, ksd_kwin_poll *poll)
{
    return ksd_kwin_parse_poll((const uint8_t *)text, strlen(text), poll);
}

static bool report_ok(const char *text, size_t length,
                      ksd_kwin_report *report)
{
    return ksd_kwin_parse_report((const uint8_t *)text, length, report);
}

static void check_poll(void)
{
    ksd_kwin_poll poll;

    assert(poll_ok("KSK1\ngen " GEN "\nlane fast\nrtt 12\nlost 0\nend\n",
                   &poll));
    assert(strcmp(poll.generation, GEN) == 0);
    assert(poll.lane == KSD_KWIN_LANE_FAST);
    assert(poll.round_trip_ms == 12u);
    assert(poll.lost == 0u);

    assert(poll_ok("KSK1\ngen " GEN "\nlane slow\nrtt 0\nlost 3\nend\n",
                   &poll));
    assert(poll.lane == KSD_KWIN_LANE_SLOW);
    assert(poll.lost == 3u);

    /* An unknown keyword is fatal. Skipping it would let a newer script lose a
     * field this daemon never learned to read, silently. */
    assert(!poll_ok("KSK1\ngen " GEN "\nlane fast\nrtt 1\nlost 0\nwat 1\nend\n",
                    &poll));

    /* Every field is required, and none may be given twice. */
    assert(!poll_ok("KSK1\nlane fast\nrtt 1\nlost 0\nend\n", &poll));
    assert(!poll_ok("KSK1\ngen " GEN "\nrtt 1\nlost 0\nend\n", &poll));
    assert(!poll_ok("KSK1\ngen " GEN "\nlane fast\nlost 0\nend\n", &poll));
    assert(!poll_ok("KSK1\ngen " GEN "\nlane fast\nrtt 1\nend\n", &poll));
    assert(!poll_ok("KSK1\ngen " GEN "\ngen " GEN
                    "\nlane fast\nrtt 1\nlost 0\nend\n", &poll));

    /* No leading zeros: two spellings for one number is how a length check and
     * a length use come to disagree. */
    assert(!poll_ok("KSK1\ngen " GEN "\nlane fast\nrtt 01\nlost 0\nend\n",
                    &poll));
    assert(!poll_ok("KSK1\ngen " GEN "\nlane fast\nrtt 1\nlost 00\nend\n",
                    &poll));

    /* Generation is exactly 32 lowercase hex digits. A short one is a
     * different token, not a truncated one. */
    assert(!poll_ok("KSK1\ngen 0123\nlane fast\nrtt 1\nlost 0\nend\n", &poll));
    assert(!poll_ok("KSK1\ngen " GEN "0\nlane fast\nrtt 1\nlost 0\nend\n",
                    &poll));
    assert(!poll_ok("KSK1\ngen 0123456789ABCDEF0123456789abcdef"
                    "\nlane fast\nrtt 1\nlost 0\nend\n", &poll));

    /* The magic, the terminator, and the lane vocabulary. */
    assert(!poll_ok("KSK2\ngen " GEN "\nlane fast\nrtt 1\nlost 0\nend\n",
                    &poll));
    assert(!poll_ok("KSK1\ngen " GEN "\nlane fast\nrtt 1\nlost 0\n", &poll));
    assert(!poll_ok("KSK1\ngen " GEN "\nlane none\nrtt 1\nlost 0\nend\n",
                    &poll));

    /* A poll carries no body, so nothing may follow its terminator. */
    assert(!poll_ok("KSK1\ngen " GEN "\nlane fast\nrtt 1\nlost 0\nend\nx",
                    &poll));

    /* An unterminated final line is not a line. */
    assert(!poll_ok("KSK1\ngen " GEN "\nlane fast\nrtt 1\nlost 0\nend",
                    &poll));
    assert(!poll_ok("", &poll));
    assert(!poll_ok("KSK1\n", &poll));
}

static void check_report(void)
{
    ksd_kwin_report report;
    static const char one[] =
        "KSK1\ngen " GEN "\ndone " SEQ " 0 5\nend\nhello";
    static const char two[] =
        "KSK1\ngen " GEN "\ndone " SEQ " 0 5\ndone 00000000000000fe 7 3\n"
        "end\nhelloabc";

    assert(report_ok(one, sizeof(one) - 1u, &report));
    assert(report.count == 1u);
    assert(strcmp(report.done[0].sequence, SEQ) == 0);
    assert(report.done[0].status == 0u);
    assert(report.done[0].body_length == 5u);
    assert(memcmp(report.done[0].body, "hello", 5u) == 0);

    /* Several results ride one report, and each body is found where its own
     * declared length says it is rather than by scanning. */
    assert(report_ok(two, sizeof(two) - 1u, &report));
    assert(report.count == 2u);
    assert(memcmp(report.done[0].body, "hello", 5u) == 0);
    assert(report.done[1].status == 7u);
    assert(memcmp(report.done[1].body, "abc", 3u) == 0);

    /* Exactly, not at most. A trailing byte after the last body is the shape a
     * smuggled record takes, and a short one truncates a result. */
    static const char over[] = "KSK1\ngen " GEN "\ndone " SEQ " 0 5\nend\nhello!";
    static const char under[] = "KSK1\ngen " GEN "\ndone " SEQ " 0 5\nend\nhell";
    assert(!report_ok(over, sizeof(over) - 1u, &report));
    assert(!report_ok(under, sizeof(under) - 1u, &report));

    /* A report with no results is not a report. */
    static const char empty[] = "KSK1\ngen " GEN "\nend\n";
    assert(!report_ok(empty, sizeof(empty) - 1u, &report));

    /* The generation is required here too. */
    static const char ungen[] = "KSK1\ndone " SEQ " 0 0\nend\n";
    assert(!report_ok(ungen, sizeof(ungen) - 1u, &report));

    /* An unknown keyword is fatal in a report as well. */
    static const char alien[] =
        "KSK1\ngen " GEN "\ndone " SEQ " 0 0\nwat 1\nend\n";
    assert(!report_ok(alien, sizeof(alien) - 1u, &report));

    /* A done line with the wrong arity, or a trailing argument. */
    static const char shortline[] = "KSK1\ngen " GEN "\ndone " SEQ " 0\nend\n";
    static const char longline[] =
        "KSK1\ngen " GEN "\ndone " SEQ " 0 0 9\nend\n";
    assert(!report_ok(shortline, sizeof(shortline) - 1u, &report));
    assert(!report_ok(longline, sizeof(longline) - 1u, &report));

    /* More results than a single report may carry. */
    static const char flood[] =
        "KSK1\ngen " GEN
        "\ndone " SEQ " 0 0\ndone " SEQ " 0 0\ndone " SEQ " 0 0"
        "\ndone " SEQ " 0 0\ndone " SEQ " 0 0\ndone " SEQ " 0 0"
        "\ndone " SEQ " 0 0\ndone " SEQ " 0 0\ndone " SEQ " 0 0"
        "\ndone " SEQ " 0 0\nend\n";
    assert(!report_ok(flood, sizeof(flood) - 1u, &report));
}


/* The serialiser must never be able to emit something the parsers would
 * reject. They are the same protocol read in two directions, and a mismatch
 * would only show up as a wedged script after a round trip. */
static void check_poll_reply(void)
{
    ksd_buffer out;
    ksd_kwin_dispatch jobs[2];
    ksd_kwin_dispatch many[KSD_KWIN_JOBS_PER_REPLY_FAST + 1u];
    static const char expected_idle[] =
        "KSK1\ngen " GEN "\nlane fast\nidle 8000\nend\n";
    static const char expected_jobs[] =
        "KSK1\ngen " GEN "\nlane fast\nidle 0\n"
        "job " SEQ " 2011 1500 5\n"
        "job 00000000000000fe 2020 0 3\n"
        "end\nhelloabc";

    /* The idle reply: no jobs, and the parked invocation still answered. */
    ksd_buffer_init(&out, 4096u);
    assert(ksd_kwin_format_poll_reply(GEN, KSD_KWIN_LANE_FAST, 8000u, NULL,
                                      0u, &out));
    assert(out.length == sizeof(expected_idle) - 1u);
    assert(memcmp(out.data, expected_idle, out.length) == 0);
    ksd_buffer_clear(&out);

    /* Two jobs with bodies. The opcode is fixed-width lowercase hex, the
     * budget and the length are decimals with no leading zeros, and the bodies
     * follow the terminator in the order their lines declared them. */
    jobs[0].sequence = SEQ;
    jobs[0].opcode = 0x2011u;
    jobs[0].budget_ms = 1500u;
    jobs[0].body = (const uint8_t *)"hello";
    jobs[0].body_length = 5u;
    jobs[1].sequence = "00000000000000fe";
    jobs[1].opcode = 0x2020u;
    jobs[1].budget_ms = 0u;
    jobs[1].body = (const uint8_t *)"abc";
    jobs[1].body_length = 3u;

    ksd_buffer_init(&out, 4096u);
    assert(ksd_kwin_format_poll_reply(GEN, KSD_KWIN_LANE_FAST, 0u, jobs, 2u,
                                      &out));
    assert(out.length == sizeof(expected_jobs) - 1u);
    assert(memcmp(out.data, expected_jobs, out.length) == 0);
    ksd_buffer_clear(&out);

    /* The slow lane carries one job, never two. Two enumerations in one
     * callback double the hole in the event stream to save a message, and a
     * serialiser that ignored the cap would route around the lane split. */
    ksd_buffer_init(&out, 4096u);
    assert(ksd_kwin_format_poll_reply(GEN, KSD_KWIN_LANE_SLOW, 0u, jobs, 1u,
                                      &out));
    ksd_buffer_clear(&out);
    ksd_buffer_init(&out, 4096u);
    assert(!ksd_kwin_format_poll_reply(GEN, KSD_KWIN_LANE_SLOW, 0u, jobs, 2u,
                                       &out));
    ksd_buffer_clear(&out);

    /* Beyond the fast lane's batch. */
    for (size_t index = 0u; index < KSD_KWIN_JOBS_PER_REPLY_FAST + 1u; index++)
        many[index] = jobs[0];
    ksd_buffer_init(&out, 8192u);
    assert(!ksd_kwin_format_poll_reply(GEN, KSD_KWIN_LANE_FAST, 0u, many,
                                       KSD_KWIN_JOBS_PER_REPLY_FAST + 1u,
                                       &out));
    ksd_buffer_clear(&out);

    /* A lane that never reaches the script has no reply to write. */
    ksd_buffer_init(&out, 4096u);
    assert(!ksd_kwin_format_poll_reply(GEN, KSD_KWIN_LANE_NONE, 0u, NULL, 0u,
                                       &out));
    ksd_buffer_clear(&out);

    /* Malformed generations and sequences are refused rather than written. */
    ksd_buffer_init(&out, 4096u);
    assert(!ksd_kwin_format_poll_reply("0123", KSD_KWIN_LANE_FAST, 0u, NULL,
                                       0u, &out));
    assert(!ksd_kwin_format_poll_reply("0123456789ABCDEF0123456789abcdef",
                                       KSD_KWIN_LANE_FAST, 0u, NULL, 0u,
                                       &out));
    jobs[0].sequence = "short";
    assert(!ksd_kwin_format_poll_reply(GEN, KSD_KWIN_LANE_FAST, 0u, jobs, 1u,
                                       &out));
    jobs[0].sequence = SEQ;
    /* A declared length with no bytes behind it would make the body region
     * disagree with the lines that describe it. */
    jobs[0].body = NULL;
    assert(!ksd_kwin_format_poll_reply(GEN, KSD_KWIN_LANE_FAST, 0u, jobs, 1u,
                                       &out));
    ksd_buffer_clear(&out);
}

static void check_report_ack(void)
{
    ksd_buffer out;
    static const char expected[] = "KSK1\ngen " GEN "\nack\nend\n";

    ksd_buffer_init(&out, 256u);
    assert(ksd_kwin_format_report_ack(GEN, &out));
    assert(out.length == sizeof(expected) - 1u);
    assert(memcmp(out.data, expected, out.length) == 0);
    ksd_buffer_clear(&out);

    ksd_buffer_init(&out, 256u);
    assert(!ksd_kwin_format_report_ack("nope", &out));
    ksd_buffer_clear(&out);
}

int main(void)
{
    check_poll();
    check_report();
    check_poll_reply();
    check_report_ack();
    return 0;
}
