#include "kwin_envelope.h"
#include "kwin_wire.h"

#include <string.h>

/* A unique bus name is what the daemon compares against, so the shape it will
 * accept is pinned here rather than left to whatever the bus happened to send.
 * ":" then at least one digit, then one or more ".digits" groups: ":1.5",
 * ":1.4242". A well-known name like "org.kde.KWin" is deliberately not a
 * unique name and is refused, because a well-known name can be handed from one
 * connection to another and the whole point of comparing the unique name is
 * that it cannot. */
static bool unique_name_valid(const char *name)
{
    size_t index = 1u;
    size_t digits = 0u;
    size_t groups = 0u;

    if (name == NULL || name[0] != ':')
        return false;
    while (name[index] != '\0') {
        char c = name[index];

        if (c == '.') {
            if (digits == 0u)
                return false;
            digits = 0u;
            groups++;
        } else if (c >= '0' && c <= '9') {
            digits++;
        } else {
            return false;
        }
        index++;
    }
    /* At least one dot, and no trailing one. */
    return groups >= 1u && digits > 0u;
}

static bool generation_valid(const char *generation)
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

bool ksd_kwin_peer_allowed(const char *expected_unique,
                           const char *sender_unique,
                           uid_t expected_uid, uid_t sender_uid,
                           const char *expected_generation,
                           const char *sender_generation)
{
    if (!unique_name_valid(expected_unique)
        || !unique_name_valid(sender_unique))
        return false;
    if (!generation_valid(expected_generation)
        || !generation_valid(sender_generation))
        return false;
    if (expected_uid != sender_uid)
        return false;
    if (strcmp(expected_unique, sender_unique) != 0)
        return false;
    return memcmp(expected_generation, sender_generation,
                  KSD_KWIN_GENERATION_HEX) == 0;
}
