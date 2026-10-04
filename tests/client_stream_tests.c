#include "keysharp_desktop/client.h"
#include "protocol.h"
#include "protocol_io.h"
#include "state_wire.h"
#include "transport.h"

#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#define TEST_FUTURE_SCOPE 0x00000100u
#define TEST_FUTURE_BACKEND 6u
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
    if (ksd_state_mutates_windows(request->opcode)) {
        ksd_encode_u64(payload + 8u + tail_length, 17u); tail_length += 8u;
    }
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

#define TEST_HANDLE UINT64_C(0x0102030405060708)
#define TEST_COOKIE UINT64_C(0x1112131415161718)
#define LE32(value) \
    (uint8_t)((uint32_t)(value)), \
    (uint8_t)((uint32_t)(value) >> 8u), \
    (uint8_t)((uint32_t)(value) >> 16u), \
    (uint8_t)((uint32_t)(value) >> 24u)
#define LE64(value) \
    LE32((uint64_t)(value)), LE32((uint64_t)(value) >> 32u)

static const uint8_t authorize_payload[16] = {
    LE32(KSD_AUTH_REQUEST), LE32(KSD_SCOPE_SCREEN_CAPTURE),
};
static const uint8_t revoke_payload[48] = {
    LE32(KSD_PERMISSION_TARGET_ALL), LE32(KSD_SCOPE_SCREEN_CAPTURE),
};
static const uint8_t capture_area_payload[] = {
    LE32(UINT32_C(0xfffffffe)), LE32(3u), LE32(4u), LE32(5u),
};
static const uint8_t capture_window_payload[] = {
    LE32(1u), LE32(1u), 'w',
};
static const uint8_t window_list_payload[8] = { LE32(1u) };
static const uint8_t handle_payload[] = { LE64(TEST_HANDLE) };
static const uint8_t cookie_payload[] = { LE64(TEST_COOKIE) };
static const uint8_t geometry_payload[] = {
    LE64(TEST_HANDLE), LE32(UINT32_C(0xfffffffe)), LE32(3u),
    LE32(4u), LE32(5u),
};
static const uint8_t value_payload[16] = {
    LE64(TEST_HANDLE), LE32(1u),
};
static const uint8_t unminimize_payload[16] = {
    LE64(TEST_HANDLE), LE32(3u),
};
static const uint8_t point_payload[16] = {
    LE32(UINT32_C(0xfffffffe)), LE32(3u), LE32(1u),
};
static const uint8_t revision_payload[] = { LE32(1u), 'r' };
static const uint8_t title_payload[] = {
    LE64(TEST_HANDLE), LE32(1u), 't',
};
static const uint8_t click_payload[] = {
    LE64(TEST_HANDLE), LE32(UINT32_C(0xfffffffe)), LE32(3u),
    LE32(1u), LE32(2u),
};
static const uint8_t button_payload[] = {
    LE64(TEST_HANDLE), LE32(UINT32_C(0xfffffffe)), LE32(3u),
    LE32(2u), LE32(1u),
};
static const uint8_t reserve_payload[24] = {
    LE64(TEST_COOKIE), LE32(UINT32_C(0xfffffffe)), LE32(3u), LE32(1000u),
};
static const uint8_t clipboard_type_payload[] = { LE32(1u), 'x' };
static const uint8_t clipboard_content_payload[] = {
    LE32(1u), 'x', LE32(1u), 'v',
};
static const uint8_t clipboard_text_payload[] = {
    LE32(24u),
    't', 'e', 'x', 't', '/', 'p', 'l', 'a', 'i', 'n', ';', 'c',
    'h', 'a', 'r', 's', 'e', 't', '=', 'u', 't', 'f', '-', '8',
    LE32(1u), 't',
};
static const uint8_t pair_payload[] = { LE32(1u), LE32(1u) };

typedef ksd_status (*round_trip_call)(ksd_connection *, ksd_error *);

#define SIMPLE_CALL(name, ...) \
    static ksd_status call_##name(ksd_connection *connection, \
                                  ksd_error *error) \
    { \
        return __VA_ARGS__; \
    }
#define STRUCT_CALL(name, type, init, ...) \
    static ksd_status call_##name(ksd_connection *connection, \
                                  ksd_error *error) \
    { \
        type output; \
        init(&output); \
        return __VA_ARGS__; \
    }
#define OWNED_CALL(name, type, init, clear, ...) \
    static ksd_status call_##name(ksd_connection *connection, \
                                  ksd_error *error) \
    { \
        type output; \
        init(&output); \
        ksd_status status = __VA_ARGS__; \
        clear(&output); \
        return status; \
    }

static bool accept_no_permissions(const ksd_permission_entry *entry,
                                  void *user_data)
{
    (void)entry;
    (void)user_data;
    return true;
}

