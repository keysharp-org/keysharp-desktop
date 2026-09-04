#ifndef KEYSHARP_DESKTOP_KWIN_ENVELOPE_H
#define KEYSHARP_DESKTOP_KWIN_ENVELOPE_H

#include "kwin_wire.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "protocol_io.h"

/* The envelope the KWin script and the daemon exchange.
 *
 * Everything here parses input written by the script, which is same-uid code
 * running inside the compositor. That is not a trust boundary the way a
 * network is, but it is a parsing boundary: a wedged or replaced script can
 * send anything at all, and the daemon must answer badly formed input with a
 * refusal rather than with undefined behaviour.
 *
 * Three rules do most of the work, and each exists because its absence is a
 * real bug rather than an untidiness:
 *
 *   An unknown keyword is fatal, not skipped. Skipping would let a newer
 *   script silently lose a field the daemon never learned to read.
 *
 *   A decimal token may not carry leading zeros. Accepting them gives two
 *   spellings for one value, and two spellings is how a length check and a
 *   length use come to disagree.
 *
 *   Declared body lengths must sum to exactly the remainder. Not at most:
 *   exactly. Trailing bytes after the last body are the shape a smuggled
 *   record takes.
 */

#define KSD_KWIN_GENERATION_HEX 32u
#define KSD_KWIN_SEQ_HEX 16u

typedef struct ksd_kwin_poll {
    char generation[KSD_KWIN_GENERATION_HEX + 1u];
    ksd_kwin_lane lane;
    /* What the script observed for its previous poll: the round trip in
     * milliseconds, and how many consecutive polls never came back. Both are
     * how a wrong reply-timeout constant on some distribution becomes a
     * journal line instead of a silent degradation nobody can diagnose. */
    uint32_t round_trip_ms;
    uint32_t lost;
} ksd_kwin_poll;

typedef struct ksd_kwin_done {
    char sequence[KSD_KWIN_SEQ_HEX + 1u];
    uint32_t status;
    const uint8_t *body;
    uint32_t body_length;
} ksd_kwin_done;

typedef struct ksd_kwin_report {
    char generation[KSD_KWIN_GENERATION_HEX + 1u];
    ksd_kwin_done done[KSD_KWIN_MAX_DONE_PER_REPORT];
    size_t count;
} ksd_kwin_report;

/* One job as it goes out to the script. The body is not copied: it must
 * outlive the call that serialises it. */
typedef struct ksd_kwin_dispatch {
    const char *sequence;
    uint16_t opcode;
    uint32_t budget_ms;
    const uint8_t *body;
    uint32_t body_length;
} ksd_kwin_dispatch;

/* The other direction: what the daemon writes back when the script polls.
 * Zero jobs with an idle line is the idle reply, and the parked invocation is
 * always completed rather than dropped.
 *
 * The same three rules the parsers enforce apply to what is written, because a
 * serialiser that can emit something its own parser would reject is a bug
 * waiting for a round trip: fixed-width lowercase hex, no leading zeros in a
 * decimal, and declared body lengths that sum to exactly the remainder. */
bool ksd_kwin_format_poll_reply(const char *generation, ksd_kwin_lane lane,
                                uint32_t idle_ms,
                                const ksd_kwin_dispatch *jobs, size_t count,
                                ksd_buffer *out);

/* The acknowledgement for a report. Carries no jobs by design: a report is the
 * script telling the daemon what finished, and answering it with work would
 * put a batch outside the lane accounting that bounds one. */
bool ksd_kwin_format_report_ack(const char *generation, ksd_buffer *out);

/* Both return false on anything they do not fully understand. The bodies a
 * report yields point into the caller's buffer and do not outlive it. */
bool ksd_kwin_parse_poll(const uint8_t *data, size_t length,
                         ksd_kwin_poll *poll);
bool ksd_kwin_parse_report(const uint8_t *data, size_t length,
                           ksd_kwin_report *report);

#endif
