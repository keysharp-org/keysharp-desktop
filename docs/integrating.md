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
polkit in a system installation, or a local consent dialog in a user installation.
A zero-scope connection reports the backend and available operations
without reading the permission store.

Requested, listed, and revoked scopes are the six desktop scopes plus shared
InputControl. This authority never grants InputMonitoring, and a zero-valued
permission entry is rejected.

Reading the clipboard is gated; replacing it is not.
`KSD_SCOPE_CLIPBOARD_MONITORING` covers `ksd_clipboard_mimetypes`,
`ksd_clipboard_content`, `ksd_clipboard_text` and the clipboard watch.
`ksd_clipboard_set_content` and `ksd_clipboard_set_text` need no grant and
raise no prompt. The compositor withholds selection writes from a client with
no focused surface, which is why the broker offers them at all, but replacing
the selection is not treated as privileged here: reading it can exfiltrate
what the user copied, and a script that sets the clipboard is ordinary. Content is at most 4193272 bytes; the library splits a larger
request across frames itself. Writing
`KSD_CLIPBOARD_TEXT_MIMETYPE` requires valid UTF-8 and is what
`ksd_clipboard_set_text` does; every other mimetype is passed through as
opaque bytes.

In a system installation, InputControl is one shared grant. An application that
holds it for `keysharp-input` holds it here too, so `ksd_mouse_*` raises no
second prompt, and revoking it through this service also stops that
application's `keysharp-input` synthesis. AudioCapture and CameraCapture are reserved:
they are requested, granted, listed, and revoked like any other scope, but no
operation requires them yet.

Grants remember an executable identity and the requested scopes. Request all
scopes your application needs together to present one consent decision. Asking
for another scope later may require another prompt. A user-installed authority
has a private grant store; its consent does not authorize the system input
service. See [application identity](app-identity.md) for interpreter, update,
and runtime-integrity limits.

Check `service.available_operations` before every optional operation. The
mask is backend-dependent. Cursor position and work area are available through
the GNOME, Cinnamon, KWin, and X11 backends.

`ksd_capture_window` takes the `id` field of the window JSON as its
`window_id`, except on KWin where it takes the optional `captureId` field. It
returns the window's own pixels, so a client-side-decorated
window carries alpha in its corners, while `ksd_capture_area` returns the
opaque composited stage. Compare colours within one path, never across both.
`include_decoration` adds the margin the compositor draws outside the visible
window -- shadow and invisible border on GNOME, where server-side decoration is
already inside the visible frame rect. A window that no longer exists, or that
the compositor cannot paint, reports `KSD_STATUS_UNAVAILABLE` on both backends.

`ksd_capture_desktop` returns the complete logical desktop. It is a separate
operation because screenshot portals capture a desktop or monitor, not an
arbitrary rectangle. It was added in client ABI minor 7. Check
`KSD_OPERATION_CAPTURE_DESKTOP` before calling it; a client that needs a
rectangle may decode and crop the returned image itself. Always inspect
`ksd_capture.format`: captures may be PNG or premultiplied BGRA pixels depending
on the backend and operation.

X11 and generic Wayland operations reuse credential-dropped workers and their
display connections for the registered session. Queries and captures use
separate workers, so a slow capture does not hold up keyboard, pointer, or
window queries on another client connection. Captures cross the authority as
sealed descriptors. KWin capture uses
a short-lived isolated worker. Screenshot-portal desktop capture includes a
portal request and PNG file/decode costs; it is not a continuous capture stream.
There is no PipeWire streaming API in this release.

## Window, display, and keyboard queries

Client ABI minor 8 adds a single-window snapshot, parent/top-level relationships,
child enumeration, point hit-testing, monitor topology, keyboard keymaps/state,
and X11 title, visibility, redraw, child focus, and client-directed button calls.
Check each operation bit; support is independent of the ABI version.

`ksd_window_query_json` returns `{"ok":true,"window":{...}}` for one handle.
Generic Wayland uses its cached toplevel table and serializes only that window;
it does not build a complete window list for this call.
X11 snapshots contain decimal `parent` and `topLevel` handles.
`ksd_window_children_json` returns `{"ok":true,"handles":["..."]}` in X11
stacking order, bottom to top. `ksd_window_at_point_json` returns the same
snapshot envelope; its `deepest` argument selects the deepest child, and X11
hit-testing respects input shapes. Missing windows return `NOT_FOUND`.

`ksd_display_list_json` returns `{"ok":true,"displays":[...]}`. Each display
has `name`, `output`, `x`, `y`, `width`, `height`, `primary`, `physicalWidth`,
`physicalHeight`, `refreshRate`, and `orientation`. Coordinates and dimensions
describe the X11 desktop; physical dimensions are millimetres and refresh rate
is hertz.

