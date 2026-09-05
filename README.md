# keysharp-desktop

A Linux desktop automation service and C library for capture, window inspection and
control, clipboard access and compositor pointer operations. Applications use
`libkeysharp-desktop.so.0`; available operations depend on the active compositor.

## Install

Download your architecture's release from [GitHub Releases](https://github.com/keysharp-org/keysharp-desktop/releases).
On Debian or Ubuntu:

```sh
sudo apt install ./keysharp-desktop_<version>_<arch>.deb
```

Other systemd distributions can extract the release archive and run `sudo ./install.sh`.
It checks and installs missing runtime dependencies before installing the services.
On GNOME or Cinnamon, run `keysharp-desktop enable-extension` as your graphical user
and log out if requested. Keysharp's combined Linux setup performs this step for you.
See [installation](docs/install.md) for source builds, updates and removal.

## Use it

```sh
cc examples/list-windows.c $(pkg-config --cflags --libs keysharp-desktop) -o list-windows
```

Or link `KeysharpDesktop::client` after `find_package(KeysharpDesktop 0.2 CONFIG REQUIRED)`.
Complete examples demonstrate [listing windows](examples/list-windows.c) and
[capturing an area](examples/capture-area.c). Running them can request permission.

Initialize public structures with their `ksd_*_init` functions, release owned results
with `ksd_*_clear`, and use one connection from one thread at a time. Read
`info.available_operations` before calling compositor operations; connecting without
requested scopes does not open a permission dialog. The [integration guide](docs/integrating.md)
documents status handling, capabilities and buffer ownership.

## Platform support

| Backend | Integration |
| --- | --- |
| X11 | Native XCB window, clipboard, capture and pointer operations |
| GNOME / Cinnamon | Bundled compositor extension |
| KWin / Plasma Wayland | Bundled KWin script and capture worker |
| Other Wayland compositors | Advertised standard, wlroots or COSMIC protocols; Hyprland IPC and screenshot portal where available |

Exact capabilities are discovered at runtime. Window fields that the compositor
cannot supply are absent. See [platform support](docs/platforms.md) for operation
coverage and capture formats. Relative pointer movement, buttons and scrolling are
compatibility fallbacks; applications using `keysharp-input` should send those there.

## Permissions

The system authority authenticates callers and holds permanent grants. A new
executable identity requests its scopes through polkit; updated executable content
needs a new grant. Input Control is shared with the system `keysharp-input` service,
so granting or revoking it affects both. [Permission details](docs/permissions.md)
explain individual scopes and revocation.

A [user installation](docs/install.md#install-for-one-user) is also available. Its
consent records are user-owned, cannot enforce isolation against same-user programs,
and do not share grants with system services. See [security](SECURITY.md) for the
threat model and reporting vulnerabilities.

## Diagnose

```sh
keysharp-desktop info
keysharp-desktop probe
systemctl --user status keysharp-desktop.service
```

`info` reads local version/ABI metadata. Run `probe` as your graphical user to inspect
the backend and advertised operations. [Repair instructions](docs/install.md#check-and-repair)
cover missing extensions, disabled units and incomplete installations.
Manage grants with `keysharp-desktop permissions list` and `keysharp-desktop permissions revoke`.

## Project and distribution

This project releases independently of Keysharp. Applications use its public C ABI;
the socket protocol is private. The Debian capability `keysharp-desktop-client-abi-0`
identifies the ABI, with its provided version recording the ABI major and minor.
Applications should require or recommend the needed minor according to whether
desktop features are optional, and leave the shared service installed on removal.

- [Build and install](docs/install.md#build-from-source)
- [Client integration](docs/integrating.md)
- [Native X11 measurements](docs/performance.md)
- [Packaging and Nix](docs/packaging.md)
- [Service protocol](docs/protocol.md)

MIT licensed.
