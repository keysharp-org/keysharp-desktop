# keysharp-desktop

A Linux desktop-automation service and C library. It provides screen capture,
window inspection and control, clipboard access, and compositor pointer
control, without giving applications direct access to privileged compositor
interfaces.

Applications link a small C library (`keysharp_desktop/client.h`) and connect to
a root-owned Unix socket at `/run/keysharp-desktop/keysharp-desktop.sock`. The
socket is mode 0666: authorization comes from kernel peer credentials and
per-application grants, not from filesystem permissions. The public C ABI is 0.1
with SONAME `libkeysharp-desktop.so.0`; the wire protocol is private.

## Install

Three routes, in order of preference. Each one leaves the authority socket
enabled and the per-session service running.

### Debian package

Download the `.deb` for your architecture from the [releases page][releases]:

```bash
sudo apt install ./keysharp-desktop_<version>_<arch>.deb
```

The package is self-contained apart from ordinary distribution dependencies,
and its post-install step enables the services for you. It provides the virtual
package `keysharp-desktop-client-abi-0`, which is the name applications depend
on.

### Portable archive

For distributions without a package. The archive carries a prebuilt payload and
its own installer, so nothing is compiled:

```bash
tar xf keysharp-desktop-<version>-linux-<arch>.tar.gz
cd keysharp-desktop-<version>-linux-<arch>
sudo ./install.sh
```

An application installer that should leave an existing, compatible system
installation alone can pass `--skip-if-compatible`.

### From source

You need CMake 3.20+, a C11 compiler, GLib, the XCB and Wayland client
libraries, and `wayland-scanner` with `wayland-protocols`; polkit is needed at
run time for authorization prompts. The X11 and Wayland backends are not
optional -- they are what serves window queries, clipboard reads and capture --
so their development packages are required to configure at all. The
`keysharp-permissions` dependency is a submodule, so clone recursively:

```bash
git clone --recurse-submodules https://github.com/keysharp-org/keysharp-desktop
cd keysharp-desktop
sudo apt install cmake make gcc pkg-config polkitd     libglib2.0-dev libxcb1-dev libxcb-shm0-dev     libwayland-dev libwayland-bin wayland-protocols
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
sudo cmake --install build
```

`wayland-protocols` must be 1.39 or newer, which is where the clipboard
protocol `ext-data-control-v1` was added. Distributions lag well behind it:
Ubuntu 24.04 ships 1.34 and Debian 12 ships 1.31, so on either you will need a
backport or a source build of that one package. Check with:

```bash
pkg-config --modversion wayland-protocols
```

That package is data only -- XML definitions and a pkg-config file, no code --
so a machine whose copy is too old does not need it upgraded system-wide. Clone
a newer one anywhere and point the build at it:

```bash
git clone --depth 1 --branch 1.39     https://gitlab.freedesktop.org/wayland/wayland-protocols.git     "$HOME/wayland-protocols"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release     -DKEYSHARP_DESKTOP_WAYLAND_PROTOCOL_DIR="$HOME/wayland-protocols"
```

The definitions are still read at build time rather than vendored into this
repository, so they cannot drift from the protocol the compositor implements --
which is the whole reason they are not checked in here.

The install step finishes the system side: it refreshes the linker cache, creates
the permission store, enables and starts
`keysharp-desktop-authority.socket`, and enables the per-user
`keysharp-desktop.service` for every account.

