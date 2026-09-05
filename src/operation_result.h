#ifndef KEYSHARP_DESKTOP_OPERATION_RESULT_H
#define KEYSHARP_DESKTOP_OPERATION_RESULT_H

#include <stdbool.h>
#include <stdint.h>

#define KSD_DIAGNOSTIC_CAPACITY 1024u

typedef struct ksd_operation_result {
    uint32_t status;
    uint32_t detail;
    char diagnostic[KSD_DIAGNOSTIC_CAPACITY];
    uint8_t *tail;
    uint32_t tail_length;
    /* A capture answers with its bytes in a sealed memfd rather than in tail,
     * so they are never copied into the response. -1 when the result carries
     * none, which is every operation but a capture. Exactly one of tail and
     * payload_fd is ever set. */
    int payload_fd;
} ksd_operation_result;

struct ksd_buffer;

void ksd_result_init(ksd_operation_result *result);
void ksd_result_clear(ksd_operation_result *result);
void ksd_result_error(ksd_operation_result *result, uint32_t status,
                      uint32_t detail, const char *diagnostic);
bool ksd_result_take(ksd_operation_result *result, uint8_t *tail,
                     uint32_t tail_length);
bool ksd_result_copy(ksd_operation_result *result, const void *tail,
                     uint32_t tail_length);
bool ksd_result_take_framed_text(struct ksd_buffer *buffer,
                                 ksd_operation_result *result,
                                 uint32_t failure_status,
                                 const char *diagnostic);
/* Takes ownership of payload_fd, which must be sealed and hold exactly
 * tail_length bytes. */
bool ksd_result_take_fd(ksd_operation_result *result, int payload_fd,
                        uint32_t tail_length);

#endif
