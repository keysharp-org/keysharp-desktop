# Installation and development

Download a release for your architecture from [GitHub Releases](https://github.com/keysharp-org/keysharp-desktop/releases).
Debian packages use `amd64` or `arm64`; archives use `x64` or `arm64`.

## Install

On Debian or Ubuntu:

```sh
sudo apt install ./keysharp-desktop_<version>_<arch>.deb
```

On another systemd distribution:

```sh
tar xf keysharp-desktop-<version>-linux-<arch>.tar.gz
cd keysharp-desktop-<version>-linux-<arch>
sudo ./install.sh
```

The archive installer checks runtime dependencies before copying files. If needed,
it installs them through apt, dnf, zypper or pacman. They are GLib, XCB (including
SHM, RandR, Shape, XKB and Composite), xkbcommon and xkbcommon-x11, Wayland,
polkit and systemd.
A running systemd system manager is required. `./check-runtime.sh` checks without
changing anything; `sudo ./check-runtime.sh --install` installs missing dependencies.
Other distributions must supply them through their package manager. Archives target
glibc 2.35 or newer.

Both routes install and enable the system authority and the graphical session service.
On GNOME or Cinnamon, run the following as your graphical user:

```sh
keysharp-desktop enable-extension
```

Log out and back in if the command asks you to. The archive installer and Keysharp's
combined setup attempt this step for the account running sudo. KWin and other
compositors do not need this extension step. Available operations vary by compositor;
see [platform support](platforms.md).

## Check and repair

```sh
keysharp-desktop info
systemctl status keysharp-desktop-authority.socket keysharp-desktop-authority.service
systemctl --user status keysharp-desktop.service
keysharp-desktop probe
```

`info` reports the local library version and ABI without contacting a service.
Run `probe` as your graphical user to inspect the session backend and available
operations. An inactive authority service is normal before the first connection
if its socket is active. To repair disabled units:

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now keysharp-desktop-authority.socket
sudo systemctl --global enable keysharp-desktop.service
systemctl --user daemon-reload
systemctl --user restart keysharp-desktop.service
```

For missing libraries or installed resources, rerun the installer through the same
channel, or reinstall the package. Do not layer an archive over a system package.
Other logged-in users pick up changes on their next login; they can also reload and
restart their user service. Use `journalctl --user -u keysharp-desktop.service` and
`journalctl -u keysharp-desktop-authority.service` to inspect failures.

## Upgrade and remove

Install a newer release through the same channel. Downloaded `.deb` files do not
configure an update repository. Check releases for security fixes and new operations;
ABI compatibility alone does not mean a release is current.

Remove a package through its package manager. Remove the default source/archive
installation with:

```sh
sudo /usr/local/share/doc/keysharp-desktop/uninstall.sh
```

Uninstall keeps shared system grants under `/var/lib/keysharp-permissions/v1`.
Revoke unwanted grants with `keysharp-desktop permissions revoke` before removal.

## Build from source

```sh
git clone --recurse-submodules https://github.com/keysharp-org/keysharp-desktop
cd keysharp-desktop
sudo apt install cmake make gcc pkg-config polkitd libglib2.0-dev \
  libxcb1-dev libxcb-shm0-dev libxcb-randr0-dev libxcb-shape0-dev libxcb-composite0-dev \
  libxkbcommon-dev libxkbcommon-x11-dev libxcb-xkb-dev libwayland-dev libwayland-bin wayland-protocols
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
sudo cmake --install build
```

The build needs CMake 3.20+, a C11 compiler and wayland-protocols 1.39+. When the
distribution's protocol XML is older, a separate checkout is sufficient:

```sh
git clone --depth 1 --branch 1.39 \
  https://gitlab.freedesktop.org/wayland/wayland-protocols.git "$HOME/wayland-protocols"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DKEYSHARP_DESKTOP_WAYLAND_PROTOCOL_DIR="$HOME/wayland-protocols"
```

The source `sudo ./install.sh` wraps configure/build/install and refreshes the
invoking user's session. Install development dependencies first. `PREFIX` and
`BUILD_DIR` select its destination and build directory. Only `/usr/local` has the
standalone uninstaller. Configure with `-DKEYSHARP_DESKTOP_SETUP_ON_INSTALL=OFF`
or install under `DESTDIR` for file-only staging. To use another permissions
checkout, set `-DKEYSHARP_PERMISSIONS_SOURCE_DIR=/absolute/path`.

## Install for one user

After installing the build dependencies, a user-owned desktop authority can run
without sudo. Permanent-grant dialogs require `zenity` or `kdialog`; without either,
requests for new grants return unavailable.

```sh
cmake -S . -B build-user -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build-user --parallel
cmake --install build-user
systemctl --user enable --now keysharp-desktop-authority-user.socket
systemctl --user enable --now keysharp-desktop.service
```

Grants live in your own state directory. Other processes running as your user can
modify that store, so these grants record consent without enforcing isolation
against same-user programs. They are separate from the root-owned system grants:
Input Control granted here does not authorize the system `keysharp-input` service.
Clients prefer a system desktop authority when one is installed. User installation
does not register a system polkit action or enable system services.
