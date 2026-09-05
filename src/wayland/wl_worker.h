#ifndef KEYSHARP_DESKTOP_WL_WORKER_H
#define KEYSHARP_DESKTOP_WL_WORKER_H

#include "operation_result.h"
#include "protocol.h"
#include "protocol_io.h"

#include <stdbool.h>
#include <sys/types.h>

/* Whether this request is one the Wayland worker serves, and well formed. */
bool ksd_wayland_request_valid(const ksd_frame *request);

struct ksd_wayland;
bool ksd_wayland_execute_on(struct ksd_wayland *connection,
                            const ksd_frame *request,
                            ksd_operation_result *result);
ksd_status ksd_wayland_open_for_session(pid_t session_pid,
                                        struct ksd_wayland **connection);

#endif
