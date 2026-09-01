# Application identity

The shared permission library derives identity from the kernel-resolved
`/proc/PID/exe` inode and records the process start time to prevent PID reuse.

For a root-owned, non-writable executable path with protected ancestors, the
identity is path-based. Otherwise it hashes the opened executable contents.
The final principal is a canonical SHA-256 value shared by cooperating
authorities. Command-line arguments are excluded because they may contain
secrets and do not form a stable executable identity.

Executable identity is a consent principal, not same-UID isolation. A shared
interpreter or runtime host gives its payloads one principal. A user who can
alter a granted program at launch or runtime may be able to borrow its grant.
Applications needing distinct principals should use distinct protected native
executables or apphosts and an external sandbox appropriate to their threat
model.
