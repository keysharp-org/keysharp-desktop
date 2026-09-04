#ifndef KEYSHARP_DESKTOP_WL_WORKER_H
#define KEYSHARP_DESKTOP_WL_WORKER_H

#include "operation_result.h"
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

#endif
