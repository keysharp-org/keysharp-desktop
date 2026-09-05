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

A grant is keyed by UID and a canonical executable identity from
`/proc/PID/exe`. Protected root-owned executable paths use a path principal;
other executables use their content hash. The authority also records process
start time to reject PID reuse. Replacing a protected executable at the same
path retains its grant; changed user-owned executable contents require a new
grant. Moving identical user-owned contents does not create a new principal.

Connection checks compare process UID, start time, executable path, device,
inode, size, and nanosecond modification/change times. An unchanged fingerprint
reuses the verified identity; a changed fingerprint runs the full identity
check. Interactive authorization always performs the full check. This avoids
rehashing a large user-owned executable for each window or pointer query.

This is a consent identity, not isolation from hostile code already running as
the same user. It assumes the granted program's launch and runtime integrity.
For example, it cannot stop the same user from launching a granted dynamically
linked executable with `LD_PRELOAD` or `LD_AUDIT`, or from injecting where OS
policy permits it. Scripts using the same interpreter executable share one
principal. Separate grants require separate protected executables or a
stronger external sandbox.

These checks observe the connected process, not an individual thread or the
current holder of a transferred socket. A granted process can delegate its open
connection to another process. Checkpoints also cannot revoke a side effect
already committed immediately before executable replacement or revocation.
The boundary is host-application consent under the runtime-integrity assumption
above, rather than a sandbox for mutually hostile processes of one UID.

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
a KDE X11 session falls back to the X11 backend.

A session with no supported compositor registers the generic backend. The
authority accepts it from the same authenticated peer as any other backend:
non-root, and an executable whose inode is the authority binary. It maps to an
operation mask dynamically probed from the live Wayland registry,
authenticated compositor IPC, and desktop portal availability. The authority
intersects that mask with a fixed ceiling and applies the same per-operation
scope checks as every other backend.

GNOME area and window capture both stay in an in-memory stream. Window capture
paints the window actor to a content texture and composites the cropped
sub-texture straight into that stream. The provider rejects the
window's scaled pixel count before it asks the compositor to paint, so an
oversized window costs no offscreen paint and no GPU readback. Cinnamon window
capture reads the actor image into a pixbuf and encodes it into a sealed memfd,
so it writes no named file either; Cinnamon area capture stays disabled,
because the only Cinnamon shell API for it does require one. Provider operation
bits report this accurately, and a test pins the advertised bits against what
the provider will actually serve, because the two drifted apart once.

Generic whole-desktop capture uses the desktop screenshot portal. The portal
chooses a named result file; the service opens it with `O_NOFOLLOW`, requires a
regular file owned by the session UID, bounds its size and PNG dimensions, and
copies it into a sealed memfd. It removes only a canonical temporary-directory
name matching `screenshot-*.png`, and only while that path still names the same
device and inode that were opened. The service never selects or writes the
named file itself.

X11 and generic Wayland requests use persistent, credential-dropped display
workers, with separate query and capture channels. Bootstrap comes from the
authority over a private stream, and each worker acknowledges credential setup
before accepting requests. Workers have
memory and descriptor limits and are reaped on exit. A process-level deadline
bounds synchronous display operations even if the display server stops replying;
idle workers and idle subscriptions have no lifetime deadline. A separate
observer exits the worker when the authority channel hangs up, including during
a blocked display call. Captures remain sealed
across the worker relay; the receiver validates descriptor type, size, required
seals, and the pending capture opcode before accepting a descriptor.

## Grants and revocation

Shared grants live in `/var/lib/keysharp-permissions/v1`. Files are root-owned,
mode 0600, and updated with locking and atomic rename. A per-identity prompt
lock prevents duplicate polkit prompts, and generation fencing prevents a late
approval from recreating a revoked grant.

Revocation advances a per-UID generation. Each scoped operation checks this
generation; unchanged generations reuse a connection's grant mask, while a
changed generation reloads its full requested scope set. Active sessions clear revoked scopes
before receiving the revoke result. Work whose generation changes before
release returns `REVOKED` and discards sensitive output.

The authority manages the six desktop scopes plus shared InputControl. It
rejects InputMonitoring. Another authority may grant or revoke InputControl;
the common prompt lock and generation fence keep the shared marker coherent.

For system authorities, InputControl is one grant. A marker is keyed by UID,
executable identity, and scope bit only, so an application granted InputControl
for `keysharp-input` synthesis already satisfies this authority's pointer
operations and prompts no second time. Revoking it here likewise stops that
application's `keysharp-input` synthesis. Consent to InputControl is consent to
synthesize input as the user through either service.

A user installation uses a private, user-owned socket and store and prompts
through zenity or kdialog. The client checks the socket and answering peer's
ownership and prefers the system authority when it is available. Local consent
does not provide same-UID isolation and does not create a system input grant.
No polkit policy is required for this user-owned consent path.

Consent persists for the approved scope set. It is not a promise of one dialog
for every future capability: newly requested scopes can prompt again, and
explicit revocation or changed content-based identity requires renewed consent.

Do not put grants, hashes, screenshots, clipboard data, or full command lines
in public diagnostics. Report vulnerabilities privately through the
repository's GitHub security advisory page.
