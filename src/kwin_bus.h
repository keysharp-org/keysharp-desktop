#ifndef KEYSHARP_DESKTOP_KWIN_BUS_H
#define KEYSHARP_DESKTOP_KWIN_BUS_H

#include "kwin_host.h"

#include <stdbool.h>

/* The D-Bus surface the KWin script calls, and the only part of this channel
 * that touches GLib.
 *
 * Queue and envelope decisions live in ksd_kwin_host, which is pure and tested
 * without a bus. This transport owns the name, pins callers to the compositor's
 * canonical bus owner, reloads the installed script, hands envelopes to the
 * host, and holds parked invocations until there is something to answer with.
 *
 * Started only for the KWin backend. Every other backend keeps the daemon's
 * plain poll loop, because none of them needs a bus name or main loop. */
typedef struct ksd_kwin_bus ksd_kwin_bus;

/* Owns io.github.keysharp.KWinProvider1 on the session bus and registers
 * /io/github/keysharp/KWinProvider. Returns NULL when the name cannot be
 * taken, which means another daemon already holds it. */
/* relay is the authority's end of the callback socket, or -1. Requests arrive
 * on it and answers go back out of order, because a cheap verb must not wait
 * behind an enumeration. */
ksd_kwin_bus *ksd_kwin_bus_start(ksd_kwin_host *host, int relay);

/* Convert one public binary request payload to the textual body carried by the
 * KWin script envelope. Exposed for the protocol-boundary test. */
bool ksd_kwin_request_text(uint16_t opcode, const uint8_t *payload,
						   uint32_t payload_length, char *text,
						   size_t capacity, uint32_t *text_length);

/* Convert the KWin script's textual result back to the public operation's
 * binary response shape. The caller initializes and clears payload. */
bool ksd_kwin_response_payload(uint16_t opcode, const uint8_t *body,
                               uint32_t body_length, ksd_buffer *payload);

/* Runs until the authority closes descriptor or the compositor changes, then
 * returns. Both are watched on the same loop rather than from another thread:
 * everything here already runs on one, and a second would need a lock around
 * the host for no gain. */
int ksd_kwin_bus_run(ksd_kwin_bus *bus, int descriptor);

void ksd_kwin_bus_stop(ksd_kwin_bus *bus);

#endif
