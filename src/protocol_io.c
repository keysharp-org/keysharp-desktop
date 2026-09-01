#include "protocol_io.h"

#include "transport.h"
#include "protocol.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>

uint16_t ksd_decode_u16(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0]
        | (uint16_t)((uint16_t)data[1] << 8u));
}

uint32_t ksd_decode_u32(const uint8_t *data)
{
    return (uint32_t)data[0]
        | ((uint32_t)data[1] << 8u)
        | ((uint32_t)data[2] << 16u)
        | ((uint32_t)data[3] << 24u);
}

uint64_t ksd_decode_u64(const uint8_t *data)
{
    return (uint64_t)ksd_decode_u32(data)
        | ((uint64_t)ksd_decode_u32(data + 4u) << 32u);
}

void ksd_encode_u16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8u);
}

void ksd_encode_u32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8u);
    data[2] = (uint8_t)(value >> 16u);
    data[3] = (uint8_t)(value >> 24u);
}

void ksd_encode_u64(uint8_t *data, uint64_t value)
{
    ksd_encode_u32(data, (uint32_t)value);
    ksd_encode_u32(data + 4u, (uint32_t)(value >> 32u));
}

static bool valid_public_header(const ksd_frame *frame)
{
    bool response = (frame->flags & KSD_FLAG_RESPONSE) != 0u;
    bool event = (frame->flags & KSD_FLAG_EVENT) != 0u;
    bool more = (frame->flags & KSD_FLAG_MORE) != 0u;

    if ((frame->flags & ~KSD_FLAG_ALL) != 0u
        || (response && event) || (more && !response))
        return false;
    if (event)
        return frame->request_id == 0u;
    if (response)
        return frame->request_id != 0u;
    return frame->request_id != 0u
        || (frame->opcode == KSD_OP_PING && frame->flags == 0u);
}

static uint64_t monotonic_milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0u;
    return (uint64_t)now.tv_sec * 1000u
        + (uint64_t)now.tv_nsec / 1000000u;
}

static uint64_t socket_deadline(int descriptor, int option)
{
    struct timeval timeout = { 0 };
    socklen_t size = sizeof(timeout);
    if (getsockopt(descriptor, SOL_SOCKET, option, &timeout, &size) != 0
        || size != sizeof(timeout)
        || (timeout.tv_sec == 0 && timeout.tv_usec == 0))
        return UINT64_MAX;
    uint64_t duration = (uint64_t)timeout.tv_sec * 1000u
        + ((uint64_t)timeout.tv_usec + 999u) / 1000u;
    uint64_t now = monotonic_milliseconds();
    return now > UINT64_MAX - duration ? UINT64_MAX : now + duration;
}

static bool wait_until(int descriptor, short events, uint64_t deadline)
{
    for (;;) {
        int timeout = -1;
        if (deadline != UINT64_MAX) {
            uint64_t now = monotonic_milliseconds();
            if (now == 0u || now >= deadline) {
                errno = ETIMEDOUT;
                return false;
            }
            uint64_t remaining = deadline - now;
            timeout = remaining > (uint64_t)INT_MAX
                ? INT_MAX : (int)remaining;
        }
        struct pollfd item = { .fd = descriptor, .events = events };
        int ready = poll(&item, 1u, timeout);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready == 0) {
            errno = ETIMEDOUT;
            return false;
        }
        if (ready < 0 || (item.revents & (POLLERR | POLLNVAL)) != 0)
            return false;
        return (item.revents & (events | POLLHUP | POLLRDHUP)) != 0;
    }
}

static ssize_t read_until(int descriptor, void *data, size_t length,
                          uint64_t deadline)
{
    uint8_t *cursor = data;
    size_t offset = 0u;
    while (offset < length) {
        if (!wait_until(descriptor, POLLIN, deadline))
            return -1;
        ssize_t count = recv(descriptor, cursor + offset, length - offset,
                             MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN
                          || errno == EWOULDBLOCK))
            continue;
        if (count <= 0)
            return offset == 0u ? count : -1;
        offset += (size_t)count;
    }
    return (ssize_t)offset;
}

