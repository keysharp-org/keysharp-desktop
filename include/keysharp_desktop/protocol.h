#ifndef KEYSHARP_DESKTOP_PROTOCOL_H
#define KEYSHARP_DESKTOP_PROTOCOL_H

#include <stdint.h>

#define KSD_PRODUCT_VERSION "0.1.0"

#define KSD_PROTOCOL_MAJOR 1u
#define KSD_PROTOCOL_MINOR 2u
#define KSD_PROTOCOL_VERSION "1.2"
#define KSD_PROTOCOL_LABEL "keysharp-desktop/session-v1"
#define KSD_HANDSHAKE_PREFIX "KSDP/" KSD_PROTOCOL_VERSION

#define KSD_CAP_SCREEN_CAPTURE       0x00000001u
#define KSD_CAP_WINDOW_MONITORING    0x00000002u
#define KSD_CAP_WINDOW_CONTROL       0x00000004u
#define KSD_CAP_AUDIO_CAPTURE        0x00000008u
#define KSD_CAP_CAMERA_CAPTURE       0x00000010u
#define KSD_CAP_CLIPBOARD_MONITORING 0x00000020u
#define KSD_CAP_ALL (KSD_CAP_SCREEN_CAPTURE | KSD_CAP_WINDOW_MONITORING \
    | KSD_CAP_WINDOW_CONTROL | KSD_CAP_AUDIO_CAPTURE \
    | KSD_CAP_CAMERA_CAPTURE | KSD_CAP_CLIPBOARD_MONITORING)

/* Stable cross-component storage scopes, distinct from wire capabilities. */
#define KSP_SCOPE_INPUT_MONITORING     0x00000001u
#define KSP_SCOPE_INPUT_CONTROL        0x00000002u
#define KSP_SCOPE_WINDOW_MONITORING    0x00000004u
#define KSP_SCOPE_WINDOW_CONTROL       0x00000008u
#define KSP_SCOPE_SCREEN_CAPTURE       0x00000010u
#define KSP_SCOPE_AUDIO_CAPTURE        0x00000020u
#define KSP_SCOPE_CAMERA_CAPTURE       0x00000040u
#define KSP_SCOPE_CLIPBOARD_MONITORING 0x00000080u

#define KSD_APP_IDENTITY_DOMAIN "org.keysharp.app-identity-v1"
#define KSD_APP_IDENTITY_PATH_KIND "path"
#define KSD_APP_IDENTITY_SHA256_KIND "sha256"

#define KSD_DEFAULT_SOCKET_SUFFIX "keysharp-desktop/keysharp-desktop.sock"
#ifndef KSD_AUTHORITY_SOCKET
#define KSD_AUTHORITY_SOCKET "/run/keysharp-desktop/authority.sock"
#endif
#define KSD_POLKIT_TIMEOUT_SECONDS 120u
#define KSD_AUTHORITY_TIMEOUT_SECONDS 125u
#ifndef KSD_HANDSHAKE_TIMEOUT_SECONDS
#define KSD_HANDSHAKE_TIMEOUT_SECONDS 130u
#endif

#define KSD_RESPONSE_OK    0x00u
#define KSD_RESPONSE_ERROR 0x01u

#define KSD_AUTH_MAGIC 0x3141444bu /* "KDA1" in little-endian byte order. */
#define KSD_AUTH_OP_CHECK      1u
#define KSD_AUTH_OP_REVOKE_UID 2u
#define KSD_AUTH_OP_INFO       3u
#define KSD_AUTH_OP_LIST_UID   4u
#define KSD_AUTH_OP_REVOKE     5u
/* LIST_UID returns a record count in granted_capabilities followed by that
 * many internal grant records. REVOKE is followed by one NUL-terminated,
 * fixed-width 65-byte application hash and uses capabilities as its scope.
 * Both are additive opcodes; the 16-byte request and response remain fixed. */
#define KSD_AUTH_FLAG_INTERACTIVE 0x0001u

#define KSD_AUTH_STATUS_GRANTED 0u
#define KSD_AUTH_STATUS_DENIED  1u
#define KSD_AUTH_STATUS_ERROR   2u

typedef struct ksd_authority_request {
    uint32_t magic;
    uint16_t major;
    uint16_t minor;
    uint16_t operation;
    uint16_t flags;
    uint32_t capabilities;
} ksd_authority_request;

typedef struct ksd_authority_response {
    uint32_t magic;
    uint16_t major;
    uint16_t minor;
    uint16_t status;
    uint16_t reserved;
    uint32_t granted_capabilities;
} ksd_authority_response;

#endif