static ksd_status call_authorize(ksd_connection *connection, ksd_error *error)
{
    uint32_t granted = 0u;
    return ksd_authorize(connection, KSD_AUTH_REQUEST,
                         KSD_SCOPE_SCREEN_CAPTURE, &granted, error);
}

SIMPLE_CALL(ping, ksd_ping(connection, error))
SIMPLE_CALL(permissions_list,
    ksd_permissions_list(connection, accept_no_permissions, NULL, error))

static ksd_status call_permissions_revoke(ksd_connection *connection,
                                          ksd_error *error)
{
    ksd_permission_revoke revoke;
    ksd_permission_revoke_init(&revoke);
    revoke.target_kind = KSD_PERMISSION_TARGET_ALL;
    revoke.scopes = KSD_SCOPE_SCREEN_CAPTURE;
    return ksd_permissions_revoke(connection, &revoke, error);
}

OWNED_CALL(capture_area, ksd_capture, ksd_capture_init, ksd_capture_clear,
    ksd_capture_area(connection, -2, 3, 4u, 5u, &output, error))
OWNED_CALL(capture_desktop, ksd_capture, ksd_capture_init, ksd_capture_clear,
    ksd_capture_desktop(connection, &output, error))
OWNED_CALL(capture_window, ksd_capture, ksd_capture_init, ksd_capture_clear,
    ksd_capture_window(connection, "w", 1u, &output, error))
STRUCT_CALL(cursor_position, ksd_point, ksd_point_init,
    ksd_cursor_position(connection, &output, error))
STRUCT_CALL(work_area, ksd_rectangle, ksd_rectangle_init,
    ksd_work_area(connection, &output, error))
OWNED_CALL(window_handles, ksd_string, ksd_string_init, ksd_string_clear,
    ksd_window_handles_json(connection, &output, error))
OWNED_CALL(window_list, ksd_string, ksd_string_init, ksd_string_clear,
    ksd_window_list_json(connection, 1u, &output, error))
OWNED_CALL(window_active, ksd_string, ksd_string_init, ksd_string_clear,
    ksd_window_active_json(connection, &output, error))
OWNED_CALL(window_query, ksd_string, ksd_string_init, ksd_string_clear,
    ksd_window_query_json(connection, TEST_HANDLE, &output, error))
OWNED_CALL(window_children, ksd_string, ksd_string_init, ksd_string_clear,
    ksd_window_children_json(connection, TEST_HANDLE, &output, error))
OWNED_CALL(window_at_point, ksd_string, ksd_string_init, ksd_string_clear,
    ksd_window_at_point_json(connection, -2, 3, 1u, &output, error))
OWNED_CALL(display_list, ksd_string, ksd_string_init, ksd_string_clear,
    ksd_display_list_json(connection, &output, error))
OWNED_CALL(keyboard_state, ksd_string, ksd_string_init, ksd_string_clear,
    ksd_keyboard_state_json(connection, &output, error))
OWNED_CALL(keyboard_state_since, ksd_string, ksd_string_init, ksd_string_clear,
    ksd_keyboard_state_since_json(connection, "r", &output, error))
SIMPLE_CALL(window_set_title,
    ksd_window_set_title(connection, TEST_HANDLE, "t", error))
SIMPLE_CALL(window_set_visible,
    ksd_window_set_visible(connection, TEST_HANDLE, 1u, error))
SIMPLE_CALL(window_redraw,
    ksd_window_redraw(connection, TEST_HANDLE, error))
SIMPLE_CALL(window_click,
    ksd_window_click(connection, TEST_HANDLE, -2, 3, 1u, 2u, error))
SIMPLE_CALL(window_button,
    ksd_window_button(connection, TEST_HANDLE, -2, 3, 2u, 1u, error))
SIMPLE_CALL(window_focus_child,
    ksd_window_focus_child(connection, TEST_HANDLE, error))
SIMPLE_CALL(window_focus, ksd_window_focus(connection, TEST_HANDLE, error))
SIMPLE_CALL(window_raise, ksd_window_raise(connection, TEST_HANDLE, error))
SIMPLE_CALL(window_lower, ksd_window_lower(connection, TEST_HANDLE, error))
SIMPLE_CALL(window_close, ksd_window_close(connection, TEST_HANDLE, error))
SIMPLE_CALL(window_kill, ksd_window_kill(connection, TEST_HANDLE, error))
SIMPLE_CALL(window_move_resize,
    ksd_window_move_resize(connection, TEST_HANDLE, -2, 3, 4u, 5u, error))
SIMPLE_CALL(window_move_resize_xid,
    ksd_window_move_resize_xid(connection, TEST_HANDLE, -2, 3, 4u, 5u,
                               error))
SIMPLE_CALL(window_set_state,
    ksd_window_set_state(connection, TEST_HANDLE, 1u, error))
