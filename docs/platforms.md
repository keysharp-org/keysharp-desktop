# Platform support

What a session supports depends on its compositor, so check
`info.available_operations` before calling an operation.

| Operation | X11 | KWin (Wayland) | GNOME Shell | Cinnamon | Generic Wayland |
| --- | --- | --- | --- | --- | --- |
| Area capture | yes | yes | in-memory | in-memory | protocol-dependent |
| Whole-desktop capture | - | - | - | - | portal-dependent |
| Window capture | yes | yes | in-memory | in-memory | protocol-dependent |
| Window queries/control | yes | yes | yes | yes | protocol-dependent |
| Push window events | yes | - | yes | yes | Hyprland |
| Clipboard reads and events | reads only | - | yes | yes | reads only |
| Clipboard writes | - | - | yes | yes | - |
| Absolute pointer / cursor position | yes | cursor query | yes | yes | protocol-dependent |
| Keyboard map/layout | XKB | - | - | - | keymap; group on Hyprland |
| Display list / work area | yes | work area | work area | work area | xdg-output; work area on Hyprland |

KWin window control includes raising and lowering through its scripting API.
KWin exposes window captions and input eligibility only as read-only state, and
its ordinary scripting API cannot request a client redraw.

X11 window events subscribe to XCB property and structure notifications on a
dedicated worker connection. Idle subscriptions issue no window queries. Changes
query the affected window; client-list changes update the subscription set.
Window title, child hierarchy, point queries, display geometry and targeted
window control are also available through the X11 backend.
Enumeration prefers EWMH stacking and includes unmanaged root windows such as
tooltips. Without EWMH it uses the root tree and ICCCM client windows; active-window
queries fall back to input focus. Push events require an EWMH client list, so
clients can poll the same query API on window managers without one.

GNOME and Cinnamon area and window capture return PNG bytes from memory inside
the shell process. Cinnamon area capture requires its stage capture API. It
composes the visible stage at the maximum scale of the outputs intersecting
the requested area, giving one pixel-to-screen transform across mixed-scale
monitors. Monitor gaps and portions outside the outputs are transparent padding;
a rectangle wholly outside the outputs fails. Cinnamon window
capture reads the window actor back through a pixbuf. GNOME window capture
paints the window's own actor, so it returns the window's alpha: a
client-side-decorated window has transparent rounded corners, where area
capture returns the opaque composited stage within the outputs. KWin
returns `KSD_CAPTURE_FORMAT_BGRA8_PREMULTIPLIED` for both capture opcodes, so
a caller compositing the result itself needs different math per backend.
KWin capture needs
a `kwin_wayland` compositor: a KDE X11 session resolves to the X11 backend, which
serves capture from the X server itself.

The GNOME extension consumes the standard Unity `LauncherEntry` signal and maps
its count, progress and urgent state onto the stock overview dash. Dash to Dock
and Ubuntu Dock consume that protocol themselves. The Cinnamon extension maps
the same state onto the grouped-window-list. Plasma's task manager also consumes
the protocol natively, so the KWin provider does not duplicate it.
This integration is application-wide and does not add a client ABI operation.

On any other compositor - sway, Hyprland, COSMIC, niri, river and the rest -
the service registers the generic backend. Its operation mask is assembled from
the protocols the live compositor actually advertises:

- `ext-data-control-v1` supplies the three clipboard reads.
- `ext-foreign-toplevel-list-v1` supplies portable window enumeration.
- wlroots foreign-toplevel management adds state, active-window lookup, focus,
  close, minimize, maximize and restore.
- COSMIC's toplevel protocols add the same window operations plus global
  geometry.
- xdg-output supplies the display list in logical coordinates.
- A window handle is derived from the toplevel's identifier and the
  compositor instance, so every connection to that instance names a window
  alike and a restarted compositor's windows never inherit one. Windows from
  the wlroots list, which carries no identifier, get handles private to their
  connection.
- wlroots screencopy or standard `ext-image-copy-capture-v1` supplies area
  capture, including mixed-scale output composition.
- The foreign-toplevel source in `ext-image-capture-source-v1` with image-copy capture
  supplies window pixels independently of overlapping windows. Pass the
  window's `captureId`; a numeric handle is not a capture identifier.
  This is not advertised when the preferred wlroots window list is used,
  because that protocol has no compatible capture identifiers.
- The desktop screenshot portal supplies whole-desktop capture. This is a
  separate operation because the portal cannot honor an arbitrary rectangle.
- wlroots virtual-pointer supplies absolute motion; authenticated Hyprland IPC
  supplies absolute motion and cursor position when those protocols are absent.
- On Hyprland, IPC facts are joined to the portable list by identifier, which
  Hyprland also reports as each client's `stableId`. IPC is used only when the
  Hyprland instance the session environment names is the compositor the
  Wayland connection reached. That adds geometry, pid,
  workspace visibility, active window, move/resize, raise/lower, hit-testing,
  opacity, keep-above, kill, window capture and pushed window events. Kill sends SIGKILL
  to the process Hyprland reports, through a pidfd and only after rechecking
  that it still owns the window; Hyprland's own kill dispatcher is not used,
  because it signals pid -1 for a window whose surface is gone. Opacity sets the
  active, inactive and full screen values absolutely; opaque clears the
  overrides so the configured look returns. Window events come from the
  compositor event socket, and geometry is sampled every 250 ms because
  Hyprland reports no move or resize event. The work area is the output at the
  layout origin minus Hyprland's reserved edges, and the display list carries
  each output's work area. Keyboard state reports the group of the keyboard
  Hyprland marks main when the keymap has more than one layout; Hyprland keeps
  a layout per keyboard device. The list runs bottom to top by Hyprland's rules:
  an open special workspace above the regular one, floating and full screen
  windows above tiled ones, then focus order, adjusted by explicit raises and
  lowers. Minimizing parks a window on the
  `special:minimized` workspace, and restoring returns it to the workspace it
  left. Moving or resizing a tiled window floats it first. Hyprland has no
  keep-above state, so keeping a window above floats and raises it, clearing
  it returns the window to the tiled layout, and a window reads as kept above
  while it floats. Dispatches use the
  Lua syntax of Lua configurations and fall back to the classic syntax.

`keysharp-desktop probe` reports `backend=generic` with only the detected
operations. Portable enumeration alone has no geometry, pid, state or active
window, and those facts are omitted rather than reported as zeros. Outside
Hyprland, none of the supported protocols can move/resize, restack, change
opacity, keep a window above, identify a process to kill or push window events,
and no compositor path changes decoration.

Hyprland positions are whole logical pixels, so a window placed at a
fractional position, such as one centred at a scale of 2, can be reported up
to one pixel from where it is drawn. A floating window stays above tiled ones
whatever its order, so lowering one cannot put it beneath them.

The session daemon is also started with the user manager, because compositors
launched without a session manager, such as Hyprland without UWSM, never
activate `graphical-session.target`. It waits until a session imports
`WAYLAND_DISPLAY` or `DISPLAY` into the user manager and then restarts into
it, or until a shell provider appears, and while registered it restarts
whenever that session changes. A logout from a session that does activate
`graphical-session.target` stops it with that target, so a later session
without a session manager has to start it, for example with
`exec-once = systemctl --user start keysharp-desktop.service` in Hyprland.

Clipboard writes on generic Wayland and X11 require a selection owner that keeps
serving subsequent paste requests. Those backends do not yet implement ownership.

Pointer control covers `ksd_mouse_move_absolute` plus three frozen fallback
calls that overlap `keysharp-input`; see [permissions](permissions.md).
