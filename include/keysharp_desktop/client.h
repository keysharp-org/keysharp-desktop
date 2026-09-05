#ifndef KEYSHARP_DESKTOP_CLIENT_H
#define KEYSHARP_DESKTOP_CLIENT_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(KEYSHARP_DESKTOP_CLIENT_BUILD)
#    define KSD_API __declspec(dllexport)
#  else
#    define KSD_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define KSD_API __attribute__((visibility("default")))
#else
#  define KSD_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define KSD_CLIENT_ABI_MAJOR 0u
/* Unknown backend, operation and scope values are delivered verbatim. */
#define KSD_CLIENT_ABI_MINOR 8u
#define KSD_DEFAULT_SOCKET_PATH "/run/keysharp-desktop/keysharp-desktop.sock"
#define KSD_SOCKET_ENV "KEYSHARP_DESKTOP_SOCKET"
#define KSD_ERROR_MESSAGE_CAPACITY 256u
#define KSD_PERMISSION_HASH_HEX_SIZE 65u
#define KSD_EXECUTABLE_PATH_SIZE 4096u
#define KSD_DEFAULT_REQUEST_TIMEOUT_MS 130000u
#define KSD_MAX_WATCH_TIMEOUT_MS 60000u

typedef uint32_t ksd_status;
enum {
    KSD_STATUS_OK = 0u,
    KSD_STATUS_DENIED = 1u,
    KSD_STATUS_UNSUPPORTED = 2u,
    KSD_STATUS_INVALID_REQUEST = 3u,
    KSD_STATUS_UNAVAILABLE = 4u,
    KSD_STATUS_BUSY = 5u,
    KSD_STATUS_NOT_FOUND = 6u,
    KSD_STATUS_RESOURCE_EXHAUSTED = 7u,
    KSD_STATUS_TIMEOUT = 8u,
    KSD_STATUS_CANCELLED = 9u,
    KSD_STATUS_REVOKED = 10u,
    KSD_STATUS_INTERNAL = 255u,
};

typedef uint32_t ksd_authorization_mode;
enum {
    KSD_AUTH_CHECK = 0u,
    KSD_AUTH_REQUEST = 1u,
};

typedef uint32_t ksd_connection_role;
enum {
    KSD_ROLE_RPC = 0u,
    KSD_ROLE_EVENT_STREAM = 1u,
    KSD_ROLE_AUTHORIZATION_LEASE = 3u,
};

typedef uint32_t ksd_permission_scopes;
#define KSD_SCOPE_INPUT_MONITORING 0x00000001u
#define KSD_SCOPE_INPUT_CONTROL 0x00000002u
#define KSD_SCOPE_WINDOW_MONITORING 0x00000004u
#define KSD_SCOPE_WINDOW_CONTROL 0x00000008u
#define KSD_SCOPE_SCREEN_CAPTURE 0x00000010u
#define KSD_SCOPE_AUDIO_CAPTURE 0x00000020u
#define KSD_SCOPE_CAMERA_CAPTURE 0x00000040u
#define KSD_SCOPE_CLIPBOARD_MONITORING 0x00000080u
/* Every scope this header names. A newer service may use bits outside it.
 * A granted mask is narrowed to the scopes this authority manages, so it never
 * reports a bit named here that this authority does not grant and never
 * reports an unnamed bit at all. A revoked mask and a stored permission
 * entry's scopes are delivered verbatim, because understating a revocation or
 * a stored grant is the unsafe direction. ksd_scope_name returns NULL for a
 * bit this header does not name; render such a bit numerically. */
#define KSD_SCOPE_ALL 0x000000ffu

/* A newer service may set operation bits this header does not name. They are
 * delivered verbatim and match no KSD_OPERATION_* test. */
typedef uint64_t ksd_operations;

/* KSD_BACKEND_GENERIC is a session whose compositor this service has no
 * dedicated backend for. Its available operations are dynamically detected
 * from compositor protocols, authenticated compositor IPC, and portals.
 * A newer service may report a backend this header does not name. The value is
 * delivered verbatim, is never KSD_BACKEND_NONE in that case, and
 * ksd_backend_name reports it as "unknown". Never infer what is supported from
 * the backend value; read available_operations, which the service computes. */
typedef uint32_t ksd_backend;
#define KSD_BACKEND_NONE 0u
#define KSD_BACKEND_KWIN 1u
#define KSD_BACKEND_GNOME 2u
#define KSD_BACKEND_CINNAMON 3u
#define KSD_BACKEND_GENERIC 4u
#define KSD_BACKEND_X11 5u

