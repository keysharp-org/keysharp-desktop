#include "keysharp_desktop/client.h"

#include "protocol.h"
#include "protocol_io.h"
#include "permission_domain.h"
#include "client_status.h"

#include <errno.h>
#include <keysharp_permissions/permissions.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define KSD_CLIENT_PATH_CAPACITY 4096u
#define KSD_CLIENT_MAX_RESPONSE (KSD_MAX_CAPTURE_BYTES + 64u)

_Static_assert(KSD_SCOPE_INPUT_MONITORING == KSP_SCOPE_INPUT_MONITORING,
               "input monitoring scope drifted");
_Static_assert(KSD_SCOPE_INPUT_CONTROL == KSP_SCOPE_INPUT_CONTROL,
               "input control scope drifted");
_Static_assert(KSD_SCOPE_WINDOW_MONITORING == KSP_SCOPE_WINDOW_MONITORING,
               "window monitoring scope drifted");
_Static_assert(KSD_SCOPE_WINDOW_CONTROL == KSP_SCOPE_WINDOW_CONTROL,
               "window control scope drifted");
_Static_assert(KSD_SCOPE_SCREEN_CAPTURE == KSP_SCOPE_SCREEN_CAPTURE,
               "screen capture scope drifted");
_Static_assert(KSD_SCOPE_AUDIO_CAPTURE == KSP_SCOPE_AUDIO_CAPTURE,
               "audio capture scope drifted");
_Static_assert(KSD_SCOPE_CAMERA_CAPTURE == KSP_SCOPE_CAMERA_CAPTURE,
               "camera capture scope drifted");
_Static_assert(KSD_SCOPE_CLIPBOARD_MONITORING
                   == KSP_SCOPE_CLIPBOARD_MONITORING,
               "clipboard monitoring scope drifted");
_Static_assert(offsetof(ksd_error, message) == 16u,
               "ksd_error common layout drifted");
_Static_assert(offsetof(ksd_service_info, available_operations) == 16u,
               "ksd_service_info common layout drifted");
_Static_assert(offsetof(ksd_permission_entry, reserved64) == 4184u,
               "ksd_permission_entry common layout drifted");
_Static_assert(offsetof(ksd_permission_revoke, pid) == 16u,
               "ksd_permission_revoke common layout drifted");

struct ksd_connection {
    pthread_mutex_t mutex;
    int descriptor;
    uint64_t next_request_id;
    uint32_t role;
    uint32_t granted_scopes;
    uint64_t available_operations;
    uint32_t backend;
    uint32_t timeout_ms;
    uint64_t request_deadline_ms;
    uint16_t subscription_opcode;
};

typedef struct ksd_client_response {
    ksd_frame frame;
    uint32_t status;
    uint32_t detail;
    const uint8_t *tail;
    uint32_t tail_length;
} ksd_client_response;

static const uint8_t client_magic[4] = {
    KSD_FRAME_MAGIC_0, KSD_FRAME_MAGIC_1,
    KSD_FRAME_MAGIC_2, KSD_FRAME_MAGIC_3,
};

static bool bytes_zero(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    for (size_t index = 0u; index < length; index++)
        if (bytes[index] != 0u)
            return false;
    return true;
}

static bool valid_bytes_output(const ksd_bytes *bytes)
{
    return bytes != NULL && bytes->struct_size >= sizeof(*bytes)
        && bytes->reserved0 == 0u
        && bytes_zero(bytes->reserved, sizeof(bytes->reserved))
        && bytes->data == NULL && bytes->length == 0u;
}

static bool valid_string_output(const ksd_string *string)
{
    return string != NULL && string->struct_size >= sizeof(*string)
        && string->reserved0 == 0u
        && bytes_zero(string->reserved, sizeof(string->reserved))
        && string->data == NULL && string->length == 0u;
}

static bool valid_string_list_output(const ksd_string_list *list)
{
    return list != NULL && list->struct_size >= sizeof(*list)
        && list->reserved0 == 0u
        && bytes_zero(list->reserved, sizeof(list->reserved))
        && list->items == NULL && list->count == 0u;
}

static bool valid_capture_output(const ksd_capture *capture)
{
    return capture != NULL && capture->struct_size >= sizeof(*capture)
        && capture->format == 0u && capture->reserved0 == 0u
        && capture->width == 0u && capture->height == 0u
        && capture->stride == 0u
        && bytes_zero(capture->reserved, sizeof(capture->reserved))
        && valid_bytes_output(&capture->data);
}

static bool valid_point_output(const ksd_point *point)
{
    return point != NULL && point->struct_size >= sizeof(*point)
        && point->x == 0 && point->y == 0 && point->reserved0 == 0u
        && bytes_zero(point->reserved, sizeof(point->reserved));
}

static bool valid_rectangle_output(const ksd_rectangle *rectangle)
{
    return rectangle != NULL
        && rectangle->struct_size >= sizeof(*rectangle)
        && rectangle->x == 0 && rectangle->y == 0
        && rectangle->width == 0u && rectangle->height == 0u
        && rectangle->reserved0 == 0u
        && bytes_zero(rectangle->reserved, sizeof(rectangle->reserved));
}

static bool valid_service_info_output(const ksd_service_info *info)
{
    return info != NULL && info->struct_size >= sizeof(*info)
        && info->abi_major == 0u && info->abi_minor == 0u
        && info->granted_scopes == 0u && info->available_operations == 0u
        && info->backend == KSD_BACKEND_NONE && info->reserved0 == 0u
        && bytes_zero(info->reserved, sizeof(info->reserved));
}

static bool valid_window_event_output(const ksd_window_event *event)
{
    return event != NULL && event->struct_size >= sizeof(*event)
        && event->kind == 0u && event->reserved0 == 0u
        && bytes_zero(event->reserved, sizeof(event->reserved))
        && valid_string_output(&event->window_json);
}

static bool valid_clipboard_event_output(const ksd_clipboard_event *event)
{
    return event != NULL && event->struct_size >= sizeof(*event)
        && event->reserved0 == 0u
        && bytes_zero(event->reserved, sizeof(event->reserved))
        && valid_string_output(&event->text)
        && valid_string_list_output(&event->mimetypes);
}

static bool valid_error(const ksd_error *error)
{
    return error == NULL || error->struct_size >= sizeof(*error);
}

static void reset_error(ksd_error *error)
{
    if (error == NULL || error->struct_size < sizeof(*error))
        return;
    uint32_t size = error->struct_size;
    memset(error, 0, sizeof(*error));
    error->struct_size = size;
}

static void set_error(ksd_error *error, uint32_t detail, int system_error,
                      const char *message)
{
    reset_error(error);
    if (error == NULL || error->struct_size < sizeof(*error))
        return;
    error->detail = detail;
    error->system_error = system_error;
    if (message != NULL)
        (void)snprintf(error->message, sizeof(error->message), "%s", message);
}

static ksd_status invalid_argument(ksd_error *error, const char *message)
{
    set_error(error, 0u, EINVAL, message);
    return KSD_STATUS_INVALID_REQUEST;
}

static ksd_status system_failure(ksd_connection *connection,
                                 ksd_error *error, const char *message)
{
    int saved_errno = errno == 0 ? EIO : errno;
    if (connection != NULL && connection->descriptor >= 0) {
        close(connection->descriptor);
        connection->descriptor = -1;
    }
    set_error(error, 0u, saved_errno, message);
    return ksd_status_for_system_error(saved_errno);
}

uint32_t ksd_client_abi_major(void) { return KSD_CLIENT_ABI_MAJOR; }
uint32_t ksd_client_abi_minor(void) { return KSD_CLIENT_ABI_MINOR; }
const char *ksd_client_product_name(void) { return KSD_PRODUCT_NAME; }
const char *ksd_client_product_version(void) { return KSD_PRODUCT_VERSION; }

const char *ksd_status_name(ksd_status status)
{
    switch (status) {
        case KSD_STATUS_OK: return "ok";
        case KSD_STATUS_DENIED: return "denied";
        case KSD_STATUS_UNSUPPORTED: return "unsupported";
        case KSD_STATUS_INVALID_REQUEST: return "invalid-request";
        case KSD_STATUS_UNAVAILABLE: return "unavailable";
        case KSD_STATUS_BUSY: return "busy";
        case KSD_STATUS_NOT_FOUND: return "not-found";
        case KSD_STATUS_RESOURCE_EXHAUSTED: return "resource-exhausted";
        case KSD_STATUS_TIMEOUT: return "timeout";
        case KSD_STATUS_CANCELLED: return "cancelled";
        case KSD_STATUS_REVOKED: return "revoked";
        case KSD_STATUS_INTERNAL: return "internal";
        default: return "unknown";
    }
}

const char *ksd_backend_name(ksd_backend backend)
{
    switch (backend) {
        case KSD_BACKEND_NONE: return "none";
        case KSD_BACKEND_KWIN: return "kwin";
        case KSD_BACKEND_GNOME: return "gnome";
        case KSD_BACKEND_CINNAMON: return "cinnamon";
        case KSD_BACKEND_GENERIC: return "generic";
        default: return "unknown";
    }
}

const char *ksd_scope_name(ksd_permission_scopes scope)
{
    switch (scope) {
        case KSD_SCOPE_INPUT_MONITORING: return "input-monitoring";
        case KSD_SCOPE_INPUT_CONTROL: return "input-control";
        case KSD_SCOPE_WINDOW_MONITORING: return "window-monitoring";
        case KSD_SCOPE_WINDOW_CONTROL: return "window-control";
        case KSD_SCOPE_SCREEN_CAPTURE: return "screen-capture";
        case KSD_SCOPE_AUDIO_CAPTURE: return "audio-capture";
        case KSD_SCOPE_CAMERA_CAPTURE: return "camera-capture";
        case KSD_SCOPE_CLIPBOARD_MONITORING:
            return "clipboard-monitoring";
        default: return NULL;
    }
}

