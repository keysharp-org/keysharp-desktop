# Session protocol 1.2

The protocol name is `keysharp-desktop/session-v1`; the product version in this
release is `0.1.0`. Major version 1 is incompatible with any other major. A
1.2 server accepts only a 1.2 client because the capability meanings are one
exact contract. Other minor versions are rejected rather than reinterpreted.

## Transport and ownership

Clients connect to `$KEYSHARP_DESKTOP_SOCKET` when set, otherwise to
`$XDG_RUNTIME_DIR/keysharp-desktop/keysharp-desktop.sock`. With systemd user
socket activation this is `%t/keysharp-desktop/keysharp-desktop.sock`.

The stream is owned by one client for its lifetime. One command may be
outstanding per connection. Clients that need parallel work open independent
connections. The server admits at most 32 clients and 32 queued captures;
captures run in FIFO order on one compositor worker. Synchronous GNOME and
Cinnamon provider calls are serialized across clients, so several automation
processes cannot race a shared compositor connection. Event streams use
dedicated client connections. The broker exits after 60 idle seconds by
default and is restarted by its socket. Set
`KEYSHARP_DESKTOP_IDLE_SECONDS` to `0` to disable idle exit or to a value up to
3600 for tests and non-systemd launches.

## Handshake

The first line is ASCII and has this exact shape:

```text
KSDP/1.2 HELLO <backend> <capabilities-hex> <check|request>\n
```

`backend` is `auto`, `kwin`, `gnome`, or `cinnamon`. Capabilities are an
eight-digit hexadecimal mask:

| Bit | Value | Capability |
| ---: | ---: | --- |
| 0 | `0x00000001` | `ScreenCapture` |
| 1 | `0x00000002` | `WindowMonitoring` |
| 2 | `0x00000004` | `WindowControl` |
| 3 | `0x00000008` | `AudioCapture` |
| 4 | `0x00000010` | `CameraCapture` |
| 5 | `0x00000020` | `ClipboardMonitoring` |

All other bits are unassigned and rejected. Global cursor-position queries are
direct provider operations, not an authorization capability, and require no
grant. `check` never opens a polkit dialog; `request` allows one when a
permanent grant is missing. A zero mask is a provider probe and does not contact
the authority. There are no capability aliases or aggregate capability bits.

Success is one line:

```text
KSDP/1.2 READY <backend|none> <granted-hex> capture-area,capture-window,window-query,window-control,clipboard-read,events,authorize,version\n
```

`auto` with no capture provider deliberately returns `READY none`, including
for capability-only authorization requests. This lets in-process X11 or
wlroots clients use the same consent authority without requiring brokered
capture. The 130-second receive timeout applies only while the initial
handshake may be waiting for polkit. It is cleared after `READY`, so an idle
authorization lease remains open until the client exits, revocation closes it,
or the service shuts down.

Handshake failures are:

```text
KSDP/1.2 ERROR <denied|unsupported|protocol|busy|internal> <message>\n
```

## Commands

After `READY`, commands are newline-terminated ASCII:

```text
area <x> <y> <width> <height>
window <handle> [0|1]
window-list <0|1>
active-window
watch-window
window-focus <handle>
window-raise <handle>
window-lower <handle>
window-close <handle>
window-kill <handle>
window-move-resize <handle> <x> <y> <width> <height>
window-move-resize-xid <xid> <x> <y> <width> <height>
window-state <handle> <state>
window-opacity <handle> <0..255>
window-above <handle> <0|1>
window-decorated <handle> <0|1>
clipboard-mimetypes
clipboard-content <hex-encoded-UTF-8-MIME-type>
clipboard-text
watch-clipboard
ping
version
quit
```

`window` defaults to including decorations. GNOME and Cinnamon handles are
unsigned decimal integers; KWin receives its handle as an opaque string.
Capture commands require the `ScreenCapture` bit in the grant returned by the
handshake. `window-list`, `active-window`, and `watch-window` require
`WindowMonitoring`. The other `window-*` commands require `WindowControl`.
Clipboard read and watch commands require `ClipboardMonitoring`. Window and
clipboard commands are available on GNOME and Cinnamon; a provider error is
returned on other backends. `ping` returns one zero status byte. `quit` closes
the stream without a response.

The ordinary success frame for a provider command is:

```text
00 <uint32 byte-length> <bytes>
```

Window queries and `clipboard-text` return UTF-8. Window controls return one
byte, `00` or `01`, inside the success payload. `clipboard-mimetypes` returns
zero or more UTF-8 names, each terminated by NUL. `clipboard-content` returns
the selected MIME data unchanged.

A watch command first returns an empty success frame. The connection then
becomes an event stream and accepts no more commands. Each window event is a
success frame containing `<UTF-8 event-name> NUL <UTF-8 window-JSON>`. Each
clipboard event contains `<UTF-8 text> NUL` followed by zero or more NUL-
terminated UTF-8 MIME names. Closing the connection cancels the subscription.

Capability grants are live session state, not snapshots that survive
revocation. The broker polls the root-owned revoke generation every 250
milliseconds. When it changes, the broker shuts down every connection whose
handshake received a nonzero capability mask, including an `auto` connection
used only to hold X11 or wlroots consent. An already executing capture may
finish; no later command is accepted. Clients that need prompt revocation notice
should retain the connection and observe EOF. A new handshake performs a stable
generation check around authorization, so it cannot retain a grant across a
concurrent revoke. Revoke generation updates under
`/run/keysharp-permissions` bracket the locked grant-store mutation, making the
invalidation signal crash-safe. The persistent marker format and
cross-component capability mapping are specified in
[`permission-store.md`](permission-store.md).

All multibyte integers below are little-endian. A failed command is:

```text
01 <uint32 message-length> <UTF-8 message>
```

A GNOME or Cinnamon PNG capture is:

```text
00 "KSSG1\0\0\0" <uint64 byte-length> <PNG bytes>
```

A KWin raw capture is:

```text
00 "KSSC1\0\0\0" <uint32 width> <uint32 height> <uint32 stride>
   <uint32 format> <uint64 byte-length> <raw bytes>
```

The `version` command returns status `00`, a little-endian `uint32` length, and
the UTF-8 text `keysharp-desktop/session-v1 1.2`.

## Discovery output

`keysharp-desktop version` and its `keysharp-desktop --info` alias return line-based
`key=value` data including:

```text
product_version=0.1.0
protocol_name=keysharp-desktop/session-v1
protocol_major=1
protocol_minor=2
```

Consumers should ignore unknown keys and reject an unsupported major version.

An interactive authorization may spend up to 120 seconds in polkit. The broker
allows 125 seconds for the authority transaction and 130 seconds for receiving
the initial client handshake. Clients should allow at least 125 seconds for a
`request` handshake; capture responses retain their separate 35-second send
bound.
