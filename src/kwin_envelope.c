#include "kwin_envelope.h"

#include "protocol_io.h"

#include <string.h>

/* A cursor over the header region. Every accessor refuses rather than
 * truncating, so a malformed line cannot be partially consumed and then read
 * as though it had been understood. */
typedef struct scanner {
    const uint8_t *data;
    size_t length;
    size_t offset;
} scanner;

static bool line(scanner *scan, const uint8_t **start, size_t *size)
{
    size_t begin = scan->offset;

    while (scan->offset < scan->length && scan->data[scan->offset] != 10)
        scan->offset++;
    if (scan->offset == scan->length)
        return false;
    *start = scan->data + begin;
    *size = scan->offset - begin;
    scan->offset++;
    return true;
}

static bool token(const uint8_t **cursor, const uint8_t *end,
                  const uint8_t **start, size_t *size)
{
    const uint8_t *scan = *cursor;

    if (scan == end || *scan == 32)
        return false;
    *start = scan;
    while (scan != end && *scan != 32)
        scan++;
    *size = (size_t)(scan - *start);
    if (scan != end)
        scan++;
    *cursor = scan;
    return true;
}

static bool keyword_is(const uint8_t *start, size_t size, const char *name)
{
    size_t length = strlen(name);
    return size == length && memcmp(start, name, length) == 0;
}

/* No leading zeros, and no value that would not survive the round trip. Two
 * spellings for one number is how a length check and a length use come to
 * disagree. */
static bool decimal(const uint8_t *start, size_t size, uint32_t limit,
                    uint32_t *value)
{
    uint32_t parsed = 0u;

    if (size == 0u || size > 10u)
        return false;
    if (size > 1u && start[0] == 48)
        return false;
    for (size_t index = 0u; index < size; index++) {
        if (start[index] < 48 || start[index] > 57)
            return false;
        if (parsed > (UINT32_MAX - (uint32_t)(start[index] - 48)) / 10u)
            return false;
        parsed = parsed * 10u + (uint32_t)(start[index] - 48);
    }
    if (parsed > limit)
        return false;
    *value = parsed;
    return true;
}

/* Lowercase only, and an exact width. A hex token of the wrong length is a
 * different identifier, not a short one. */
static bool hex(const uint8_t *start, size_t size, size_t expected,
                char *destination)
{
    if (size != expected)
        return false;
    for (size_t index = 0u; index < size; index++) {
        uint8_t byte = start[index];
        bool digit = byte >= 48 && byte <= 57;
        bool lower = byte >= 97 && byte <= 102;

        if (!digit && !lower)
            return false;
        destination[index] = (char)byte;
    }
    destination[size] = 0;
    return true;
}

static bool magic_ok(scanner *scan)
{
    const uint8_t *start;
    size_t size;

    return line(scan, &start, &size) && keyword_is(start, size, "KSK1");
}

bool ksd_kwin_parse_poll(const uint8_t *data, size_t length,
                         ksd_kwin_poll *poll)
{
    scanner scan = { data, length, 0u };
    bool have_generation = false;
    bool have_lane = false;
    bool have_round_trip = false;
    bool have_lost = false;
    const uint8_t *start;
    size_t size;

    if (data == NULL || poll == NULL || length > KSD_KWIN_MAX_HEADER_BYTES
        || !magic_ok(&scan))
        return false;
    memset(poll, 0, sizeof(*poll));

    while (line(&scan, &start, &size)) {
        const uint8_t *cursor = start;
        const uint8_t *end = start + size;
        const uint8_t *word;
        size_t word_size;

        if (!token(&cursor, end, &word, &word_size))
            return false;
        if (keyword_is(word, word_size, "end")) {
            /* A poll carries no body, so nothing may follow the header. */
            return cursor == end && scan.offset == scan.length
                && have_generation && have_lane && have_round_trip
                && have_lost;
        }
        if (!token(&cursor, end, &start, &size) || cursor != end)
            return false;
        if (keyword_is(word, word_size, "gen")) {
            if (have_generation
                || !hex(start, size, KSD_KWIN_GENERATION_HEX,
                        poll->generation))
                return false;
            have_generation = true;
        } else if (keyword_is(word, word_size, "lane")) {
            if (have_lane)
                return false;
            if (keyword_is(start, size, "fast"))
                poll->lane = KSD_KWIN_LANE_FAST;
            else if (keyword_is(start, size, "slow"))
                poll->lane = KSD_KWIN_LANE_SLOW;
            else
                return false;
            have_lane = true;
        } else if (keyword_is(word, word_size, "rtt")) {
            if (have_round_trip
                || !decimal(start, size, 600000u, &poll->round_trip_ms))
                return false;
            have_round_trip = true;
        } else if (keyword_is(word, word_size, "lost")) {
            if (have_lost || !decimal(start, size, 65535u, &poll->lost))
                return false;
            have_lost = true;
        } else {
            /* Unknown keyword is fatal. Skipping it would let a newer script
             * silently lose a field this daemon never learned to read. */
            return false;
        }
    }
    return false;
}

