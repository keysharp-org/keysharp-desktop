# Packaging

The source tree uses the pinned `third_party/keysharp-permissions` submodule.
`KEYSHARP_PERMISSIONS_SOURCE_DIR` is an explicit developer override; builds do
not search sibling directories.

## Required install

Runtime files:

```text
bin/keysharp-desktop
libexec/keysharp-desktop-capture-worker        root:root 0700
lib/libkeysharp-desktop.so{,.0,.0.2.0}
include/keysharp_desktop/client.h
lib/pkgconfig/keysharp-desktop.pc
lib/cmake/KeysharpDesktop/*
```

Service and provider resources:

```text
lib/systemd/user/keysharp-desktop.service
lib/systemd/system/keysharp-desktop-authority.{service,socket}
lib/tmpfiles.d/keysharp-desktop-permissions.conf
share/polkit-1/actions/org.keysharp.desktop.policy
share/applications/org.keysharp.DesktopCapture.desktop
share/gnome-shell/extensions/keysharp@keysharp.io/*
share/cinnamon/extensions/keysharp@keysharp.io/*
```

The installed CMake target is `KeysharpDesktop::client`; the pkg-config name is
`keysharp-desktop`. Private headers are not installed.

## Activation

The system socket is `/run/keysharp-desktop/keysharp-desktop.sock`, owned by
root and mode 0666. It activates `keysharp-desktop authority-daemon`.

`keysharp-desktop.service` runs `keysharp-desktop daemon` inside each graphical
user session. It connects outbound and registers the session backend; it is
not socket-activated and does not accept application traffic.

Install and removal scripts reload the dynamic-linker cache after adding or
removing the SONAME library. They reload systemd, enable the system socket,
and refresh the invoking graphical user's service. Other active users refresh
at their next login or with `systemctl --user daemon-reload`.

## Ownership and removal

Package managers own their dependency graph. The portable installer refuses a
partial, package-managed, or differently rooted installation. Compatibility
reuse requires the ABI library and development metadata plus every service,
policy, provider, and root-only worker resource.

The standalone uninstaller removes only this project's manifest. It retains
`/var/lib/keysharp-permissions/v1` because other authorities may use the same
grants.

The Debian package provides `keysharp-desktop-client-abi-0` and obtains ELF
dependencies through `dpkg-shlibdeps`. Its preinstall guard rejects unmanaged
`/usr/local` files that could shadow the packaged runtime or providers.

CI builds and tests x64 and arm64, consumes the staged CMake and pkg-config
metadata, validates exported symbols and provider sources, assembles Debian
and portable artifacts, and runs `nix flake check --no-write-lock-file`.