#define KSD_OPERATION_CAPTURE_AREA UINT64_C(0x0000000000000001)
#define KSD_OPERATION_CAPTURE_WINDOW UINT64_C(0x0000000000000002)
#define KSD_OPERATION_WINDOW_LIST UINT64_C(0x0000000000000004)
#define KSD_OPERATION_WINDOW_ACTIVE UINT64_C(0x0000000000000008)
#define KSD_OPERATION_WINDOW_WATCH UINT64_C(0x0000000000000010)
#define KSD_OPERATION_WINDOW_FOCUS UINT64_C(0x0000000000000020)
#define KSD_OPERATION_WINDOW_RAISE UINT64_C(0x0000000000000040)
#define KSD_OPERATION_WINDOW_LOWER UINT64_C(0x0000000000000080)
#define KSD_OPERATION_WINDOW_CLOSE UINT64_C(0x0000000000000100)
#define KSD_OPERATION_WINDOW_KILL UINT64_C(0x0000000000000200)
#define KSD_OPERATION_WINDOW_MOVE_RESIZE UINT64_C(0x0000000000000400)
#define KSD_OPERATION_WINDOW_MOVE_RESIZE_XID UINT64_C(0x0000000000000800)
#define KSD_OPERATION_WINDOW_SET_STATE UINT64_C(0x0000000000001000)
#define KSD_OPERATION_WINDOW_SET_OPACITY UINT64_C(0x0000000000002000)
#define KSD_OPERATION_WINDOW_SET_ABOVE UINT64_C(0x0000000000004000)
#define KSD_OPERATION_WINDOW_SET_DECORATED UINT64_C(0x0000000000008000)
#define KSD_OPERATION_WINDOW_RESERVE UINT64_C(0x0000000000010000)
#define KSD_OPERATION_WINDOW_GET_RESERVED UINT64_C(0x0000000000020000)
#define KSD_OPERATION_CLIPBOARD_MIMETYPES UINT64_C(0x0000000000040000)
#define KSD_OPERATION_CLIPBOARD_CONTENT UINT64_C(0x0000000000080000)
#define KSD_OPERATION_CLIPBOARD_TEXT UINT64_C(0x0000000000100000)
#define KSD_OPERATION_CLIPBOARD_WATCH UINT64_C(0x0000000000200000)
#define KSD_OPERATION_MOUSE_MOVE_ABSOLUTE UINT64_C(0x0000000000400000)
#define KSD_OPERATION_MOUSE_MOVE_RELATIVE UINT64_C(0x0000000000800000)
#define KSD_OPERATION_MOUSE_BUTTON UINT64_C(0x0000000001000000)
#define KSD_OPERATION_MOUSE_SCROLL UINT64_C(0x0000000002000000)
#define KSD_OPERATION_CURSOR_POSITION UINT64_C(0x0000000004000000)
#define KSD_OPERATION_WORK_AREA UINT64_C(0x0000000008000000)
#define KSD_OPERATION_CLIPBOARD_SET_CONTENT UINT64_C(0x0000000010000000)
/* Enumeration without properties. A handle says a window exists and nothing
 * else -- not its title, not its owner, not where it is -- so it carries no
 * grant, while reading anything ABOUT a window still does. That split is why
 * this is a separate operation rather than a flag on the window list: the list
 * returns titles and pids in the same reply, which makes the whole reply the
 * gated side. */
#define KSD_OPERATION_WINDOW_HANDLES UINT64_C(0x0000000020000000)
/* Hide a compositor-managed utility window from taskbar, pager and switcher.
 * This is currently available on KWin. */
#define KSD_OPERATION_WINDOW_SET_SKIP_TASKBAR UINT64_C(0x0000000040000000)
/* A complete logical desktop image. This is distinct from CAPTURE_AREA because
 * screenshot portals return a whole desktop and cannot honor an arbitrary
 * rectangle without client-side image decoding. */