bool ksd_kwin_parse_report(const uint8_t *data, size_t length,
                           ksd_kwin_report *report)
{
    scanner scan = { data, length, 0u };
    bool have_generation = false;
    uint64_t declared = 0u;
    const uint8_t *start;
    size_t size;

    if (data == NULL || report == NULL || !magic_ok(&scan))
        return false;
    memset(report, 0, sizeof(*report));

    while (line(&scan, &start, &size)) {
        const uint8_t *cursor = start;
        const uint8_t *end = start + size;
        const uint8_t *word;
        size_t word_size;

        if (scan.offset > KSD_KWIN_MAX_HEADER_BYTES
            || !token(&cursor, end, &word, &word_size))
            return false;

        if (keyword_is(word, word_size, "end")) {
            size_t remaining = scan.length - scan.offset;
            size_t consumed = 0u;

            /* Exactly, not at most. Trailing bytes after the last body are the
             * shape a smuggled record takes. */
            if (cursor != end || report->count == 0u
                || declared != (uint64_t)remaining)
                return false;
            for (size_t index = 0u; index < report->count; index++) {
                report->done[index].body = scan.data + scan.offset + consumed;
                consumed += report->done[index].body_length;
            }
            return have_generation;
        }
        if (keyword_is(word, word_size, "gen")) {
            if (!token(&cursor, end, &start, &size) || cursor != end
                || have_generation
                || !hex(start, size, KSD_KWIN_GENERATION_HEX,
                        report->generation))
                return false;
            have_generation = true;
            continue;
        }
        if (!keyword_is(word, word_size, "done"))
            return false;
        if (report->count == KSD_KWIN_MAX_DONE_PER_REPORT)
            return false;

        ksd_kwin_done *done = &report->done[report->count];
        uint32_t body_length = 0u;

        if (!token(&cursor, end, &start, &size)
            || !hex(start, size, KSD_KWIN_SEQ_HEX, done->sequence)
            || !token(&cursor, end, &start, &size)
            || !decimal(start, size, 255u, &done->status)
            || !token(&cursor, end, &start, &size)
            || !decimal(start, size, KSD_MAX_TEXT_BYTES, &body_length)
            || cursor != end)
            return false;
        done->body_length = body_length;
        declared += body_length;
        if (declared > KSD_MAX_TEXT_BYTES)
            return false;
        report->count++;
    }
    return false;
}

/* Fixed-width lowercase hex, the only spelling the parsers accept. Written out
 * rather than with a printf format so there is one definition of the width and
 * no chance of a %x that disagrees with it. */
static bool append_hex(ksd_buffer *out, uint64_t value, size_t digits)
{
    static const char alphabet[] = "0123456789abcdef";
    char text[16];

    if (digits > sizeof(text))
        return false;
    for (size_t index = 0u; index < digits; index++) {
        unsigned shift = (unsigned)((digits - 1u - index) * 4u);
        text[index] = alphabet[(value >> shift) & 0xfu];
    }
    return ksd_buffer_bytes(out, text, digits);
}

/* No leading zeros: two spellings for one number is how a length check and a
 * length use come to disagree, which is the rule the parsers enforce and so
 * the rule this must obey. */
