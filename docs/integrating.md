# Application integration

Use `keysharp_desktop/client.h` and `libkeysharp-desktop.so.0`. The socket
protocol is private.

```cmake
find_package(KeysharpDesktop CONFIG REQUIRED)
target_link_libraries(app PRIVATE KeysharpDesktop::client)
```

pkg-config name: `keysharp-desktop`.

## Connection

Initialize every public structure with its matching `ksd_*_init` function.
Initialize `ksd_error` when diagnostics are needed. A connection is used by
one thread at a time.

```c
ksd_connect_options options;
ksd_service_info service;
ksd_error error;
ksd_connection *connection = NULL;

ksd_connect_options_init(&options);
ksd_service_info_init(&service);
ksd_error_init(&error);
options.requested_scopes = KSD_SCOPE_WINDOW_MONITORING;

ksd_status status = ksd_connect(&options, &connection, &service, &error);
```

A null `socket_path` uses `KSD_DEFAULT_SOCKET_PATH`. A custom absolute path is
useful for tests. `KSD_AUTH_CHECK` never prompts; `KSD_AUTH_REQUEST` may use
polkit. A zero-scope connection reports the backend and available operations
without reading the permission store.

Requested, listed, and revoked scopes are the six desktop scopes plus shared
InputControl. InputMonitoring and unknown or zero-valued permission entries
are rejected by this authority.

Check `service.available_operations` before every optional operation. The
mask is backend-dependent. Cursor position and work area are available through
the GNOME and Cinnamon providers but not through KWin.

## Results

Owned strings, byte arrays, lists, captures, and events must be released with
the matching `ksd_*_clear` function. Points and rectangles contain no owned
memory. Clear an owned result before passing it to another output call.
Permission-list entries are borrowed only during the visitor call.

Move/resize uses `INT32_MIN` for an unchanged X or Y coordinate and zero for
an unchanged width or height. A request that changes nothing is invalid.

Status values distinguish denial, unavailable backend support, invalid input,
resource limits, timeout, cancellation, revocation, and internal failure. An
unknown status or malformed result invalidates the connection.

## Roles

- `KSD_ROLE_RPC` is for typed calls and permission administration.
- `KSD_ROLE_EVENT_STREAM` carries one window or clipboard subscription.
- `KSD_ROLE_AUTHORIZATION_LEASE` reports live revocation.

Use separate connections for concurrency. Watch timeouts are 1 through 60000
milliseconds. Finite lease timeouts make shutdown straightforward.

## Window JSON

Window-list and active-window calls return UTF-8 JSON. Consumers should read
the documented fields they need and ignore unknown fields. Common fields are
`id`, `title`, `appId`, `pid`, `frame`, `client`, `active`, `minimized`,
`maximized`, `visible`, `alwaysOnTop`, `decorated`, and
`onCurrentWorkspace`. Rectangles contain `x`, `y`, `width`, and `height`.

See `client.h` for the complete ABI and [SECURITY.md](../SECURITY.md) for the
trust model.
