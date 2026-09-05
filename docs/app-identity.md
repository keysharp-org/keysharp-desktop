# Application identity

The shared permission library derives identity from the kernel-resolved
`/proc/PID/exe` inode and records the process start time to prevent PID reuse.

For a root-owned, non-writable executable path with protected ancestors, the
identity is path-based. Otherwise it hashes the opened executable contents.
The final principal is a canonical SHA-256 value shared by cooperating
authorities. Command-line arguments are excluded because they may contain
secrets and do not form a stable executable identity.

Installing a new root-protected executable at the same protected path retains
the path principal and its grants. Changed contents of a user-writable
executable produce a new principal; identical contents moved to another path
share the existing content principal. The permission is per scope, so requesting
additional scopes can require further consent even for the same principal.

Active connections compare UID, process start time, `/proc/PID/exe` path, and
the executable's device, inode, size, and nanosecond change/modification times.
An unchanged fingerprint reuses the verified principal; a change triggers full
identity revalidation. Interactive consent performs full checks before and
after the prompt. Loaded libraries, scripts, managed assemblies, environment
variables, and transferred connection descriptors remain outside this identity.

Executable identity is a consent principal, not same-UID isolation. A shared
interpreter or runtime host gives its payloads one principal. A user who can
alter a granted program at launch or runtime may be able to borrow its grant.
Applications needing distinct principals should use distinct protected native
executables or apphosts and an external sandbox appropriate to their threat
model.