static bool append_decimal(ksd_buffer *out, uint32_t value)
{
    char text[16];
    size_t length = 0u;

    if (value == 0u)
        return ksd_buffer_bytes(out, "0", 1u);
    while (value != 0u && length < sizeof(text)) {
        text[length++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    if (value != 0u)
        return false;
    for (size_t index = 0u; index < length / 2u; index++) {
        char swap = text[index];

        text[index] = text[length - 1u - index];
        text[length - 1u - index] = swap;
    }
    return ksd_buffer_bytes(out, text, length);
}

static bool valid_generation(const char *generation)
{
    if (generation == NULL)
        return false;
    for (size_t index = 0u; index < KSD_KWIN_GENERATION_HEX; index++) {
        char digit = generation[index];

        if (!((digit >= '0' && digit <= '9')
              || (digit >= 'a' && digit <= 'f')))
            return false;
    }
    return generation[KSD_KWIN_GENERATION_HEX] == '\0';
}

static bool valid_sequence(const char *sequence)
{
    if (sequence == NULL)
        return false;
    for (size_t index = 0u; index < KSD_KWIN_SEQ_HEX; index++) {
        char digit = sequence[index];

        if (!((digit >= '0' && digit <= '9')
              || (digit >= 'a' && digit <= 'f')))
            return false;
    }
    return sequence[KSD_KWIN_SEQ_HEX] == '\0';
}

bool ksd_kwin_format_poll_reply(const char *generation, ksd_kwin_lane lane,
                                uint32_t idle_ms,
                                const ksd_kwin_dispatch *jobs, size_t count,
                                ksd_buffer *out)
{
    const char *lane_name = lane == KSD_KWIN_LANE_FAST ? "fast" : "slow";
    size_t limit = lane == KSD_KWIN_LANE_FAST
        ? KSD_KWIN_JOBS_PER_REPLY_FAST : KSD_KWIN_JOBS_PER_REPLY_SLOW;
    bool ok;

    if (out == NULL || !valid_generation(generation))
        return false;
    /* A lane that takes no jobs cannot be replied to, and a batch larger than
     * the lane's cap is refused here rather than sent: the cap is what bounds
     * one uninterruptible callback inside the compositor, and a serialiser
     * that ignored it would route around the whole lane split. */
    if (lane != KSD_KWIN_LANE_FAST && lane != KSD_KWIN_LANE_SLOW)
        return false;
    if (count > limit)
        return false;
    for (size_t index = 0u; index < count; index++) {
        if (!valid_sequence(jobs[index].sequence))
            return false;
        if (jobs[index].body_length != 0u && jobs[index].body == NULL)
            return false;
    }

    ok = ksd_buffer_bytes(out, "KSK1\n", 5u)
        && ksd_buffer_bytes(out, "gen ", 4u)
        && ksd_buffer_bytes(out, generation, KSD_KWIN_GENERATION_HEX)
        && ksd_buffer_bytes(out, "\n", 1u)
        && ksd_buffer_bytes(out, "lane ", 5u)
        && ksd_buffer_bytes(out, lane_name, strlen(lane_name))
        && ksd_buffer_bytes(out, "\n", 1u)
        && ksd_buffer_bytes(out, "idle ", 5u)
        && append_decimal(out, idle_ms)
        && ksd_buffer_bytes(out, "\n", 1u);
    for (size_t index = 0u; ok && index < count; index++) {
        ok = ksd_buffer_bytes(out, "job ", 4u)
            && ksd_buffer_bytes(out, jobs[index].sequence, KSD_KWIN_SEQ_HEX)
            && ksd_buffer_bytes(out, " ", 1u)
            && append_hex(out, jobs[index].opcode, 4u)
            && ksd_buffer_bytes(out, " ", 1u)
            && append_decimal(out, jobs[index].budget_ms)
            && ksd_buffer_bytes(out, " ", 1u)
            && append_decimal(out, jobs[index].body_length)
            && ksd_buffer_bytes(out, "\n", 1u);
    }
    ok = ok && ksd_buffer_bytes(out, "end\n", 4u);
    /* The bodies follow the terminator in the order their lines declared them,
     * and their declared lengths sum to exactly the remainder. */
    for (size_t index = 0u; ok && index < count; index++) {
        if (jobs[index].body_length == 0u)
            continue;
        ok = ksd_buffer_bytes(out, jobs[index].body,
                              jobs[index].body_length);
    }
    return ok;
}

bool ksd_kwin_format_report_ack(const char *generation, ksd_buffer *out)
{
    if (out == NULL || !valid_generation(generation))
        return false;
    return ksd_buffer_bytes(out, "KSK1\n", 5u)
        && ksd_buffer_bytes(out, "gen ", 4u)
        && ksd_buffer_bytes(out, generation, KSD_KWIN_GENERATION_HEX)
        && ksd_buffer_bytes(out, "\nack\nend\n", 9u);
}