void ksd_error_init(ksd_error *error)
{
    if (error != NULL) {
        memset(error, 0, sizeof(*error));
        error->struct_size = sizeof(*error);
    }
}

void ksd_connect_options_init(ksd_connect_options *options)
{
    if (options != NULL) {
        memset(options, 0, sizeof(*options));
        options->struct_size = sizeof(*options);
        options->role = KSD_ROLE_RPC;
        options->authorization_mode = KSD_AUTH_CHECK;
        options->timeout_ms = KSD_DEFAULT_REQUEST_TIMEOUT_MS;
    }
}

void ksd_service_info_init(ksd_service_info *info)
{
    if (info != NULL) {
        memset(info, 0, sizeof(*info));
        info->struct_size = sizeof(*info);
    }
}

void ksd_bytes_init(ksd_bytes *bytes)
{
    if (bytes != NULL) {
        memset(bytes, 0, sizeof(*bytes));
        bytes->struct_size = sizeof(*bytes);
    }
}

void ksd_string_init(ksd_string *string)
{
    if (string != NULL) {
        memset(string, 0, sizeof(*string));
        string->struct_size = sizeof(*string);
    }
}

void ksd_string_list_init(ksd_string_list *list)
{
    if (list != NULL) {
        memset(list, 0, sizeof(*list));
        list->struct_size = sizeof(*list);
    }
}

void ksd_capture_init(ksd_capture *capture)
{
    if (capture != NULL) {
        memset(capture, 0, sizeof(*capture));
        capture->struct_size = sizeof(*capture);
        ksd_bytes_init(&capture->data);
    }
}

void ksd_point_init(ksd_point *point)
{
    if (point != NULL) {
        memset(point, 0, sizeof(*point));
        point->struct_size = sizeof(*point);
    }
}

void ksd_rectangle_init(ksd_rectangle *rectangle)
{
    if (rectangle != NULL) {
        memset(rectangle, 0, sizeof(*rectangle));
        rectangle->struct_size = sizeof(*rectangle);
    }
}

void ksd_permission_entry_init(ksd_permission_entry *entry)
{
    if (entry != NULL) {
        memset(entry, 0, sizeof(*entry));
        entry->struct_size = sizeof(*entry);
    }
}

void ksd_permission_revoke_init(ksd_permission_revoke *revoke)
{
    if (revoke != NULL) {
        memset(revoke, 0, sizeof(*revoke));
        revoke->struct_size = sizeof(*revoke);
    }
}

void ksd_window_event_init(ksd_window_event *event)
{
    if (event != NULL) {
        memset(event, 0, sizeof(*event));
        event->struct_size = sizeof(*event);
        ksd_string_init(&event->window_json);
    }
}

void ksd_clipboard_event_init(ksd_clipboard_event *event)
{
    if (event != NULL) {
        memset(event, 0, sizeof(*event));
        event->struct_size = sizeof(*event);
        ksd_string_init(&event->text);
        ksd_string_list_init(&event->mimetypes);
    }
}

void ksd_bytes_clear(ksd_bytes *bytes)
{
    if (bytes == NULL || bytes->struct_size < sizeof(*bytes))
        return;
    uint32_t size = bytes->struct_size;
    free(bytes->data);
    memset(bytes, 0, sizeof(*bytes));
    bytes->struct_size = size;
}

void ksd_string_clear(ksd_string *string)
{
    if (string == NULL || string->struct_size < sizeof(*string))
        return;
    uint32_t size = string->struct_size;
    free(string->data);
    memset(string, 0, sizeof(*string));
    string->struct_size = size;
}

void ksd_string_list_clear(ksd_string_list *list)
{
    if (list == NULL || list->struct_size < sizeof(*list))
        return;
    uint32_t size = list->struct_size;
    for (size_t index = 0u; index < list->count; index++)
        ksd_string_clear(&list->items[index]);
    free(list->items);
    memset(list, 0, sizeof(*list));
    list->struct_size = size;
}

void ksd_capture_clear(ksd_capture *capture)
{
    if (capture == NULL || capture->struct_size < sizeof(*capture))
        return;
    uint32_t size = capture->struct_size;
    ksd_bytes_clear(&capture->data);
    memset(capture, 0, sizeof(*capture));
    capture->struct_size = size;
    ksd_bytes_init(&capture->data);
}

void ksd_window_event_clear(ksd_window_event *event)
{
    if (event == NULL || event->struct_size < sizeof(*event))
        return;
    uint32_t size = event->struct_size;
    ksd_string_clear(&event->window_json);
    memset(event, 0, sizeof(*event));
    event->struct_size = size;
    ksd_string_init(&event->window_json);
}

void ksd_clipboard_event_clear(ksd_clipboard_event *event)
{
    if (event == NULL || event->struct_size < sizeof(*event))
        return;
    uint32_t size = event->struct_size;
    ksd_string_clear(&event->text);
    ksd_string_list_clear(&event->mimetypes);
    memset(event, 0, sizeof(*event));
    event->struct_size = size;
    ksd_string_init(&event->text);
    ksd_string_list_init(&event->mimetypes);
}

static bool valid_role(uint32_t role)
{
    return role == KSD_ROLE_RPC || role == KSD_ROLE_EVENT_STREAM
        || role == KSD_ROLE_AUTHORIZATION_LEASE;
}

static bool valid_status(uint32_t status)
{
    return status <= KSD_STATUS_REVOKED || status == KSD_STATUS_INTERNAL;
}

static bool set_timeouts(int descriptor, uint32_t timeout_ms)
{
    struct timeval timeout = {
        .tv_sec = (time_t)(timeout_ms / 1000u),
        .tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u),
    };
    return setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO,
                      &timeout, sizeof(timeout)) == 0
        && setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO,
                      &timeout, sizeof(timeout)) == 0;
}

static uint64_t monotonic_milliseconds(void)
{
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now) == 0
        ? (uint64_t)now.tv_sec * 1000u
            + (uint64_t)now.tv_nsec / 1000000u
        : 0u;
}

static bool set_remaining_timeout(ksd_connection *connection,
                                  uint64_t deadline)
{
    uint64_t now = monotonic_milliseconds();
    if (now == 0u || now >= deadline) {
        errno = ETIMEDOUT;
        return false;
    }
    uint64_t remaining = deadline - now;
    return set_timeouts(connection->descriptor,
        remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining);
}

static bool default_socket_path(char path[KSD_CLIENT_PATH_CAPACITY])
{
    const char *configured = getenv(KSD_SOCKET_ENV);
    int length;
    if (configured != NULL && configured[0] != '\0')
        length = snprintf(path, KSD_CLIENT_PATH_CAPACITY, "%s", configured);
    else
        length = snprintf(path, KSD_CLIENT_PATH_CAPACITY, "%s",
                          KSD_DEFAULT_SOCKET_PATH);
    return length > 0 && (size_t)length < KSD_CLIENT_PATH_CAPACITY;
}

static int connect_socket(const char *configured, uint32_t timeout_ms)
{
    char path[KSD_CLIENT_PATH_CAPACITY];
    struct sockaddr_un address;
    int descriptor;
    if (configured == NULL) {
        if (!default_socket_path(path)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        configured = path;
    }
    size_t length = strlen(configured);
    if (length == 0u || configured[0] != '/'
        || length >= sizeof(address.sun_path)) {
        errno = EINVAL;
        return -1;
    }
    descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (descriptor < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, configured, length + 1u);
    struct ucred peer;
    socklen_t peer_size = sizeof(peer);
    if (!set_timeouts(descriptor, timeout_ms)
        || connect(descriptor, (const struct sockaddr *)&address,
                   sizeof(address)) != 0
        || getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED,
                      &peer, &peer_size) != 0
        || peer_size != sizeof(peer) || peer.uid != 0u || peer.gid != 0u) {
        int saved_errno = errno;
        if (saved_errno == 0)
            saved_errno = EPERM;
        close(descriptor);
        errno = saved_errno;
        return -1;
    }
    return descriptor;
}

static uint64_t next_request_id(ksd_connection *connection)
{
    connection->next_request_id++;
    if (connection->next_request_id == 0u)
        connection->next_request_id++;
    return connection->next_request_id;
}

static bool apply_revoked_event(ksd_connection *connection,
                                const ksd_frame *frame,
                                uint32_t *revoked_scopes)
{
    if (frame->opcode != KSD_OP_SESSION_REVOKED
        || frame->flags != KSD_FLAG_EVENT || frame->request_id != 0u
        || frame->payload_length != 8u
        || ksd_decode_u32(frame->payload + 4u) != 0u)
        return false;
    uint32_t revoked = ksd_decode_u32(frame->payload);
    if (revoked == 0u)
        return false;
    connection->granted_scopes &= ~revoked;
    if (revoked_scopes != NULL)
        *revoked_scopes = revoked;
    return true;
}

