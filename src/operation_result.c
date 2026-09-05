#include "operation_result.h"

#include "protocol.h"
#include "protocol_io.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void ksd_result_init(ksd_operation_result *result)
{
    memset(result, 0, sizeof(*result));
    result->status = KSD_STATUS_INTERNAL;
    result->payload_fd = -1;
}

static void release_payload_fd(ksd_operation_result *result)
{
    if (result->payload_fd >= 0)
        close(result->payload_fd);
    result->payload_fd = -1;
}

void ksd_result_clear(ksd_operation_result *result)
{
    if (result == NULL)
        return;
    free(result->tail);
    release_payload_fd(result);
    ksd_result_init(result);
}

void ksd_result_error(ksd_operation_result *result, uint32_t status,
                      uint32_t detail, const char *diagnostic)
{
    if (result == NULL)
        return;
    free(result->tail);
    release_payload_fd(result);
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
    release_payload_fd(result);
    result->status = KSD_STATUS_OK;
    result->detail = 0u;
    result->diagnostic[0] = '\0';
    result->tail = tail;
    result->tail_length = tail_length;
    return true;
}

bool ksd_result_take_fd(ksd_operation_result *result, int payload_fd,
                        uint32_t tail_length)
{
    if (result == NULL || payload_fd < 0 || tail_length == 0u) {
        if (payload_fd >= 0)
            close(payload_fd);
        return false;
    }
    free(result->tail);
    release_payload_fd(result);
    result->status = KSD_STATUS_OK;
    result->detail = 0u;
    result->diagnostic[0] = '\0';
    result->tail = NULL;
    result->tail_length = tail_length;
    result->payload_fd = payload_fd;
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

bool ksd_result_take_framed_text(ksd_buffer *buffer,
                                 ksd_operation_result *result,
                                 uint32_t failure_status,
                                 const char *diagnostic)
{
    bool ok = ksd_buffer_frame_text(buffer, KSD_MAX_TEXT_BYTES);

    if (ok) {
        uint8_t *tail = buffer->data;
        uint32_t length = (uint32_t)buffer->length;

        buffer->data = NULL;
        buffer->length = 0u;
        buffer->capacity = 0u;
        ok = ksd_result_take(result, tail, length);
    }
    ksd_buffer_clear(buffer);
    if (!ok)
        ksd_result_error(result, failure_status, 0u, diagnostic);
    return ok;
}
