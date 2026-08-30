# Shared permission-store contract

`keysharp-input` and `keysharp-desktop` are independent projects. They do
not link to a common trust library and neither service depends on the other
being installed. They implement this small on-disk contract independently so
their root authorities can safely share identity, storage, locking, revocation,
and package-lifecycle infrastructure without interpreting scopes owned by the
other component.

## Identity and scopes

A grant is keyed by the tuple `(uid, application identity, capability)`.
Both components derive the application identity with
`org.keysharp.app-identity-v1`: a protected executable is identified by its
absolute path; an executable which its user can replace is identified by its
SHA-256 content digest.

The stable cross-component capability values are:

| Value | Scope |
| ---: | --- |
| `0x00000001` | input monitoring |
| `0x00000002` | input control |
| `0x00000004` | window monitoring |
| `0x00000008` | window control |
| `0x00000010` | screen capture |
| `0x00000020` | audio capture |
| `0x00000040` | camera capture |
| `0x00000080` | clipboard monitoring |

These values are a storage contract, not the desktop session protocol's local
capability numbers. The desktop authority owns window monitoring, window
control, screen capture, audio capture, camera capture, and clipboard
monitoring. The input authority owns input monitoring and input control.
Global cursor-position queries are permission-free and never create or consult
a marker.

## Why this store exists

The portal [PermissionStore backend](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.impl.portal.PermissionStore.html)
stores portal-owned resource decisions as permission arrays plus free-form
data. It does not enforce privileged input, capture, or automation operations,
and it does not provide the consent UI for these independently brokered
capabilities.

[polkit](https://polkit.pages.freedesktop.org/polkit/polkit.8.html) is used for
the privileged authorization transaction and its policy-agent prompt. Its
retained authorizations are temporary, however, and applications must not
manufacture administrator policy rules to turn a user's answer into a durable
per-application exception. The marker store records durable executable grants
for both enforcement authorities in one coordinated, revocable namespace.
Polkit remains the gate for creating them, not their long-term database.

## Files and concurrency

The persistent root is `/var/lib/keysharp-permissions/v1`, owned by root and
mode `0700`. Each grant is one root-owned, mode-`0600` marker:

```text
grant-<uid>-<64-lowercase-hex-identity>-<8-hex-capability>.grant
```

Its contents are:

```text
keysharp-permission-v1
<uid>\t<identity>\t<capability>\t<unix-time>\t<display-path>
```

Authorization requires the filename and record to agree. Readers reject
symlinks, non-regular files, an unexpected owner, permissive modes, malformed
records, and unknown identity text.

The executable path is display-only metadata. Before it is stored or included
in a polkit detail or message, every byte below `0x20` and `0x7f` is replaced
with `?`, so a Linux filename cannot inject a new line or terminal control into
a prompt.

One file represents one bit. Adding a grant uses a mode-`0600` temporary
file, `fsync`, atomic `rename`, and a directory `fsync`. Revocation
unlinks only explicitly owned capability files. Unknown files and capability
values are preserved. Both implementations coordinate through
`/var/lib/keysharp-permissions/v1/.lock` with `flock`. Each marker is written
atomically without changing any other scope's marker.

Unix DAC cannot grant write access "to these two binaries" as executable
identities. Safety instead comes from the two narrowly sandboxed root
authorities, the protected directory, validated marker files, the common lock,
and each daemon masking requests to the capabilities it actually enforces.

## Live revocation and prompt races

The two authorities serialize interactive first-use decisions for the same
application with a root-owned, mode-`0600` runtime lock:

```text
/run/keysharp-permissions/.prompt-<uid>-<64-lowercase-hex-identity>.lock
```

The file is opened with `O_NOFOLLOW`, verified as a root-owned regular file,
and locked exclusively with `flock`. The lock is deliberately per application,
not per capability, so overlapping requests from the two independently
installed helpers cannot open simultaneous policy-agent dialogs. After
acquiring it, an authority revalidates the process and re-reads grants and the
revoke generation. It skips polkit if every requested capability was granted
while this request waited; otherwise it prompts only for the still-missing
capabilities and retains the prompt lock through grant persistence.
Implementations close but do not unlink the lock file: unlinking could split
current and subsequent waiters across different inodes.

The prompt lock never substitutes for the persistent store lock. Grant reads
and writes take the store lock only for their short filesystem operations, and
no store lock is held while the policy-agent UI is open. Revocation does not
take the prompt lock, so it can proceed while a prompt is pending.

Every revoke is bracketed, while holding the common store lock, by increments
of the root-owned generation file
`/run/keysharp-permissions/revoke-<uid>.generation`. The desktop user service
watches it and closes capability-bearing sessions. `keysharp-inputd` watches
`/run/keysharp-permissions` with inotify; generation move and close-write edges
re-read one generation per connected UID and the affected app markers. An
unexpected generation-file delete, queue overflow, watch loss, malformed
notification data, generation-read failure, or poll error closes the watcher,
clears cached persistent grants, hooks, and `BlockInput` state, fences queued
synthesis, releases grabs, and fails open for physical input. Input-event
dispatch never performs store I/O.

An interactive polkit request records the generation after acquiring the
prompt lock and before opening the dialog. Its eventual grant is written only
while holding the common store lock and only if the generation is unchanged.
A revoke which overlaps a prompt therefore wins and cannot be undone by a late
prompt completion.

Administration is scoped through the authenticated authority socket. The
desktop CLI lists only desktop-domain grants for its kernel-authenticated peer
UID, and revokes either selected capabilities for one 64-hex application
identity or, only with an explicit `--all`, every desktop-domain grant for that
UID. A scoped revoke still advances the generation before and after mutation,
so it also fences an in-flight prompt for the selected application.

## Package ownership and removal

The marker and generation roots are runtime state, not files owned by either
package. Each package installs the same narrow `tmpfiles.d` declaration so it
works when installed alone. The services deliberately do not claim the shared
paths with systemd `StateDirectory=` or `RuntimeDirectory=`: `systemctl clean`
on one service could otherwise delete permission state still used by the other.
Installing either component may create the directories. Normal removal and
package-specific `--purge` never delete shared grants; doing so could revoke
authorization needed by another installed application or component. Grants
are removed through the authenticated permission CLI. A system-wide "remove
all shared permission state" operation is valid only as an explicit
administrative action after verifying that no installed package still uses
this contract.
