#include "keysharp_desktop/client.h"
#include "protocol.h"
#include "protocol_io.h"

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#define TEST_FUTURE_SCOPE 0x00000100u
#define TEST_FUTURE_BACKEND 5u
#define TEST_FUTURE_OPERATION UINT64_C(0x0000000020000000)

ksd_connection *ksd_client_test_adopt_descriptor(int descriptor);
void ksd_client_test_set_role(ksd_connection *connection, uint32_t role);
bool ksd_client_test_parse_hello_tail(const uint8_t *tail,
                                      uint32_t tail_length,
                                      uint32_t *granted,
                                      uint64_t *operations,
                                      uint32_t *backend);

typedef struct visitor_state {
    unsigned int calls;
} visitor_state;

static void write_ok_response(int descriptor, const ksd_frame *request,
                              const void *tail, uint32_t tail_length,
                              bool more)
{
    uint8_t payload[256] = { 0 };
    assert(request != NULL && request->request_id != 0u);
    assert(tail_length <= sizeof(payload) - 8u);
    assert(tail_length == 0u || tail != NULL);
    ksd_encode_u32(payload, KSD_STATUS_OK);
    if (tail_length != 0u)
        memcpy(payload + 8u, tail, tail_length);
    ksd_frame response = {
        .magic = {
            KSD_FRAME_MAGIC_0, KSD_FRAME_MAGIC_1,
            KSD_FRAME_MAGIC_2, KSD_FRAME_MAGIC_3,
        },
        .major = KSD_PROTOCOL_MAJOR,
        .minor = KSD_PROTOCOL_MINOR,
        .opcode = request->opcode,
        .flags = (uint16_t)(KSD_FLAG_RESPONSE
            | (more ? KSD_FLAG_MORE : 0u)),
        .payload_length = 8u + tail_length,
        .request_id = request->request_id,
        .payload = payload,
    };
    assert(ksd_frame_write(descriptor, &response));
}

static bool cancel_first_permission(const ksd_permission_entry *entry,
                                    void *user_data)
{
    visitor_state *state = user_data;
    state->calls++;
    assert(entry->scopes == KSD_SCOPE_SCREEN_CAPTURE);
    assert(strcmp(entry->executable, "/first") == 0);
    return false;
}

static bool accept_future_permission(const ksd_permission_entry *entry,
                                     void *user_data)
{
    visitor_state *state = user_data;
    state->calls++;
    assert(entry->scopes
           == (KSD_SCOPE_SCREEN_CAPTURE | TEST_FUTURE_SCOPE));
    assert(strcmp(entry->executable, "/future") == 0);
    return true;
}

static void write_permission_entry(int descriptor, const ksd_frame *request,
                                   const char *path, uint32_t scopes,
                                   uint64_t granted_at)
{
    uint8_t tail[48u + 32u] = { 0 };
    size_t path_length = strlen(path);
    assert(path_length <= 32u);
    ksd_encode_u32(tail, scopes);
    ksd_encode_u32(tail + 4u, (uint32_t)path_length);
    ksd_encode_u64(tail + 8u, granted_at);
    memset(tail + 16u, (int)(granted_at & 0xffu), 32u);
    memcpy(tail + 48u, path, path_length);
    write_ok_response(descriptor, request, tail,
                      48u + (uint32_t)path_length, true);
}

static void encode_hello_tail(uint8_t tail[24], uint32_t granted,
                              uint64_t operations, uint32_t backend)
{
    memset(tail, 0, 24u);
    ksd_encode_u32(tail, granted);
    ksd_encode_u64(tail + 8u, operations);
    ksd_encode_u32(tail + 16u, backend);
}

static void check_hello_tail_compatibility(void)
{
    uint8_t tail[24];
    uint32_t granted = 0u;
    uint64_t operations = 0u;
    uint32_t backend = 0u;

    encode_hello_tail(tail, KSD_SCOPE_SCREEN_CAPTURE,
                      KSD_OPERATION_CAPTURE_AREA, KSD_BACKEND_GNOME);
    assert(ksd_client_test_parse_hello_tail(tail, 24u, &granted,
                                            &operations, &backend));
    assert(granted == KSD_SCOPE_SCREEN_CAPTURE);
    assert(operations == KSD_OPERATION_CAPTURE_AREA);
    assert(backend == KSD_BACKEND_GNOME);

    encode_hello_tail(tail,
                      KSD_SCOPE_SCREEN_CAPTURE | KSD_SCOPE_INPUT_MONITORING
                          | TEST_FUTURE_SCOPE,
                      KSD_OPERATION_CAPTURE_AREA | TEST_FUTURE_OPERATION,
                      TEST_FUTURE_BACKEND);
    assert(ksd_client_test_parse_hello_tail(tail, 24u, &granted,
                                            &operations, &backend));
    assert(granted == KSD_SCOPE_SCREEN_CAPTURE);
    assert(operations
           == (KSD_OPERATION_CAPTURE_AREA | TEST_FUTURE_OPERATION));
    assert(backend == TEST_FUTURE_BACKEND);
    assert(strcmp(ksd_backend_name(backend), "unknown") == 0);

    encode_hello_tail(tail, 0u, 0u, KSD_BACKEND_NONE);
    tail[4] = 1u;
    assert(!ksd_client_test_parse_hello_tail(tail, 24u, &granted,
                                             &operations, &backend));
    encode_hello_tail(tail, 0u, 0u, KSD_BACKEND_NONE);
    tail[20] = 1u;
    assert(!ksd_client_test_parse_hello_tail(tail, 24u, &granted,
                                             &operations, &backend));
    encode_hello_tail(tail, 0u, 0u, KSD_BACKEND_NONE);
    assert(!ksd_client_test_parse_hello_tail(tail, 23u, &granted,
                                             &operations, &backend));
}