static bool parse_response(const ksd_frame *frame, uint16_t opcode,
                           uint64_t request_id, ksd_client_response *response,
                           ksd_error *error)
{
    ksd_cursor cursor;
    uint32_t status;
    uint32_t detail;
    if (frame->opcode != opcode || frame->request_id != request_id
        || (frame->flags & KSD_FLAG_RESPONSE) == 0u
        || (frame->flags & KSD_FLAG_EVENT) != 0u
        || frame->payload_length < 8u)
        return false;
    ksd_cursor_init(&cursor, frame->payload, frame->payload_length);
    if (!ksd_cursor_u32(&cursor, &status)
        || !ksd_cursor_u32(&cursor, &detail) || !valid_status(status))
        return false;
    response->status = status;
    response->detail = detail;
    if (status == KSD_STATUS_OK) {
        response->tail = cursor.data + cursor.offset;
        response->tail_length = (uint32_t)(cursor.length - cursor.offset);
        reset_error(error);
        return true;
    }
    if ((frame->flags & KSD_FLAG_MORE) != 0u)
        return false;
    const char *message = NULL;
    char diagnostic[KSD_ERROR_MESSAGE_CAPACITY] = { 0 };
    if (!ksd_cursor_finished(&cursor)) {
        uint32_t length;
        const uint8_t *bytes;
        if (!ksd_cursor_u32(&cursor, &length)
            || length >= sizeof(diagnostic)
            || !ksd_cursor_bytes(&cursor, length, &bytes)
            || !ksd_cursor_finished(&cursor)
            || !ksd_utf8_valid(bytes, length, false))
            return false;
        memcpy(diagnostic, bytes, length);
        diagnostic[length] = '\0';
        message = diagnostic;
    }
    set_error(error, detail, 0, message);
    return true;
}

static void response_clear(ksd_client_response *response)
{
    if (response != NULL) {
        ksd_frame_clear(&response->frame);
        memset(response, 0, sizeof(*response));
    }
}

static ksd_status read_response_locked(ksd_connection *connection,
                                       uint16_t opcode,
                                       uint64_t request_id,
                                       ksd_client_response *response,
                                       ksd_error *error)
{
    for (;;) {
        ksd_frame frame;
        if (!set_remaining_timeout(connection,
                                   connection->request_deadline_ms))
            return system_failure(connection, error,
                                  "desktop service response timed out");
        int received = ksd_frame_read(connection->descriptor, client_magic,
            KSD_PROTOCOL_MAJOR, KSD_PROTOCOL_MINOR, KSD_CLIENT_MAX_RESPONSE,
            true, &frame);
        if (received != 1)
            return system_failure(connection, error,
                                  "desktop service response failed");
        if ((frame.flags & KSD_FLAG_EVENT) != 0u) {
            bool valid = apply_revoked_event(connection, &frame, NULL);
            ksd_frame_clear(&frame);
            if (!valid) {
                errno = EPROTO;
                return system_failure(connection, error,
                                      "desktop service sent an invalid event");
            }
            continue;
        }
        memset(response, 0, sizeof(*response));
        response->frame = frame;
        if (!parse_response(&response->frame, opcode, request_id,
                            response, error)) {
            response_clear(response);
            errno = EPROTO;
            return system_failure(connection, error,
                                  "desktop service sent an invalid response");
        }
        return response->status;
    }
}

static bool write_request_locked(ksd_connection *connection, uint16_t opcode,
                                 const void *payload, uint32_t payload_length,
                                 uint64_t request_id)
{
    const uint8_t *cursor = payload;
    uint32_t offset = 0u;

    if (payload_length > KSD_MAX_REQUEST_TOTAL_PAYLOAD
        || (payload_length != 0u && cursor == NULL)) {
        errno = EINVAL;
        return false;
    }
    do {
        uint32_t remaining = payload_length - offset;
        bool more = remaining > KSD_MAX_REQUEST_PAYLOAD;
        ksd_frame request = {
            .magic = {
                KSD_FRAME_MAGIC_0, KSD_FRAME_MAGIC_1,
                KSD_FRAME_MAGIC_2, KSD_FRAME_MAGIC_3,
            },
            .major = KSD_PROTOCOL_MAJOR,
            .minor = KSD_PROTOCOL_MINOR,
            .opcode = opcode,
            .flags = (uint16_t)(more ? KSD_FLAG_MORE : 0u),
            .payload_length = more ? KSD_MAX_REQUEST_PAYLOAD : remaining,
            .request_id = request_id,
            .payload = cursor == NULL ? NULL : (uint8_t *)(cursor + offset),
        };
        if (!set_remaining_timeout(connection,
                                   connection->request_deadline_ms)
            || !ksd_frame_write(connection->descriptor, &request))
            return false;
        offset += request.payload_length;
    } while (offset < payload_length);
    return true;
}

static ksd_status request_locked(ksd_connection *connection, uint16_t opcode,
                                 const void *payload, uint32_t payload_length,
                                 ksd_client_response *response,
                                 ksd_error *error)
{
    uint64_t request_id = next_request_id(connection);
    if (connection->descriptor < 0) {
        errno = ENOTCONN;
        return system_failure(connection, error,
                              "desktop service connection is closed");
    }
    uint64_t now = monotonic_milliseconds();
    if (now == 0u || now > UINT64_MAX - connection->timeout_ms) {
        errno = EIO;
        return system_failure(connection, error,
                              "desktop request clock failed");
    }
    connection->request_deadline_ms = now + connection->timeout_ms;
    if (!set_remaining_timeout(connection, connection->request_deadline_ms))
        return system_failure(connection, error,
                              "desktop service request timed out");
    if (!write_request_locked(connection, opcode, payload, payload_length,
                              request_id))
        return system_failure(connection, error,
                              "desktop service request failed");
    return read_response_locked(connection, opcode, request_id,
                                response, error);
}

static ksd_status request(ksd_connection *connection, uint16_t opcode,
                          const void *payload, uint32_t payload_length,
                          ksd_client_response *response, ksd_error *error)
{
    if (connection == NULL || response == NULL || !valid_error(error))
        return invalid_argument(error, "invalid desktop client argument");
    pthread_mutex_lock(&connection->mutex);
    ksd_status status = request_locked(connection, opcode, payload,
                                       payload_length, response, error);
    pthread_mutex_unlock(&connection->mutex);
    return status;
}

static bool parse_hello_tail(const uint8_t *tail, uint32_t tail_length,
                             uint32_t *granted, uint64_t *operations,
                             uint32_t *backend)
{
    ksd_cursor cursor;
    uint32_t reserved0;
    uint32_t reserved1;
    ksd_cursor_init(&cursor, tail, tail_length);
    if (!ksd_cursor_u32(&cursor, granted)
        || !ksd_cursor_u32(&cursor, &reserved0)
        || !ksd_cursor_u64(&cursor, operations)
        || !ksd_cursor_u32(&cursor, backend)
        || !ksd_cursor_u32(&cursor, &reserved1)
        || !ksd_cursor_finished(&cursor)
        || reserved0 != 0u || reserved1 != 0u)
        return false;
    *granted &= (uint32_t)KSD_DESKTOP_ACCEPTED_SCOPES;
    return true;
}

ksd_status ksd_connect(const ksd_connect_options *options,
                       ksd_connection **connection,
                       ksd_service_info *info, ksd_error *error)
{
    uint8_t payload[16] = { 0 };
    ksd_client_response response = { 0 };
    if (!valid_error(error) || options == NULL || info == NULL
        || connection == NULL || *connection != NULL
        || options->struct_size < sizeof(*options)
        || !valid_service_info_output(info)
        || !valid_role(options->role)
        || (options->authorization_mode != KSD_AUTH_CHECK
            && options->authorization_mode != KSD_AUTH_REQUEST)
        || (options->requested_scopes
            & ~(uint32_t)KSD_DESKTOP_ACCEPTED_SCOPES) != 0u
        || options->timeout_ms == 0u || options->timeout_ms > 300000u
        || options->flags != 0u
        || !bytes_zero(options->reserved, sizeof(options->reserved)))
        return invalid_argument(error, "invalid desktop connect options");

    ksd_connection *created = calloc(1u, sizeof(*created));
    if (created == NULL) {
        set_error(error, 0u, ENOMEM, "could not allocate desktop connection");
        return KSD_STATUS_RESOURCE_EXHAUSTED;
    }
    created->descriptor = -1;
    created->timeout_ms = options->timeout_ms;
    created->role = options->role;
    if (pthread_mutex_init(&created->mutex, NULL) != 0) {
        free(created);
        set_error(error, 0u, ENOMEM,
                  "could not initialize desktop connection");
        return KSD_STATUS_RESOURCE_EXHAUSTED;
    }
    created->descriptor = connect_socket(options->socket_path,
                                         options->timeout_ms);
    if (created->descriptor < 0) {
        ksd_status status = system_failure(created, error,
                                           "could not connect to desktop service");
        pthread_mutex_destroy(&created->mutex);
        free(created);
        return status;
    }

    ksd_encode_u16(payload, (uint16_t)options->role);
    ksd_encode_u16(payload + 2u, (uint16_t)options->authorization_mode);
    ksd_encode_u32(payload + 4u, options->requested_scopes);
    pthread_mutex_lock(&created->mutex);
    ksd_status status = request_locked(created, KSD_OP_HELLO,
        payload, sizeof(payload), &response, error);
    pthread_mutex_unlock(&created->mutex);
    if (status != KSD_STATUS_OK)
        goto failed;

    uint32_t granted;
    uint64_t operations;
    uint32_t backend;
    if ((response.frame.flags & KSD_FLAG_MORE) != 0u
        || !parse_hello_tail(response.tail, response.tail_length,
                             &granted, &operations, &backend)) {
        response_clear(&response);
        errno = EPROTO;
        status = system_failure(created, error,
                                "desktop service returned an invalid HELLO");
        goto failed;
    }
    created->granted_scopes = granted;
    created->available_operations = operations;
    created->backend = backend;
    uint32_t info_size = info->struct_size;
    memset(info, 0, sizeof(*info));
    info->struct_size = info_size;
    info->abi_major = KSD_CLIENT_ABI_MAJOR;
    info->abi_minor = KSD_CLIENT_ABI_MINOR;
    info->granted_scopes = granted;
    info->available_operations = operations;
    info->backend = backend;
    response_clear(&response);
    *connection = created;
    return KSD_STATUS_OK;

failed:
    response_clear(&response);
    if (created->descriptor >= 0)
        close(created->descriptor);
    pthread_mutex_destroy(&created->mutex);
    free(created);
    return status;
}