#define KSD_OPERATION_CAPTURE_DESKTOP UINT64_C(0x0000000080000000)
#define KSD_OPERATION_WINDOW_QUERY UINT64_C(0x0000000100000000)
#define KSD_OPERATION_WINDOW_CHILDREN UINT64_C(0x0000000200000000)
#define KSD_OPERATION_WINDOW_AT_POINT UINT64_C(0x0000000400000000)
#define KSD_OPERATION_DISPLAY_LIST UINT64_C(0x0000000800000000)
#define KSD_OPERATION_KEYBOARD_STATE UINT64_C(0x0000001000000000)
#define KSD_OPERATION_WINDOW_SET_TITLE UINT64_C(0x0000002000000000)
#define KSD_OPERATION_WINDOW_SET_VISIBLE UINT64_C(0x0000004000000000)
#define KSD_OPERATION_WINDOW_REDRAW UINT64_C(0x0000008000000000)
#define KSD_OPERATION_WINDOW_CLICK UINT64_C(0x0000010000000000)
#define KSD_OPERATION_WINDOW_BUTTON UINT64_C(0x0000020000000000)
#define KSD_OPERATION_WINDOW_FOCUS_CHILD UINT64_C(0x0000040000000000)


/* The mimetype ksd_clipboard_set_text writes. A caller that wants the same
 * bytes through ksd_clipboard_set_content must use this exact string; the
 * service validates UTF-8 only for this one mimetype and passes every other
 * mimetype through as opaque bytes. */
#define KSD_CLIPBOARD_TEXT_MIMETYPE "text/plain;charset=utf-8"

typedef uint16_t ksd_capture_format;
#define KSD_CAPTURE_FORMAT_PNG 1u
#define KSD_CAPTURE_FORMAT_BGRA8_PREMULTIPLIED 2u

typedef uint16_t ksd_window_event_kind;
#define KSD_WINDOW_EVENT_CREATE 1u
#define KSD_WINDOW_EVENT_CLOSE 2u
#define KSD_WINDOW_EVENT_ACTIVE 3u
#define KSD_WINDOW_EVENT_TITLE 4u
#define KSD_WINDOW_EVENT_MINIMIZE 5u
#define KSD_WINDOW_EVENT_RESTORE 6u
#define KSD_WINDOW_EVENT_MOVE 7u
#define KSD_WINDOW_EVENT_ACTIVE_STATE 8u

typedef struct ksd_connection ksd_connection;

typedef struct ksd_error {
    uint32_t struct_size;
    uint32_t detail;
    int32_t system_error;
    uint32_t reserved0;
    char message[KSD_ERROR_MESSAGE_CAPACITY];
    uint64_t reserved[4];
} ksd_error;

typedef struct ksd_connect_options {
    uint32_t struct_size;
    uint32_t role;
    uint32_t authorization_mode;
    uint32_t requested_scopes;
    const char *socket_path;
    uint32_t timeout_ms;
    uint32_t flags;
    uint64_t reserved[4];
} ksd_connect_options;

typedef struct ksd_service_info {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t granted_scopes;
    uint64_t available_operations;
    uint32_t backend;
    uint32_t reserved0;
    uint64_t reserved[4];
} ksd_service_info;

typedef struct ksd_bytes {
    uint32_t struct_size;
    uint32_t reserved0;
    uint8_t *data;
    size_t length;
    uint64_t reserved[2];
} ksd_bytes;

typedef struct ksd_string {
    uint32_t struct_size;
    uint32_t reserved0;
    char *data;
    size_t length;
    uint64_t reserved[2];
} ksd_string;

typedef struct ksd_string_list {
    uint32_t struct_size;
    uint32_t reserved0;
    ksd_string *items;
    size_t count;
    uint64_t reserved[2];
} ksd_string_list;

typedef struct ksd_capture {
    uint32_t struct_size;
    uint16_t format;
    uint16_t reserved0;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    ksd_bytes data;
    uint32_t reserved[8];
} ksd_capture;

typedef struct ksd_point {
    uint32_t struct_size;
    int32_t x;
    int32_t y;
    uint32_t reserved0;
    uint64_t reserved[2];
} ksd_point;

typedef struct ksd_rectangle {
    uint32_t struct_size;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t reserved0;
    uint64_t reserved[2];
} ksd_rectangle;

typedef struct ksd_permission_entry {
    uint32_t struct_size;
    uint32_t scopes;
    uint64_t granted_at_utc;
    char hash[KSD_PERMISSION_HASH_HEX_SIZE];
    char executable[KSD_EXECUTABLE_PATH_SIZE];
    uint8_t reserved[7];
    uint64_t reserved64[4];
} ksd_permission_entry;

typedef bool (*ksd_permission_visitor)(
    const ksd_permission_entry *entry, void *user_data);

