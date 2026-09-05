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
contains or forwards application frames. A daemon without a dedicated provider
registers the generic backend together with the operation mask it derived from
the live Wayland registry, authenticated compositor-specific interfaces, and
the session's desktop portals. The authority accepts that mask only from the
authenticated peer and intersects it with the generic backend's fixed ceiling.

Every frame has a 24-byte little-endian header. Request frames carry at most
4 KiB of payload; result text is bounded separately.
Request IDs are nonzero except for the one-way `PING`, which uses zero and has
no response. Responses match opcode and request ID. Results begin with status
and detail fields. Events use request ID zero. Unknown flags,
opcodes, reserved values, trailing bytes, noncanonical booleans, invalid UTF-8,
or limit violations fail closed.

A request larger than one frame is chunked with `MORE`. Every frame of the
sequence repeats one opcode and one nonzero request ID; each `MORE` frame is
exactly 4 KiB and the unflagged frame that ends the sequence is 1 byte to
4 KiB, so a payload of 4 KiB or less is never chunked and no payload has two
legal encodings. The reassembled total is at most 4 MiB, that is 1024 frames.
Only an RPC session may chunk, and only for an opcode the authority marks
chunkable; the clipboard write is the only such opcode, so every other opcode
is refused at the first frame. A chunked request is fully reassembled before
its permission scope is checked, so a session with no grant can spend one
assembly reservation before being denied; the per-user and global assembly
budgets and the ten-second deadline bound that. Nothing may be interleaved with a sequence, not
even a `PING`: a short chunk, an empty chunk, a chunk for another opcode or
request ID, a response or event frame, an oversized total, a sequence from a
role that may not chunk, and a sequence still incomplete after ten seconds all
end the connection with no reply. The only chunk failure that answers first is
the authority's assembly budget, which replies `RESOURCE_EXHAUSTED` and then
ends the connection. That budget is separate from the capture budget so
neither can starve the other, and it is capped per user as well as globally so
no one user can exhaust it for every other user. A session's partial request is
freed with the session. There is no way to cancel a sequence except by disconnecting, and
responses and events are not chunked this way; `PERMISSIONS_LIST` keeps its
own `MORE`-per-entry form.

A capture is admitted against its own budget, which bounds how many captures
may be in flight at once rather than how much a capture may return. Four are
admitted across the whole service and two per user, so one user cannot deny
captures to another and a busy user cannot deny them to everyone. A capture
refused on either limit answers `RESOURCE_EXHAUSTED` immediately; it is never
queued, because a caller that would rather do something else should not be made
to wait to find out. Only the three capture opcodes consult this budget. Other
operations retain their own authority threads. X11 and generic Wayland
captures use a separate persistent display worker from window, keyboard, and
pointer queries, so a slow screenshot does not queue in front of those queries.
The returned capture is a sealed memfd, passed by descriptor rather than copied
into the public response. Encoding and decoding may need additional temporary
buffers inside a worker or provider. A slow client retains its admission slot
until the descriptor response is sent or its I/O deadline expires.

Framing is strict; vocabulary is not. The client library carries an
unrecognized backend value and unrecognized operation bits through to its
caller, narrows a granted scope mask to the scopes this authority manages, and
passes a revoked scope mask and a stored permission entry's scopes through
unchanged, so nothing understates a grant or a revocation. The authority still
rejects any scope an application requests that it does not name.

Typed operation families cover capture, window monitoring/control, clipboard
monitoring, clipboard writing, pointer control, cursor position, and work
area. Each operation
has an availability bit. Sensitive families map to exactly one durable scope;
cursor position, work area, window handles, display topology, keyboard state,
and clipboard writes have no scope and do not consult the grant store.

Client ABI minor 8 adds these operations to protocol 2.0. All integers below
are little-endian; JSON replies have a `u32` UTF-8 byte length followed by text.

| Opcode | Operation bit | Request | Scope |
| --- | --- | --- | --- |
| `0x2014` | 32 | window query: `u64 handle` | WindowMonitoring |
| `0x2015` | 33 | children: `u64 parent` | WindowMonitoring |
| `0x2016` | 34 | at point: `i32 x, i32 y, u32 deepest, u32 zero` | WindowMonitoring |
| `0x2052` | 35 | display list: empty | none |
| `0x2053` | 36 | keyboard state: empty, or `u32 length, UTF-8 revision` (at most 64 bytes) | none |
| `0x2070` | 37 | title: `u64 handle, u32 length, UTF-8 title` | WindowControl |
| `0x2071` | 38 | visible: `u64 handle, u32 boolean, u32 zero` | WindowControl |
| `0x2072` | 39 | redraw: `u64 handle` | WindowControl |
| `0x2073` | 40 | click: `u64 handle, i32 x, i32 y, u32 button, u32 count` | WindowControl |
| `0x2074` | 41 | button: `u64 handle, i32 x, i32 y, u32 button, u32 down` | WindowControl |
| `0x2075` | 42 | child focus: `u64 handle` | WindowControl |

These new window-control operations return an empty success result. Query
schemas and the keyboard `mapRevision` cache contract are documented in
[application integration](integrating.md#window-display-and-keyboard-queries).

The private persistent-worker relay uses a stream socket. Bootstrap carries the
fixed header, supplementary groups, and two capture-pipe descriptors; a
one-byte ready acknowledgement separates bootstrap from requests. Normal
responses use status/detail and the operation tail. A capture response sets
private flag `0x8000`, carries status/detail/descriptor length, and transfers one
sealed memfd. This flag is internal to the worker relay, not a public client
protocol flag. An operation returning `UNAVAILABLE` leaves a healthy relay
usable; transport failure retires it. Workers are never retried automatically
for an already dispatched operation.

Capture payloads are bounded by checked dimensions, pixel count, encoded byte
count, process limits, and a global authority budget. A window capture is
bounded twice: the provider rejects the window's scaled pixel count before the
compositor paints anything, and the returned PNG's own IHDR is re-checked
against the same limits. Text, mimetypes, event data, window reservations,
provider actors, and I/O all have explicit bounds or absolute deadlines.

GNOME and Cinnamon providers use private GDBus servers and export their objects
only after the peer's kernel credentials identify root. At backend registration,
the authority looks up the process owning the compositor's canonical session-bus
name in a credential-dropped helper, then pins the captured process identity;
it never attempts a root connection to the user bus. KWin script calls are also
pinned to the unique owner of its canonical bus name. Both GNOME capture calls
answer on the private peer with PNG bytes from an in-memory stream. KWin capture
uses an isolated worker, a root-owned directional pipe, and a write-only D-Bus
call child. The non-dumpable worker drains pixels while that child returns only
fixed-size metadata. Generic whole-desktop capture asks the desktop screenshot
portal and copies the validated PNG into a sealed memfd before returning it.
These transports are private.