void ksd_disconnect(ksd_connection *connection)
{
    if (connection == NULL)
        return;
    if (connection->descriptor >= 0) {
        (void)shutdown(connection->descriptor, SHUT_RDWR);
        close(connection->descriptor);
    }
    pthread_mutex_destroy(&connection->mutex);
    free(connection);
}

#ifdef KSD_CLIENT_TESTING
ksd_connection *ksd_client_test_adopt_descriptor(int descriptor)
{
    ksd_connection *connection = calloc(1u, sizeof(*connection));
    if (connection == NULL)
        return NULL;
    connection->descriptor = descriptor;
    connection->role = KSD_ROLE_RPC;
    connection->timeout_ms = 1000u;
    if (pthread_mutex_init(&connection->mutex, NULL) != 0) {
        close(descriptor);
        free(connection);
        return NULL;
    }
    return connection;
}

void ksd_client_test_set_role(ksd_connection *connection, uint32_t role)
{
    if (connection != NULL)
        connection->role = role;
}

bool ksd_client_test_parse_hello_tail(const uint8_t *tail,
                                      uint32_t tail_length,
                                      uint32_t *granted,
                                      uint64_t *operations,
                                      uint32_t *backend)
{
    return parse_hello_tail(tail, tail_length, granted, operations, backend);
}
#endif

ksd_status ksd_authorize(ksd_connection *connection,
                         ksd_authorization_mode mode,
                         uint32_t requested_scopes, uint32_t *granted_scopes,
                         ksd_error *error)
{
    uint8_t payload[16] = { 0 };
    ksd_client_response response = { 0 };
    if (connection == NULL || granted_scopes == NULL || !valid_error(error)
        || requested_scopes == 0u
        || (requested_scopes
            & ~(uint32_t)KSD_DESKTOP_ACCEPTED_SCOPES) != 0u
        || (mode != KSD_AUTH_CHECK && mode != KSD_AUTH_REQUEST))
        return invalid_argument(error, "invalid authorization request");
    ksd_encode_u16(payload, (uint16_t)mode);
    ksd_encode_u32(payload + 4u, requested_scopes);
    ksd_status status = request(connection, KSD_OP_AUTHORIZE,
                                payload, sizeof(payload), &response, error);
    if (status != KSD_STATUS_OK) {
        response_clear(&response);
        return status;
    }
    ksd_cursor cursor;
    uint32_t granted;
    uint32_t reserved;
    ksd_cursor_init(&cursor, response.tail, response.tail_length);
    if ((response.frame.flags & KSD_FLAG_MORE) != 0u
        || !ksd_cursor_u32(&cursor, &granted)
        || !ksd_cursor_u32(&cursor, &reserved)
        || !ksd_cursor_finished(&cursor) || reserved != 0u) {
        response_clear(&response);
        errno = EPROTO;
        return system_failure(connection, error,
                              "desktop service returned invalid authorization");
    }
    granted &= (uint32_t)KSD_DESKTOP_ACCEPTED_SCOPES;
    connection->granted_scopes = granted;
    *granted_scopes = granted;
    response_clear(&response);
    return KSD_STATUS_OK;
}

ksd_status ksd_ping(ksd_connection *connection, ksd_error *error)
{
    ksd_client_response response = { 0 };
    ksd_status status = request(connection, KSD_OP_PING,
                                NULL, 0u, &response, error);
    if (status == KSD_STATUS_OK
        && (response.tail_length != 0u
            || (response.frame.flags & KSD_FLAG_MORE) != 0u)) {
        response_clear(&response);
        errno = EPROTO;
        return system_failure(connection, error,
                              "desktop service returned an invalid PING");
    }
    response_clear(&response);
    return status;
}

uint32_t ksd_connection_granted_scopes(const ksd_connection *connection)
{
    return connection == NULL ? 0u : connection->granted_scopes;
}

ksd_operations ksd_connection_available_operations(
    const ksd_connection *connection)
{
    return connection == NULL ? 0u : connection->available_operations;
}

ksd_backend ksd_connection_backend(const ksd_connection *connection)
{
    return connection == NULL ? KSD_BACKEND_NONE : connection->backend;
}

uint32_t ksd_lease_granted_scopes(const ksd_connection *connection)
{
    return connection != NULL && connection->role == KSD_ROLE_AUTHORIZATION_LEASE
        ? connection->granted_scopes : 0u;
}

static void raw_hash_to_text(const uint8_t raw[32], char hash[65])
{
    static const char digits[] = "0123456789abcdef";
    for (size_t index = 0u; index < 32u; index++) {
        hash[index * 2u] = digits[raw[index] >> 4u];
        hash[index * 2u + 1u] = digits[raw[index] & 0x0fu];
    }
    hash[64] = '\0';
}

static bool canonical_hash(const char hash[65])
{
    if (hash == NULL || hash[64] != '\0')
        return false;
    for (size_t index = 0u; index < 64u; index++)
        if (!((hash[index] >= '0' && hash[index] <= '9')
              || (hash[index] >= 'a' && hash[index] <= 'f')))
            return false;
    return true;
}

static void text_hash_to_raw(const char hash[65], uint8_t raw[32])
{
    for (size_t index = 0u; index < 32u; index++) {
        unsigned high = hash[index * 2u] <= '9'
            ? (unsigned)(hash[index * 2u] - '0')
            : (unsigned)(hash[index * 2u] - 'a' + 10);
        unsigned low = hash[index * 2u + 1u] <= '9'
            ? (unsigned)(hash[index * 2u + 1u] - '0')
            : (unsigned)(hash[index * 2u + 1u] - 'a' + 10);
        raw[index] = (uint8_t)((high << 4u) | low);
    }
}

static bool parse_permission_entry(const ksd_client_response *response,
                                   ksd_permission_entry *entry)
{
    ksd_cursor cursor;
    uint32_t path_length;
    const uint8_t *hash;
    const uint8_t *path;
    ksd_cursor_init(&cursor, response->tail, response->tail_length);
    ksd_permission_entry_init(entry);
    if (!ksd_cursor_u32(&cursor, &entry->scopes)
        || !ksd_cursor_u32(&cursor, &path_length)
        || !ksd_cursor_u64(&cursor, &entry->granted_at_utc)
        || !ksd_cursor_bytes(&cursor, 32u, &hash)
        || path_length == 0u || path_length >= sizeof(entry->executable)
        || !ksd_cursor_bytes(&cursor, path_length, &path)
        || !ksd_cursor_finished(&cursor)
        || entry->scopes == 0u
        || !ksd_utf8_valid(path, path_length, false))
        return false;
    raw_hash_to_text(hash, entry->hash);
    memcpy(entry->executable, path, path_length);
    entry->executable[path_length] = '\0';
    return true;
}

ksd_status ksd_permissions_list(ksd_connection *connection,
                                ksd_permission_visitor visitor,
                                void *user_data, ksd_error *error)
{
    if (connection == NULL || visitor == NULL || !valid_error(error)
        || connection->role != KSD_ROLE_RPC)
        return invalid_argument(error, "invalid permissions list request");
    pthread_mutex_lock(&connection->mutex);
    ksd_client_response response = { 0 };
    ksd_status status = request_locked(connection, KSD_OP_PERMISSIONS_LIST,
                                       NULL, 0u, &response, error);
    bool cancelled = false;
    for (;;) {
        if (status != KSD_STATUS_OK)
            break;
        bool more = (response.frame.flags & KSD_FLAG_MORE) != 0u;
        if (!more) {
            if (response.tail_length != 0u) {
                errno = EPROTO;
                status = system_failure(connection, error,
                    "desktop service returned invalid permission terminator");
            }
            break;
        }
        ksd_permission_entry entry;
        if (!parse_permission_entry(&response, &entry)) {
            errno = EPROTO;
            status = system_failure(connection, error,
                                    "desktop service returned invalid permission");
            break;
        }
        if (!cancelled && !visitor(&entry, user_data))
            cancelled = true;
        uint64_t request_id = response.frame.request_id;
        response_clear(&response);
        status = read_response_locked(connection, KSD_OP_PERMISSIONS_LIST,
                                      request_id, &response, error);
    }
    if (status == KSD_STATUS_OK && cancelled) {
        set_error(error, 0u, 0, "permission visitor cancelled");
        status = KSD_STATUS_CANCELLED;
    }
    response_clear(&response);
    pthread_mutex_unlock(&connection->mutex);
    return status;
}

