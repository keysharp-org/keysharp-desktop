#ifndef KEYSHARP_DESKTOP_CAPTURE_WORKER_H
#define KEYSHARP_DESKTOP_CAPTURE_WORKER_H

#include "operation_result.h"
#include "protocol_io.h"

#include <keysharp_permissions/permissions.h>
#include <stdbool.h>
#include <sys/types.h>

typedef bool (*ksd_capture_worker_continue_fn)(void *user_data);

void ksd_capture_worker_execute(const ksp_identity *identity, gid_t gid,
                                const ksd_frame *request,
                                ksd_capture_worker_continue_fn keep_running,
                                void *user_data,
                                ksd_operation_result *result);
int ksd_capture_worker_main(int argc, char **argv);

#endif
