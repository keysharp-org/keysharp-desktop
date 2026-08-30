# Integrating with keysharp-desktop

How another project depends on `keysharp-desktop` and talks to its per-user
broker. For the complete message set see [protocol.md](protocol.md); for the
security boundary see [../SECURITY.md](../SECURITY.md).

## The dependency model

There is no library to link. A client needs two things:

- **At build time:** the wire contract in `keysharp_desktop/protocol.h`. It is
  installed under the standard include directory, and it is MIT-licensed, so
  vendoring a copy into your own tree is equally valid.
- **At run time:** an installed `keysharp-desktop`, with its user socket
  activated and — on GNOME or Cinnamon — its compositor extension enabled.

Your build does not need the service present, and your application should treat
its absence as a normal condition — see
[Degrading when it is absent](#degrading-when-it-is-absent).

## Declaring the dependency

Distribution packages should express the relationship through the package
manager: `Depends` when your application is useless without desktop
integration, or `Recommends` when those features are optional. Package removal
and purge both preserve the shared permission store.

Applications distributed outside a package manager can bundle the portable
archive and install it only when a compatible service is missing:

```sh
sudo ./install.sh --skip-if-compatible
```

A root-owned installation implementing the same protocol major and minor is
reused unchanged, but only after its units, polkit action, shared-state
declaration, authority wiring, and user-socket enablement all verify.

Never run the component's uninstaller from your own uninstaller. The broker can
have other clients.

## Finding the socket

| Context | Path |
|---|---|
| Explicit override | `$KEYSHARP_DESKTOP_SOCKET` |
| Default | `$XDG_RUNTIME_DIR/keysharp-desktop/keysharp-desktop.sock` |

The root authority listens separately on `/run/keysharp-desktop/authority.sock`.
Clients do not connect to it directly: the broker hands your already-connected
socket to the authority with `SCM_RIGHTS`, so authorization is performed against
your kernel-authenticated PID and UID rather than anything you assert.

## Wire basics

The session protocol is line-based UTF-8 text over an `AF_UNIX` stream socket —
no byte-order or struct-packing concerns. Each request and reply is one
newline-terminated line.

Errors take the form:

```text
KSDP/1.2 ERROR <code> <message>
```

## Handshake and version negotiation

The first line is the hello:

```text
KSDP/1.2 HELLO <backend> <8-hex-capabilities> <mode>
```

| Field | Values |
|---|---|
| `KSDP/<version>` | must equal the version you built against |
| `<backend>` | `auto`, or a specific backend name |
| `<capabilities>` | exactly 8 hex digits: the `KSD_CAP_*` bits you need, `00000000` for none |
| `<mode>` | `check` reads existing grants only; `request` may open polkit |

A successful reply names the active backend, the granted capability mask, and
the operations the session supports:

```text
KSDP/1.2 READY <backend> <8-hex-granted> capture-area,capture-window,...
```

Use the granted mask, not the requested one.

A hello naming a different version of this protocol is rejected with
`ERROR protocol incompatible protocol version`, which is distinct from
`ERROR protocol invalid handshake` for a malformed request — so a client can
tell version skew from a bad line and report it accurately. Treat a differing
major version as incompatible and disable the affected features.

A capability-free hello (`00000000 check`) is a liveness probe. It succeeds with
`READY none` when no supported capture provider is active, which is the normal
result on an unsupported compositor or before the extension is enabled. That is
what `keysharp-desktop probe` does.

## Capabilities and permissions

Two distinct namespaces meet here, and confusing them is the most common
integration mistake.

**Session capability bits** are what you put in the handshake:

| Constant | Value |
|---|---:|
| `KSD_CAP_SCREEN_CAPTURE` | `0x01` |
| `KSD_CAP_WINDOW_MONITORING` | `0x02` |
| `KSD_CAP_WINDOW_CONTROL` | `0x04` |
| `KSD_CAP_AUDIO_CAPTURE` | `0x08` |
| `KSD_CAP_CAMERA_CAPTURE` | `0x10` |
| `KSD_CAP_CLIPBOARD_MONITORING` | `0x20` |

**Durable scopes** are what the user consents to, and what is stored and
revoked. They are the shared cross-component namespace, and their values are
deliberately *not* the same as the session bits above:

| Constant | Value |
|---|---:|
| `KSP_SCOPE_WINDOW_MONITORING` | `0x04` |
| `KSP_SCOPE_WINDOW_CONTROL` | `0x08` |
| `KSP_SCOPE_SCREEN_CAPTURE` | `0x10` |
| `KSP_SCOPE_AUDIO_CAPTURE` | `0x20` |
| `KSP_SCOPE_CAMERA_CAPTURE` | `0x40` |
| `KSP_SCOPE_CLIPBOARD_MONITORING` | `0x80` |

`keysharp-desktop` owns these six. Input monitoring (`0x01`) and input control
(`0x02`) belong to a separate authority and are never granted here. The full
contract is in [permission-store.md](permission-store.md).

Global cursor-position queries and clipboard writes are permission-free and
never read or write the store.

Use `check` mode for a settings screen or startup probe so you never prompt
unexpectedly, and `request` at the moment the user asks for the feature.

## Handling revocation

Revocation advances a root-owned runtime generation. The broker polls it and
closes every connection that received a nonzero capability mask, including
capability-only `auto` connections. A capture already executing may finish, but
no later command on that connection is accepted.

The practical pattern: hold an authorization connection open and treat its
closure as the revocation signal. On closure, drop cached capabilities,
reconnect, and re-handshake before using any gated operation.

## Degrading when it is absent

Check for the socket and fall back cleanly when it is missing, the handshake
fails, the major version differs, or the reply is `READY none`. The last case is
common and benign: the service is installed and healthy but this compositor has
no provider. A user in that situation should get an application that runs with
its desktop features disabled, and a message naming what to enable.

## A minimal client

`src/desktopctl.c` is a complete, working client covering socket discovery,
handshake, capability status and grant requests, and the administration
commands. Read it first; it is the reference implementation of everything above.

## Checklist

- [ ] Vendor or install `keysharp_desktop/protocol.h`
- [ ] Declare the runtime dependency in your package metadata
- [ ] Discover the socket, and degrade cleanly when it is absent
- [ ] Send the hello with the capability bits you need, and honour the granted mask
- [ ] Reject a differing protocol major version
- [ ] Handle `READY none` as an expected unsupported-compositor result
- [ ] Use `check` mode for status probes so you never prompt unexpectedly
- [ ] Treat connection closure as revocation, then reconnect and re-handshake
- [ ] Never invoke the component's uninstaller from your own