/* A capture arrives as a sealed memfd, never as payload bytes. The contents
 * only have to satisfy parse_capture_tail, which checks the 20-byte header and
 * the declared length; it does not decode the image. */
static int capture_memfd(uint32_t width, uint32_t height,
                         uint32_t byte_length, bool seal)
{
    uint8_t header[20] = { 0 };
    uint8_t *pixels = calloc(byte_length, 1u);
    int seals = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
    int descriptor = memfd_create("keysharp-desktop-test",
                                  MFD_CLOEXEC | MFD_ALLOW_SEALING);
    assert(pixels != NULL && descriptor >= 0);
    ksd_encode_u16(header, KSD_CAPTURE_FORMAT_PNG);
    ksd_encode_u32(header + 4u, width);
    ksd_encode_u32(header + 8u, height);
    ksd_encode_u32(header + 16u, byte_length);
    assert(write(descriptor, header, sizeof(header)) == (ssize_t)sizeof(header));
    assert(write(descriptor, pixels, byte_length) == (ssize_t)byte_length);
    free(pixels);
    if (seal)
        assert(fcntl(descriptor, F_ADD_SEALS, seals) == 0);
    return descriptor;
}

static void write_capture_response(int descriptor, const ksd_frame *request,
                                   int payload_fd)
{
    uint8_t payload[8] = { 0 };
    ksd_encode_u32(payload, KSD_STATUS_OK);
    ksd_frame response = {
        .magic = {
            KSD_FRAME_MAGIC_0, KSD_FRAME_MAGIC_1,
            KSD_FRAME_MAGIC_2, KSD_FRAME_MAGIC_3,
        },
        .major = KSD_PROTOCOL_MAJOR,
        .minor = KSD_PROTOCOL_MINOR,
        .opcode = request->opcode,
        .flags = KSD_FLAG_RESPONSE,
        .request_id = request->request_id,
        .payload = payload,
        .payload_length = (uint32_t)sizeof(payload),
    };
    assert(ksd_frame_write_fd(descriptor, &response, payload_fd));
    assert(close(payload_fd) == 0);
}

