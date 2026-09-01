#include "keysharp_desktop/client.h"
#include "protocol.h"
#include "protocol_io.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

ksd_connection *ksd_client_test_adopt_descriptor(int descriptor);

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

static void write_permission_entry(int descriptor, const ksd_frame *request,
                                   const char *path, uint64_t granted_at)
{
    uint8_t tail[48u + 32u] = { 0 };
    size_t path_length = strlen(path);
    assert(path_length <= 32u);
    ksd_encode_u32(tail, KSD_SCOPE_SCREEN_CAPTURE);
    ksd_encode_u32(tail + 4u, (uint32_t)path_length);
    ksd_encode_u64(tail + 8u, granted_at);
    memset(tail + 16u, (int)(granted_at & 0xffu), 32u);
    memcpy(tail + 48u, path, path_length);
    write_ok_response(descriptor, request, tail,
                      48u + (uint32_t)path_length, true);
}

int main(void)
{
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);

    ksd_frame list_request = {
        .opcode = KSD_OP_PERMISSIONS_LIST,
        .request_id = 1u,
    };
    write_permission_entry(sockets[1], &list_request, "/first", 1u);
    write_permission_entry(sockets[1], &list_request, "/second", 2u);
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

    ksd_disconnect(connection);
    close(sockets[1]);
    return 0;
}
