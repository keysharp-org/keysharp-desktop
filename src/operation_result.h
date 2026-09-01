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
} ksd_operation_result;

void ksd_result_init(ksd_operation_result *result);
void ksd_result_clear(ksd_operation_result *result);
void ksd_result_error(ksd_operation_result *result, uint32_t status,
                      uint32_t detail, const char *diagnostic);
bool ksd_result_take(ksd_operation_result *result, uint8_t *tail,
                     uint32_t tail_length);
bool ksd_result_copy(ksd_operation_result *result, const void *tail,
                     uint32_t tail_length);

#endif
