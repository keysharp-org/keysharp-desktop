# keysharp-desktop — Claude Code Guide

## Project overview

keysharp-desktop is a standalone Linux desktop-automation service and C library. It
gives an unprivileged application screen capture, window inspection and control,
clipboard observation, and compositor pointer control, without handing it direct access
to privileged compositor interfaces.

The pieces:

- `keysharp-desktop authority-daemon` — the socket-activated root authority that
  authenticates peers and holds grants.
- `keysharp-desktop daemon` — a supervised per-user service that registers the current
  graphical session. It never proxies application messages.
- `keysharp-desktop-capture-worker` — a root-only KWin capture worker.
- GNOME Shell and Cinnamon extensions — compositor operations over a private
  root-authenticated peer connection.
- `libkeysharp-desktop.so.0` — the public client library, header
  `keysharp_desktop/client.h`.

Applications connect directly to `/run/keysharp-desktop/keysharp-desktop.sock`. The
socket is mode 0666; authorization comes from kernel peer credentials and recorded
grants, not from filesystem permissions on the socket.

The public client ABI version is declared in `include/keysharp_desktop/client.h`. The wire protocol is private and is an implementation
detail of that ABI.

## Relationship to Keysharp

This is an independent project. [Keysharp](https://github.com/keysharp-org/Keysharp)
is currently its main consumer, but it is a consumer like any other, and nothing here
may assume Keysharp is the caller.

- **The contract is the client ABI**, expressed as `libkeysharp-desktop.so.0`, the
  pkg-config/CMake package, and the Debian capability `keysharp-desktop-client-abi-0`.
  Product versions select release artifacts; the client ABI decides compatibility.
- **Releases are independent.** This project versions and releases on its own cadence.
  Keysharp resolves it at install time from this repository's own releases, so a release
  here does not need a Keysharp release to reach users.
- **The dependency runs one way.** Nothing in this repository reads Keysharp's
  configuration, links its assemblies, special-cases its process, or is built from its
  tree. A feature that only makes sense for Keysharp belongs in Keysharp.
- **Keysharp degrades without it.** Keysharp runs when this service is absent; its
  capture, window and clipboard integration are unavailable until it is installed. Keep
  that true: every client-side failure mode is a normal runtime condition.
- `keysharp-permissions` is a pinned submodule, shared with `keysharp-input`, so a grant
  recorded by either is visible to both. Both `ci.yml` and `release.yml` assert the
  reviewed revision, so moving the pin means updating those literals, the
  `keysharp-permissions` flake input, and the matching pin in `keysharp-input`.

## Repository structure

```
src/
├── main.c, cli.c           # CLI entry and subcommands
├── authorityd.c            # root authority: peer credentials, grants, session registry
├── operation_scope.c       # opcode -> permission scope, operation bit, chunkability
├── session_backend.c       # per-user session daemon
├── backend.c, provider.c   # backend selection and the private provider connection
├── capture_worker*.c       # root-only KWin capture worker
├── local_capture.c         # in-process capture paths
├── client.c                # libkeysharp-desktop.so — the whole public ABI
└── transport.c, protocol_io.c
include/keysharp_desktop/   # client.h — the public surface
providers/{gnome,cinnamon}/ # shell extensions
interfaces/private/         # the private provider interface
data/                       # units, polkit policy, tmpfiles, .pc.in, desktop entry
packaging/                  # install-release.sh (archive installer), debian/
tests/                      # ctest sources and policy checks
```

## Build and test

Needs CMake 3.20+, a C11 compiler, and GLib (`gio-2.0`, `gio-unix-2.0`); polkit is
needed at run time for authorization prompts. Clone recursively for the
`keysharp-permissions` submodule.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
sudo cmake --install build
```

The install step finishes the system side: linker cache, permission store, enabling and
starting `keysharp-desktop-authority.socket`, and `--global enable` of the per-user
service. Configure with `-DKEYSHARP_DESKTOP_SETUP_ON_INSTALL=OFF` to install files only,
which is what packaging does. `sudo ./install.sh` wraps configure, build and install,
and refreshes the invoking user's session — the one step
CMake cannot do, because it needs that user's bus.

Use `-DKEYSHARP_PERMISSIONS_SOURCE_DIR=/absolute/path` to develop against a sibling
`keysharp-permissions` checkout instead of the submodule.

## Backends and scopes

What a session supports depends on its compositor, so a client reads
`info.available_operations` before calling an operation. KWin provides area and window
capture on a `kwin_wayland` session and falls back to `KSD_BACKEND_GENERIC` on
KDE X11; GNOME Shell and
Cinnamon provide window operations and events, clipboard reads and events, pointer
control, cursor position and work area, with in-memory area and window capture on GNOME. Every
other compositor registers `KSD_BACKEND_GENERIC`, which now serves what the shared Wayland
protocols allow a client OUTSIDE the compositor to do: the three clipboard reads over
ext-data-control-v1, and the window list over ext-foreign-toplevel-list-v1. That is a
ceiling, not a promise -- the daemon probes what its compositor actually advertises and
narrows the registration to it, so a client is told what it can really have.

Nothing beyond that may be inferred, and the gap is not laziness. Nine operations are
impossible for a client on the outside: no Wayland protocol lets one client restack
another's window, set its geometry or opacity, keep it above, change its decoration, learn
a pid to signal, or correlate a toplevel to the process about to create it. There is also
no way to ask which window has focus, which is why there is no active-window verb here.

This service manages the six desktop scopes plus the shared `INPUT_CONTROL` used by
pointer calls. `INPUT_MONITORING` is a canonical shared value that this service does not
grant or revoke — that belongs to `keysharp-input`.

`INPUT_CONTROL` is one grant, not one per service. A marker is keyed by UID, executable
identity, and scope bit only, so an application granted it for `keysharp-input` reaches
these pointer operations without a second prompt, and a revocation from either side
removes the one marker. Say so in any documentation that touches the scope.

Maintainer decision: `MOUSE_MOVE_ABSOLUTE` stays broker-owned, because it places the
pointer at an exact compositor pixel with no evdev axis normalization and no uinput
dependency. `MOUSE_MOVE_RELATIVE`, `MOUSE_BUTTON`, and `MOUSE_SCROLL` are frozen — a
documented fallback for callers not using `keysharp-input`. Do not extend them to KWin
or any new backend, and do not delete them: nothing is removed and no ABI changes.
`tests/provider_gate_test.cmake` enforces the KWin half.

`ksd_connect` names the scopes an application wants; `ksd_authorize` with
`KSD_AUTH_REQUEST` opens the polkit dialog. Connecting with no scopes reads
`available_operations` without prompting.

## Conventions

- C11, four-space indent, `ksd_` prefix on everything public.
- Three rules cover the client API: initialize every options and result object with its
  `ksd_*_init`, release owned output with the matching `ksd_*_clear`, and use one
  connection from one thread at a time.
- Comments explain why, in one to three lines. Do not restate the code, describe what it
  replaced, or capitalise words for emphasis.
- Shell lifecycle scripts are POSIX `sh`, and CI shellchecks
  `install.sh uninstall.sh packaging/install-release.sh`.
- Provider JavaScript is validated in CI; keep the GNOME and Cinnamon extensions in step
  with the private provider interface.
- Public API changes need `include/keysharp_desktop/client.h`, `docs/integrating.md`, and
  the complete C programs in `examples/` updated together.

## Traps worth knowing

- **A build sandbox is not a system.** `nix flake check` runs ctest inside nix's sandbox,
  where coreutils is one multi-call binary chosen by `argv[0]`, `/bin` holds only `sh`,
  `/etc` has no `os-release`, `HOME` points nowhere, and no uid reads as root. Anything
  in a test that copies a coreutils tool under a new name, sources a script that hardens
  `PATH` to system directories, or asserts a path is root-owned fails there and nowhere
  else.
- **nixpkgs' cmake hook makes every `CMAKE_INSTALL_*DIR` absolute**, which bakes the
  prefix into the exported CMake targets and breaks `install-tree-consumer-test`, because
  a DESTDIR-staged consumer then resolves imported files to the real `$out`.
  `nix/package.nix` sets them back to relative values, and the `.pc.in` uses
  `@CMAKE_INSTALL_FULL_LIBDIR@` rather than composing `${prefix}` itself.
- **The permission store refuses any parent a third party can write to.** That rules out
  `/tmp` for test fixtures; `permissions-core-tests` and `permission-domain-tests` build
  theirs under `$HOME`, falling back to the build tree only when `HOME` does not exist.
- **CPack stages without `DESTDIR`**, so the `DESTDIR` guard on the post-install step does
  not cover the `.deb` build. That configure passes
  `-DKEYSHARP_DESKTOP_SETUP_ON_INSTALL=OFF` explicitly.

## Useful entry points

| Task | Start here |
|------|-----------|
| Public API change | `include/keysharp_desktop/client.h`, `src/client.c` |
| Authorization or grants | `src/authorityd.c`, the `keysharp-permissions` submodule |
| Backend selection | `src/backend.c`, `src/provider.c` |
| Capture | `src/capture_worker.c`, `src/local_capture.c` |
| Compositor operations | `providers/gnome/`, `providers/cinnamon/` |
| Session lifecycle | `src/session_backend.c`, `data/*.service.in` |
| Install layout or channels | `CMakeLists.txt` install rules, `packaging/install-release.sh` |
| Release artifacts | `.github/workflows/release.yml`, `docs/packaging.md` |