typedef struct ksd_permission_revoke {
    uint32_t struct_size;
    uint32_t target_kind;
    uint32_t scopes;
    uint32_t reserved0;
    uint64_t pid;
    char hash[KSD_PERMISSION_HASH_HEX_SIZE];
    uint8_t reserved1[7];
    uint64_t reserved[4];
} ksd_permission_revoke;

typedef uint32_t ksd_permission_target_kind;
enum {
    KSD_PERMISSION_TARGET_HASH = 1u,
    KSD_PERMISSION_TARGET_PID = 2u,
    KSD_PERMISSION_TARGET_ALL = 3u,
};

typedef struct ksd_window_event {
    uint32_t struct_size;
    uint16_t kind;
    uint16_t reserved0;
    ksd_string window_json;
    uint32_t reserved[8];
} ksd_window_event;

typedef struct ksd_clipboard_event {
    uint32_t struct_size;
    uint32_t reserved0;
    ksd_string text;
    ksd_string_list mimetypes;
    uint32_t reserved[8];
} ksd_clipboard_event;

KSD_API uint32_t ksd_client_abi_major(void);
KSD_API uint32_t ksd_client_abi_minor(void);
KSD_API const char *ksd_client_product_name(void);
KSD_API const char *ksd_client_product_version(void);
KSD_API const char *ksd_status_name(ksd_status status);
KSD_API const char *ksd_backend_name(ksd_backend backend);
KSD_API const char *ksd_scope_name(ksd_permission_scopes scope);

KSD_API void ksd_error_init(ksd_error *error);
KSD_API void ksd_connect_options_init(ksd_connect_options *options);
KSD_API void ksd_service_info_init(ksd_service_info *info);
KSD_API void ksd_bytes_init(ksd_bytes *bytes);
KSD_API void ksd_string_init(ksd_string *string);
KSD_API void ksd_string_list_init(ksd_string_list *list);
KSD_API void ksd_capture_init(ksd_capture *capture);
KSD_API void ksd_point_init(ksd_point *point);
KSD_API void ksd_rectangle_init(ksd_rectangle *rectangle);
KSD_API void ksd_permission_entry_init(ksd_permission_entry *entry);
KSD_API void ksd_permission_revoke_init(ksd_permission_revoke *revoke);
KSD_API void ksd_window_event_init(ksd_window_event *event);
KSD_API void ksd_clipboard_event_init(ksd_clipboard_event *event);
KSD_API void ksd_bytes_clear(ksd_bytes *bytes);
KSD_API void ksd_string_clear(ksd_string *string);
KSD_API void ksd_string_list_clear(ksd_string_list *list);
KSD_API void ksd_capture_clear(ksd_capture *capture);
KSD_API void ksd_window_event_clear(ksd_window_event *event);
KSD_API void ksd_clipboard_event_clear(ksd_clipboard_event *event);

KSD_API ksd_status ksd_connect(const ksd_connect_options *options,
                       ksd_connection **connection,
                       ksd_service_info *service_info, ksd_error *error);
KSD_API void ksd_disconnect(ksd_connection *connection);
KSD_API ksd_status ksd_authorize(ksd_connection *connection,
                         ksd_authorization_mode mode,
                         uint32_t requested_scopes, uint32_t *granted_scopes,
                         ksd_error *error);
KSD_API ksd_status ksd_ping(ksd_connection *connection, ksd_error *error);
KSD_API uint32_t ksd_connection_granted_scopes(
    const ksd_connection *connection);
KSD_API ksd_operations ksd_connection_available_operations(
    const ksd_connection *connection);
KSD_API ksd_backend ksd_connection_backend(const ksd_connection *connection);
KSD_API ksd_status ksd_lease_next(ksd_connection *connection,
                          uint32_t timeout_ms,
                          uint32_t *revoked_scopes, ksd_error *error);
KSD_API uint32_t ksd_lease_granted_scopes(
    const ksd_connection *connection);

KSD_API ksd_status ksd_permissions_list(ksd_connection *connection,
                                ksd_permission_visitor visitor,
                                void *user_data, ksd_error *error);
KSD_API ksd_status ksd_permissions_revoke(
    ksd_connection *connection, const ksd_permission_revoke *revoke,
    ksd_error *error);

KSD_API ksd_status ksd_capture_area(ksd_connection *connection,
                            int32_t x, int32_t y,
                            uint32_t width, uint32_t height,
                            ksd_capture *capture, ksd_error *error);
