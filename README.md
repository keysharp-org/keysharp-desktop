# keysharp-desktop

keysharp-desktop is a standalone Linux desktop-automation service and C
library. It provides screen capture, window inspection and control, clipboard
observation, and compositor pointer control without giving applications direct
access to privileged compositor interfaces.

The public C ABI is 0.1 and uses SONAME `libkeysharp-desktop.so.0`. The wire
protocol is private.

## Components

- `libkeysharp-desktop.so.0` provides `keysharp_desktop/client.h` and the CMake
  target `KeysharpDesktop::client`.
- `keysharp-desktop authority-daemon` is the socket-activated root authority.
- `keysharp-desktop daemon` registers the current graphical session with the
  authority. It is a supervised per-user service and never proxies application
  messages.
- `keysharp-desktop-capture-worker` is a root-only KWin capture worker.
- GNOME Shell and Cinnamon extensions provide compositor operations over a
  private root-authenticated peer connection.

Applications connect directly to the root-owned Unix socket at
`/run/keysharp-desktop/keysharp-desktop.sock`. The socket is mode 0666;
authorization comes from kernel peer credentials and application grants, not
filesystem access to the socket.

## Build and install

The pinned `keysharp-permissions` dependency is a git submodule.

```sh
git clone --recurse-submodules https://github.com/keysharp-org/keysharp-desktop
cd keysharp-desktop
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
sudo cmake --install build
```

An alternate dependency checkout is a developer-only override:

```sh
cmake -S . -B build \
  -DKEYSHARP_PERMISSIONS_SOURCE_DIR=/absolute/path/to/keysharp-permissions
```

After a source install:

```sh
sudo systemd-tmpfiles --create
sudo systemctl enable --now keysharp-desktop-authority.socket
sudo systemctl --global enable keysharp-desktop.service
systemctl --user daemon-reload
systemctl --user start keysharp-desktop.service
```

The Debian package is self-contained apart from normal distribution
dependencies; installing it does not require another project artifact. It
provides `keysharp-desktop-client-abi-0`.

Portable installations include
`/usr/local/share/doc/keysharp-desktop/uninstall.sh`. It removes only this
project's installed files and retains shared grants in
`/var/lib/keysharp-permissions/v1`.

## Client API

Include `keysharp_desktop/client.h` and link with either:

```sh
pkg-config --cflags --libs keysharp-desktop
```

or:

```cmake
find_package(KeysharpDesktop 0.2 CONFIG REQUIRED)
target_link_libraries(my-app PRIVATE KeysharpDesktop::client)
```

Initialize every options/result object with its matching `ksd_*_init`
function. Release owned output with the matching `ksd_*_clear` function. A
connection is used by one thread at a time.

```c
ksd_connect_options options;
ksd_service_info info;
ksd_error error;
ksd_connection *connection = NULL;

ksd_connect_options_init(&options);
ksd_service_info_init(&info);
ksd_error_init(&error);
options.requested_scopes = KSD_SCOPE_WINDOW_MONITORING;

if (ksd_connect(&options, &connection, &info, &error) == KSD_STATUS_OK) {
    ksd_string windows;
    ksd_string_init(&windows);
    if (ksd_window_list_json(connection, 0, &windows, &error)
            == KSD_STATUS_OK)
        ksd_string_clear(&windows);
    ksd_disconnect(connection);
}
```

Use `info.available_operations` before calling an operation:

- KWin: area and window capture.
- GNOME Shell: in-memory area capture, window operations/events, clipboard
  reads/events, pointer control, cursor position, and work area.
- Cinnamon: window operations/events, clipboard reads/events, pointer control,
  cursor position, and work area.

GNOME window capture and Cinnamon capture are not advertised because their
available shell APIs require named temporary image files.

## Permissions

The shared durable scopes are:

| Constant | Meaning |
| --- | --- |
| `KSD_SCOPE_INPUT_MONITORING` | observe arbitrary input |
| `KSD_SCOPE_INPUT_CONTROL` | synthesize or suppress input |
| `KSD_SCOPE_WINDOW_MONITORING` | inspect or watch windows |
| `KSD_SCOPE_WINDOW_CONTROL` | change windows |
| `KSD_SCOPE_SCREEN_CAPTURE` | capture screen content |
| `KSD_SCOPE_AUDIO_CAPTURE` | capture audio |
| `KSD_SCOPE_CAMERA_CAPTURE` | capture camera video |
| `KSD_SCOPE_CLIPBOARD_MONITORING` | read or watch the clipboard |

This service manages its six desktop scopes plus the shared InputControl scope
used by pointer calls. InputMonitoring is a canonical shared value but is not
accepted for grant or revocation here. Polkit authenticates a new grant
request; the result remains until explicitly revoked. Shared scope values and
marker handling come from the pinned `keysharp-permissions` library.

## Command line

```text
keysharp-desktop version
keysharp-desktop info
keysharp-desktop probe [--socket PATH]
keysharp-desktop permissions list [--socket PATH]
keysharp-desktop permissions revoke (--hash HASH|--pid PID|--all) [SCOPE ...] [--socket PATH]
keysharp-desktop daemon
```

Exit status is 0 for success, 1 for an operational or authorization failure,
and 2 for invalid syntax.

## Troubleshooting

```sh
keysharp-desktop probe
systemctl --user status keysharp-desktop.service
sudo systemctl status keysharp-desktop-authority.socket \
  keysharp-desktop-authority.service
```

See [SECURITY.md](SECURITY.md), [docs/integrating.md](docs/integrating.md), and
[docs/packaging.md](docs/packaging.md) for the security, API, and packaging
contracts.

Licensed under the MIT License. Descolada is the sole contributor.
