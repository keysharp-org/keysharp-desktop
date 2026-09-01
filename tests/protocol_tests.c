#include "protocol.h"
#include "protocol_io.h"

#include <assert.h>
#include <string.h>

static ksd_frame frame(uint16_t opcode, uint16_t flags, uint64_t request_id)
{
    ksd_frame value = {
        .magic = {
            KSD_FRAME_MAGIC_0, KSD_FRAME_MAGIC_1,
            KSD_FRAME_MAGIC_2, KSD_FRAME_MAGIC_3,
        },
        .major = KSD_PROTOCOL_MAJOR,
        .minor = KSD_PROTOCOL_MINOR,
        .opcode = opcode,
        .flags = flags,
        .request_id = request_id,
    };
    return value;
}

static bool packed_frame_is_valid(ksd_frame *value)
{
    static const uint8_t magic[4] = {
        KSD_FRAME_MAGIC_0, KSD_FRAME_MAGIC_1,
        KSD_FRAME_MAGIC_2, KSD_FRAME_MAGIC_3,
    };
    ksd_buffer packed;
    ksd_frame decoded;
    ksd_buffer_init(&packed, 1024u);
    assert(ksd_frame_pack(value, &packed));
    bool valid = ksd_frame_unpack(packed.data, packed.length, magic,
        KSD_PROTOCOL_MAJOR, KSD_PROTOCOL_MINOR, 1024u, true, &decoded);
    if (valid)
        ksd_frame_clear(&decoded);
    ksd_buffer_clear(&packed);
    return valid;
}

int main(void)
{
    assert(KSD_PROTOCOL_MAJOR == 2u);
    assert(KSD_PROTOCOL_MINOR == 0u);
    assert(strcmp(KSD_CLIENT_PROTOCOL_NAME, "keysharp-desktop/client") == 0);
    assert(KSD_FRAME_HEADER_SIZE == 24u);
    assert(KSD_FRAME_REQUEST_ID_OFFSET == 16u);
    assert(KSD_OPERATION_CAPTURE_AREA == (UINT64_C(1) << 0u));
    assert(KSD_OPERATION_CLIPBOARD_WATCH == (UINT64_C(1) << 21u));
    assert(KSD_OPERATION_MOUSE_MOVE_ABSOLUTE == (UINT64_C(1) << 22u));
    assert(KSD_OPERATION_MOUSE_SCROLL == (UINT64_C(1) << 25u));
    assert(KSD_OPERATION_CURSOR_POSITION == (UINT64_C(1) << 26u));
    assert(KSD_OPERATION_WORK_AREA == (UINT64_C(1) << 27u));

    ksd_frame request = frame(KSD_OP_HELLO, 0u, 1u);
    assert(packed_frame_is_valid(&request));
    ksd_frame response = frame(KSD_OP_HELLO, KSD_FLAG_RESPONSE, 1u);
    assert(packed_frame_is_valid(&response));
    ksd_frame event = frame(KSD_OP_SESSION_REVOKED, KSD_FLAG_EVENT, 0u);
    assert(packed_frame_is_valid(&event));
    ksd_frame ping = frame(KSD_OP_PING, 0u, 0u);
    assert(packed_frame_is_valid(&ping));

    request.request_id = 0u;
    assert(!packed_frame_is_valid(&request));
    response.request_id = 0u;
    assert(!packed_frame_is_valid(&response));
    event.request_id = 1u;
    assert(!packed_frame_is_valid(&event));
    event.request_id = 0u;
    event.flags = KSD_FLAG_EVENT | KSD_FLAG_RESPONSE;
    assert(!packed_frame_is_valid(&event));
    event.flags = KSD_FLAG_MORE;
    assert(!packed_frame_is_valid(&event));
    event.flags = 0x8000u;
    assert(!packed_frame_is_valid(&event));

    static const uint8_t valid_utf8[] = { 'a', 0xe2u, 0x82u, 0xacu };
    static const uint8_t overlong[] = { 0xc0u, 0x80u };
    static const uint8_t surrogate[] = { 0xedu, 0xa0u, 0x80u };
    assert(ksd_utf8_valid(valid_utf8, sizeof(valid_utf8), false));
    assert(!ksd_utf8_valid(overlong, sizeof(overlong), false));
    assert(!ksd_utf8_valid(surrogate, sizeof(surrogate), false));
    assert(!ksd_utf8_valid((const uint8_t *)"a\0b", 3u, false));
    return 0;
}
