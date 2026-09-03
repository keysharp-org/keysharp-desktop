#include "protocol.h"
#include "protocol_io.h"

#include <assert.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

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

static const uint8_t wire_magic[4] = {
    KSD_FRAME_MAGIC_0, KSD_FRAME_MAGIC_1,
    KSD_FRAME_MAGIC_2, KSD_FRAME_MAGIC_3,
};

static int sealed_memfd(const char *contents, size_t length)
{
    int seals = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
    int descriptor = memfd_create("keysharp-desktop-test",
                                  MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (descriptor < 0)
        return -1;
    if (write(descriptor, contents, length) != (ssize_t)length
        || fcntl(descriptor, F_ADD_SEALS, seals) != 0) {
        close(descriptor);
        return -1;
    }
    return descriptor;
}

static void check_frame_fd_round_trip(void)
{
    static const char pixels[] = "not really a png";
    static const uint8_t payload[] = { 1u, 2u, 3u, 4u };
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);

    ksd_frame sent = frame(KSD_OP_CAPTURE_AREA, KSD_FLAG_RESPONSE, 7u);
    sent.payload = (uint8_t *)payload;
    sent.payload_length = (uint32_t)sizeof(payload);

    int passed = sealed_memfd(pixels, sizeof(pixels));
    assert(passed >= 0);
    assert(ksd_frame_write_fd(pair[0], &sent, passed));
    assert(close(passed) == 0);

    ksd_frame received;
    int got = -1;
    assert(ksd_frame_read_fd(pair[1], wire_magic, KSD_PROTOCOL_MAJOR,
                             KSD_PROTOCOL_MINOR, KSD_MAX_REQUEST_PAYLOAD,
                             false, &received, &got) == 1);
    assert(received.opcode == KSD_OP_CAPTURE_AREA);
    assert(received.request_id == 7u);
    assert(received.payload_length == sizeof(payload));
    assert(memcmp(received.payload, payload, sizeof(payload)) == 0);

    /* The descriptor must arrive sealed against every kind of change, or a
     * peer could rewrite the pixels after their length has been agreed. */
    assert(got >= 0);
    int seals = fcntl(got, F_GET_SEALS);
    assert((seals & F_SEAL_WRITE) != 0);
    assert((seals & F_SEAL_SHRINK) != 0);
    assert((seals & F_SEAL_GROW) != 0);
    char echoed[sizeof(pixels)] = { 0 };
    assert(pread(got, echoed, sizeof(echoed), 0) == (ssize_t)sizeof(echoed));
    assert(memcmp(echoed, pixels, sizeof(pixels)) == 0);
    ksd_frame_clear(&received);
    assert(close(got) == 0);

    /* A frame written without a descriptor must still read back through the
     * descriptor-aware path, because a reader cannot know in advance which
     * form a response will take. */
    ksd_frame plain = frame(KSD_OP_PING, KSD_FLAG_RESPONSE, 9u);
    assert(ksd_frame_write(pair[0], &plain));
    int none = 0;
    assert(ksd_frame_read_fd(pair[1], wire_magic, KSD_PROTOCOL_MAJOR,
                             KSD_PROTOCOL_MINOR, KSD_MAX_REQUEST_PAYLOAD,
                             false, &received, &none) == 1);
    assert(none == -1);
    assert(received.opcode == KSD_OP_PING);
    ksd_frame_clear(&received);

    assert(close(pair[0]) == 0);
    assert(close(pair[1]) == 0);
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
    assert(KSD_MAX_REQUEST_TOTAL_PAYLOAD
           == 1024u * KSD_MAX_REQUEST_PAYLOAD);
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

    ksd_frame chunked = frame(KSD_OP_CLIPBOARD_TEXT, KSD_FLAG_MORE, 4u);
    assert(packed_frame_is_valid(&chunked));
    chunked.request_id = 0u;
    assert(!packed_frame_is_valid(&chunked));
    chunked.flags = (uint16_t)(KSD_FLAG_MORE | KSD_FLAG_EVENT);
    assert(!packed_frame_is_valid(&chunked));
    chunked.flags = (uint16_t)(KSD_FLAG_MORE | KSD_FLAG_RESPONSE);
    assert(!packed_frame_is_valid(&chunked));
    ksd_frame chunked_ping = frame(KSD_OP_PING, KSD_FLAG_MORE, 0u);
    assert(!packed_frame_is_valid(&chunked_ping));

    static const uint8_t valid_utf8[] = { 'a', 0xe2u, 0x82u, 0xacu };
    static const uint8_t overlong[] = { 0xc0u, 0x80u };
    static const uint8_t surrogate[] = { 0xedu, 0xa0u, 0x80u };
    assert(ksd_utf8_valid(valid_utf8, sizeof(valid_utf8), false));
    assert(!ksd_utf8_valid(overlong, sizeof(overlong), false));
    assert(!ksd_utf8_valid(surrogate, sizeof(surrogate), false));
    assert(!ksd_utf8_valid((const uint8_t *)"a\0b", 3u, false));

    check_frame_fd_round_trip();
    return 0;
}
