#ifndef KEYSHARP_DESKTOP_LOCAL_CAPTURE_H
#define KEYSHARP_DESKTOP_LOCAL_CAPTURE_H

#include "operation_result.h"
#include "protocol_io.h"

void ksd_local_capture_execute(const ksd_frame *request,
                               int capture_read_fd, int capture_write_fd,
                               int capture_spool_fd,
                               ksd_operation_result *result);
bool ksd_local_capture_request_valid(const ksd_frame *request);
bool ksd_capture_tail_valid(const void *tail, uint32_t tail_length);
bool ksd_capture_pipe_valid(const int descriptors[2]);
bool ksd_capture_spool_valid(int descriptor);
bool ksd_capture_child_endpoints_valid(int write_descriptor,
                                       int metadata_descriptor);
bool ksd_capture_pipe_drain_until(int pipe_descriptor,
                                  int spool_descriptor,
                                  uint64_t deadline,
                                  uint32_t *length);

#endif
