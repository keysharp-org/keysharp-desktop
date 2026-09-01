#include "operation_result.h"

#include "protocol.h"
#include "protocol_io.h"

#include <stdlib.h>
#include <string.h>

void ksd_result_init(ksd_operation_result *result)
{
    memset(result, 0, sizeof(*result));
    result->status = KSD_STATUS_INTERNAL;
}

void ksd_result_clear(ksd_operation_result *result)
{
    if (result == NULL)
        return;
    free(result->tail);
    ksd_result_init(result);
}

void ksd_result_error(ksd_operation_result *result, uint32_t status,
                      uint32_t detail, const char *diagnostic)
{
    if (result == NULL)
        return;
    free(result->tail);
    result->tail = NULL;
    result->tail_length = 0u;
    result->status = status;
    result->detail = detail;
    result->diagnostic[0] = '\0';
    if (diagnostic == NULL)
        return;
    size_t source_length = strlen(diagnostic);
    size_t length = source_length < sizeof(result->diagnostic) - 1u
        ? source_length : sizeof(result->diagnostic) - 1u;
    while (length != 0u
        && !ksd_utf8_valid((const uint8_t *)diagnostic, length, false))
        length--;
    if (length != 0u)
        memcpy(result->diagnostic, diagnostic, length);
    result->diagnostic[length] = '\0';
}

bool ksd_result_take(ksd_operation_result *result, uint8_t *tail,
                     uint32_t tail_length)
{
    if (result == NULL || (tail_length != 0u && tail == NULL)) {
        free(tail);
        return false;
    }
    free(result->tail);
    result->status = KSD_STATUS_OK;
    result->detail = 0u;
    result->diagnostic[0] = '\0';
    result->tail = tail;
    result->tail_length = tail_length;
    return true;
}

bool ksd_result_copy(ksd_operation_result *result, const void *tail,
                     uint32_t tail_length)
{
    uint8_t *copy = NULL;
    if (tail_length != 0u) {
        if (tail == NULL)
            return false;
        copy = malloc(tail_length);
        if (copy == NULL)
            return false;
        memcpy(copy, tail, tail_length);
    }
    return ksd_result_take(result, copy, tail_length);
}
