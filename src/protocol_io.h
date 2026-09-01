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

bool ksd_utf8_valid(const uint8_t *data, size_t length, bool allow_nul);

#endif