/* Captures the complete logical desktop. This separate operation is suitable
 * for screenshot portals, which do not accept an arbitrary rectangle. */
KSD_API ksd_status ksd_capture_desktop(ksd_connection *connection,
                               ksd_capture *capture, ksd_error *error);
KSD_API ksd_status ksd_capture_window(ksd_connection *connection,
                              const char *window_id,
                              uint32_t include_decoration,
                              ksd_capture *capture, ksd_error *error);

KSD_API ksd_status ksd_cursor_position(ksd_connection *connection,
                                ksd_point *position, ksd_error *error);
KSD_API ksd_status ksd_work_area(ksd_connection *connection,
                         ksd_rectangle *area, ksd_error *error);

/* Every window's handle, and nothing else about any of them. Needs no grant
 * and raises no prompt: a handle says a window exists, which is not something
 * a consent dialog can meaningfully ask about. Reading a window's title, class,
 * owner or geometry is ksd_window_list_json, and that does need one.
 *
 * The reply is {"ok":true,"handles":["...", ...]}. Handles are opaque strings
 * whatever the backend: an XID on X11, a compositor identifier on Wayland. */
KSD_API ksd_status ksd_window_handles_json(ksd_connection *connection,
                                           ksd_string *json,
                                           ksd_error *error);
KSD_API ksd_status ksd_window_list_json(ksd_connection *connection,
                                uint32_t include_hidden,
                                ksd_string *json, ksd_error *error);
KSD_API ksd_status ksd_window_active_json(ksd_connection *connection,
                                  ksd_string *json,
                                  ksd_error *error);

/* Window snapshots include a validFields array. Missing fields have unknown
 * values; callers must not interpret a placeholder as a compositor fact. */
KSD_API ksd_status ksd_window_query_json(ksd_connection *connection,
    uint64_t handle, ksd_string *json, ksd_error *error);
KSD_API ksd_status ksd_window_children_json(ksd_connection *connection,
    uint64_t parent, ksd_string *json, ksd_error *error);
KSD_API ksd_status ksd_window_at_point_json(ksd_connection *connection,
    int32_t x, int32_t y, uint32_t deepest, ksd_string *json, ksd_error *error);
KSD_API ksd_status ksd_display_list_json(ksd_connection *connection,
    ksd_string *json, ksd_error *error);
KSD_API ksd_status ksd_keyboard_state_json(ksd_connection *connection,
    ksd_string *json, ksd_error *error);
/* Pass the previous mapRevision (up to 64 UTF-8 bytes) to omit an unchanged
 * keymap. Retain the previous keymap when that field is absent in the reply. */
KSD_API ksd_status ksd_keyboard_state_since_json(ksd_connection *connection,
    const char *known_revision, ksd_string *json, ksd_error *error);
KSD_API ksd_status ksd_window_set_title(ksd_connection *connection,
    uint64_t handle, const char *title, ksd_error *error);
KSD_API ksd_status ksd_window_set_visible(ksd_connection *connection,
    uint64_t handle, uint32_t visible, ksd_error *error);
KSD_API ksd_status ksd_window_redraw(ksd_connection *connection,
    uint64_t handle, ksd_error *error);
/* Coordinates are client-local; buttons 1..5 and click counts 1..100. */
KSD_API ksd_status ksd_window_click(ksd_connection *connection,
    uint64_t handle, int32_t x, int32_t y, uint32_t button,
    uint32_t count, ksd_error *error);
KSD_API ksd_status ksd_window_button(ksd_connection *connection,
    uint64_t handle, int32_t x, int32_t y, uint32_t button,
    uint32_t down, ksd_error *error);
KSD_API ksd_status ksd_window_focus_child(ksd_connection *connection,
    uint64_t handle, ksd_error *error);

KSD_API ksd_status ksd_window_focus(ksd_connection *connection,
                            uint64_t handle,
                            ksd_error *error);
KSD_API ksd_status ksd_window_raise(ksd_connection *connection,
                            uint64_t handle,
                            ksd_error *error);
KSD_API ksd_status ksd_window_lower(ksd_connection *connection,
                            uint64_t handle,
                            ksd_error *error);
KSD_API ksd_status ksd_window_close(ksd_connection *connection,
                            uint64_t handle,
                            ksd_error *error);
KSD_API ksd_status ksd_window_kill(ksd_connection *connection,
                           uint64_t handle,
                           ksd_error *error);
