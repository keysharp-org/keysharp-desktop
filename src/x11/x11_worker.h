#ifndef KEYSHARP_DESKTOP_X11_WORKER_H
#define KEYSHARP_DESKTOP_X11_WORKER_H

#include "operation_result.h"
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

#endif
