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
The authority retains `CAP_SETUID` explicitly so its short-lived session-bus
query can drop irrevocably to the registering user's credentials before it
connects. `NoNewPrivileges=` remains in force for the authority and the helper.

`keysharp-desktop.service` runs `keysharp-desktop daemon` inside each graphical
user session. It connects outbound and registers the session backend; it is
not socket-activated and does not accept application traffic. It holds one
registration at a time and restarts when the compositor identity changes. A
session with no supported compositor registers the generic backend rather than
retrying forever, then upgrades if a supported provider appears later.
GNOME and Cinnamon providers also start the unit through the user systemd
manager when they load. This covers shells such as Cinnamon that do not start
`graphical-session.target`. The unit is wanted by `default.target` as well, for
compositors started without a session manager, such as Hyprland without UWSM:
started before a session has imported `WAYLAND_DISPLAY` or `DISPLAY`, the
daemon waits for one and then restarts into it.

The user manager can survive a logout while its graphical-session environment
is replaced for the next login. The daemon refreshes the session-defining
variables from that live environment while waiting and while rechecking a
registered backend, so a restart between desktops cannot pin it to the old
desktop identity. It reads its own session facts through `getenv`, never from
`/proc/self/environ`: that file keeps the values the process was exec'd with
for its whole life, and `setenv` does not rewrite it, so a resolver reading it
would measure the stale identity against itself and never see the change.
A manager environment naming no display of either kind is ignored rather than
adopted, because a session that never imported its environment leaves that
block behind and adopting it would restart the daemon on every recheck.

Install and removal scripts reload the dynamic-linker cache after adding or
removing the SONAME library. They reload systemd, enable the system socket,
and refresh the invoking graphical user's service. Other active users refresh
at their next login or with `systemctl --user daemon-reload`.

## Compositor scripts

KWin is driven by a script installed read-only under
`share/kwin/scripts/io.github.keysharp.desktop.kwin/`. After acquiring its
provider bus name, the per-user daemon asks KWin to reload that path; it never
materializes, copies, or rewrites it. The user unit therefore declares no `ReadWritePaths=`,
`RuntimeDirectory=`, `StateDirectory=`, or `BindPaths=`; `release-layout-test`
fails if one appears in `data/keysharp-desktop.service.in` or in
`nix/module.nix`. The unit cannot use user-manager filesystem namespacing:
systemd then exposes host root as the overflow UID, hiding the ownership the
daemon must authenticate on `/run/keysharp-desktop/keysharp-desktop.sock`.
`NoNewPrivileges=`, address-family restriction, and personality locking remain
in force without changing that view of the system authority.

`/run/user/<uid>/keysharp-desktop` belongs to the GNOME and Cinnamon
extensions, which create it at mode 0700 and bind their provider sockets
inside it. No unit may claim it as a `RuntimeDirectory=`: systemd would adjust
its ownership at start and remove it at stop, unlinking a live provider socket
that the extension does not recreate.

## Ownership and removal

Package managers own their dependency graph. The portable installer refuses a
partial, package-managed, or differently rooted installation. Compatibility
reuse requires the ABI library and development metadata plus every service,
policy, provider, and root-only worker resource.

The standalone uninstaller removes only this project's manifest. It retains
`/var/lib/keysharp-permissions/v1` because other authorities may use the same
grants.

The Debian package provides `keysharp-desktop-client-abi-0` with version `0.<minor>`
derived from the public header, independent of the product release. Consumers can
require an additive API with a versioned dependency. The package obtains ELF
dependencies through `dpkg-shlibdeps`. Its preinstall guard rejects unmanaged
`/usr/local` files that could shadow the packaged runtime or providers.

CI builds and tests x64 and arm64, consumes the staged CMake and pkg-config
metadata, validates exported symbols and provider sources, assembles Debian
and portable artifacts, and runs `nix flake check --no-write-lock-file`.

## Launchpad PPA

Each release is also uploaded to `ppa:descolada/keysharp`, which carries Keysharp
and keysharp-input as well, for the Ubuntu series listed in `PPA_SERIES` in the
release workflow. `packaging/debian/` is the single package definition: CPack takes
its maintainer scripts, and Launchpad builds the same package from a source upload
that `packaging/ppa/build-source.sh` makes of the tagged tree and its submodule.
Both builds read the client ABI capability from the public header, and both keep
the capture worker root-only.

Launchpad accepts each version once, so the workflow first builds every series and
architecture from that upload with `packaging/ppa/rehearse.sh`: in a clean container
of the series, offline and unprivileged, as Launchpad does. Rerunning the workflow
uploads only what the PPA lacks. Failed transfers are retried up to three times,
waiting 10 and then 20 seconds, without changing the signed files. The PPA is checked
after each failed transfer to avoid repeating an upload it has accepted.

For an existing GitHub release, dispatch `Release` from `main` with its `tag` and
`ppa_only` set to `true`. This builds and rehearses the source uploads without
rebuilding or publishing GitHub assets. The upstream source remains the tagged tree;
Debian packaging, rehearsal and upload tools come from the workflow revision, so a
packaging fix can be applied without moving the release tag.

The `ppa_revision` input uploads a released version
again as `<version>-1~<series><revision>`, reusing the upstream tarball Launchpad
already holds. Keep the revision unchanged when recovering a transfer that Launchpad
never accepted. Raise it when replacing an accepted package, including one whose
Launchpad build failed. For example, after a packaging fix for the accepted v1.0.0
Noble upload:

```bash
gh workflow run release.yml --repo keysharp-org/keysharp-desktop --ref main \
  -f tag=v1.0.0 -f ppa_only=true -f ppa_revision=2
```

To rehearse locally, with Docker installed:

```bash
SERIES=noble bash packaging/ppa/build-source.sh
bash packaging/ppa/rehearse.sh dist/ppa/keysharp-desktop_*~noble1.dsc noble
```

Uploads are signed with the organization secrets `PPA_GPG_PRIVATE_KEY`, the
armored secret key of a GPG identity registered with the PPA owner's Launchpad
account, and `PPA_GPG_PASSPHRASE` when that key has a passphrase.