int main(void)
{
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);

    ksd_frame list_request = {
        .opcode = KSD_OP_PERMISSIONS_LIST,
        .request_id = 1u,
    };
    write_permission_entry(sockets[1], &list_request, "/first",
                           KSD_SCOPE_SCREEN_CAPTURE, 1u);
    write_permission_entry(sockets[1], &list_request, "/second",
                           KSD_SCOPE_SCREEN_CAPTURE, 2u);
    write_ok_response(sockets[1], &list_request, NULL, 0u, false);

    uint8_t position_tail[8u];
    ksd_encode_u32(position_tail, 12u);
    ksd_encode_u32(position_tail + 4u, 34u);
    ksd_frame position_request = {
        .opcode = KSD_OP_CURSOR_POSITION,
        .request_id = 2u,
    };
    write_ok_response(sockets[1], &position_request, position_tail,
                      sizeof(position_tail), false);

    ksd_frame future_list_request = {
        .opcode = KSD_OP_PERMISSIONS_LIST,
        .request_id = 3u,
    };
    write_permission_entry(sockets[1], &future_list_request, "/future",
                           KSD_SCOPE_SCREEN_CAPTURE | TEST_FUTURE_SCOPE, 3u);
    write_ok_response(sockets[1], &future_list_request, NULL, 0u, false);

    ksd_frame capture_request = {
        .opcode = KSD_OP_CAPTURE_AREA,
        .request_id = 4u,
    };
    write_capture_response(sockets[1], &capture_request,
                           capture_memfd(2u, 2u, 64u, true));

    ksd_frame unsealed_request = {
        .opcode = KSD_OP_CAPTURE_AREA,
        .request_id = 5u,
    };
    write_capture_response(sockets[1], &unsealed_request,
                           capture_memfd(2u, 2u, 64u, false));

    check_hello_tail_compatibility();

    ksd_connection *connection =
        ksd_client_test_adopt_descriptor(sockets[0]);
    assert(connection != NULL);
    ksd_error error;
    ksd_error_init(&error);

    ksd_capture capture;
    ksd_capture_init(&capture);
    capture.data.data = (uint8_t *)(uintptr_t)1u;
    assert(ksd_capture_area(connection, 0, 0, 1u, 1u, &capture, &error)
           == KSD_STATUS_INVALID_REQUEST);

    ksd_string string;
    ksd_string_init(&string);
    string.data = (char *)(uintptr_t)1u;
    assert(ksd_window_list_json(connection, 0u, &string, &error)
           == KSD_STATUS_INVALID_REQUEST);

    ksd_string_list strings;
    ksd_string_list_init(&strings);
    strings.items = (ksd_string *)(uintptr_t)1u;
    assert(ksd_clipboard_mimetypes(connection, &strings, &error)
           == KSD_STATUS_INVALID_REQUEST);

    ksd_bytes bytes;
    ksd_bytes_init(&bytes);
    bytes.data = (uint8_t *)(uintptr_t)1u;
    assert(ksd_clipboard_content(connection, "text/plain", &bytes, &error)
           == KSD_STATUS_INVALID_REQUEST);

    ksd_point nonempty_point;
    ksd_point_init(&nonempty_point);
    nonempty_point.x = 1;
    assert(ksd_cursor_position(connection, &nonempty_point, &error)
           == KSD_STATUS_INVALID_REQUEST);

    ksd_rectangle nonempty_rectangle;
    ksd_rectangle_init(&nonempty_rectangle);
    nonempty_rectangle.width = 1u;
    assert(ksd_work_area(connection, &nonempty_rectangle, &error)
           == KSD_STATUS_INVALID_REQUEST);

    ksd_window_event window_event;
    ksd_window_event_init(&window_event);
    window_event.window_json.data = (char *)(uintptr_t)1u;
    assert(ksd_window_watch_next(connection, 1u, &window_event, &error)
           == KSD_STATUS_INVALID_REQUEST);

    ksd_clipboard_event clipboard_event;
    ksd_clipboard_event_init(&clipboard_event);
    clipboard_event.mimetypes.items = (ksd_string *)(uintptr_t)1u;
    assert(ksd_clipboard_watch_next(connection, 1u, &clipboard_event, &error)
           == KSD_STATUS_INVALID_REQUEST);

    visitor_state state = { 0 };
    assert(ksd_permissions_list(connection, cancel_first_permission, &state,
                                &error) == KSD_STATUS_CANCELLED);
    assert(state.calls == 1u);

    ksd_point point;
    ksd_point_init(&point);
    ksd_error_init(&error);
    assert(ksd_cursor_position(connection, &point, &error) == KSD_STATUS_OK);
    assert(point.x == 12 && point.y == 34);

    visitor_state future = { 0 };
    assert(ksd_permissions_list(connection, accept_future_permission,
                                &future, &error) == KSD_STATUS_OK);
    assert(future.calls == 1u);

    /* The pixels travel in the descriptor, so the response payload carries
     * none of them and the client reads them out of the mapping. */
    ksd_capture received;
    ksd_capture_init(&received);
    ksd_error_init(&error);
    assert(ksd_capture_area(connection, 0, 0, 2u, 2u, &received, &error)
           == KSD_STATUS_OK);
    assert(received.width == 2u && received.height == 2u);
    assert(received.data.length == 64u && received.data.data != NULL);
    ksd_capture_clear(&received);

    /* An unsealed descriptor could be rewritten after its length was agreed,
     * so it is refused rather than mapped. */
    ksd_capture unsealed;
    ksd_capture_init(&unsealed);
    ksd_error_init(&error);
    assert(ksd_capture_area(connection, 0, 0, 2u, 2u, &unsealed, &error)
           != KSD_STATUS_OK);
    ksd_capture_clear(&unsealed);

    ksd_disconnect(connection);
    close(sockets[1]);

    int lease_sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0,
                      lease_sockets) == 0);
    uint8_t revoked_payload[8] = { 0 };
    ksd_encode_u32(revoked_payload,
                   KSD_SCOPE_SCREEN_CAPTURE | TEST_FUTURE_SCOPE);
    ksd_frame revoked_event = {
        .magic = {
            KSD_FRAME_MAGIC_0, KSD_FRAME_MAGIC_1,
            KSD_FRAME_MAGIC_2, KSD_FRAME_MAGIC_3,
        },
        .major = KSD_PROTOCOL_MAJOR,
        .minor = KSD_PROTOCOL_MINOR,
        .opcode = KSD_OP_SESSION_REVOKED,
        .flags = KSD_FLAG_EVENT,
        .payload_length = 8u,
        .request_id = 0u,
        .payload = revoked_payload,
    };
    assert(ksd_frame_write(lease_sockets[1], &revoked_event));

    ksd_connection *lease =
        ksd_client_test_adopt_descriptor(lease_sockets[0]);
    assert(lease != NULL);
    ksd_client_test_set_role(lease, KSD_ROLE_AUTHORIZATION_LEASE);
    uint32_t revoked = 0u;
    ksd_error_init(&error);
    assert(ksd_lease_next(lease, 1000u, &revoked, &error) == KSD_STATUS_OK);
    assert(revoked == (KSD_SCOPE_SCREEN_CAPTURE | TEST_FUTURE_SCOPE));
    ksd_disconnect(lease);
    close(lease_sockets[1]);
    return 0;
}
