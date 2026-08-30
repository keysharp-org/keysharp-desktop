# Privileged application identity v1

`keysharp-desktop` and `keysharp-input` use the same executable-level identity
algorithm and per-capability marker namespace. Their executables, authorization
actions, protocols, and implementations remain independent.

The final identifier is lowercase hexadecimal SHA-256 of:

```text
ASCII("org.keysharp.app-identity-v1") || 00 || ASCII(kind) || 00 || identity
```

There is no terminator after `identity`.

- Kind `path` is used when the already-open executable inode and every ancestor
  of its resolved absolute path are owned by UID 0 and have no group or other
  write bits. Ancestors are checked without following symlinks, and the final
  path must still name the opened regular-file inode. `identity` is the exact
  absolute bytes returned by `readlink("/proc/PID/exe")`.
- Otherwise kind `sha256` is used. `identity` is the 64 lowercase ASCII hex
  bytes of SHA-256 over the already-open executable file contents.

Command-line arguments are excluded: they can contain secrets, can be changed
or reproduced by the process, and would make permanent grants unstable.

The kernel path is `/proc/PID/exe`, so a shared host such as `dotnet` or a
general-purpose interpreter gives every payload in that host the same executable
identity. Applications that require distinct grants must use distinct native or
apphost executables rather than a shared system interpreter.

Shared test vectors:

| Kind | Identity | Final hash |
| --- | --- | --- |
| `path` | `/usr/bin/example-app` | `a39558ae92a7f5227560ccf6e10e2941aeeceb8a3b608b4fea274f98dc41f1ae` |
| `sha256` | 64 ASCII `0` bytes | `73cd7ab5e10d259a782b6e021af8326514447477af0358481ee31fc5fee7d434` |

The authority obtains PID and UID from `SO_PEERCRED`, records `/proc/PID/stat`
field 22, and rejects identification if that start time changes. After polkit
returns it identifies the process again and requires both the start time and app
hash to match before persisting a grant.