ksd_status ksd_permissions_revoke(ksd_connection *connection,
                                  const ksd_permission_revoke *revoke,
                                  ksd_error *error)
{
    uint8_t payload[48] = { 0 };
    if (connection == NULL || revoke == NULL || !valid_error(error)
        || connection->role != KSD_ROLE_RPC
        || revoke->struct_size < sizeof(*revoke)
        || revoke->scopes == 0u
        || (revoke->scopes
            & ~(uint32_t)KSD_DESKTOP_MANAGED_SCOPES) != 0u
        || revoke->reserved0 != 0u
        || !bytes_zero(revoke->reserved1, sizeof(revoke->reserved1))
        || !bytes_zero(revoke->reserved, sizeof(revoke->reserved)))
        return invalid_argument(error, "invalid permissions revoke request");
    if (revoke->target_kind == KSD_PERMISSION_TARGET_HASH) {
        if (revoke->pid != 0u || !canonical_hash(revoke->hash))
            return invalid_argument(error, "invalid permission hash target");
        text_hash_to_raw(revoke->hash, payload + 16u);
    } else if (revoke->target_kind == KSD_PERMISSION_TARGET_PID) {
        if (revoke->pid == 0u || revoke->pid > INT_MAX
            || !bytes_zero(revoke->hash, sizeof(revoke->hash)))
            return invalid_argument(error, "invalid permission pid target");
    } else if (revoke->target_kind == KSD_PERMISSION_TARGET_ALL) {
        if (revoke->pid != 0u
            || !bytes_zero(revoke->hash, sizeof(revoke->hash)))
            return invalid_argument(error, "invalid permission all target");
    } else {
        return invalid_argument(error, "invalid permission target kind");
    }
    ksd_encode_u32(payload, revoke->target_kind);
    ksd_encode_u32(payload + 4u, revoke->scopes);
    ksd_encode_u64(payload + 8u, revoke->pid);
    ksd_client_response response = { 0 };
    ksd_status status = request(connection, KSD_OP_PERMISSIONS_REVOKE,
                                payload, sizeof(payload), &response, error);
    if (status == KSD_STATUS_OK
        && (response.tail_length != 0u
            || (response.frame.flags & KSD_FLAG_MORE) != 0u)) {
        response_clear(&response);
        errno = EPROTO;
        return system_failure(connection, error,
                              "desktop service returned invalid revoke response");
    }
    response_clear(&response);
    return status;
}

static ksd_status allocation_failure(ksd_error *error)
{
    set_error(error, 0u, ENOMEM, "desktop client allocation failed");
    return KSD_STATUS_RESOURCE_EXHAUSTED;
}

static bool valid_rpc(const ksd_connection *connection)
{
    return connection != NULL && connection->role == KSD_ROLE_RPC;
}

static bool valid_rectangle(int32_t x, int32_t y,
                            uint32_t width, uint32_t height)
{
    return width != 0u && height != 0u
        && width <= KSD_MAX_CAPTURE_DIMENSION
        && height <= KSD_MAX_CAPTURE_DIMENSION
        && (int64_t)x + width <= INT32_MAX
        && (int64_t)y + height <= INT32_MAX;
}

static bool valid_window_geometry(int32_t x, int32_t y,
                                  uint32_t width, uint32_t height)
{
    return !(x == INT32_MIN && y == INT32_MIN
             && width == 0u && height == 0u)
        && width <= KSD_MAX_CAPTURE_DIMENSION
        && height <= KSD_MAX_CAPTURE_DIMENSION
        && (x == INT32_MIN || width == 0u
            || (int64_t)x + width <= INT32_MAX)
        && (y == INT32_MIN || height == 0u
            || (int64_t)y + height <= INT32_MAX);
}

static bool valid_capture_rectangle(int32_t x, int32_t y,
                                    uint32_t width, uint32_t height)
{
    return valid_rectangle(x, y, width, height)
        && (uint64_t)width * height <= KSD_MAX_CAPTURE_PIXELS;
}

static ksd_status invalid_response(ksd_connection *connection,
                                   ksd_client_response *response,
                                   ksd_error *error, const char *message)
{
    response_clear(response);
    errno = EPROTO;
    return system_failure(connection, error, message);
}

static ksd_status request_empty_result(ksd_connection *connection,
                                       uint16_t opcode,
                                       const void *payload,
                                       uint32_t payload_length,
                                       ksd_error *error)
{
    ksd_client_response response = { 0 };
    ksd_status status = request(connection, opcode, payload, payload_length,
                                &response, error);
    if (status == KSD_STATUS_OK
        && (response.tail_length != 0u
            || (response.frame.flags & KSD_FLAG_MORE) != 0u))
        return invalid_response(connection, &response, error,
                                "desktop service returned invalid operation data");
    response_clear(&response);
    return status;
}

static bool parse_owned_string(const void *data, uint32_t length,
                               bool prefixed, ksd_string *string)
{
    ksd_cursor cursor;
    uint32_t text_length = length;
    const uint8_t *text = data;
    if (prefixed) {
        ksd_cursor_init(&cursor, data, length);
        if (!ksd_cursor_u32(&cursor, &text_length)
            || text_length > KSD_MAX_TEXT_BYTES
            || !ksd_cursor_bytes(&cursor, text_length, &text)
            || !ksd_cursor_finished(&cursor))
            return false;
    }
    if (text_length > KSD_MAX_TEXT_BYTES
        || !ksd_utf8_valid(text, text_length, false))
        return false;
    char *copy = malloc((size_t)text_length + 1u);
    if (copy == NULL)
        return false;
    if (text_length != 0u)
        memcpy(copy, text, text_length);
    copy[text_length] = '\0';
    string->data = copy;
    string->length = text_length;
    return true;
}

static bool parse_owned_bytes(const void *data, uint32_t length,
                              ksd_bytes *bytes)
{
    ksd_cursor cursor;
    uint32_t byte_length;
    const uint8_t *value;
    ksd_cursor_init(&cursor, data, length);
    if (!ksd_cursor_u32(&cursor, &byte_length)
        || byte_length > KSD_MAX_TEXT_BYTES
        || !ksd_cursor_bytes(&cursor, byte_length, &value)
        || !ksd_cursor_finished(&cursor))
        return false;
    uint8_t *copy = NULL;
    if (byte_length != 0u) {
        copy = malloc(byte_length);
        if (copy == NULL)
            return false;
        memcpy(copy, value, byte_length);
    }
    bytes->data = copy;
    bytes->length = byte_length;
    return true;
}

static bool parse_string_list_tail(const void *data, uint32_t length,
                                   ksd_string_list *list)
{
    ksd_cursor cursor;
    uint32_t count;
    uint32_t reserved;
    ksd_cursor_init(&cursor, data, length);
    if (!ksd_cursor_u32(&cursor, &count)
        || !ksd_cursor_u32(&cursor, &reserved)
        || count > KSD_MAX_MIMETYPES || reserved != 0u)
        return false;
    ksd_string *items = count == 0u ? NULL : calloc(count, sizeof(*items));
    if (count != 0u && items == NULL)
        return false;
    for (uint32_t index = 0u; index < count; index++)
        ksd_string_init(&items[index]);
    for (uint32_t index = 0u; index < count; index++) {
        uint32_t item_length;
        const uint8_t *item;
        if (!ksd_cursor_u32(&cursor, &item_length)
            || item_length == 0u || item_length > KSD_MAX_MIMETYPE_BYTES
            || !ksd_cursor_bytes(&cursor, item_length, &item)
            || !parse_owned_string(item, item_length, false, &items[index])) {
            ksd_string_list partial;
            ksd_string_list_init(&partial);
            partial.items = items;
            partial.count = count;
            ksd_string_list_clear(&partial);
            return false;
        }
    }
    if (!ksd_cursor_finished(&cursor)) {
        ksd_string_list partial;
        ksd_string_list_init(&partial);
        partial.items = items;
        partial.count = count;
        ksd_string_list_clear(&partial);
        return false;
    }
    list->items = items;
    list->count = count;
    return true;
}

static bool parse_capture_tail(const void *data, uint32_t length,
                               ksd_capture *capture)
{
    ksd_cursor cursor;
    uint16_t format;
    uint16_t reserved;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t byte_length;
    const uint8_t *bytes;
    ksd_cursor_init(&cursor, data, length);
    if (!ksd_cursor_u16(&cursor, &format)
        || !ksd_cursor_u16(&cursor, &reserved)
        || !ksd_cursor_u32(&cursor, &width)
        || !ksd_cursor_u32(&cursor, &height)
        || !ksd_cursor_u32(&cursor, &stride)
        || !ksd_cursor_u32(&cursor, &byte_length)
        || !ksd_cursor_bytes(&cursor, byte_length, &bytes)
        || !ksd_cursor_finished(&cursor) || reserved != 0u
        || width == 0u || height == 0u
        || width > KSD_MAX_CAPTURE_DIMENSION
        || height > KSD_MAX_CAPTURE_DIMENSION
        || (uint64_t)width * height > KSD_MAX_CAPTURE_PIXELS
        || byte_length == 0u || byte_length > KSD_MAX_CAPTURE_BYTES
        || (format == KSD_CAPTURE_FORMAT_PNG && stride != 0u)
        || (format == KSD_CAPTURE_FORMAT_BGRA8_PREMULTIPLIED
            && (stride < (uint64_t)width * 4u
                || (uint64_t)stride * height != byte_length))
        || (format != KSD_CAPTURE_FORMAT_PNG
            && format != KSD_CAPTURE_FORMAT_BGRA8_PREMULTIPLIED))
        return false;
    uint8_t *copy = malloc(byte_length);
    if (copy == NULL)
        return false;
    memcpy(copy, bytes, byte_length);
    capture->format = format;
    capture->width = width;
    capture->height = height;
    capture->stride = stride;
    capture->data.data = copy;
    capture->data.length = byte_length;
    return true;
}

