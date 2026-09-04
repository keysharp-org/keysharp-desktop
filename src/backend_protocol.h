#ifndef KEYSHARP_DESKTOP_BACKEND_PROTOCOL_H
#define KEYSHARP_DESKTOP_BACKEND_PROTOCOL_H

#include <stdint.h>

#define KSD_SYSTEM_SOCKET "/run/keysharp-desktop/keysharp-desktop.sock"
/* v2 carries the operations the session daemon believes it can serve, so a
 * daemon whose compositor is missing a capability can say so instead of the
 * authority guessing from a static table. The authority may only WITHHOLD from
 * what the backend statically supports, never add: a daemon narrows its own
 * capability and can never claim one it does not have.
 *
 * Layout: magic(4) version(2) flags(2) backend(4) reserved(4) mask(8)
 * reserved(8). The record simply grew; this project is pre-alpha and both ends
 * ship together, so there is no v1 left to speak. */
#define KSD_BACKEND_REGISTRATION_SIZE 32u
#define KSD_BACKEND_REGISTRATION_VERSION 2u
/* The daemon is handing over a socket the authority may call back on. Only
 * KWin needs it, because a KWin script cannot be reached on the session bus
 * the way a shell extension can. */
#define KSD_BACKEND_FLAG_PROVIDER_FD 0x0001u
#define KSD_BACKEND_FLAGS_ALL KSD_BACKEND_FLAG_PROVIDER_FD
#define KSD_BACKEND_REGISTRATION_TIMEOUT_MS 5000u
#define KSD_BACKEND_ACK_ACCEPTED 0u
#define KSD_BACKEND_ACK_REJECTED 1u

static const uint8_t ksd_backend_registration_magic[4] = {
    (uint8_t)'K', (uint8_t)'S', (uint8_t)'D', (uint8_t)'B',
};

static const uint8_t ksd_backend_ack_magic[4] = {
    (uint8_t)'K', (uint8_t)'S', (uint8_t)'D', (uint8_t)'A',
};

#endif