It does **not** turn on the shell extension, and on GNOME or Cinnamon nothing
works until you do: the daemon finds no provider, registers no backend, and
clients are told the selected backend is `None`. See
[Enable the compositor extension](#enable-the-compositor-extension) below.

An account that is already logged in picks the user service up at its next
login, or immediately with:

```bash
systemctl --user daemon-reload
systemctl --user restart keysharp-desktop.service
```

`sudo ./install.sh` does all of the above and refreshes the invoking user's
session for you; `PREFIX` and `BUILD_DIR` move the destination and the build
tree. It does not install distribution packages -- run the `apt` line above
first. To build and test without installing anything, stop after `ctest`:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Configure with `-DKEYSHARP_DESKTOP_SETUP_ON_INSTALL=OFF` to install the files
without touching systemd, which is what packaging does. To develop against a
sibling `keysharp-permissions` checkout instead of the submodule, use
`-DKEYSHARP_PERMISSIONS_SOURCE_DIR=/absolute/path/to/keysharp-permissions`.

### Without root

All three routes above need administrator rights, because the authority runs as
root and serves every user on the machine. It can also be installed by one
user, for that user alone, with no `sudo` anywhere:

```bash
cmake -S . -B build -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build -j"$(nproc)"
cmake --install build
systemctl --user enable --now keysharp-desktop-authority-user.socket
systemctl --user enable --now keysharp-desktop.service
```

The authority then runs as you, binds its socket in your `XDG_RUNTIME_DIR`
instead of `/run`, and keeps grants under `$XDG_STATE_HOME/keysharp-desktop`
rather than `/var/lib`. Everything else behaves the same. Where a system
installation is also present, its socket is used and this one is ignored --
clients prefer the system authority, so a user installation cannot displace it.

**What you give up.** In a system installation, grants are held by root, so a
grant is a rule an application cannot rewrite: "this application may capture the
screen" is enforced against it. In a user installation, the store belongs to
you, and any process running as you can edit it. Grants become a record of what
you allowed rather than a boundary against the programs you run. The daemon says
so once at startup rather than leaving it implied.

That distinction is the only one. It costs nothing on X11, where any program
that can reach your display can already read every window and take screenshots
whatever this service decides. It matters most on Wayland, where the compositor
does isolate applications from each other and the grant is what keysharp-desktop
adds on top -- so if you are on Wayland and want grants that hold against the
applications themselves, install as root.

### Enable the compositor extension

On GNOME Shell and Cinnamon, the compositor operations come from a bundled shell
extension, which every install places on the system but does not turn on. Enable
it once per user account:

```bash
gnome-extensions enable keysharp@keysharp.io
```

On Cinnamon, enable *Keysharp Desktop Integration* in System Settings ->
Extensions. Either way, log out and back in afterwards, then confirm the
operations are advertised with `keysharp-desktop info`. KWin needs no extension,
but it needs a Plasma Wayland session: a KDE X11 session registers no backend.

### Check the install

```bash
keysharp-desktop probe
keysharp-desktop info
```

### Uninstall

Remove a packaged install with your package manager. A source or portable
install under `/usr/local` carries its own uninstaller:

```bash
sudo /usr/local/share/doc/keysharp-desktop/uninstall.sh
```

It removes only this project's files and keeps the shared grants in
`/var/lib/keysharp-permissions/v1`, because other authorities use the same
store.

## How it fits together

- `libkeysharp-desktop.so.0` is what applications link. It provides
  `keysharp_desktop/client.h` and the CMake target `KeysharpDesktop::client`.
- `keysharp-desktop authority-daemon` is the socket-activated root authority
  that applications talk to.
- `keysharp-desktop daemon` is a supervised per-user service. It registers the
  current graphical session with the authority and never proxies application
  messages.
- `keysharp-desktop-capture-worker` is a root-only KWin capture worker.
- The GNOME Shell and Cinnamon extensions perform compositor operations over a
  private peer connection and export their objects only to a peer whose kernel
  credentials identify root.

## Client API

Build against the client library:

```bash
cc my-app.c $(pkg-config --cflags --libs keysharp-desktop)
```

or, from CMake:

```cmake
find_package(KeysharpDesktop 0.2 CONFIG REQUIRED)
target_link_libraries(my-app PRIVATE KeysharpDesktop::client)
```

Three rules cover the whole API: initialize every options and result object with
its matching `ksd_*_init` function, release owned output with the matching
`ksd_*_clear` function, and use a connection from one thread at a time. Every
call takes an initialized `ksd_error` and returns a `ksd_status`, so the two
examples below are complete programs.

`ksd_connect` names the scopes an application wants, and `ksd_authorize` with
`KSD_AUTH_REQUEST` is what opens the polkit dialog the first time it runs. Once
the user approves, the grant is remembered and later runs go straight through.
Connect requesting no scopes to read `info.available_operations` without opening
a dialog at all.

### List the open windows

```c
#include <keysharp_desktop/client.h>
#include <stdio.h>

int main(void)
{
    ksd_connect_options options;
    ksd_service_info info;
    ksd_error error;
    ksd_connection *connection = NULL;
    ksd_string windows;
    uint32_t granted = 0u;

    ksd_connect_options_init(&options);
    ksd_service_info_init(&info);
    ksd_error_init(&error);
    ksd_string_init(&windows);
    options.requested_scopes = KSD_SCOPE_WINDOW_MONITORING;

    if (ksd_connect(&options, &connection, &info, &error) != KSD_STATUS_OK) {
        fprintf(stderr, "connect: %s\n", error.message);
        return 1;
    }
    if (ksd_authorize(connection, KSD_AUTH_REQUEST, KSD_SCOPE_WINDOW_MONITORING,
                      &granted, &error) != KSD_STATUS_OK) {
        fprintf(stderr, "authorize: %s\n", error.message);
        ksd_disconnect(connection);
        return 1;
    }
    if (ksd_window_list_json(connection, 0, &windows, &error) == KSD_STATUS_OK) {
        printf("%s\n", windows.data);
        ksd_string_clear(&windows);
    } else {
        fprintf(stderr, "window list: %s\n", error.message);
    }
    ksd_disconnect(connection);
    return 0;
}
```

Each window in that JSON carries the handle `ksd_window_focus`,
`ksd_window_raise`, `ksd_window_close` and `ksd_window_move_resize` take.
Changing a window needs `KSD_SCOPE_WINDOW_CONTROL` as well.

### Capture part of the screen

`ksd_capture_area` fills a `ksd_capture` whose `data` the caller releases with
`ksd_capture_clear`. On the backends that advertise capture the format is
`KSD_CAPTURE_FORMAT_PNG`, so the bytes can go straight to a file.

```c
#include <keysharp_desktop/client.h>
#include <stdio.h>

int main(void)
{
    ksd_connect_options options;
    ksd_service_info info;
    ksd_error error;
    ksd_connection *connection = NULL;
    ksd_capture capture;
    uint32_t granted = 0u;
    FILE *file;

    ksd_connect_options_init(&options);
    ksd_service_info_init(&info);
    ksd_error_init(&error);
    ksd_capture_init(&capture);
    options.requested_scopes = KSD_SCOPE_SCREEN_CAPTURE;

    if (ksd_connect(&options, &connection, &info, &error) != KSD_STATUS_OK) {
        fprintf(stderr, "connect: %s\n", error.message);
        return 1;
    }
    if (ksd_authorize(connection, KSD_AUTH_REQUEST, KSD_SCOPE_SCREEN_CAPTURE,
                      &granted, &error) != KSD_STATUS_OK) {
        fprintf(stderr, "authorize: %s\n", error.message);
        ksd_disconnect(connection);
        return 1;
    }
    if (ksd_capture_area(connection, 0, 0, 400, 300, &capture, &error)
            != KSD_STATUS_OK) {
        fprintf(stderr, "capture: %s\n", error.message);
        ksd_disconnect(connection);
        return 1;
    }
    if (capture.format == KSD_CAPTURE_FORMAT_PNG
        && (file = fopen("shot.png", "wb")) != NULL) {
        fwrite(capture.data.data, 1, capture.data.length, file);
        fclose(file);
        printf("wrote shot.png (%ux%u)\n", capture.width, capture.height);
    }
    ksd_capture_clear(&capture);
    ksd_disconnect(connection);
    return 0;
}
```

### Beyond the examples

`ksd_cursor_position` and `ksd_work_area` answer screen queries.
`ksd_window_event_init` with an event-stream connection watches windows, and the
clipboard equivalents watch copies. `ksd_permissions_list` and
`ksd_permissions_revoke` back a settings UI.
[docs/integrating.md](docs/integrating.md) covers the whole API.

## Available operations

What a session supports depends on its compositor, so check
`info.available_operations` before calling an operation.

| Operation | KWin (Wayland) | GNOME Shell | Cinnamon |
| --- | --- | --- | --- |
| Area capture | yes | in-memory | - |
| Window capture | yes | in-memory | in-memory |
| Window operations and events | - | yes | yes |
| Clipboard reads and events | - | yes | yes |
| Clipboard writes | - | yes | yes |
| Pointer control, cursor position, work area | - | yes | yes |

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
the service registers the generic backend, which is no longer empty. It serves
what the shared Wayland protocols allow a client on the OUTSIDE of a compositor
to do: the three clipboard reads over `ext-data-control-v1`, and the window
list over `ext-foreign-toplevel-list-v1`. `keysharp-desktop probe` reports
`backend=generic` with whichever of those the compositor actually implements -
the daemon probes and narrows, so the mask is what you can really have rather
than a ceiling.

Two limits are worth stating plainly, because both are protocol and not
backlog. The window list carries a title, an app id and an opaque identifier,
and omits geometry, pid and window state entirely rather than reporting zeros
for them - `ext-foreign-toplevel-list` does not carry those, and a zero would
read as a fact. And nine operations are impossible for a client outside the
compositor: no Wayland protocol lets one client restack another's window, set
its geometry or opacity, keep it above, change its decoration, learn a pid to
signal, or correlate a toplevel to the process about to create it. Nothing
reports which window has focus either, so there is no active-window verb here.
Those report `unavailable`, and always will.

Clipboard writes are absent for a different reason, and the same one as on
X11: owning a selection means staying alive to serve it, and the worker that
answers exits with its operation.

Pointer control covers `ksd_mouse_move_absolute` plus three frozen fallback
calls that overlap `keysharp-input`; see Permissions below.

## Permissions

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

Polkit authenticates a new grant request, and the result remains until it is
explicitly revoked. Shared scope values and marker handling come from the pinned
`keysharp-permissions` library.

### InputControl is one grant shared with keysharp-input

A grant marker is keyed by UID, executable identity, and one scope bit. It
carries no service name, and every authority reads the same directory, so
InputControl is a single grant rather than one grant per service.

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

## Command line

```text
keysharp-desktop version
keysharp-desktop info
keysharp-desktop probe [--socket PATH]
keysharp-desktop permissions list [--socket PATH]
keysharp-desktop permissions revoke (--hash HASH|--pid PID|--all) [SCOPE ...] [--socket PATH]
keysharp-desktop daemon
```

Exit status is 0 for success, 1 for an operational or authorization failure, and
2 for invalid syntax.

## Troubleshooting

```sh
keysharp-desktop probe
systemctl --user status keysharp-desktop.service
sudo systemctl status keysharp-desktop-authority.socket \
  keysharp-desktop-authority.service
```

## More documentation

- [docs/integrating.md](docs/integrating.md) - the client API in depth
- [SECURITY.md](SECURITY.md) - trust boundaries and reporting
- [docs/packaging.md](docs/packaging.md) - install layout and release artifacts

## License

MIT.

[releases]: https://github.com/keysharp-org/keysharp-desktop/releases
