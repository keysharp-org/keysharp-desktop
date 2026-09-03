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
InputControl. This authority never grants InputMonitoring, and a zero-valued
permission entry is rejected.

InputControl is one shared grant, not one grant per service. An application that
holds it for `keysharp-input` holds it here too, so `ksd_mouse_*` raises no
second prompt, and revoking it through this service also stops that
application's `keysharp-input` synthesis. AudioCapture and CameraCapture are reserved:
they are requested, granted, listed, and revoked like any other scope, but no
operation requires them yet.

Check `service.available_operations` before every optional operation. The
mask is backend-dependent. Cursor position and work area are available through
the GNOME and Cinnamon providers but not through KWin.

`ksd_capture_window` takes the `id` field of the window JSON as its
`window_id`. It returns the window's own pixels, so a client-side-decorated
window carries alpha in its corners, while `ksd_capture_area` returns the
opaque composited stage. Compare colours within one path, never across both.
`include_decoration` adds the margin the compositor draws outside the visible
window -- shadow and invisible border on GNOME, where server-side decoration is
already inside the visible frame rect. A window that no longer exists, or that
the compositor cannot paint, reports `KSD_STATUS_UNAVAILABLE` on both backends.

## Unsupported compositors

A session whose compositor this service has no backend for reports
`KSD_BACKEND_GENERIC` and an `available_operations` of zero. It is a working
connection, not an error: `ksd_connect` succeeds, `ksd_authorize` still prompts
through polkit and still records a durable grant, and `ksd_permissions_list`
and `ksd_permissions_revoke` still work. Every typed operation returns
`KSD_STATUS_UNAVAILABLE`. Treat it the way the operation mask already tells you
to: check `available_operations` and offer only what is there.

The service registers the generic backend when the session names no desktop it
has a backend for. On GNOME, Cinnamon and KDE it waits about two minutes for a
provider to appear first, so a shell extension enabled during login is not
missed. An extension enabled after that is picked up at the next login, or
after `systemctl --user restart keysharp-desktop.service`.

## Forward compatibility

From `ksd_client_abi_minor() >= 2` the library accepts a service whose
vocabulary is newer than its own. Framing stays strict in both directions:
reserved fields, trailing bytes, unknown flags, and a malformed tail all fail
closed and invalidate the connection. Enumerated values do not:

- `service.backend` may hold a value this header does not name. It is
  delivered verbatim, is never `KSD_BACKEND_NONE` in that case, and
  `ksd_backend_name` reports it as `unknown`. Never infer support from the
  backend value; read `available_operations`, which the service computes.
  `KSD_BACKEND_GENERIC` is named from `ksd_client_abi_minor() >= 3`; an older
  library reports that same session as `unknown` with the same zero operation
  mask, so the difference misleads no caller.
- `available_operations` may carry bits this header does not name. They are
  delivered verbatim and match no `KSD_OPERATION_*` test.
- `service.granted_scopes` and the mask from `ksd_authorize` are narrowed to
  the scopes this authority manages, so a caller is never told it holds a
  scope this library cannot honour.
- The mask from `ksd_lease_next` and `entry->scopes` from
  `ksd_permissions_list` are delivered verbatim, so a revocation and a stored
  grant are never understated. `ksd_scope_name` returns `NULL` for a bit this
  header does not name; render such a bit numerically.

A newer library meeting an older service sees no difference: an older service
only ever reports values the older vocabulary already contained.

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

## Pointer operations

`ksd_mouse_move_absolute` places the pointer at an exact compositor pixel
coordinate and remains the supported call for that.

`ksd_mouse_move_relative`, `ksd_mouse_button`, and `ksd_mouse_scroll` are
frozen. They duplicate `keysharp-input`, which synthesizes the same events
through evdev under the same InputControl grant. They keep their ABI and stay
available on GNOME and Cinnamon, but they gain no new backend. Prefer
`keysharp-input` for relative motion, buttons, and scrolling in an application
that already links it.

Relative motion here reads the current pointer position and warps to the sum, so
it is not evdev-relative: pointer acceleration and motion coalescing do not
apply. `ksd_mouse_button` accepts buttons 1, 2, 3, 8, and 9. `ksd_mouse_scroll`
takes 120 units per notch and rejects zero or a magnitude above 12000.

## Window JSON

Window-list and active-window calls return UTF-8 JSON. Consumers should read
the documented fields they need and ignore unknown fields. Common fields are
`id`, `title`, `appId`, `pid`, `frame`, `client`, `active`, `minimized`,
`maximized`, `visible`, `alwaysOnTop`, `decorated`, and
`onCurrentWorkspace`. Rectangles contain `x`, `y`, `width`, and `height`.

See `client.h` for the complete ABI and [SECURITY.md](../SECURITY.md) for the
trust model.