static ksd_status request_capture(ksd_connection *connection,
                                  uint16_t opcode,
                                  const void *payload,
                                  uint32_t payload_length,
                                  ksd_capture *capture,
                                  ksd_error *error)
{
    if (!valid_rpc(connection) || !valid_capture_output(capture)
        || !valid_error(error))
        return invalid_argument(error, "invalid capture request");
    ksd_client_response response = { 0 };
    ksd_status status = request(connection, opcode, payload, payload_length,
                                &response, error);
    if (status != KSD_STATUS_OK) {
        response_clear(&response);
        return status;
    }
    ksd_capture parsed;
    ksd_capture_init(&parsed);
    if ((response.frame.flags & KSD_FLAG_MORE) != 0u
        || !parse_capture_tail(response.tail, response.tail_length, &parsed)) {
        ksd_capture_clear(&parsed);
        return invalid_response(connection, &response, error,
                                "desktop service returned invalid capture data");
    }
    response_clear(&response);
    ksd_capture_clear(capture);
    *capture = parsed;
    return KSD_STATUS_OK;
}

ksd_status ksd_capture_area(ksd_connection *connection, int32_t x, int32_t y,
                            uint32_t width, uint32_t height,
                            ksd_capture *capture, ksd_error *error)
{
    uint8_t payload[16];
    if (!valid_capture_rectangle(x, y, width, height))
        return invalid_argument(error, "invalid capture rectangle");
    ksd_encode_u32(payload, (uint32_t)x);
    ksd_encode_u32(payload + 4u, (uint32_t)y);
    ksd_encode_u32(payload + 8u, width);
    ksd_encode_u32(payload + 12u, height);
    return request_capture(connection, KSD_OP_CAPTURE_AREA,
                           payload, sizeof(payload), capture, error);
}

ksd_status ksd_capture_window(ksd_connection *connection,
                              const char *window_id,
                              uint32_t include_decoration,
                              ksd_capture *capture, ksd_error *error)
{
    if (window_id == NULL || include_decoration > 1u)
        return invalid_argument(error, "invalid window capture request");
    size_t length = strlen(window_id);
    if (length == 0u || length > 128u
        || !ksd_utf8_valid((const uint8_t *)window_id, length, false))
        return invalid_argument(error, "invalid window identifier");
    uint8_t payload[8u + 128u] = { 0 };
    ksd_encode_u32(payload, include_decoration);
    ksd_encode_u32(payload + 4u, (uint32_t)length);
    memcpy(payload + 8u, window_id, length);
    return request_capture(connection, KSD_OP_CAPTURE_WINDOW,
                           payload, 8u + (uint32_t)length, capture, error);
}

ksd_status ksd_cursor_position(ksd_connection *connection,
                               ksd_point *position, ksd_error *error)
{
    if (!valid_rpc(connection) || !valid_point_output(position)
        || !valid_error(error))
        return invalid_argument(error, "invalid cursor-position request");
    ksd_client_response response = { 0 };
    ksd_status status = request(connection, KSD_OP_CURSOR_POSITION,
                                NULL, 0u, &response, error);
    if (status != KSD_STATUS_OK) {
        response_clear(&response);
        return status;
    }
    if ((response.frame.flags & KSD_FLAG_MORE) != 0u
        || response.tail_length != 8u) {
        return invalid_response(connection, &response, error,
            "desktop service returned invalid cursor-position data");
    }
    int32_t x = (int32_t)ksd_decode_u32(response.tail);
    int32_t y = (int32_t)ksd_decode_u32(response.tail + 4u);
    response_clear(&response);
    uint32_t size = position->struct_size;
    memset(position, 0, sizeof(*position));
    position->struct_size = size;
    position->x = x;
    position->y = y;
    return KSD_STATUS_OK;
}

ksd_status ksd_work_area(ksd_connection *connection,
                         ksd_rectangle *area, ksd_error *error)
{
    if (!valid_rpc(connection) || !valid_rectangle_output(area)
        || !valid_error(error))
        return invalid_argument(error, "invalid work-area request");
    ksd_client_response response = { 0 };
    ksd_status status = request(connection, KSD_OP_WORK_AREA,
                                NULL, 0u, &response, error);
    if (status != KSD_STATUS_OK) {
        response_clear(&response);
        return status;
    }
    if ((response.frame.flags & KSD_FLAG_MORE) != 0u
        || response.tail_length != 16u) {
        return invalid_response(connection, &response, error,
            "desktop service returned invalid work-area data");
    }
    int32_t x = (int32_t)ksd_decode_u32(response.tail);
    int32_t y = (int32_t)ksd_decode_u32(response.tail + 4u);
    uint32_t width = ksd_decode_u32(response.tail + 8u);
    uint32_t height = ksd_decode_u32(response.tail + 12u);
    if (width == 0u || height == 0u || width > INT32_MAX
        || height > INT32_MAX || (int64_t)x + width > INT32_MAX
        || (int64_t)y + height > INT32_MAX)
        return invalid_response(connection, &response, error,
            "desktop service returned invalid work-area bounds");
    response_clear(&response);
    uint32_t size = area->struct_size;
    memset(area, 0, sizeof(*area));
    area->struct_size = size;
    area->x = x;
    area->y = y;
    area->width = width;
    area->height = height;
    return KSD_STATUS_OK;
}

static ksd_status request_string_result(ksd_connection *connection,
                                        uint16_t opcode,
                                        const void *payload,
                                        uint32_t payload_length,
                                        ksd_string *string,
                                        ksd_error *error)
{
    if (!valid_rpc(connection) || !valid_string_output(string)
        || !valid_error(error))
        return invalid_argument(error, "invalid desktop text request");
    ksd_client_response response = { 0 };
    ksd_status status = request(connection, opcode, payload, payload_length,
                                &response, error);
    if (status != KSD_STATUS_OK) {
        response_clear(&response);
        return status;
    }
    ksd_string parsed;
    ksd_string_init(&parsed);
    if ((response.frame.flags & KSD_FLAG_MORE) != 0u
        || !parse_owned_string(response.tail, response.tail_length,
                               true, &parsed)) {
        ksd_string_clear(&parsed);
        return invalid_response(connection, &response, error,
                                "desktop service returned invalid text data");
    }
    response_clear(&response);
    ksd_string_clear(string);
    *string = parsed;
    return KSD_STATUS_OK;
}

ksd_status ksd_window_list_json(ksd_connection *connection,
                                uint32_t include_hidden,
                                ksd_string *json, ksd_error *error)
{
    uint8_t payload[8] = { 0 };
    if (include_hidden > 1u)
        return invalid_argument(error, "invalid include-hidden value");
    ksd_encode_u32(payload, include_hidden);
    return request_string_result(connection, KSD_OP_WINDOW_LIST,
                                 payload, sizeof(payload), json, error);
}

ksd_status ksd_window_active_json(ksd_connection *connection,
                                  ksd_string *json, ksd_error *error)
{
    return request_string_result(connection, KSD_OP_WINDOW_ACTIVE,
                                 NULL, 0u, json, error);
}

static ksd_status request_handle(ksd_connection *connection, uint16_t opcode,
                                 uint64_t handle, ksd_error *error)
{
    uint8_t payload[8];
    if (!valid_rpc(connection) || handle == 0u || !valid_error(error))
        return invalid_argument(error, "invalid window handle request");
    ksd_encode_u64(payload, handle);
    return request_empty_result(connection, opcode, payload, sizeof(payload),
                                error);
}

ksd_status ksd_window_focus(ksd_connection *connection, uint64_t handle,
                            ksd_error *error)
{
    return request_handle(connection, KSD_OP_WINDOW_FOCUS, handle, error);
}

ksd_status ksd_window_raise(ksd_connection *connection, uint64_t handle,
                            ksd_error *error)
{
    return request_handle(connection, KSD_OP_WINDOW_RAISE, handle, error);
}

ksd_status ksd_window_lower(ksd_connection *connection, uint64_t handle,
                            ksd_error *error)
{
    return request_handle(connection, KSD_OP_WINDOW_LOWER, handle, error);
}

ksd_status ksd_window_close(ksd_connection *connection, uint64_t handle,
                            ksd_error *error)
{
    return request_handle(connection, KSD_OP_WINDOW_CLOSE, handle, error);
}

ksd_status ksd_window_kill(ksd_connection *connection, uint64_t handle,
                           ksd_error *error)
{
    return request_handle(connection, KSD_OP_WINDOW_KILL, handle, error);
}

static ksd_status request_geometry(ksd_connection *connection,
                                   uint16_t opcode, uint64_t handle,
                                   int32_t x, int32_t y,
                                   uint32_t width, uint32_t height,
                                   ksd_error *error)
{
    uint8_t payload[24];
    if (!valid_rpc(connection) || handle == 0u
        || !valid_window_geometry(x, y, width, height)
        || !valid_error(error))
        return invalid_argument(error, "invalid window geometry request");
    ksd_encode_u64(payload, handle);
    ksd_encode_u32(payload + 8u, (uint32_t)x);
    ksd_encode_u32(payload + 12u, (uint32_t)y);
    ksd_encode_u32(payload + 16u, width);
    ksd_encode_u32(payload + 20u, height);
    return request_empty_result(connection, opcode, payload, sizeof(payload),
                                error);
}

ksd_status ksd_window_move_resize(ksd_connection *connection,
                                  uint64_t handle, int32_t x, int32_t y,
                                  uint32_t width, uint32_t height,
                                  ksd_error *error)
{
    return request_geometry(connection, KSD_OP_WINDOW_MOVE_RESIZE, handle,
                            x, y, width, height, error);
}

ksd_status ksd_window_move_resize_xid(ksd_connection *connection,
                                      uint64_t xid, int32_t x, int32_t y,
                                      uint32_t width, uint32_t height,
                                      ksd_error *error)
{
    return request_geometry(connection, KSD_OP_WINDOW_MOVE_RESIZE_XID, xid,
                            x, y, width, height, error);
}

