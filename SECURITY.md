# Security model

The root authority listens on
`/run/keysharp-desktop/keysharp-desktop.sock`, owned by root and mode 0666.
Every connection is identified with `SO_PEERCRED`. Application frames cannot
supply an identity, backend, provider credential, or grant result.

The authority accepts only small bounded request frames, maps every sensitive
operation to one permission scope, and checks the executable identity and
grant before and after work. Client, per-UID, capture-memory, string, geometry,
event, overlay, reservation, and I/O limits fail closed.

## Application identity

A grant is keyed by UID and the hash of the kernel-resolved executable. The
authority also records the process start time and revalidates it while a
connection is active.

This is a consent identity, not isolation from hostile code already running as
the same user. It assumes the granted program's launch and runtime integrity.
For example, it cannot stop the same user from launching a granted dynamically
linked executable with `LD_PRELOAD` or `LD_AUDIT`, or from injecting where OS
policy permits it. Scripts using the same interpreter executable share one
principal. Separate grants require separate protected executables or a
stronger external sandbox.

## Session registration and providers

The supervised per-user daemon connects outbound to the public authority
socket and sends one fixed registration record. The authority verifies peer
credentials, executable inode, process identity, and detected desktop. Only
one live backend is accepted per UID. The daemon never accepts application
connections and never forwards application frames.

GNOME Shell and Cinnamon expose sensitive methods only on private peer sockets
under the user's runtime directory. Those sockets accept UID 0. Before use,
the authority validates the provider peer, unique session-bus owner, and
root-owned provider executable under an absolute deadline. No connection-cache
lock is held while connecting or performing D-Bus I/O.

Provider executable validation has the same-UID runtime-integrity limit
as application grants. Executable paths alone do not defeat same-UID injection.

A compositor with no extension mechanism of its own is driven by a script the
package installs read-only, which the daemon names to the compositor by
absolute path. The compositor opens that path itself, so the daemon writes
nothing: its unit keeps `ProtectSystem=strict` and `ProtectHome=read-only` with
no `ReadWritePaths=`, `RuntimeDirectory=`, `StateDirectory=`, or `BindPaths=`
exception. This does not make the loaded script trustworthy: the daemon is
itself a same-UID session-bus client and can name any path it likes, so the
read-only delivery bounds packaging drift, not a compromised daemon. Nothing
in such a script is secret. The compositor runs as the session
user, so any file it can read, any same-UID process can read, and any same-UID
process can load a script of its own through the same unrestricted session-bus
interface. Script contents therefore carry no nonce and confer no authority.
The authority still identifies a compositor peer by its root-owned executable,
exactly as it does for GNOME and Cinnamon.

## Capture

KWin capture runs in a separate root-only executable. The authority supplies a
root-owned mode-000 directional pipe before the worker drops to the session
UID. The non-dumpable worker retains the read end and a root-owned anonymous
spool. Its short-lived D-Bus child closes every capture descriptor except the
write end before becoming dumpable, and returns only fixed-size metadata.
The worker drains concurrently, seals the exact validated bytes, and converts
them only after the child exits. KWin capture requires Linux Yama
`ptrace_scope=1`; other values fail closed. It also requires a root-owned
`kwin_wayland` compositor, which backend selection checks before registering, so
a KDE X11 session falls back to the generic backend and advertises no capture.

A session with no supported compositor registers the generic backend. The
authority accepts it from the same authenticated peer as any other backend:
non-root, and an executable whose inode is the authority binary. It maps to an
empty operation mask, so it grants no capability and every typed operation is
refused. It is the only backend the authority does not cross-check against the
session the daemon runs in, because there is nothing for it to authorize.

GNOME area and window capture both stay in an in-memory stream. Window capture
paints the window actor to a content texture and composites the cropped
sub-texture straight into that stream, so no capture path on any backend writes
a named file another same-UID process could open. The provider rejects the
window's scaled pixel count before it asks the compositor to paint, so an
oversized window costs no offscreen paint and no GPU readback. All Cinnamon
capture stays disabled because the available Cinnamon shell APIs require named
temporary files. Provider operation bits report this accurately.

## Grants and revocation

Shared grants live in `/var/lib/keysharp-permissions/v1`. Files are root-owned,
mode 0600, and updated with locking and atomic rename. A per-identity prompt
lock prevents duplicate polkit prompts, and generation fencing prevents a late
approval from recreating a revoked grant.

Revocation advances a per-UID generation. Active sessions clear revoked scopes
before receiving the revoke result. Work whose generation changes before
release returns `REVOKED` and discards sensitive output.

The authority manages the six desktop scopes plus shared InputControl. It
rejects InputMonitoring. Another authority may grant or revoke InputControl;
the common prompt lock and generation fence keep the shared marker coherent.

InputControl is one grant, not one grant per service. A marker is keyed by UID,
executable identity, and scope bit only, so an application granted InputControl
for `keysharp-input` synthesis already satisfies this authority's pointer
operations and prompts no second time. Revoking it here likewise stops that
application's `keysharp-input` synthesis. Consent to InputControl is consent to
synthesize input as the user through either service.

Do not put grants, hashes, screenshots, clipboard data, or full command lines
in public diagnostics. Report vulnerabilities privately through the
repository's GitHub security advisory page.