/* INT32_MIN leaves a position coordinate unchanged. Zero leaves a size
 * coordinate unchanged. At least one coordinate must change. */
KSD_API ksd_status ksd_window_move_resize(ksd_connection *connection,
                                  uint64_t handle,
                                  int32_t x, int32_t y, uint32_t width,
                                  uint32_t height, ksd_error *error);
KSD_API ksd_status ksd_window_move_resize_xid(ksd_connection *connection,
                                      uint64_t xid,
                                      int32_t x, int32_t y, uint32_t width,
                                      uint32_t height, ksd_error *error);
KSD_API ksd_status ksd_window_set_state(ksd_connection *connection,
                                uint64_t handle,
                                uint32_t state, ksd_error *error);
KSD_API ksd_status ksd_window_set_opacity(ksd_connection *connection,
                                  uint64_t handle,
                                  uint32_t opacity, ksd_error *error);
KSD_API ksd_status ksd_window_set_above(ksd_connection *connection,
                                uint64_t handle,
                                uint32_t above, ksd_error *error);
KSD_API ksd_status ksd_window_set_decorated(ksd_connection *connection,
                                    uint64_t handle,
                                    uint32_t decorated, ksd_error *error);
KSD_API ksd_status ksd_window_set_skip_taskbar(ksd_connection *connection,
                                    uint64_t handle,
                                    uint32_t skip, ksd_error *error);
KSD_API ksd_status ksd_window_reserve(ksd_connection *connection,
                              uint64_t cookie,
                              int32_t x, int32_t y, uint32_t ttl_ms,
                              ksd_error *error);
KSD_API ksd_status ksd_window_get_reserved(ksd_connection *connection,
                                   uint64_t cookie,
                                   uint64_t *handle, ksd_error *error);

KSD_API ksd_status ksd_clipboard_mimetypes(ksd_connection *connection,
                                   ksd_string_list *mimetypes,
                                   ksd_error *error);
KSD_API ksd_status ksd_clipboard_content(ksd_connection *connection,
                                 const char *mimetype,
                                 ksd_bytes *content, ksd_error *error);
KSD_API ksd_status ksd_clipboard_text(ksd_connection *connection,
                              ksd_string *text,
                              ksd_error *error);
/* Replace the whole selection with one mimetype's bytes. Needs no grant:
 * reading the selection is gated, replacing it is not. length may be zero,
 * which clears the selection. length is at most 4193272 bytes; the request is split across
 * frames by the library. */
KSD_API ksd_status ksd_clipboard_set_content(ksd_connection *connection,
                                     const char *mimetype,
                                     const void *data, size_t length,
                                     ksd_error *error);
KSD_API ksd_status ksd_clipboard_set_text(ksd_connection *connection,
                                  const char *text, ksd_error *error);

KSD_API ksd_status ksd_mouse_move_absolute(ksd_connection *connection,
                                   int32_t x, int32_t y,
                                   ksd_error *error);
KSD_API ksd_status ksd_mouse_move_relative(ksd_connection *connection,
                                   int32_t dx, int32_t dy,
                                   ksd_error *error);
KSD_API ksd_status ksd_mouse_button(ksd_connection *connection,
                            uint32_t button,
                            uint32_t pressed, ksd_error *error);
KSD_API ksd_status ksd_mouse_scroll(ksd_connection *connection,
                            int32_t delta,
                            uint32_t vertical, ksd_error *error);

KSD_API ksd_status ksd_window_watch_subscribe(ksd_connection *connection,
                                      ksd_error *error);
/* Polling timeouts must be from 1 through KSD_MAX_WATCH_TIMEOUT_MS. */
KSD_API ksd_status ksd_window_watch_next(ksd_connection *connection,
                                 uint32_t timeout_ms,
                                 ksd_window_event *event, ksd_error *error);
KSD_API ksd_status ksd_clipboard_watch_subscribe(ksd_connection *connection,
                                         ksd_error *error);
KSD_API ksd_status ksd_clipboard_watch_next(ksd_connection *connection,
                                    uint32_t timeout_ms,
                                    ksd_clipboard_event *event,
                                    ksd_error *error);

/* A connection is used by one thread at a time. Result and event buffers are
 * owned by the caller and released with the matching clear function. A
 * permission entry is borrowed only for the duration of its visitor call. */

#ifdef __cplusplus
}
#endif

#endif