static ksd_status request_window_value(ksd_connection *connection,
                                       uint16_t opcode, uint64_t handle,
                                       uint32_t value, uint32_t maximum,
                                       ksd_error *error)
{
    uint8_t payload[16] = { 0 };
    if (!valid_rpc(connection) || handle == 0u || value > maximum
        || !valid_error(error))
        return invalid_argument(error, "invalid window value request");
    ksd_encode_u64(payload, handle);
    ksd_encode_u32(payload + 8u, value);
    return request_empty_result(connection, opcode, payload, sizeof(payload),
                                error);
}

ksd_status ksd_window_set_state(ksd_connection *connection, uint64_t handle,
                                uint32_t state, ksd_error *error)
{
    return request_window_value(connection, KSD_OP_WINDOW_SET_STATE,
                                handle, state, 2u, error);
}

ksd_status ksd_window_set_opacity(ksd_connection *connection,
                                  uint64_t handle, uint32_t opacity,
                                  ksd_error *error)
{
    return request_window_value(connection, KSD_OP_WINDOW_SET_OPACITY,
                                handle, opacity, 255u, error);
}

ksd_status ksd_window_set_above(ksd_connection *connection, uint64_t handle,
                                uint32_t above, ksd_error *error)
{
    return request_window_value(connection, KSD_OP_WINDOW_SET_ABOVE,
                                handle, above, 1u, error);
}

ksd_status ksd_window_set_decorated(ksd_connection *connection,
                                    uint64_t handle, uint32_t decorated,
                                    ksd_error *error)
{
    return request_window_value(connection, KSD_OP_WINDOW_SET_DECORATED,
                                handle, decorated, 1u, error);
}

ksd_status ksd_window_reserve(ksd_connection *connection, uint64_t cookie,
                              int32_t x, int32_t y, uint32_t ttl_ms,
                              ksd_error *error)
{
    uint8_t payload[24] = { 0 };
    if (!valid_rpc(connection) || cookie == 0u || ttl_ms == 0u
        || ttl_ms > 60000u || !valid_error(error))
        return invalid_argument(error, "invalid window reservation request");
    ksd_encode_u64(payload, cookie);
    ksd_encode_u32(payload + 8u, (uint32_t)x);
    ksd_encode_u32(payload + 12u, (uint32_t)y);
    ksd_encode_u32(payload + 16u, ttl_ms);
    return request_empty_result(connection, KSD_OP_WINDOW_RESERVE,
                                payload, sizeof(payload), error);
}

ksd_status ksd_window_get_reserved(ksd_connection *connection,
                                   uint64_t cookie, uint64_t *handle,
                                   ksd_error *error)
{
    uint8_t payload[8];
    if (!valid_rpc(connection) || cookie == 0u || handle == NULL
        || !valid_error(error))
        return invalid_argument(error, "invalid reserved-window request");
    ksd_encode_u64(payload, cookie);
    ksd_client_response response = { 0 };
    ksd_status status = request(connection, KSD_OP_WINDOW_GET_RESERVED,
                                payload, sizeof(payload), &response, error);
    if (status != KSD_STATUS_OK) {
        response_clear(&response);
        return status;
    }
    if ((response.frame.flags & KSD_FLAG_MORE) != 0u
        || response.tail_length != 8u
        || ksd_decode_u64(response.tail) == 0u)
        return invalid_response(connection, &response, error,
                                "desktop service returned an invalid window");
    *handle = ksd_decode_u64(response.tail);
    response_clear(&response);
    return KSD_STATUS_OK;
}

ksd_status ksd_clipboard_mimetypes(ksd_connection *connection,
                                   ksd_string_list *mimetypes,
                                   ksd_error *error)
{
    if (!valid_rpc(connection) || !valid_string_list_output(mimetypes)
        || !valid_error(error))
        return invalid_argument(error, "invalid clipboard format request");
    ksd_client_response response = { 0 };
    ksd_status status = request(connection, KSD_OP_CLIPBOARD_MIMETYPES,
                                NULL, 0u, &response, error);
    if (status != KSD_STATUS_OK) {
        response_clear(&response);
        return status;
    }
    ksd_string_list parsed;
    ksd_string_list_init(&parsed);
    if ((response.frame.flags & KSD_FLAG_MORE) != 0u
        || !parse_string_list_tail(response.tail, response.tail_length,
                                   &parsed)) {
        ksd_string_list_clear(&parsed);
        return invalid_response(connection, &response, error,
                                "desktop service returned invalid formats");
    }
    response_clear(&response);
    ksd_string_list_clear(mimetypes);
    *mimetypes = parsed;
    return KSD_STATUS_OK;
}

ksd_status ksd_clipboard_content(ksd_connection *connection,
                                 const char *mimetype,
                                 ksd_bytes *content, ksd_error *error)
{
    if (!valid_rpc(connection) || mimetype == NULL
        || !valid_bytes_output(content)
        || !valid_error(error))
        return invalid_argument(error, "invalid clipboard content request");
    size_t length = strlen(mimetype);
    if (length == 0u || length > KSD_MAX_MIMETYPE_BYTES
        || !ksd_utf8_valid((const uint8_t *)mimetype, length, false))
        return invalid_argument(error, "invalid clipboard mimetype");
    uint8_t payload[4u + KSD_MAX_MIMETYPE_BYTES];
    ksd_encode_u32(payload, (uint32_t)length);
    memcpy(payload + 4u, mimetype, length);
    ksd_client_response response = { 0 };
    ksd_status status = request(connection, KSD_OP_CLIPBOARD_CONTENT,
        payload, 4u + (uint32_t)length, &response, error);
    if (status != KSD_STATUS_OK) {
        response_clear(&response);
        return status;
    }
    ksd_bytes parsed;
    ksd_bytes_init(&parsed);
    if ((response.frame.flags & KSD_FLAG_MORE) != 0u
        || !parse_owned_bytes(response.tail, response.tail_length, &parsed)) {
        ksd_bytes_clear(&parsed);
        return invalid_response(connection, &response, error,
                                "desktop service returned invalid clipboard data");
    }
    response_clear(&response);
    ksd_bytes_clear(content);
    *content = parsed;
    return KSD_STATUS_OK;
}

ksd_status ksd_clipboard_text(ksd_connection *connection, ksd_string *text,
                              ksd_error *error)
{
    return request_string_result(connection, KSD_OP_CLIPBOARD_TEXT,
                                 NULL, 0u, text, error);
}

static ksd_status request_pair(ksd_connection *connection, uint16_t opcode,
                               uint32_t first, uint32_t second,
                               ksd_error *error)
{
    uint8_t payload[8];
    if (!valid_rpc(connection) || !valid_error(error))
        return invalid_argument(error, "invalid pointer request");
    ksd_encode_u32(payload, first);
    ksd_encode_u32(payload + 4u, second);
    return request_empty_result(connection, opcode, payload, sizeof(payload),
                                error);
}

ksd_status ksd_mouse_move_absolute(ksd_connection *connection,
                                   int32_t x, int32_t y, ksd_error *error)
{
    return request_pair(connection, KSD_OP_MOUSE_MOVE_ABSOLUTE,
                        (uint32_t)x, (uint32_t)y, error);
}

ksd_status ksd_mouse_move_relative(ksd_connection *connection,
                                   int32_t dx, int32_t dy, ksd_error *error)
{
    return request_pair(connection, KSD_OP_MOUSE_MOVE_RELATIVE,
                        (uint32_t)dx, (uint32_t)dy, error);
}

ksd_status ksd_mouse_button(ksd_connection *connection, uint32_t button,
                            uint32_t pressed, ksd_error *error)
{
    if ((button != 1u && button != 2u && button != 3u
         && button != 8u && button != 9u) || pressed > 1u)
        return invalid_argument(error, "invalid pointer button request");
    return request_pair(connection, KSD_OP_MOUSE_BUTTON,
                        button, pressed, error);
}

ksd_status ksd_mouse_scroll(ksd_connection *connection, int32_t delta,
                            uint32_t vertical, ksd_error *error)
{
    if (delta == 0 || delta < -KSD_MAX_MOUSE_SCROLL_DELTA
        || delta > KSD_MAX_MOUSE_SCROLL_DELTA || vertical > 1u)
        return invalid_argument(error, "invalid pointer scroll request");
    return request_pair(connection, KSD_OP_MOUSE_SCROLL,
                        (uint32_t)delta, vertical, error);
}

static ksd_status watch_subscribe(ksd_connection *connection,
                                  uint16_t opcode, ksd_error *error)
{
    if (connection == NULL || connection->role != KSD_ROLE_EVENT_STREAM
        || connection->subscription_opcode != 0u || !valid_error(error))
        return invalid_argument(error, "invalid desktop watch subscription");
    ksd_status status = request_empty_result(connection, opcode,
                                             NULL, 0u, error);
    if (status == KSD_STATUS_OK)
        connection->subscription_opcode = opcode;
    return status;
}

ksd_status ksd_window_watch_subscribe(ksd_connection *connection,
                                      ksd_error *error)
{
    return watch_subscribe(connection, KSD_OP_WINDOW_WATCH, error);
}

ksd_status ksd_clipboard_watch_subscribe(ksd_connection *connection,
                                         ksd_error *error)
{
    return watch_subscribe(connection, KSD_OP_CLIPBOARD_WATCH, error);
}