SIMPLE_CALL(window_unminimize,
    ksd_window_set_state(connection, TEST_HANDLE, KSD_WINDOW_STATE_UNMINIMIZED, error))
SIMPLE_CALL(window_set_opacity,
    ksd_window_set_opacity(connection, TEST_HANDLE, 1u, error))
SIMPLE_CALL(window_set_above,
    ksd_window_set_above(connection, TEST_HANDLE, 1u, error))
SIMPLE_CALL(window_set_decorated,
    ksd_window_set_decorated(connection, TEST_HANDLE, 1u, error))
SIMPLE_CALL(window_set_skip_taskbar,
    ksd_window_set_skip_taskbar(connection, TEST_HANDLE, 1u, error))
SIMPLE_CALL(window_reserve,
    ksd_window_reserve(connection, TEST_COOKIE, -2, 3, 1000u, error))

static ksd_status call_window_get_reserved(ksd_connection *connection,
                                           ksd_error *error)
{
    uint64_t handle = 0u;
    return ksd_window_get_reserved(connection, TEST_COOKIE, &handle, error);
}

OWNED_CALL(clipboard_mimetypes, ksd_string_list, ksd_string_list_init,
    ksd_string_list_clear,
    ksd_clipboard_mimetypes(connection, &output, error))
OWNED_CALL(clipboard_content, ksd_bytes, ksd_bytes_init, ksd_bytes_clear,
    ksd_clipboard_content(connection, "x", &output, error))
OWNED_CALL(clipboard_text, ksd_string, ksd_string_init, ksd_string_clear,
    ksd_clipboard_text(connection, &output, error))
SIMPLE_CALL(clipboard_set_content,
    ksd_clipboard_set_content(connection, "x", "v", 1u, error))
SIMPLE_CALL(clipboard_set_text,
    ksd_clipboard_set_text(connection, "t", error))
SIMPLE_CALL(mouse_move_absolute,
    ksd_mouse_move_absolute(connection, 1, 1, error))
SIMPLE_CALL(mouse_move_relative,
    ksd_mouse_move_relative(connection, 1, 1, error))
SIMPLE_CALL(mouse_button, ksd_mouse_button(connection, 1u, 1u, error))
SIMPLE_CALL(mouse_scroll, ksd_mouse_scroll(connection, 1, 1u, error))
SIMPLE_CALL(state_subscribe, ksd_state_subscribe(connection, KSD_STATE_GRANTS, error))

typedef enum response_fixture {
    RESPONSE_EMPTY,
    RESPONSE_AUTHORIZE,
    RESPONSE_CAPTURE,
    RESPONSE_POINT,
    RESPONSE_RECTANGLE,
    RESPONSE_STRING,
    RESPONSE_HANDLE,
    RESPONSE_STRING_LIST,
    RESPONSE_BYTES,
} response_fixture;

typedef struct round_trip_case {
    uint16_t opcode;
    uint32_t role;
    round_trip_call call;
    const uint8_t *payload;
    uint32_t payload_length;
    response_fixture response;
} round_trip_case;