static bool write_until(int descriptor, const void *data, size_t length,
                        uint64_t deadline)
{
    const uint8_t *cursor = data;
    size_t offset = 0u;
    while (offset < length) {
        if (!wait_until(descriptor, POLLOUT, deadline))
            return false;
        ssize_t count = send(descriptor, cursor + offset, length - offset,
                             MSG_DONTWAIT | MSG_NOSIGNAL);
        if (count < 0 && (errno == EINTR || errno == EAGAIN
                          || errno == EWOULDBLOCK))
            continue;
        if (count <= 0)
            return false;
        offset += (size_t)count;
    }
    return true;
}

int ksd_frame_read(int descriptor, const uint8_t magic[4],
                   uint16_t major, uint16_t minor, uint32_t maximum_payload,
                   bool public_rules, ksd_frame *frame)
{
    uint8_t header[KSD_FRAME_HEADER_SIZE];

    if (descriptor < 0 || magic == NULL || frame == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(frame, 0, sizeof(*frame));
    uint64_t deadline = socket_deadline(descriptor, SO_RCVTIMEO);
    ssize_t header_count = read_until(descriptor, header, sizeof(header),
                                      deadline);
    if (header_count <= 0)
        return (int)header_count;

    memcpy(frame->magic, header + KSD_FRAME_MAGIC_OFFSET, sizeof(frame->magic));
    frame->major = ksd_decode_u16(header + KSD_FRAME_MAJOR_OFFSET);
    frame->minor = ksd_decode_u16(header + KSD_FRAME_MINOR_OFFSET);
    frame->opcode = ksd_decode_u16(header + KSD_FRAME_OPCODE_OFFSET);
    frame->flags = ksd_decode_u16(header + KSD_FRAME_FLAGS_OFFSET);
    frame->payload_length =
        ksd_decode_u32(header + KSD_FRAME_PAYLOAD_LENGTH_OFFSET);
    frame->request_id =
        ksd_decode_u64(header + KSD_FRAME_REQUEST_ID_OFFSET);
    if (memcmp(frame->magic, magic, sizeof(frame->magic)) != 0
        || frame->major != major || frame->minor != minor
        || frame->payload_length > maximum_payload
        || (public_rules && !valid_public_header(frame))) {
        errno = EPROTO;
        return -1;
    }
    if (frame->payload_length == 0u)
        return 1;
    frame->payload = malloc(frame->payload_length);
    if (frame->payload == NULL)
        return -1;
    if (read_until(descriptor, frame->payload, frame->payload_length,
                   deadline) != (ssize_t)frame->payload_length) {
        ksd_frame_clear(frame);
        return -1;
    }
    return 1;
}

bool ksd_frame_write(int descriptor, const ksd_frame *frame)
{
    uint8_t header[KSD_FRAME_HEADER_SIZE] = { 0 };

    if (descriptor < 0 || frame == NULL
        || (frame->payload_length != 0u && frame->payload == NULL))
        return false;
    memcpy(header + KSD_FRAME_MAGIC_OFFSET, frame->magic, sizeof(frame->magic));
    ksd_encode_u16(header + KSD_FRAME_MAJOR_OFFSET, frame->major);
    ksd_encode_u16(header + KSD_FRAME_MINOR_OFFSET, frame->minor);
    ksd_encode_u16(header + KSD_FRAME_OPCODE_OFFSET, frame->opcode);
    ksd_encode_u16(header + KSD_FRAME_FLAGS_OFFSET, frame->flags);
    ksd_encode_u32(header + KSD_FRAME_PAYLOAD_LENGTH_OFFSET,
                   frame->payload_length);
    ksd_encode_u64(header + KSD_FRAME_REQUEST_ID_OFFSET, frame->request_id);
    uint64_t deadline = socket_deadline(descriptor, SO_SNDTIMEO);
    return write_until(descriptor, header, sizeof(header), deadline)
        && (frame->payload_length == 0u
            || write_until(descriptor, frame->payload,
                           frame->payload_length, deadline));
}

bool ksd_frame_pack(const ksd_frame *frame, ksd_buffer *buffer)
{
    uint8_t header[KSD_FRAME_HEADER_SIZE] = { 0 };

    if (frame == NULL || buffer == NULL
        || (frame->payload_length != 0u && frame->payload == NULL))
        return false;
    memcpy(header + KSD_FRAME_MAGIC_OFFSET, frame->magic, sizeof(frame->magic));
    ksd_encode_u16(header + KSD_FRAME_MAJOR_OFFSET, frame->major);
    ksd_encode_u16(header + KSD_FRAME_MINOR_OFFSET, frame->minor);
    ksd_encode_u16(header + KSD_FRAME_OPCODE_OFFSET, frame->opcode);
    ksd_encode_u16(header + KSD_FRAME_FLAGS_OFFSET, frame->flags);
    ksd_encode_u32(header + KSD_FRAME_PAYLOAD_LENGTH_OFFSET,
                   frame->payload_length);
    ksd_encode_u64(header + KSD_FRAME_REQUEST_ID_OFFSET, frame->request_id);
    return ksd_buffer_bytes(buffer, header, sizeof(header))
        && ksd_buffer_bytes(buffer, frame->payload, frame->payload_length);
}

bool ksd_frame_unpack(const void *data, size_t length,
                      const uint8_t magic[4], uint16_t major, uint16_t minor,
                      uint32_t maximum_payload, bool public_rules,
                      ksd_frame *frame)
{
    const uint8_t *bytes = data;
    if (data == NULL || magic == NULL || frame == NULL
        || length < KSD_FRAME_HEADER_SIZE)
        return false;
    memset(frame, 0, sizeof(*frame));
    memcpy(frame->magic, bytes + KSD_FRAME_MAGIC_OFFSET, sizeof(frame->magic));
    frame->major = ksd_decode_u16(bytes + KSD_FRAME_MAJOR_OFFSET);
    frame->minor = ksd_decode_u16(bytes + KSD_FRAME_MINOR_OFFSET);
    frame->opcode = ksd_decode_u16(bytes + KSD_FRAME_OPCODE_OFFSET);
    frame->flags = ksd_decode_u16(bytes + KSD_FRAME_FLAGS_OFFSET);
    frame->payload_length =
        ksd_decode_u32(bytes + KSD_FRAME_PAYLOAD_LENGTH_OFFSET);
    frame->request_id =
        ksd_decode_u64(bytes + KSD_FRAME_REQUEST_ID_OFFSET);
    if (memcmp(frame->magic, magic, sizeof(frame->magic)) != 0
        || frame->major != major || frame->minor != minor
        || frame->payload_length > maximum_payload
        || length != KSD_FRAME_HEADER_SIZE + (size_t)frame->payload_length
        || (public_rules && !valid_public_header(frame)))
        return false;
    if (frame->payload_length == 0u)
        return true;
    frame->payload = malloc(frame->payload_length);
    if (frame->payload == NULL)
        return false;
    memcpy(frame->payload, bytes + KSD_FRAME_HEADER_SIZE,
           frame->payload_length);
    return true;
}

void ksd_frame_clear(ksd_frame *frame)
{
    if (frame == NULL)
        return;
    free(frame->payload);
    memset(frame, 0, sizeof(*frame));
}

bool ksd_frame_is_request(const ksd_frame *frame)
{
    return frame != NULL
        && (frame->flags & (KSD_FLAG_RESPONSE | KSD_FLAG_EVENT)) == 0u;
}

void ksd_cursor_init(ksd_cursor *cursor, const void *data, size_t length)
{
    cursor->data = data;
    cursor->length = length;
    cursor->offset = 0u;
}

bool ksd_cursor_bytes(ksd_cursor *cursor, size_t length, const uint8_t **value)
{
    if (cursor == NULL || value == NULL || length > cursor->length
        || cursor->offset > cursor->length - length)
        return false;
    *value = cursor->data + cursor->offset;
    cursor->offset += length;
    return true;
}

bool ksd_cursor_u16(ksd_cursor *cursor, uint16_t *value)
{
    const uint8_t *data;
    if (value == NULL || !ksd_cursor_bytes(cursor, 2u, &data))
        return false;
    *value = ksd_decode_u16(data);
    return true;
}

bool ksd_cursor_u32(ksd_cursor *cursor, uint32_t *value)
{
    const uint8_t *data;
    if (value == NULL || !ksd_cursor_bytes(cursor, 4u, &data))
        return false;
    *value = ksd_decode_u32(data);
    return true;
}

bool ksd_cursor_i32(ksd_cursor *cursor, int32_t *value)
{
    uint32_t encoded;
    if (value == NULL || !ksd_cursor_u32(cursor, &encoded))
        return false;
    *value = (int32_t)encoded;
    return true;
}

bool ksd_cursor_u64(ksd_cursor *cursor, uint64_t *value)
{
    const uint8_t *data;
    if (value == NULL || !ksd_cursor_bytes(cursor, 8u, &data))
        return false;
    *value = ksd_decode_u64(data);
    return true;
}

bool ksd_cursor_finished(const ksd_cursor *cursor)
{
    return cursor != NULL && cursor->offset == cursor->length;
}

void ksd_buffer_init(ksd_buffer *buffer, size_t maximum)
{
    memset(buffer, 0, sizeof(*buffer));
    buffer->maximum = maximum;
}

void ksd_buffer_clear(ksd_buffer *buffer)
{
    if (buffer == NULL)
        return;
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static bool buffer_reserve(ksd_buffer *buffer, size_t extra)
{
    if (buffer == NULL || extra > buffer->maximum
        || buffer->length > buffer->maximum - extra)
        return false;
    size_t needed = buffer->length + extra;
    if (needed <= buffer->capacity)
        return true;
    size_t capacity = buffer->capacity == 0u ? 64u : buffer->capacity;
    while (capacity < needed) {
        if (capacity > buffer->maximum / 2u) {
            capacity = buffer->maximum;
            break;
        }
        capacity *= 2u;
    }
    uint8_t *replacement = realloc(buffer->data, capacity);
    if (replacement == NULL)
        return false;
    buffer->data = replacement;
    buffer->capacity = capacity;
    return true;
}

bool ksd_buffer_bytes(ksd_buffer *buffer, const void *data, size_t length)
{
    if ((length != 0u && data == NULL) || !buffer_reserve(buffer, length))
        return false;
    if (length != 0u)
        memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    return true;
}

bool ksd_buffer_u16(ksd_buffer *buffer, uint16_t value)
{
    uint8_t encoded[2];
    ksd_encode_u16(encoded, value);
    return ksd_buffer_bytes(buffer, encoded, sizeof(encoded));
}

bool ksd_buffer_u32(ksd_buffer *buffer, uint32_t value)
{
    uint8_t encoded[4];
    ksd_encode_u32(encoded, value);
    return ksd_buffer_bytes(buffer, encoded, sizeof(encoded));
}

bool ksd_buffer_u64(ksd_buffer *buffer, uint64_t value)
{
    uint8_t encoded[8];
    ksd_encode_u64(encoded, value);
    return ksd_buffer_bytes(buffer, encoded, sizeof(encoded));
}

bool ksd_utf8_valid(const uint8_t *data, size_t length, bool allow_nul)
{
    size_t index = 0u;
    if (length != 0u && data == NULL)
        return false;
    while (index < length) {
        uint8_t first = data[index++];
        if (first < 0x80u) {
            if (first == 0u && !allow_nul)
                return false;
            continue;
        }
        uint32_t value;
        size_t continuation;
        uint32_t minimum;
        if (first >= 0xc2u && first <= 0xdfu) {
            value = first & 0x1fu;
            continuation = 1u;
            minimum = 0x80u;
        } else if (first >= 0xe0u && first <= 0xefu) {
            value = first & 0x0fu;
            continuation = 2u;
            minimum = 0x800u;
        } else if (first >= 0xf0u && first <= 0xf4u) {
            value = first & 0x07u;
            continuation = 3u;
            minimum = 0x10000u;
        } else {
            return false;
        }
        if (continuation > length - index)
            return false;
        for (size_t offset = 0u; offset < continuation; offset++) {
            uint8_t next = data[index++];
            if ((next & 0xc0u) != 0x80u)
                return false;
            value = (value << 6u) | (uint32_t)(next & 0x3fu);
        }
        if (value < minimum || value > 0x10ffffu
            || (value >= 0xd800u && value <= 0xdfffu))
            return false;
    }
    return true;
}
