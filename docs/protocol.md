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
contains or forwards application frames. A daemon in a session with no
supported compositor registers the generic backend, which the authority accepts
from the same authenticated peer but which maps to an empty operation mask, so
the registration confers nothing and every typed operation stays unavailable.

Every frame has a 24-byte little-endian header and at most 4 KiB of payload.
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
to wait to find out. Only the two capture opcodes consult this budget, so a
capture in flight does not delay a window, clipboard or pointer request. Each
reservation is the worst case a capture may return, not what it does return, so
the bound is deliberately pessimistic in that direction. It is optimistic in
another: a reservation counts the returned bytes once, while the operation
result and the response payload are both live from the moment the response is
built until the write to the client completes, so the real peak is about twice
the reservation. The reservation is also held across that write, and the send
timeout is long, so a client that reads slowly holds its slot for as long as it
takes to drain.

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
cursor-position and work-area queries have no scope and do not consult the
grant store.

Capture payloads are bounded by checked dimensions, pixel count, encoded byte
count, process limits, and a global authority budget. A window capture is
bounded twice: the provider rejects the window's scaled pixel count before the
compositor paints anything, and the returned PNG's own IHDR is re-checked
against the same limits. Text, mimetypes, event data, window reservations,
provider actors, and I/O all have explicit bounds or absolute deadlines.

GNOME and Cinnamon providers use private root-authenticated GDBus peers. Both
GNOME capture calls answer on that peer with PNG bytes from an in-memory
stream. KWin capture uses an isolated worker, a root-owned directional pipe, and a
write-only D-Bus call child. The non-dumpable worker drains pixels while that
child returns only fixed-size metadata. These transports are private.
