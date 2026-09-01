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

## Capture

KWin capture runs in a separate root-only executable. The authority supplies a
root-owned mode-000 directional pipe before the worker drops to the session
UID. The non-dumpable worker retains the read end and a root-owned anonymous
spool. Its short-lived D-Bus child closes every capture descriptor except the
write end before becoming dumpable, and returns only fixed-size metadata.
The worker drains concurrently, seals the exact validated bytes, and converts
them only after the child exits. KWin capture requires Linux Yama
`ptrace_scope=1`; other values fail closed.

GNOME area capture stays in an in-memory stream. GNOME window capture and all
Cinnamon capture are disabled because the available shell APIs require named
temporary files that other same-UID processes could open. Provider operation
bits report this accurately.

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

Do not put grants, hashes, screenshots, clipboard data, or full command lines
in public diagnostics. Report vulnerabilities privately through the
repository's GitHub security advisory page.
