#ifndef KEYSHARP_DESKTOP_KWIN_RELAY_H
#define KEYSHARP_DESKTOP_KWIN_RELAY_H

#include "operation_result.h"
#include "protocol_io.h"

#include <stdbool.h>
#include <stdint.h>

/* The authority's end of the socket a KWin daemon handed over at registration.
 *
 * The authority runs one thread per connection and they all share this one
 * socket, so responses are correlated by request id rather than by order. The
 * obvious alternative -- a lock held across the whole exchange -- would put
 * every KWin consumer behind whichever one is currently waiting on the
 * compositor, which is the single-channel serialisation the lane split exists
 * to remove. It would also make the daemon's queue pointless, since only one
 * job could ever be in it.
 *
 * There is no reader thread. Whichever caller finds no reader takes the role,
 * reads one frame, hands it to whoever was waiting for it, and gives the role
 * up. A caller whose own answer arrives while another is reading is woken by
 * that reader. */
typedef struct ksd_kwin_relay ksd_kwin_relay;

/* Takes ownership of descriptor. */
ksd_kwin_relay *ksd_kwin_relay_create(int descriptor);
void ksd_kwin_relay_destroy(ksd_kwin_relay *relay);

/* Sends one request and waits for its answer. Many callers may be inside this
 * at once. Fills result with the daemon's answer, or with TIMEOUT if the
 * deadline passes -- which is deliberately distinct from BUSY: a request that
 * went down this socket may have reached the compositor, and telling a caller
 * otherwise would make close and move-resize unsafe to retry. */
bool ksd_kwin_relay_call(ksd_kwin_relay *relay, const ksd_frame *request,
                         uint64_t deadline_ms, ksd_operation_result *result);

#endif
