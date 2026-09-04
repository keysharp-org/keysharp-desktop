#ifndef KEYSHARP_DESKTOP_X11_WORKER_H
#define KEYSHARP_DESKTOP_X11_WORKER_H

#include "operation_result.h"
#include "protocol.h"
#include "protocol_io.h"

#include <stdbool.h>
#include <sys/types.h>

/* Whether this request is one the X11 worker serves, and well formed. */
bool ksd_x11_request_valid(const ksd_frame *request);

/* Runs one X11 request in the forked worker, after privileges have been
 * dropped. session_pid is the registered session daemon, whose environment
 * names the display; see ksd_capture_worker_execute. */
void ksd_x11_execute(const ksd_frame *request, pid_t session_pid,
                     ksd_operation_result *result);

/* The same work, on a connection the caller already holds and keeps.
 *
 * The one-shot form above opens a display, serves one request and closes it,
 * which is right when the process is about to exit. A worker that serves many
 * requests must not pay a connect and its round trips per request, so it opens
 * once and calls this. Returns false only when the connection itself has
 * failed, which tells the caller to reopen rather than to answer. */
struct ksd_x11;
bool ksd_x11_execute_on(struct ksd_x11 *connection, const ksd_frame *request,
                        ksd_operation_result *result);

/* Opens the display the registered session daemon names. Split out so a
 * persistent worker can hold one open across requests. */
ksd_status ksd_x11_open_for_session(pid_t session_pid,
                                    struct ksd_x11 **connection);

#endif
