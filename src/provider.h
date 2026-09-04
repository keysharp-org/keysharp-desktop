#ifndef KEYSHARP_DESKTOP_PROVIDER_H
#define KEYSHARP_DESKTOP_PROVIDER_H

#include "backend.h"
#include "operation_result.h"
#include "protocol_io.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

typedef bool (*ksd_provider_event_fn)(uint16_t opcode,
                                      const void *payload,
                                      uint32_t payload_length,
                                      void *user_data);
typedef bool (*ksd_provider_cancel_fn)(void *user_data);

/* Whether this provider can actually serve a capture opcode. It must agree
 * with the corresponding bit in ksd_backend_operations for every backend: the
 * two drifted apart once, and Cinnamon window capture was advertised,
 * implemented and refused for as long as they disagreed. */
bool ksd_provider_capture_supported(ksd_backend backend, uint16_t opcode);
void ksd_provider_execute(uid_t uid, pid_t pid, pid_t provider_pid,
                          ksd_backend backend,
                          const ksd_frame *request,
                          ksd_operation_result *result);
int ksd_provider_watch(uid_t uid, pid_t provider_pid, ksd_backend backend,
                       bool clipboard,
                       ksd_provider_event_fn emit,
                       ksd_provider_cancel_fn cancelled,
                       void *user_data, char *diagnostic,
                       size_t diagnostic_capacity);

#endif