`ksd_keyboard_state_json` returns a UTF-8 XKB `keymap`, layout names in
`layouts`, and an opaque `mapRevision`. On X11 it also returns `group`,
`depressed`, `latched`, `locked`, `capsLock`, `numLock`, and `scrollLock`.
The masks are XKB modifier masks; layout group is zero-based. Inspect
`validFields` before consuming optional state. All Wayland backends use the
compositor's ordinary keyboard interface for keymaps and layout names,
including sessions with GNOME, Cinnamon, and KWin window providers. They omit
global group and modifier state:
an unfocused Wayland client cannot observe those values reliably.

Pass the last `mapRevision` to `ksd_keyboard_state_since_json` on subsequent
queries. An unchanged map is omitted, so retain your previous keymap. Map
notifications invalidate the worker's cache; this API uses queries rather than
a public keyboard subscription. Use a dedicated connection and your own polling
cadence for state notifications. Display and keyboard queries require no scope.

`ksd_window_click` sends complete clicks using X11 button numbers 1 through 5.
`ksd_window_button` sends one press or release using numbers 1 through 32.
Coordinates are local to the client window. Applications may ignore these
synthetic X11 events. `ksd_window_focus_child` sets
focus on a child window, while `ksd_window_focus` requests window-manager
activation. These calls require WindowControl, including on X11.

## Generic Wayland compositors

A Wayland session without a dedicated provider reports
`KSD_BACKEND_GENERIC`. It is a working, dynamically probed backend:
`available_operations` contains only the operations supported by the
compositor's advertised portable, wlroots, COSMIC, or authenticated Hyprland
interfaces and by the desktop screenshot portal. It can legitimately be zero.
Check the mask and offer only what is there; an unavailable typed operation
returns `KSD_STATUS_UNAVAILABLE`.

The service registers the generic backend when the session names no desktop it
has a backend for. On GNOME, Cinnamon and KDE it waits about two minutes for a
provider to appear first, so a shell extension enabled during login is not
missed. It re-checks the compositor while registered and restarts its backend
when the provider or desktop changes, including when a shell extension is
enabled after startup.

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
  library reports that same session as `unknown` with the same operation mask.
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
`ksd_window_set_skip_taskbar` changes the KWin taskbar, pager, and switcher
hints together; check its operation bit because other backends do not offer it.

Status values distinguish denial, unavailable backend support, invalid input,
resource limits, timeout, cancellation, revocation, and internal failure. An
unknown status or malformed result invalidates the connection.

## Roles

- `KSD_ROLE_RPC` is for typed calls and permission administration.
- `KSD_ROLE_EVENT_STREAM` carries one window or clipboard subscription.
- `KSD_ROLE_AUTHORIZATION_LEASE` reports live revocation.

Use separate connections for concurrency. Watch timeouts are 1 through 60000
milliseconds. Finite lease timeouts make shutdown straightforward.

One connection carries one request at a time, and the service answers each on
that connection's own thread. A capture holds its thread for as long as the
capture takes, which on a large window can be seconds, so **a caller that wants
a capture and other work to proceed at the same time must hold two
connections**. Handing the pixels over as a descriptor removed the transfer
from that window; it did not remove the window.

`BUSY` and `TIMEOUT` are not interchangeable, and the difference decides
whether a retry is safe. `BUSY` means the request never reached the compositor
and provably did not execute, so retrying it is always safe. `TIMEOUT` means it
was dispatched and its outcome is unknown, so retrying `WINDOW_CLOSE`,
`WINDOW_MOVE_RESIZE` or `WINDOW_SET_STATE` can apply it twice.

On KWin every operation runs on the compositor's own thread, one at a time, so
a caller is held to four requests in flight. Over that it is answered `BUSY`
immediately rather than queued: the limit is per process rather than per user
because every consumer of one desktop shares a user, and it is the individual
caller that has to be held back.

A capture is admitted against a budget: one per process, two per user, four
across the service. The per-process limit exists because every consumer of one
desktop shares a user, so without it a single process could hold both of its
user's slots and every other process on that desktop would be refused for the
duration. A refused capture answers `RESOURCE_EXHAUSTED` immediately rather
than queueing, so a caller that would rather do something else is told at once.

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
KWin also supplies `captureId`, its opaque ScreenShot2 identifier; use `id`
for control operations and `captureId` only with window capture.

Snapshots may include `validFields`, the fields known for that specific window.
An omitted field is unknown; zero, false, or an empty legacy placeholder does
not prove that the compositor reported that value. Older providers may omit
the validity array, so treat them as the legacy schema and check field presence.
Frame and client rectangles can differ; do not infer client bounds from frame
bounds when `client` is not valid. Numeric generic Wayland handles are opaque,
connection-lifetime identifiers and must be refreshed after a worker or
compositor reconnect. They are randomized so an old handle does not alias the
first window on a replacement connection. On the portable foreign-toplevel
backend, `compositorId` retains the compositor's original string identifier;
pass the numeric `id` to queries and controls.

See `client.h` for the complete ABI and [SECURITY.md](../SECURITY.md) for the
trust model.
