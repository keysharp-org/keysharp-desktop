# Permissions

The shared durable scopes are:

| Constant | Meaning |
| --- | --- |
| `KSD_SCOPE_INPUT_MONITORING` | observe arbitrary input |
| `KSD_SCOPE_INPUT_CONTROL` | synthesize or suppress input |
| `KSD_SCOPE_WINDOW_MONITORING` | inspect or watch windows |
| `KSD_SCOPE_WINDOW_CONTROL` | change windows |
| `KSD_SCOPE_SCREEN_CAPTURE` | capture screen content |
| `KSD_SCOPE_AUDIO_CAPTURE` | capture audio (reserved) |
| `KSD_SCOPE_CAMERA_CAPTURE` | capture camera video (reserved) |
| `KSD_SCOPE_CLIPBOARD_MONITORING` | read or watch the clipboard |

This service manages its six desktop scopes plus the shared InputControl scope
that pointer calls use. InputMonitoring is a canonical shared value, but this
service does not accept it for a grant or a revocation: the client library, the
HELLO parser, the authority's scope check, the polkit action, and the permission
store each reject it.

AudioCapture and CameraCapture are reserved for planned work. A request for
either is accepted, prompts through polkit, and produces a durable grant that
`permissions list` and `permissions revoke` handle like any other scope, but no
operation consumes them yet.

For a system installation, polkit authenticates a new grant request, and the result remains until it is
explicitly revoked. Shared scope values and marker handling come from the pinned
`keysharp-permissions` library.

### InputControl is one grant shared with keysharp-input

A system grant marker is keyed by UID, executable identity, and one scope bit. It
carries no service name, and every authority reads the same directory, so
InputControl is a single grant rather than one grant per service.

A user-owned desktop authority uses its own consent dialog and grant store.
Those records do not authorize the system input service and do not enforce
isolation against programs running as the same user. See [user installation](install.md#install-for-one-user).

An application that was granted InputControl for `keysharp-input` synthesis
therefore already holds it here: `ksd_mouse_*` succeeds with no second prompt.
The reverse holds too. `keysharp-desktop permissions revoke` with no scope
argument includes InputControl, so it also stops that application's
`keysharp-input` synthesis.

Read the scope as one decision about synthesizing input as the user. This
service only synthesizes pointer events; it neither suppresses input nor
observes key or button events, which is what InputMonitoring covers. Cursor
position and work area are unscoped queries.

### Pointer operations are a frozen fallback

`ksd_mouse_move_absolute` stays owned by this service. The GNOME and Cinnamon
providers hand the compositor an exact pixel position, with no normalization to
an evdev axis range and no dependency on a uinput device being available.

`ksd_mouse_move_relative`, `ksd_mouse_button`, and `ksd_mouse_scroll` overlap
with `keysharp-input`, which synthesizes the same events through evdev under the
same InputControl grant. They are frozen: they keep working, keep their opcodes
and exported symbols, and stay available on GNOME and Cinnamon, but they gain no
new backend. If a KWin provider later grows pointer support it will advertise
`KSD_OPERATION_MOUSE_MOVE_ABSOLUTE` only.

Callers that already link `keysharp-input` should synthesize relative motion,
buttons, and scrolling there. Callers that do not may keep using these three.
Relative motion here is not evdev-relative: the provider reads the current
pointer position and warps to the sum, so pointer acceleration and motion
coalescing do not apply.

Freezing is a documentation decision and is reversible. No operation, opcode,
scope, exported symbol, or provider method is removed, and no existing caller
has to change.