#define EMPTY_CASE(name, opcode, response) \
    { opcode, KSD_ROLE_RPC, call_##name, NULL, 0u, response }
#define PAYLOAD_CASE(name, opcode, payload, response) \
    { opcode, KSD_ROLE_RPC, call_##name, payload, sizeof(payload), response }

static const round_trip_case round_trip_cases[] = {
    { KSD_OP_AUTHORIZE, KSD_ROLE_AUTHORIZATION_LEASE, call_authorize, authorize_payload, sizeof(authorize_payload), RESPONSE_AUTHORIZE },
    EMPTY_CASE(ping, KSD_OP_PING, RESPONSE_EMPTY),
    EMPTY_CASE(permissions_list, KSD_OP_PERMISSIONS_LIST, RESPONSE_EMPTY),
    PAYLOAD_CASE(permissions_revoke, KSD_OP_PERMISSIONS_REVOKE,
                 revoke_payload, RESPONSE_EMPTY),
    PAYLOAD_CASE(capture_area, KSD_OP_CAPTURE_AREA, capture_area_payload,
                 RESPONSE_CAPTURE),
    EMPTY_CASE(capture_desktop, KSD_OP_CAPTURE_DESKTOP, RESPONSE_CAPTURE),
    PAYLOAD_CASE(capture_window, KSD_OP_CAPTURE_WINDOW,
                 capture_window_payload, RESPONSE_CAPTURE),
    EMPTY_CASE(cursor_position, KSD_OP_CURSOR_POSITION, RESPONSE_POINT),
    EMPTY_CASE(work_area, KSD_OP_WORK_AREA, RESPONSE_RECTANGLE),
    EMPTY_CASE(window_handles, KSD_OP_WINDOW_HANDLES, RESPONSE_STRING),
    PAYLOAD_CASE(window_list, KSD_OP_WINDOW_LIST, window_list_payload,
                 RESPONSE_STRING),
    EMPTY_CASE(window_active, KSD_OP_WINDOW_ACTIVE, RESPONSE_STRING),
    PAYLOAD_CASE(window_query, KSD_OP_WINDOW_QUERY, handle_payload,
                 RESPONSE_STRING),
    PAYLOAD_CASE(window_children, KSD_OP_WINDOW_CHILDREN, handle_payload,
                 RESPONSE_STRING),
    PAYLOAD_CASE(window_at_point, KSD_OP_WINDOW_AT_POINT, point_payload,
                 RESPONSE_STRING),
    EMPTY_CASE(display_list, KSD_OP_DISPLAY_LIST, RESPONSE_STRING),
    EMPTY_CASE(keyboard_state, KSD_OP_KEYBOARD_STATE, RESPONSE_STRING),
    PAYLOAD_CASE(keyboard_state_since, KSD_OP_KEYBOARD_STATE,
                 revision_payload, RESPONSE_STRING),
    PAYLOAD_CASE(window_set_title, KSD_OP_WINDOW_SET_TITLE, title_payload,
                 RESPONSE_EMPTY),
    PAYLOAD_CASE(window_set_visible, KSD_OP_WINDOW_SET_VISIBLE, value_payload,
                 RESPONSE_EMPTY),
    PAYLOAD_CASE(window_redraw, KSD_OP_WINDOW_REDRAW, handle_payload,
                 RESPONSE_EMPTY),
    PAYLOAD_CASE(window_click, KSD_OP_WINDOW_CLICK, click_payload,
                 RESPONSE_EMPTY),
    PAYLOAD_CASE(window_button, KSD_OP_WINDOW_BUTTON, button_payload,
                 RESPONSE_EMPTY),
    PAYLOAD_CASE(window_focus_child, KSD_OP_WINDOW_FOCUS_CHILD,
                 handle_payload, RESPONSE_EMPTY),
    PAYLOAD_CASE(window_focus, KSD_OP_WINDOW_FOCUS, handle_payload,
                 RESPONSE_EMPTY),
    PAYLOAD_CASE(window_raise, KSD_OP_WINDOW_RAISE, handle_payload,
                 RESPONSE_EMPTY),
    PAYLOAD_CASE(window_lower, KSD_OP_WINDOW_LOWER, handle_payload,
                 RESPONSE_EMPTY),
    PAYLOAD_CASE(window_close, KSD_OP_WINDOW_CLOSE, handle_payload,
                 RESPONSE_EMPTY),
    PAYLOAD_CASE(window_kill, KSD_OP_WINDOW_KILL, handle_payload,
                 RESPONSE_EMPTY),
    PAYLOAD_CASE(window_move_resize, KSD_OP_WINDOW_MOVE_RESIZE,
                 geometry_payload, RESPONSE_EMPTY),
    PAYLOAD_CASE(window_move_resize_xid, KSD_OP_WINDOW_MOVE_RESIZE_XID,
                 geometry_payload, RESPONSE_EMPTY),
    PAYLOAD_CASE(window_set_state, KSD_OP_WINDOW_SET_STATE, value_payload,
                 RESPONSE_EMPTY),
    PAYLOAD_CASE(window_unminimize, KSD_OP_WINDOW_SET_STATE, unminimize_payload,
                 RESPONSE_EMPTY),
    PAYLOAD_CASE(window_set_opacity, KSD_OP_WINDOW_SET_OPACITY, value_payload,
                 RESPONSE_EMPTY),
    PAYLOAD_CASE(window_set_above, KSD_OP_WINDOW_SET_ABOVE, value_payload,
                 RESPONSE_EMPTY),
    PAYLOAD_CASE(window_set_decorated, KSD_OP_WINDOW_SET_DECORATED,
                 value_payload, RESPONSE_EMPTY),
    PAYLOAD_CASE(window_set_skip_taskbar, KSD_OP_WINDOW_SET_SKIP_TASKBAR,
                 value_payload, RESPONSE_EMPTY),
    PAYLOAD_CASE(window_reserve, KSD_OP_WINDOW_RESERVE, reserve_payload,
                 RESPONSE_EMPTY),
    PAYLOAD_CASE(window_get_reserved, KSD_OP_WINDOW_GET_RESERVED,
                 cookie_payload, RESPONSE_HANDLE),
    EMPTY_CASE(clipboard_mimetypes, KSD_OP_CLIPBOARD_MIMETYPES,
               RESPONSE_STRING_LIST),
    PAYLOAD_CASE(clipboard_content, KSD_OP_CLIPBOARD_CONTENT,
                 clipboard_type_payload, RESPONSE_BYTES),
    EMPTY_CASE(clipboard_text, KSD_OP_CLIPBOARD_TEXT, RESPONSE_STRING),
    PAYLOAD_CASE(clipboard_set_content, KSD_OP_CLIPBOARD_SET_CONTENT,
                 clipboard_content_payload, RESPONSE_EMPTY),
    PAYLOAD_CASE(clipboard_set_text, KSD_OP_CLIPBOARD_SET_CONTENT,
                 clipboard_text_payload, RESPONSE_EMPTY),
    PAYLOAD_CASE(mouse_move_absolute, KSD_OP_MOUSE_MOVE_ABSOLUTE,
                 pair_payload, RESPONSE_EMPTY),
    PAYLOAD_CASE(mouse_move_relative, KSD_OP_MOUSE_MOVE_RELATIVE,
                 pair_payload, RESPONSE_EMPTY),
    PAYLOAD_CASE(mouse_button, KSD_OP_MOUSE_BUTTON, pair_payload,
                 RESPONSE_EMPTY),
    PAYLOAD_CASE(mouse_scroll, KSD_OP_MOUSE_SCROLL, pair_payload,
                 RESPONSE_EMPTY),
    { KSD_OP_STATE_SUBSCRIBE, KSD_ROLE_AUTHORIZATION_LEASE, call_state_subscribe,
        (const uint8_t[]) { LE32(KSD_STATE_GRANTS) }, 4u, RESPONSE_EMPTY },
};

_Static_assert(sizeof(round_trip_cases) / sizeof(round_trip_cases[0]) == 49u,
               "every request-producing client API needs a round-trip case");

static void write_round_trip_response(int descriptor,
                                      const round_trip_case *test)
{
    static const uint8_t authorize_tail[8] = {
        LE32(KSD_SCOPE_SCREEN_CAPTURE),
    };
    static const uint8_t point_tail[] = { LE32(1u), LE32(2u) };
    static const uint8_t rectangle_tail[] = {
        LE32(1u), LE32(2u), LE32(3u), LE32(4u),
    };
    static const uint8_t string_tail[] = { LE32(0u) };
    static const uint8_t handle_tail[] = { LE64(TEST_HANDLE) };
    static const uint8_t string_list_tail[8] = { 0 };
    static const uint8_t bytes_tail[] = { LE32(0u) };
    static const struct {
        const uint8_t *data;
        uint32_t length;
    } tails[] = {
        [RESPONSE_EMPTY] = { NULL, 0u },
        [RESPONSE_AUTHORIZE] = { authorize_tail, sizeof(authorize_tail) },
        [RESPONSE_POINT] = { point_tail, sizeof(point_tail) },
        [RESPONSE_RECTANGLE] = { rectangle_tail, sizeof(rectangle_tail) },
        [RESPONSE_STRING] = { string_tail, sizeof(string_tail) },
        [RESPONSE_HANDLE] = { handle_tail, sizeof(handle_tail) },
        [RESPONSE_STRING_LIST] = {
            string_list_tail, sizeof(string_list_tail),
        },
        [RESPONSE_BYTES] = { bytes_tail, sizeof(bytes_tail) },
    };
    ksd_frame request = {
        .opcode = test->opcode,
        .request_id = 1u,
    };
    if (test->response == RESPONSE_CAPTURE) {
        write_capture_response(descriptor, &request,
                               capture_memfd(1u, 1u, 1u, true));
        return;
    }
    assert(test->response < sizeof(tails) / sizeof(tails[0]));
    write_ok_response(descriptor, &request, tails[test->response].data,
                      tails[test->response].length, false);
}

static void check_client_request_round_trips(void)
{
    static const uint8_t magic[4] = {
        KSD_FRAME_MAGIC_0, KSD_FRAME_MAGIC_1,
        KSD_FRAME_MAGIC_2, KSD_FRAME_MAGIC_3,
    };
    for (size_t index = 0u;
         index < sizeof(round_trip_cases) / sizeof(round_trip_cases[0]);
         index++) {
        const round_trip_case *test = &round_trip_cases[index];
        int sockets[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets)
               == 0);
        ksd_connection *connection =
            ksd_client_test_adopt_descriptor(sockets[0]);
        assert(connection != NULL);
        ksd_client_test_set_role(connection, test->role);
        write_round_trip_response(sockets[1], test);

        ksd_error error;
        ksd_error_init(&error);
        assert(test->call(connection, &error) == KSD_STATUS_OK);

        ksd_frame request;
        assert(ksd_frame_read(sockets[1], magic, KSD_PROTOCOL_MAJOR,
                              KSD_PROTOCOL_MINOR, KSD_MAX_REQUEST_PAYLOAD,
                              true, &request) == 1);
        assert(request.opcode == test->opcode && request.flags == 0u
               && request.request_id == 1u
               && request.payload_length == test->payload_length);
        assert(test->payload_length == 0u
               || memcmp(request.payload, test->payload,
                         test->payload_length) == 0);
        ksd_frame_clear(&request);
        ksd_disconnect(connection);
        assert(close(sockets[1]) == 0);
    }
}

#undef PAYLOAD_CASE
#undef EMPTY_CASE
#undef OWNED_CALL
#undef STRUCT_CALL
#undef SIMPLE_CALL
#undef LE64
#undef LE32

static ksd_frame state_frame(ksd_state_event *event, ksd_buffer *payload)
{
    ksd_buffer_init(payload, KSD_MAX_TEXT_BYTES + 256u);
    assert(ksd_state_event_encode(event, payload));
    return (ksd_frame) { .magic = { 'K', 'S', 'D', 'P' }, .major = KSD_PROTOCOL_MAJOR,
        .opcode = KSD_OP_STATE_EVENT, .flags = KSD_FLAG_EVENT, .payload = payload->data, .payload_length = (uint32_t)payload->length };
}

static void check_typed_window(void)
{
    const char json[] = "{\"id\":\"42\",\"title\":\"caf\\u00e9 \\ud83d\\udc08\",\"appId\":\"Editor\",\"frame\":{\"x\":-12,\"y\":3,\"width\":500,\"height\":200},\"visible\":false,\"active\":true,\"validFields\":[\"title\",\"frame\",\"visible\",\"active\"]}";
    ksd_state_event event; ksd_state_event_init(&event);
    assert(ksd_state_window_json((const uint8_t *)json, strlen(json), &event.window));
    assert(event.window.handle == 42u && event.window.frame_x == -12);
    assert(event.window.frame_width == 500u && event.window.flags == KSD_WINDOW_ACTIVE);
    assert((event.window.valid_fields & KSD_FIELD_APP_ID) == 0u);
    assert(strcmp(event.window.title.data, "caf\xc3\xa9 \xf0\x9f\x90\x88") == 0);
    event.kind = KSD_STATE_HIDE; event.domain = KSD_STATE_WINDOWS; event.epoch = 77u; event.sequence = 9u;
    ksd_buffer bytes; ksd_frame frame = state_frame(&event, &bytes); ksd_state_event parsed;
    assert(ksd_state_event_decode(frame.payload, frame.payload_length, &parsed));
    assert(parsed.kind == KSD_STATE_HIDE && parsed.sequence == 9u && parsed.epoch == 77u);
    assert(ksd_state_window_equal(&event.window, &parsed.window));
    ksd_state_event_clear(&parsed); ksd_buffer_clear(&bytes); ksd_state_event_clear(&event);
    const char child[] = "{\"ok\":true,\"window\":{\"id\":\"42\",\"topLevel\":\"7\",\"buffer\":{\"x\":-9,\"y\":11,\"width\":80,\"height\":40}}}";
    assert(ksd_state_window_json((const uint8_t *)child, strlen(child), &event.window));
    assert(ksd_state_window_is_child_json((const uint8_t *)child, strlen(child)));
    assert(event.window.surface_x == -9 && event.window.surface_y == 11 && event.window.surface_width == 80u);
    assert((event.window.valid_fields & KSD_FIELD_SURFACE) != 0u);
    ksd_state_event_clear(&event);
    const char unsupported[] = "{\"id\":\"42\",\"buffer\":{\"x\":9,\"width\":80},\"validFields\":[]}";
    assert(ksd_state_window_json((const uint8_t *)unsupported, strlen(unsupported), &event.window));
    assert((event.window.valid_fields & (KSD_FIELD_BUFFER | KSD_FIELD_SURFACE)) == 0u);
    ksd_state_event_clear(&event);
}

static void check_buffered_readiness(void)
{
    int sockets[2]; assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    ksd_connection *connection = ksd_client_test_adopt_descriptor(sockets[0]); assert(connection != NULL);
    ksd_client_test_set_role(connection, KSD_ROLE_AUTHORIZATION_LEASE);
    ksd_state_event event; ksd_state_event_init(&event); event.kind = KSD_STATE_GRANT_CHANGED;
    event.domain = KSD_STATE_GRANTS; event.epoch = 88u; event.sequence = 1u; event.granted_scopes = KSD_SCOPE_WINDOW_MONITORING;
    ksd_buffer payload; ksd_frame frame = state_frame(&event, &payload); assert(ksd_frame_write(sockets[1], &frame));
    uint8_t granted[8] = { KSD_SCOPE_WINDOW_MONITORING };
    ksd_frame response_request = { .opcode = KSD_OP_AUTHORIZE, .request_id = 1u };
    write_ok_response(sockets[1], &response_request, granted, sizeof(granted), false);
    uint32_t scopes; ksd_error error; ksd_error_init(&error);
    assert(ksd_authorize(connection, KSD_AUTH_CHECK, KSD_SCOPE_WINDOW_MONITORING, &scopes, &error) == KSD_STATUS_OK);
    struct pollfd ready = { .fd = ksd_connection_fd(connection), .events = POLLIN };
    assert(poll(&ready, 1u, 0) == 1);
    ksd_state_event output; ksd_state_event_init(&output);
    assert(ksd_state_next(connection, 0u, &output, &error) == KSD_STATUS_OK);
    assert(output.sequence == 1u && output.granted_scopes == scopes); ksd_state_event_clear(&output);
    assert(ksd_state_next(connection, 0u, &output, &error) == KSD_STATUS_TIMEOUT);
    assert(poll(&ready, 1u, 0) == 0);

    /* Subscription masks change on the same socket, and each response remains ordered. */
    response_request.opcode = KSD_OP_STATE_SUBSCRIBE; response_request.request_id = 2u;
    write_ok_response(sockets[1], &response_request, NULL, 0u, false);
    assert(ksd_state_subscribe(connection, KSD_STATE_WINDOWS | KSD_STATE_GRANTS, &error) == KSD_STATUS_OK);
    response_request.request_id = 3u;
    write_ok_response(sockets[1], &response_request, NULL, 0u, false);
    assert(ksd_state_subscribe(connection, KSD_STATE_GRANTS, &error) == KSD_STATUS_OK);
    const uint8_t magic[4] = { 'K', 'S', 'D', 'P' };
    ksd_frame request;
    for (unsigned i = 0u; i < 3u; i++) {
        assert(ksd_frame_read(sockets[1], magic, KSD_PROTOCOL_MAJOR, 0u, 4096u, true, &request) == 1);
        if (i != 0u) assert(request.opcode == KSD_OP_STATE_SUBSCRIBE && request.request_id == i + 1u);
        ksd_frame_clear(&request);
    }

    ksd_buffer packed; ksd_buffer_init(&packed, 4096u); assert(ksd_frame_pack(&frame, &packed));
    assert(write(sockets[1], packed.data, 7u) == 7);
    uint64_t before = ksd_monotonic_milliseconds();
    assert(ksd_state_next(connection, 0u, &output, &error) == KSD_STATUS_TIMEOUT);
    assert(ksd_monotonic_milliseconds() - before < 100u && poll(&ready, 1u, 0) == 0);
    assert(write(sockets[1], packed.data + 7u, packed.length - 7u) == (ssize_t)(packed.length - 7u));
    assert(poll(&ready, 1u, 1000) == 1);
    assert(ksd_state_next(connection, 0u, &output, &error) == KSD_STATUS_OK); ksd_state_event_clear(&output);
    ksd_state_event clipboard; ksd_state_event_init(&clipboard);
    clipboard.domain = KSD_STATE_CLIPBOARD; clipboard.epoch = 88u;
    const uint32_t clipboard_kinds[] = { KSD_STATE_SNAPSHOT_BEGIN, KSD_STATE_SNAPSHOT_END, KSD_STATE_CHANGED };
    for (unsigned i = 0u; i < 3u; i++) {
        clipboard.kind = clipboard_kinds[i]; clipboard.sequence = i == 2u ? 1u : 0u;
        ksd_buffer data; ksd_frame next = state_frame(&clipboard, &data);
        assert(ksd_frame_write(sockets[1], &next));
        assert(ksd_state_next(connection, 0u, &output, &error) == KSD_STATUS_OK);
        assert(output.domain == KSD_STATE_CLIPBOARD && output.kind == clipboard.kind
            && output.epoch == clipboard.epoch && output.sequence == clipboard.sequence);
        ksd_state_event_clear(&output); ksd_buffer_clear(&data);
    }
    close(sockets[1]); before = ksd_monotonic_milliseconds();
    assert(poll(&ready, 1u, 1000) == 1);
    assert(ksd_state_next(connection, 0u, &output, &error) != KSD_STATUS_OK);
    assert(ksd_monotonic_milliseconds() - before < 100u);
    ksd_buffer_clear(&packed); ksd_buffer_clear(&payload); ksd_disconnect(connection);
}

typedef struct queue_writer { int descriptor; ksd_frame *frame; } queue_writer;

static void *write_queue(void *context)
{
    queue_writer *writer = context;
    for (unsigned i = 0u; i < 257u; i++) assert(ksd_frame_write(writer->descriptor, writer->frame));
    return NULL;
}

static void check_queue_limit_and_terminal_readiness(void)
{
    int sockets[2]; assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    ksd_connection *connection = ksd_client_test_adopt_descriptor(sockets[0]); assert(connection != NULL);
    ksd_client_test_set_role(connection, KSD_ROLE_AUTHORIZATION_LEASE);
    ksd_state_event event; ksd_state_event_init(&event); event.kind = KSD_STATE_CHANGED;
    event.domain = KSD_STATE_KEYBOARD; event.epoch = 1u;
    ksd_buffer bytes; ksd_frame frame = state_frame(&event, &bytes);
    queue_writer writer = { .descriptor = sockets[1], .frame = &frame }; pthread_t thread;
    assert(pthread_create(&thread, NULL, write_queue, &writer) == 0);
    ksd_error error; ksd_error_init(&error); uint32_t scopes;
    assert(ksd_authorize(connection, KSD_AUTH_CHECK, KSD_SCOPE_WINDOW_MONITORING, &scopes, &error) != KSD_STATUS_OK);
    assert(strstr(error.message, "queue") != NULL);
    assert(pthread_join(thread, NULL) == 0);
    for (unsigned i = 0u; i < 256u; i++) {
        ksd_state_event output; ksd_state_event_init(&output);
        assert(ksd_state_next(connection, 0u, &output, &error) == KSD_STATUS_OK); ksd_state_event_clear(&output);
    }
    struct pollfd ready = { .fd = ksd_connection_fd(connection), .events = POLLIN };
    assert(poll(&ready, 1u, 0) == 1);
    ksd_state_event output; ksd_state_event_init(&output);
    assert(ksd_state_next(connection, 0u, &output, &error) != KSD_STATUS_OK);
    assert(poll(&ready, 1u, 0) == 1);
    ksd_buffer_clear(&bytes); close(sockets[1]); ksd_disconnect(connection);
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

    check_typed_window();
    check_buffered_readiness();
    check_queue_limit_and_terminal_readiness();
    check_hello_tail_compatibility();
    check_client_request_round_trips();

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

    ksd_state_event state_event; ksd_state_event_init(&state_event);
    state_event.window.app_id.data = (char *)(uintptr_t)1u;
    assert(ksd_state_next(connection, 0u, &state_event, &error) == KSD_STATUS_INVALID_REQUEST);

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
    ksd_state_event revoke_state; ksd_state_event_init(&revoke_state);
    ksd_error_init(&error);
    assert(ksd_state_next(lease, 1000u, &revoke_state, &error) == KSD_STATUS_OK);
    assert(revoke_state.domain == KSD_STATE_GRANTS && revoke_state.kind == KSD_STATE_GRANT_CHANGED);
    assert(revoke_state.revoked_scopes == (KSD_SCOPE_SCREEN_CAPTURE | TEST_FUTURE_SCOPE));
    ksd_state_event_clear(&revoke_state);
    ksd_disconnect(lease);
    close(lease_sockets[1]);

    /* A nonblocking drain applies arrived revocations and preserves the last grant while idle. */
    int refresh_sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0,
                      refresh_sockets) == 0);
    ksd_connection *refreshing =
        ksd_client_test_adopt_descriptor(refresh_sockets[0]);
    assert(refreshing != NULL);
    ksd_client_test_set_role(refreshing, KSD_ROLE_AUTHORIZATION_LEASE);
    const uint32_t both = KSD_SCOPE_SCREEN_CAPTURE | KSD_SCOPE_WINDOW_CONTROL;
    const ksd_frame authorize_request = {
        .opcode = KSD_OP_AUTHORIZE,
        .request_id = 1u,
    };
    uint8_t grant_tail[8] = { 0 };
    ksd_encode_u32(grant_tail, both);
    write_ok_response(refresh_sockets[1], &authorize_request, grant_tail,
                      sizeof(grant_tail), false);
    uint32_t granted = 0u;
    ksd_error_init(&error);
    assert(ksd_authorize(refreshing, KSD_AUTH_REQUEST, both, &granted, &error)
           == KSD_STATUS_OK);
    assert(granted == both);

    ksd_state_event_init(&revoke_state); ksd_error_init(&error);
    assert(ksd_state_next(refreshing, 0u, &revoke_state, &error) == KSD_STATUS_TIMEOUT);
    assert(granted == both);

    ksd_encode_u32(revoked_payload, KSD_SCOPE_SCREEN_CAPTURE);
    assert(ksd_frame_write(refresh_sockets[1], &revoked_event));
    ksd_error_init(&error);
    assert(ksd_state_next(refreshing, 0u, &revoke_state, &error) == KSD_STATUS_OK);
    assert(revoke_state.granted_scopes == KSD_SCOPE_WINDOW_CONTROL);
    assert(revoke_state.revoked_scopes == KSD_SCOPE_SCREEN_CAPTURE);
    ksd_state_event_clear(&revoke_state);

    close(refresh_sockets[1]);
    ksd_error_init(&error);
    assert(ksd_state_next(refreshing, 0u, &revoke_state, &error) != KSD_STATUS_OK);
    ksd_disconnect(refreshing);
    return 0;
}
