# Shared permission store

Permanent grants use the canonical contract implemented by the pinned
`keysharp-permissions` library. Both authorities consume that implementation;
this project does not duplicate scope values, identity hashing, locking, or
marker parsing.

The scopes are:

| Value | Name |
| ---: | --- |
| `0x01` | input monitoring |
| `0x02` | input control |
| `0x04` | window monitoring |
| `0x08` | window control |
| `0x10` | screen capture |
| `0x20` | audio capture |
| `0x40` | camera capture |
| `0x80` | clipboard monitoring |

Markers live below `/var/lib/keysharp-permissions/v1`. They are root-owned,
mode 0600, atomically replaced regular files inside root-only directories.
Malformed names, records, ownership, modes, links, or sizes are ignored.

A grant key consists of UID, executable identity, and one scope bit. The
stored path is sanitized display metadata. Generation fencing prevents a late
polkit approval from recreating a concurrently revoked grant. A shared
per-identity prompt lock prevents duplicate prompts across authorities.

InputControl is intentionally shared: this authority may grant or revoke it
because its pointer operations exercise that scope. The common lock and store
make that change immediately visible to every participating authority.

Revocation advances the UID generation. Active sessions discard affected
authorization before acknowledging the revoke request and withhold sensitive
results if their generation changed during work.

The project uninstaller never deletes the shared store. Grants are removed by
an explicit permission-revoke command.
