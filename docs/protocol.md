# Private protocol

Protocol 2.0 is an implementation detail between `libkeysharp-desktop.so.0`
and the matching service. Applications use the public C ABI. Private headers
are neither installed nor a compatibility contract.

The root authority accepts one bounded `KSDP` frame stream on
`/run/keysharp-desktop/keysharp-desktop.sock`. The first frame is HELLO. Linux
peer credentials supply the application UID and PID; request bytes cannot
override identity, provider, backend, or authorization.

The same listener recognizes a fixed-size `KSDB` registration record from the
supervised user daemon. Registration is authenticated separately and never
contains or forwards application frames.

Every request has a 24-byte little-endian header and at most 4 KiB of payload.
Request IDs are nonzero except for the one-way `PING`, which uses zero and has
no response. Responses match opcode and request ID. Results begin with status
and detail fields. Events use request ID zero. Unknown flags,
opcodes, reserved values, trailing bytes, noncanonical booleans, invalid UTF-8,
or limit violations fail closed.

Typed operation families cover capture, window monitoring/control, clipboard
monitoring, pointer control, cursor position, and work area. Each operation
has an availability bit. Sensitive families map to exactly one durable scope;
cursor-position and work-area queries have no scope and do not consult the
grant store.

Capture payloads are bounded by checked dimensions, pixel count, encoded byte
count, process limits, and a global authority budget. Text, mimetypes, event
data, window reservations, provider actors, and I/O all have explicit bounds
or absolute deadlines.

GNOME and Cinnamon providers use private root-authenticated GDBus peers. KWin
capture uses an isolated worker, a root-owned directional pipe, and a
write-only D-Bus call child. The non-dumpable worker drains pixels while that
child returns only fixed-size metadata. These transports are private.
