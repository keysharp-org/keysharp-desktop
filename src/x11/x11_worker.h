#ifndef KEYSHARP_DESKTOP_X11_WORKER_H
#define KEYSHARP_DESKTOP_X11_WORKER_H

#include "operation_result.h"
#include "protocol.h"
#include "protocol_io.h"

#include <stdbool.h>
#include <sys/types.h>

/* Whether this request is one the X11 worker serves, and well formed. */
bool ksd_x11_request_valid(const ksd_frame *request);

struct ksd_x11;
bool ksd_x11_execute_on(struct ksd_x11 *connection, const ksd_frame *request,
                        ksd_operation_result *result);

/* Opens the display the registered session daemon names. */
ksd_status ksd_x11_open_for_session(pid_t session_pid,
                                    struct ksd_x11 **connection);

#endif
