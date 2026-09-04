#ifndef KEYSHARP_DESKTOP_CAPTURE_WORKER_H
#define KEYSHARP_DESKTOP_CAPTURE_WORKER_H

#include "operation_result.h"
#include "protocol_io.h"

#include <keysharp_permissions/permissions.h>
#include <stdbool.h>
#include <sys/types.h>

typedef bool (*ksd_capture_worker_continue_fn)(void *user_data);

/* session_pid is the REGISTERED session daemon's pid, not the calling
 * client's. The worker reads the display environment from it after dropping
 * privileges, so it must name the party the authority authenticated. */
void ksd_capture_worker_execute(const ksp_identity *identity, gid_t gid,
                                const ksd_frame *request,
                                ksd_capture_worker_continue_fn keep_running,
                                void *user_data, pid_t session_pid,
                                ksd_operation_result *result);
int ksd_capture_worker_main(int argc, char **argv);

/* What the forked worker will run. Exposed so the parent-side admission and
 * the child-side dispatch cannot drift apart, and so a gate can assert they
 * are the same set. */
bool ksd_capture_worker_request_valid(const ksd_frame *request);

#endif
