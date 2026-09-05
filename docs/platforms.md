# Platform support

What a session supports depends on its compositor, so check
`info.available_operations` before calling an operation.

| Operation | X11 | KWin (Wayland) | GNOME Shell | Cinnamon | Generic Wayland |
| --- | --- | --- | --- | --- | --- |
| Area capture | yes | yes | in-memory | - | protocol-dependent |
| Whole-desktop capture | - | - | - | - | portal-dependent |
| Window capture | yes | yes | in-memory | in-memory | - |
| Window queries/control | yes | yes | yes | yes | protocol-dependent |
| Push window events | yes | - | yes | yes | - |
| Clipboard reads and events | reads only | - | yes | yes | reads only |
| Clipboard writes | - | - | yes | yes | - |
| Absolute pointer / cursor position | yes | cursor query | yes | yes | protocol-dependent |
| Keyboard map/layout | XKB | - | - | - | compositor-dependent |

X11 window events subscribe to XCB property and structure notifications on a
dedicated worker connection. Idle subscriptions issue no window queries. Changes
query the affected window; client-list changes update the subscription set.
Window title, child hierarchy, point queries, display geometry and targeted
window control are also available through the X11 backend.
Enumeration prefers EWMH stacking and includes unmanaged root windows such as
tooltips. Without EWMH it uses the root tree and ICCCM client windows; active-window
queries fall back to input focus. Push events require an EWMH client list, so
clients can poll the same query API on window managers without one.

Cinnamon *area* capture is not advertised because the shell APIs available for
it require named temporary image files; Cinnamon window capture reads the
window actor back through a pixbuf and needs no file. Both GNOME capture paths stream PNG bytes
into an in-memory output stream inside the shell process; neither writes a
file. GNOME window capture paints the window's own actor, so it returns the
window's alpha: a client-side-decorated window has transparent rounded
corners, where GNOME area capture returns the opaque composited stage. KWin
returns `KSD_CAPTURE_FORMAT_BGRA8_PREMULTIPLIED` for both capture opcodes, so
a caller compositing the result itself needs different math per backend.
KWin capture needs
a `kwin_wayland` compositor: a KDE X11 session resolves to the X11 backend, which
serves capture from the X server itself.

On any other compositor - sway, Hyprland, COSMIC, niri, river and the rest -
the service registers the generic backend. Its operation mask is assembled from
the protocols the live compositor actually advertises:

- `ext-data-control-v1` supplies the three clipboard reads.
- `ext-foreign-toplevel-list-v1` supplies portable window enumeration.
- wlroots foreign-toplevel management adds state, active-window lookup, focus,
  close, minimize, maximize and restore.
- COSMIC's toplevel protocols add the same window operations plus global
  geometry.
- wlroots screencopy or standard `ext-image-copy-capture-v1` supplies area
  capture, including mixed-scale output composition.
- The desktop screenshot portal supplies whole-desktop capture. This is a
  separate operation because the portal cannot honor an arbitrary rectangle.
- wlroots virtual-pointer supplies absolute motion; authenticated Hyprland IPC
  supplies absolute motion and cursor position when those protocols are absent.

`keysharp-desktop probe` reports `backend=generic` with only the detected
operations. Portable enumeration alone has no geometry, pid, state or active
window, and those facts are omitted rather than reported as zeros. None of the
supported outside-compositor protocols can move/resize, restack, change opacity
or decoration, or identify a process to kill, so those operations remain
unavailable.

Clipboard writes on generic Wayland and X11 require a selection owner that keeps
serving subsequent paste requests. Those backends do not yet implement ownership.

Pointer control covers `ksd_mouse_move_absolute` plus three frozen fallback
calls that overlap `keysharp-input`; see [permissions](permissions.md).
