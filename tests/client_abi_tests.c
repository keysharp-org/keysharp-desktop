#include "keysharp_desktop/client.h"
#include "client_status.h"

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    assert(KSD_CLIENT_ABI_MAJOR == 0u);
    assert(KSD_CLIENT_ABI_MINOR == 1u);
    assert(KSD_ROLE_AUTHORIZATION_LEASE == 3u);
    assert(strcmp(KSD_DEFAULT_SOCKET_PATH,
                  "/run/keysharp-desktop/keysharp-desktop.sock") == 0);
    assert(offsetof(ksd_error, message) == 16u);
    assert(offsetof(ksd_service_info, available_operations) == 16u);
    assert(sizeof(ksd_service_info) == 64u);
    assert(sizeof(ksd_point) == 32u);
    assert(sizeof(ksd_rectangle) == 40u);
    assert(offsetof(ksd_permission_entry, reserved64) == 4184u);
    assert(offsetof(ksd_permission_revoke, pid) == 16u);
    assert(sizeof(ksd_bytes) == 40u);
    assert(offsetof(ksd_bytes, data) == 8u);
    assert(offsetof(ksd_bytes, length) == 16u);
    assert(offsetof(ksd_bytes, reserved) == 24u);
    assert(sizeof(ksd_string) == 40u);
    assert(sizeof(ksd_string_list) == 40u);
    assert(offsetof(ksd_capture, data) == 24u);
    assert(offsetof(ksd_capture, reserved) == 64u);
    assert(sizeof(ksd_capture) == 96u);
    assert(sizeof(ksd_window_event) == 80u);
    assert(sizeof(ksd_clipboard_event) == 120u);

    ksd_error error;
    ksd_connect_options options;
    ksd_service_info info;
    ksd_point point;
    ksd_rectangle rectangle;
    ksd_permission_entry entry;
    ksd_permission_revoke revoke;
    ksd_capture capture;
    ksd_bytes bytes;
    ksd_string string;
    ksd_string_list strings;
    ksd_window_event window_event;
    ksd_clipboard_event clipboard_event;
    ksd_error_init(&error);
    ksd_connect_options_init(&options);
    ksd_service_info_init(&info);
    ksd_point_init(&point);
    ksd_rectangle_init(&rectangle);
    ksd_permission_entry_init(&entry);
    ksd_permission_revoke_init(&revoke);
    ksd_capture_init(&capture);
    ksd_bytes_init(&bytes);
    ksd_string_init(&string);
    ksd_string_list_init(&strings);
    ksd_window_event_init(&window_event);
    ksd_clipboard_event_init(&clipboard_event);
    assert(error.struct_size == sizeof(error));
    assert(options.struct_size == sizeof(options));
    assert(options.role == KSD_ROLE_RPC);
    assert(options.authorization_mode == KSD_AUTH_CHECK);
    assert(info.struct_size == sizeof(info));
    assert(point.struct_size == sizeof(point));
    assert(rectangle.struct_size == sizeof(rectangle));

    ksd_connection *connection = NULL;
    ksd_connect_options_init(&options);
    ksd_error_init(&error);
    options.role = 2u;
    assert(ksd_connect(&options, &connection, &info, &error)
           == KSD_STATUS_INVALID_REQUEST);
    assert(connection == NULL);
    ksd_connect_options_init(&options);
    ksd_service_info_init(&info);
    info.backend = KSD_BACKEND_GNOME;
    assert(ksd_connect(&options, &connection, &info, &error)
           == KSD_STATUS_INVALID_REQUEST);
    assert(connection == NULL);
    ksd_service_info_init(&info);
    options.requested_scopes = KSD_SCOPE_INPUT_MONITORING;
    assert(ksd_connect(&options, &connection, &info, &error)
           == KSD_STATUS_INVALID_REQUEST);
    assert(connection == NULL);
    assert(entry.struct_size == sizeof(entry));
    assert(revoke.struct_size == sizeof(revoke));
    assert(capture.struct_size == sizeof(capture));
    assert(capture.data.struct_size == sizeof(capture.data));
    assert(bytes.struct_size == sizeof(bytes));
    assert(string.struct_size == sizeof(string));
    assert(strings.struct_size == sizeof(strings));
    assert(window_event.window_json.struct_size
        == sizeof(window_event.window_json));
    assert(clipboard_event.text.struct_size
        == sizeof(clipboard_event.text));
    assert(clipboard_event.mimetypes.struct_size
        == sizeof(clipboard_event.mimetypes));
    assert(window_event.struct_size == sizeof(window_event));
    assert(clipboard_event.struct_size == sizeof(clipboard_event));
    assert(strcmp(ksd_backend_name(KSD_BACKEND_GNOME), "gnome") == 0);
    assert(strcmp(ksd_status_name(KSD_STATUS_REVOKED), "revoked") == 0);
    assert(ksd_status_for_system_error(ETIMEDOUT) == KSD_STATUS_TIMEOUT);
    assert(ksd_status_for_system_error(EAGAIN) == KSD_STATUS_TIMEOUT);
    assert(ksd_status_for_system_error(EPIPE) == KSD_STATUS_UNAVAILABLE);
    bytes.data = malloc(8u);
    bytes.length = 8u;
    assert(bytes.data != NULL);
    ksd_bytes_clear(&bytes);
    assert(bytes.struct_size == sizeof(bytes));
    assert(bytes.data == NULL && bytes.length == 0u);
    ksd_bytes uninitialized = { 0 };
    uninitialized.data = (uint8_t *)(uintptr_t)1u;
    ksd_bytes_clear(&uninitialized);
    assert(uninitialized.data == (uint8_t *)(uintptr_t)1u);
    return 0;
}
