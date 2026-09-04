#ifndef KEYSHARP_DESKTOP_WL_WORKER_H
#define KEYSHARP_DESKTOP_WL_WORKER_H

#include "operation_result.h"
#include "protocol.h"
#include "protocol_io.h"

#include <stdbool.h>
#include <sys/types.h>

/* Whether this request is one the Wayland worker serves, and well formed. */
bool ksd_wayland_request_valid(const ksd_frame *request);

/* Runs one request in the forked worker, after privileges have been dropped.
 * session_pid is the registered session daemon, whose environment names the
 * compositor; see ksd_session_environ_value. */
void ksd_wayland_execute(const ksd_frame *request, pid_t session_pid,
                         ksd_operation_result *result);

/* The same work on a connection the caller holds and keeps. A connect here
 * costs a socket connect plus two registry round trips, so a worker that
 * serves many requests must not pay it per request. Returns false when the
 * connection has failed, which is the caller's cue to reopen. */
struct ksd_wayland;
bool ksd_wayland_execute_on(struct ksd_wayland *connection,
                            const ksd_frame *request,
                            ksd_operation_result *result);
ksd_status ksd_wayland_open_for_session(pid_t session_pid,
                                        struct ksd_wayland **connection);

#endif
