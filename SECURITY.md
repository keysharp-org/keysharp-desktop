# Security model

## Privilege separation

`keysharp-desktop serve` runs as the logged-in user and refuses UID 0 or any
real/effective credential mismatch. It never receives root privilege. Its
user socket has mode `0600` beneath a `0700` runtime directory.

Only `keysharp-desktop authority` runs as root and it requires real and
effective UID and GID 0. Sharing one executable does not merge the two process
privilege domains, and the executable must never be installed setuid. Its system socket is writable
by users because authorization requests must cross the privilege boundary, but
callers cannot state an identity. For an authorization check, the broker passes
the original connected client file descriptor with `SCM_RIGHTS`; the authority
derives PID, UID, and process start time from that socket's `SO_PEERCRED` data.
List and revoke operations use the control socket's own peer UID and can
therefore inspect or remove only the caller's grants. Scoped revoke accepts a
validated 64-hex application identity and desktop capability mask; UID-wide
revocation requires the explicit `--all` CLI form.

The authority asks polkit to authorize the original client process, not the
broker. It rechecks the process start time after polkit returns and writes a
root-owned `0600` store through a locked, fsynced, atomic rename. Stored grants
are permanent until revoked; no session or deny records are stored. A revoke
also advances a root-owned runtime generation. The session broker observes it
within 250 milliseconds and shuts down every capability-bearing connection.
Work already executing may complete, but the connection accepts no subsequent
command. Capability-only clients receive the same closure signal. Generation
updates bracket the locked grant-store mutation, so a service crash cannot
remove a grant without first invalidating sessions that held it.

Before opening an interactive polkit request, either compatible helper takes
the same root-owned `0600`, `O_NOFOLLOW` prompt lock for the client's UID and
application identity. It revalidates the client and shared grants after waiting,
then holds that lock until any new marker is durable. The grant-store lock is
not held across UI, and revoke deliberately ignores the prompt lock; the revoke
generation fence prevents a late approval from restoring a revoked grant.

## Application identity

The grant key is the tuple `(UID, SHA-256 executable identity)`. For a
root-owned executable whose entire resolved path is root-owned and not group-
or other-writable, the identity covers its canonical kernel path so package
updates keep their grants. The final path is checked against the opened inode.
Otherwise the identity covers executable content, so replacing a development
binary changes it. This matches keysharp-input's permanent executable-level grants and
macOS TCC-style application consent. Command-line arguments are deliberately
excluded because they are forgeable, unstable, and not an operating-system
security boundary.

The kernel-derived UID, executable, and PID start time protect against other
UIDs, a different executable, and PID reuse. Linux does not provide a strong
security boundary between hostile processes running under the same UID. A
same-UID process can debug another dumpable process, and every script hosted by
the same interpreter necessarily shares that interpreter's executable grant.

## Compositor providers

GNOME and Cinnamon accept capture, broker registration, global window
monitoring and control, and clipboard reads and change delivery only from a
session-bus caller whose
`/proc/PID/exe` basename is `keysharp-desktop` and whose executable is
root-owned and not group- or other-writable. This prevents accidental bypass by
ordinary provider clients, but the same-UID limitation above still applies.
Provider installation must preserve root ownership and mode `0755` or stricter.

Global cursor-position queries and clipboard writes are intentionally
permission-free provider operations. Process-owned overlays and placement
reservations also remain direct because they are scoped to the registering
client rather than arbitrary desktop state.

The provider boundary makes the broker the effective gate for calls made
through these extension interfaces. It is not a system-wide mandatory-access
control boundary: X11 and public desktop-session APIs, including Cinnamon's
`org.Cinnamon.Eval` when enabled and KWin's ordinary window-management
interfaces, remain callable by same-user processes. Authorization there records
application consent but cannot make the underlying APIs exclusive to this
broker. Audio and camera capture performed through other platform APIs likewise
remains the client's responsibility to precede with the matching authorization
request.

## Reporting issues

Do not include grant-store contents, full command lines, screenshots, or other
private desktop data in a public report. Open a minimal report with the affected
version and contact the project maintainers privately when disclosure would
expose user data or an authorization bypass.
