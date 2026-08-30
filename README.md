# keysharp-desktop

`keysharp-desktop` is a standalone Linux desktop-integration service. It gives
any application a stable, multi-client API for screen capture, global window
monitoring and control, and clipboard monitoring, together with a small
per-application authorization API. It has no dependency on the project it was
extracted from.

It covers GNOME, Cinnamon, and KWin/KDE Plasma, where a Wayland compositor
normally makes these operations impossible for an ordinary application.

## Components

One root-owned executable, `keysharp-desktop`, provides several modes:

- `keysharp-desktop serve` — an unprivileged, socket-activated per-user broker.
  It owns the compositor connection, serves concurrent clients, and serializes
  capture through a bounded FIFO queue.
- `keysharp-desktop authority` — a socket-activated root authority. It receives
  the client's connected socket from the broker with `SCM_RIGHTS`, reads the
  kernel-authenticated PID and UID with `SO_PEERCRED`, invokes polkit when a
  grant is missing, and maintains permanent per-capability grants.
- `keysharp-desktop version|probe|status|grant|list|revoke` — diagnostics and
  grant administration.

GNOME Shell and Cinnamon providers implement compositor-side capture, window,
clipboard, pointer, and overlay interfaces. KWin capture uses
`org.kde.KWin.ScreenShot2` directly and needs no shell extension.

## Requirements

A C11 compiler, CMake 3.20 or newer, pkg-config, GLib/GIO, gio-unix, pthreads,
and polkit.

```sh
# Debian/Ubuntu
sudo apt install build-essential cmake ninja-build pkg-config libglib2.0-dev policykit-1

# Fedora
sudo dnf install gcc cmake ninja-build pkgconf-pkg-config glib2-devel polkit

# Arch
sudo pacman -S base-devel cmake ninja pkgconf glib2 polkit
```

## Build and test

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Install

```sh
sudo cmake --install build
sudo systemctl enable --now keysharp-desktop-authority.socket
systemctl --user daemon-reload
systemctl --user enable --now keysharp-desktop.socket
```

Distribution packages install to `/usr`; source and portable installs use
`/usr/local` so they never overwrite package-manager files. Release archives,
the `install.sh --skip-if-compatible` reuse path, and the rules for moving
between portable and packaged installations are covered in
[docs/packaging.md](docs/packaging.md).

### Enable the compositor provider

On GNOME and Cinnamon, enable the `keysharp@keysharp.io` extension in your
desktop's normal extension manager after installing. This may require logging
out and back in. KWin and KDE Plasma need no extension.

### Verify

```sh
keysharp-desktop version
keysharp-desktop probe
```

`version` is machine-readable `key=value` output. `probe` performs a
capability-free handshake, so it reports `READY none` when no supported capture
provider is active — expected on a session whose compositor is not supported or
whose extension is not yet enabled.

## Authorization

Six permanent capabilities gate the brokered operations:

| Capability | CLI name | Purpose |
| --- | --- | --- |
| `ScreenCapture` | `screen-capture` | Capture an area or a window |
| `WindowMonitoring` | `window-monitoring` | Read global window lists, metadata, focus, geometry |
| `WindowControl` | `window-control` | Activate, move, resize, close, or otherwise control windows |
| `AudioCapture` | `audio-capture` | Capture audio input or output |
| `CameraCapture` | `camera-capture` | Capture camera video |
| `ClipboardMonitoring` | `clipboard-monitoring` | Observe clipboard contents or changes |

A `check` only reads the grant store. A `request` may run
`/usr/bin/pkcheck --allow-user-interaction` for `org.keysharp.desktop.grant`. A
successful check is persisted as a root-owned marker; there is no allow-once
decision and no custom prompt. Global cursor-position queries and clipboard
writes are permission-free and never read or write the store.

Audio and camera capture are grantable scopes for clients that perform those
operations through a platform API; this release has no broker command for
either.

Grants are keyed by UID and the caller's kernel-resolved executable, never its
arguments. See [docs/app-identity.md](docs/app-identity.md) and
[SECURITY.md](SECURITY.md) for the identity contract, and
[docs/permission-store.md](docs/permission-store.md) for the on-disk format
this service shares with other compatible authorities.

### Administration

```sh
keysharp-desktop status screen-capture
keysharp-desktop grant screen-capture
keysharp-desktop list
keysharp-desktop revoke <64-hex-app-hash> screen-capture
keysharp-desktop revoke --all
```

`status` and `grant` describe the invoking `keysharp-desktop` process; they are
diagnostics, not a way to grant another application by name. `list` reports the
current UID's application hashes, desktop-domain capabilities, and display-only
executable paths. Omitting the capability from `revoke` selects every
desktop-domain capability for that hash; the UID-wide form requires an explicit
`--all`. Both authenticate the current UID from the authority socket rather
than trusting a command-line UID.

Revocation advances a root-owned runtime generation. The broker polls it and
closes every connection that received a nonzero capability mask, so a client
can hold an authorization connection and treat its closure as the revocation
signal.

## Using it from your own application

Clients speak the session protocol over the per-user broker socket. The wire
contract is the installed header `keysharp_desktop/protocol.h`, and
[docs/integrating.md](docs/integrating.md) is the integration guide, and
[docs/protocol.md](docs/protocol.md) documents the handshake, message set, and
compatibility rules. `src/desktopctl.c` is a complete worked client.

Distribution packages that ship a client should express the relationship
through the package manager (`Depends` or `Recommends`) rather than installing
or removing this service themselves — the broker can have other clients.

## NixOS

```nix
{
  inputs.keysharp-desktop.url = "github:keysharp-org/keysharp-desktop";
  outputs = { nixpkgs, keysharp-desktop, ... }: {
    nixosConfigurations.host = nixpkgs.lib.nixosSystem {
      modules = [
        keysharp-desktop.nixosModules.default
        { services.keysharp-desktop.enable = true; }
      ];
    };
  };
}
```

The flake exports packages and modules under both `default` and
`keysharp-desktop`, for `x86_64-linux` and `aarch64-linux`.

## Uninstall

```sh
sudo /usr/local/share/doc/keysharp-desktop/uninstall.sh
```

The broker can have other clients, so an application's own uninstaller must
never run this. Run it only after establishing that no other application uses
the component. It always preserves permanent grants; revoke those through the
authenticated CLI above.

## Documentation

| Document | Contents |
|---|---|
| [docs/integrating.md](docs/integrating.md) | Depending on and writing a client for the service |
| [docs/protocol.md](docs/protocol.md) | Session protocol and compatibility rules |
| [SECURITY.md](SECURITY.md) | Security boundary and reporting |
| [docs/permission-store.md](docs/permission-store.md) | Shared on-disk grant contract |
| [docs/app-identity.md](docs/app-identity.md) | Executable-identity algorithm |
| [docs/packaging.md](docs/packaging.md) | Release archives, packages, removal policy |

The private compositor XML descriptions under `interfaces/` are for provider
development and are not installed as runtime files.

## License

MIT. See [LICENSE](LICENSE) and [PROVENANCE.md](PROVENANCE.md).
