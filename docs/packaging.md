# Packaging and installation policy

Release archives, distribution packages, the compatible-reuse rules, and removal
policy. For ordinary build and install steps see the [README](../README.md).

Release archives contain a prebuilt `/usr/local` payload plus root `install.sh`
and `uninstall.sh` scripts. An application bundling the archive can invoke
`sudo ./install.sh --skip-if-compatible`; a root-owned installation implementing
this exact protocol major and minor is reused unchanged
only after its unit files, polkit action, shared-state declaration, and authority
service wiring and global user-socket enablement are verified. Configuration
resources must resolve through protected root-owned paths; the installer also
checks the policy defaults, shared tmpfiles contract, KWin entry, and provider
resources instead of treating an empty or unrelated file as compatible. The installer
refuses to layer `/usr/local` over a
distribution- or Nix-managed copy, and the portable uninstaller refuses to
remove files while a different installation root is present.
Distribution packages use `/usr`; portable and source installs use `/usr/local`
so they do not overwrite package-manager files.
Release binaries are built natively on Ubuntu 22.04 and target its glibc 2.35
baseline.
Release assets use `keysharp-desktop-<version>-linux-<x64|arm64>.tar.gz` for
portable archives and the conventional
`keysharp-desktop_<version>_<amd64|arm64>.deb` name for Debian packages.
Nix validation is enabled once `flake.lock` is committed. Generate it on a
machine with Nix, run `nix flake check --no-write-lock-file`, and review the
pinned revision and content hash; do not hand-author a `narHash`. Until then,
CI and release workflows skip Nix validation while continuing to build and
publish the portable archives and Debian packages. Nix package publication
should be enabled only after the reviewed lock file is present.
Release packages and the source installer enable the system authority socket and make
the user socket available globally. When installation runs through `sudo`, the
installer resolves the numeric `SUDO_UID`, verifies that user's runtime
directory and active bus, and uses a clean environment to reload that manager
and start the socket. A fresh install or upgrade also restarts that broker when
it is already running, so it cannot keep an old executable or protocol mapped.
Compatible-reuse mode leaves the broker running and only makes the socket
available. If no active invoking session can be resolved safely, new logins
start it automatically and the installer prints the explicit `systemctl --user`
commands needed by an already logged-in user. Other logged-in users are not
enumerated or restarted; they must run those commands or log out after an
upgrade.

The portable uninstaller disables this component's system socket and global
user-socket enablement, then removes its files below `/usr/local` and its
component-named polkit action below `/usr/share`. It always preserves shared
permanent grants. Before removing files, the uninstaller stops the invoking
user's broker and socket through the same verified `SUDO_UID` path, then reloads
that user manager after removal. It prints a handoff for other logged-in users, which must
stop the two units and reload their manager or log out. The Debian pre-install
script refuses to unpack over an actual root-owned portable `/usr/local`
installation and points the administrator to its separate uninstaller; this
prevents `/usr/local` unit files from shadowing package-owned `/usr` units.
An application's uninstaller must not invoke the component uninstaller: the
broker can have other clients. Run it only after checking that no other
application uses the component. There is no portable consumer registry because
manual installs and distribution packages cannot maintain a reliable common
reference count.
Distribution client packages should instead declare `keysharp-desktop` through
their package manager. That dependency graph, rather than a client's maintainer
script, decides when the package is no longer required. Package removal and
purge both keep shared grants, which are deleted only through an authenticated
