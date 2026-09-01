#ifndef KEYSHARP_DESKTOP_BACKEND_PROTOCOL_H
#define KEYSHARP_DESKTOP_BACKEND_PROTOCOL_H

#include <stdint.h>

#define KSD_SYSTEM_SOCKET "/run/keysharp-desktop/keysharp-desktop.sock"
#define KSD_BACKEND_REGISTRATION_SIZE 16u
#define KSD_BACKEND_REGISTRATION_VERSION 1u
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