static ksd_status read_event_locked(ksd_connection *connection,
                                    uint32_t timeout_ms,
                                    uint16_t expected_opcode,
                                    ksd_frame *event,
                                    uint32_t *revoked_scopes,
                                    ksd_error *error)
{
    struct pollfd item = {
        .fd = connection->descriptor,
        .events = POLLIN | POLLHUP | POLLERR,
    };
    uint64_t deadline = UINT64_MAX;
    if (timeout_ms != UINT32_MAX) {
        uint64_t now = monotonic_milliseconds();
        if (now == 0u || now > UINT64_MAX - timeout_ms)
            return system_failure(connection, error,
                                  "desktop event clock failed");
        deadline = now + timeout_ms;
    }
    int ready;
    for (;;) {
        int timeout = -1;
        if (deadline != UINT64_MAX) {
            uint64_t now = monotonic_milliseconds();
            if (now == 0u || now >= deadline) {
                reset_error(error);
                return KSD_STATUS_TIMEOUT;
            }
            uint64_t remaining = deadline - now;
            timeout = remaining > (uint64_t)INT_MAX
                ? INT_MAX : (int)remaining;
        }
        ready = poll(&item, 1u, timeout);
        if (ready < 0 && errno == EINTR)
            continue;
        break;
    }
    if (ready == 0) {
        reset_error(error);
        return KSD_STATUS_TIMEOUT;
    }
    if (ready < 0 || ((item.revents & POLLIN) == 0
        && (item.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0)) {
        if (ready >= 0)
            errno = ECONNRESET;
        return system_failure(connection, error,
                              "desktop event connection ended");
    }
    uint32_t frame_timeout = UINT32_MAX;
    if (deadline != UINT64_MAX) {
        uint64_t now = monotonic_milliseconds();
        if (now == 0u || now >= deadline) {
            reset_error(error);
            return KSD_STATUS_TIMEOUT;
        }
        uint64_t remaining = deadline - now;
        frame_timeout = remaining > UINT32_MAX
            ? UINT32_MAX : (uint32_t)remaining;
    }
    if (!set_timeouts(connection->descriptor, frame_timeout))
        return system_failure(connection, error,
                              "desktop event timeout failed");
    int received = ksd_frame_read(connection->descriptor, client_magic,
        KSD_PROTOCOL_MAJOR, KSD_PROTOCOL_MINOR, KSD_CLIENT_MAX_RESPONSE,
        true, event);
    if (received != 1)
        return system_failure(connection, error,
                              "desktop event receive failed");
    if (event->opcode == KSD_OP_SESSION_REVOKED) {
        uint32_t revoked;
        if (!apply_revoked_event(connection, event, &revoked)) {
            ksd_frame_clear(event);
            errno = EPROTO;
            return system_failure(connection, error,
                                  "desktop service sent invalid revocation");
        }
        ksd_frame_clear(event);
        if (revoked_scopes != NULL)
            *revoked_scopes = revoked;
        set_error(error, revoked, 0, "desktop permission was revoked");
        return KSD_STATUS_REVOKED;
    }
    if (event->opcode != expected_opcode || event->flags != KSD_FLAG_EVENT
        || event->request_id != 0u) {
        ksd_frame_clear(event);
        errno = EPROTO;
        return system_failure(connection, error,
                              "desktop service sent an unexpected event");
    }
    reset_error(error);
    return KSD_STATUS_OK;
}

static ksd_status event_next(ksd_connection *connection,
                             uint32_t timeout_ms, uint16_t subscription,
                             uint16_t event_opcode, ksd_frame *event,
                             ksd_error *error)
{
    if (connection == NULL || event == NULL || !valid_error(error)
        || connection->role != KSD_ROLE_EVENT_STREAM
        || connection->subscription_opcode != subscription
        || timeout_ms == 0u || timeout_ms > KSD_MAX_WATCH_TIMEOUT_MS)
        return invalid_argument(error, "invalid desktop watch poll");
    pthread_mutex_lock(&connection->mutex);
    ksd_status status = read_event_locked(connection, timeout_ms,
                                          event_opcode, event, NULL, error);
    pthread_mutex_unlock(&connection->mutex);
    return status;
}

ksd_status ksd_window_watch_next(ksd_connection *connection,
                                 uint32_t timeout_ms,
                                 ksd_window_event *event, ksd_error *error)
{
    if (!valid_window_event_output(event))
        return invalid_argument(error, "invalid window event output");
    ksd_frame frame = { 0 };
    ksd_status status = event_next(connection, timeout_ms,
        KSD_OP_WINDOW_WATCH, KSD_OP_WINDOW_EVENT, &frame, error);
    if (status != KSD_STATUS_OK)
        return status;
    ksd_cursor cursor;
    uint16_t kind;
    uint16_t reserved;
    uint32_t length;
    const uint8_t *json;
    ksd_cursor_init(&cursor, frame.payload, frame.payload_length);
    if (!ksd_cursor_u16(&cursor, &kind)
        || !ksd_cursor_u16(&cursor, &reserved)
        || !ksd_cursor_u32(&cursor, &length)
        || !ksd_cursor_bytes(&cursor, length, &json)
        || !ksd_cursor_finished(&cursor) || reserved != 0u
        || kind < KSD_WINDOW_EVENT_CREATE
        || kind > KSD_WINDOW_EVENT_ACTIVE_STATE) {
        ksd_frame_clear(&frame);
        errno = EPROTO;
        return system_failure(connection, error,
                              "desktop service sent invalid window event");
    }
    ksd_window_event parsed;
    ksd_window_event_init(&parsed);
    parsed.kind = kind;
    if (!parse_owned_string(json, length, false, &parsed.window_json)) {
        ksd_frame_clear(&frame);
        ksd_window_event_clear(&parsed);
        errno = EPROTO;
        return system_failure(connection, error,
                              "desktop service sent invalid window data");
    }
    ksd_frame_clear(&frame);
    ksd_window_event_clear(event);
    *event = parsed;
    return KSD_STATUS_OK;
}

ksd_status ksd_clipboard_watch_next(ksd_connection *connection,
                                    uint32_t timeout_ms,
                                    ksd_clipboard_event *event,
                                    ksd_error *error)
{
    if (!valid_clipboard_event_output(event))
        return invalid_argument(error, "invalid clipboard event output");
    ksd_frame frame = { 0 };
    ksd_status status = event_next(connection, timeout_ms,
        KSD_OP_CLIPBOARD_WATCH, KSD_OP_CLIPBOARD_EVENT, &frame, error);
    if (status != KSD_STATUS_OK)
        return status;
    ksd_cursor cursor;
    uint32_t text_length;
    uint32_t count;
    const uint8_t *text;
    ksd_cursor_init(&cursor, frame.payload, frame.payload_length);
    if (!ksd_cursor_u32(&cursor, &text_length)
        || !ksd_cursor_u32(&cursor, &count)
        || text_length > KSD_MAX_TEXT_BYTES || count > KSD_MAX_MIMETYPES
        || !ksd_cursor_bytes(&cursor, text_length, &text)) {
        ksd_frame_clear(&frame);
        errno = EPROTO;
        return system_failure(connection, error,
                              "desktop service sent invalid clipboard event");
    }
    ksd_clipboard_event parsed;
    ksd_clipboard_event_init(&parsed);
    if (!parse_owned_string(text, text_length, false, &parsed.text))
        goto invalid_clipboard_event;
    parsed.mimetypes.items = count == 0u
        ? NULL : calloc(count, sizeof(*parsed.mimetypes.items));
    if (count != 0u && parsed.mimetypes.items == NULL) {
        ksd_frame_clear(&frame);
        ksd_clipboard_event_clear(&parsed);
        return allocation_failure(error);
    }
    parsed.mimetypes.count = count;
    for (uint32_t index = 0u; index < count; index++)
        ksd_string_init(&parsed.mimetypes.items[index]);
    for (uint32_t index = 0u; index < count; index++) {
        uint32_t length;
        const uint8_t *value;
        if (!ksd_cursor_u32(&cursor, &length)
            || length == 0u || length > KSD_MAX_MIMETYPE_BYTES
            || !ksd_cursor_bytes(&cursor, length, &value)
            || !parse_owned_string(value, length, false,
                                   &parsed.mimetypes.items[index]))
            goto invalid_clipboard_event;
    }
    if (!ksd_cursor_finished(&cursor))
        goto invalid_clipboard_event;
    ksd_frame_clear(&frame);
    ksd_clipboard_event_clear(event);
    *event = parsed;
    return KSD_STATUS_OK;

invalid_clipboard_event:
    ksd_frame_clear(&frame);
    ksd_clipboard_event_clear(&parsed);
    errno = EPROTO;
    return system_failure(connection, error,
                          "desktop service sent invalid clipboard data");
}

ksd_status ksd_lease_next(ksd_connection *connection, uint32_t timeout_ms,
                          uint32_t *revoked_scopes, ksd_error *error)
{
    if (connection == NULL || revoked_scopes == NULL || !valid_error(error)
        || connection->role != KSD_ROLE_AUTHORIZATION_LEASE
        || timeout_ms == 0u
        || (timeout_ms > INT_MAX && timeout_ms != UINT32_MAX))
        return invalid_argument(error, "invalid authorization lease poll");
    pthread_mutex_lock(&connection->mutex);
    ksd_frame frame = { 0 };
    uint32_t revoked = 0u;
    ksd_status status = read_event_locked(connection, timeout_ms,
        KSD_OP_SESSION_REVOKED, &frame, &revoked, error);
    if (status == KSD_STATUS_REVOKED) {
        *revoked_scopes = revoked;
        reset_error(error);
        status = KSD_STATUS_OK;
    } else if (status == KSD_STATUS_OK) {
        ksd_frame_clear(&frame);
        errno = EPROTO;
        status = system_failure(connection, error,
                                "authorization lease sent invalid event");
    }
    pthread_mutex_unlock(&connection->mutex);
    return status;
}
