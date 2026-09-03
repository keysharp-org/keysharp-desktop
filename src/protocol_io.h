#ifndef KEYSHARP_DESKTOP_PROTOCOL_IO_H
#define KEYSHARP_DESKTOP_PROTOCOL_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ksd_frame {
    uint8_t magic[4];
    uint16_t major;
    uint16_t minor;
    uint16_t opcode;
    uint16_t flags;
    uint32_t payload_length;
    uint64_t request_id;
    uint8_t *payload;
} ksd_frame;

typedef struct ksd_cursor {
    const uint8_t *data;
    size_t length;
    size_t offset;
} ksd_cursor;

typedef struct ksd_buffer {
    uint8_t *data;
    size_t length;
    size_t capacity;
    size_t maximum;
} ksd_buffer;

typedef enum ksd_assembly_result {
    KSD_ASSEMBLY_INVALID = 0,
    KSD_ASSEMBLY_PENDING = 1,
    KSD_ASSEMBLY_COMPLETE = 2,
} ksd_assembly_result;

typedef struct ksd_request_assembly {
    ksd_buffer payload;
    uint64_t request_id;
    uint16_t opcode;
    bool active;
    bool complete;
} ksd_request_assembly;

uint16_t ksd_decode_u16(const uint8_t *data);
uint32_t ksd_decode_u32(const uint8_t *data);
uint64_t ksd_decode_u64(const uint8_t *data);
void ksd_encode_u16(uint8_t *data, uint16_t value);
void ksd_encode_u32(uint8_t *data, uint32_t value);
void ksd_encode_u64(uint8_t *data, uint64_t value);

int ksd_frame_read(int descriptor, const uint8_t magic[4],
                   uint16_t major, uint16_t minor, uint32_t maximum_payload,
                   bool public_rules, ksd_frame *frame);
bool ksd_frame_write(int descriptor, const ksd_frame *frame);
void ksd_frame_clear(ksd_frame *frame);
bool ksd_frame_is_request(const ksd_frame *frame);
bool ksd_frame_pack(const ksd_frame *frame, ksd_buffer *buffer);
bool ksd_frame_unpack(const void *data, size_t length,
                      const uint8_t magic[4], uint16_t major, uint16_t minor,
                      uint32_t maximum_payload, bool public_rules,
                      ksd_frame *frame);

void ksd_cursor_init(ksd_cursor *cursor, const void *data, size_t length);
bool ksd_cursor_u16(ksd_cursor *cursor, uint16_t *value);
bool ksd_cursor_u32(ksd_cursor *cursor, uint32_t *value);
bool ksd_cursor_i32(ksd_cursor *cursor, int32_t *value);
bool ksd_cursor_u64(ksd_cursor *cursor, uint64_t *value);
bool ksd_cursor_bytes(ksd_cursor *cursor, size_t length, const uint8_t **value);
bool ksd_cursor_finished(const ksd_cursor *cursor);

void ksd_buffer_init(ksd_buffer *buffer, size_t maximum);
void ksd_buffer_clear(ksd_buffer *buffer);
bool ksd_buffer_u16(ksd_buffer *buffer, uint16_t value);
bool ksd_buffer_u32(ksd_buffer *buffer, uint32_t value);
bool ksd_buffer_u64(ksd_buffer *buffer, uint64_t value);
bool ksd_buffer_bytes(ksd_buffer *buffer, const void *data, size_t length);

/*
 * Reassembly of a chunked request. Every frame of a sequence carries one
 * opcode and one nonzero request id; each frame flagged KSD_FLAG_MORE is
 * exactly KSD_MAX_REQUEST_PAYLOAD bytes and the unflagged frame that ends the
 * sequence is one to KSD_MAX_REQUEST_PAYLOAD bytes. Anything else, including
 * a frame for another exchange, is KSD_ASSEMBLY_INVALID and the caller must
 * abandon the connection. ksd_request_assembly_take moves the reassembled
 * payload into a frame the caller clears with ksd_frame_clear.
 */
void ksd_request_assembly_init(ksd_request_assembly *assembly);
void ksd_request_assembly_clear(ksd_request_assembly *assembly);
bool ksd_request_assembly_active(const ksd_request_assembly *assembly);
ksd_assembly_result ksd_request_assembly_accept(
    ksd_request_assembly *assembly, const ksd_frame *frame);
bool ksd_request_assembly_take(ksd_request_assembly *assembly,
                               ksd_frame *frame);

bool ksd_utf8_valid(const uint8_t *data, size_t length, bool allow_nul);

#endif
