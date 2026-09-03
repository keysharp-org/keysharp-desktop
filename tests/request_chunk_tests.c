#include "protocol.h"
#include "operation_scope.h"
#include "protocol_io.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TEST_CHUNK_OPCODE KSD_OP_CLIPBOARD_SET_CONTENT
#define TEST_CHUNK_REQUEST_ID 7u

static uint8_t block[KSD_MAX_REQUEST_PAYLOAD];

static ksd_frame chunk_frame(uint16_t opcode, uint16_t flags,
                             uint64_t request_id, uint32_t payload_length)
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
        .payload_length = payload_length,
        .request_id = request_id,
        .payload = payload_length == 0u ? NULL : block,
    };
    return value;
}

static ksd_frame full_chunk(void)
{
    return chunk_frame(TEST_CHUNK_OPCODE, KSD_FLAG_MORE,
                       TEST_CHUNK_REQUEST_ID, KSD_MAX_REQUEST_PAYLOAD);
}

static void reject_first(const ksd_frame *first)
{
    ksd_request_assembly assembly;
    ksd_request_assembly_init(&assembly);
    assert(ksd_request_assembly_accept(&assembly, first)
           == KSD_ASSEMBLY_INVALID);
    assert(!ksd_request_assembly_active(&assembly));
    ksd_request_assembly_clear(&assembly);
}

static void reject_second(const ksd_frame *second)
{
    ksd_request_assembly assembly;
    ksd_frame first = full_chunk();
    ksd_frame taken;
    ksd_request_assembly_init(&assembly);
    assert(ksd_request_assembly_accept(&assembly, &first)
           == KSD_ASSEMBLY_PENDING);
    assert(ksd_request_assembly_active(&assembly));
    assert(!ksd_request_assembly_take(&assembly, &taken));
    assert(ksd_request_assembly_accept(&assembly, second)
           == KSD_ASSEMBLY_INVALID);
    assert(!ksd_request_assembly_take(&assembly, &taken));
    ksd_request_assembly_clear(&assembly);
    assert(!ksd_request_assembly_active(&assembly));
}

int main(void)
{
    assert(!ksd_request_chunk_admissible(KSD_OP_HELLO, 0u, 1u));
    assert(!ksd_request_chunk_admissible(KSD_OP_AUTHORIZE, 0u, 1u));
    assert(!ksd_request_chunk_admissible(KSD_OP_PING, 0u, 0u));
    assert(!ksd_request_chunk_admissible(KSD_OP_CLIPBOARD_TEXT, 0u, 1u));
    assert(!ksd_request_chunk_admissible(KSD_OP_CLIPBOARD_CONTENT, 0u, 1u));
    assert(!ksd_request_chunk_admissible(TEST_CHUNK_OPCODE, 0u, 0u));
    assert(!ksd_request_chunk_admissible(TEST_CHUNK_OPCODE,
                                         KSD_FLAG_RESPONSE, 1u));
    assert(!ksd_request_chunk_admissible(TEST_CHUNK_OPCODE,
                                         KSD_FLAG_EVENT, 1u));
    assert(ksd_request_chunk_admissible(TEST_CHUNK_OPCODE, 0u, 1u));
    assert(ksd_request_chunk_admissible(TEST_CHUNK_OPCODE,
                                        KSD_FLAG_MORE, 1u));
    for (uint32_t opcode = 0u; opcode <= 0xFFFFu; opcode++)
        assert(ksd_request_chunk_admissible((uint16_t)opcode, 0u, 1u)
               == (opcode == TEST_CHUNK_OPCODE));
    ksd_request_assembly assembly;
    ksd_frame taken;

    for (size_t index = 0u; index < sizeof(block); index++)
        block[index] = (uint8_t)(index & 0xffu);

    assert(KSD_MAX_REQUEST_TOTAL_PAYLOAD
           == 1024u * KSD_MAX_REQUEST_PAYLOAD);

    ksd_frame unchunked = chunk_frame(TEST_CHUNK_OPCODE, 0u,
        TEST_CHUNK_REQUEST_ID, 16u);
    ksd_frame anonymous = chunk_frame(TEST_CHUNK_OPCODE, KSD_FLAG_MORE, 0u,
        KSD_MAX_REQUEST_PAYLOAD);
    ksd_frame short_chunk = chunk_frame(TEST_CHUNK_OPCODE, KSD_FLAG_MORE,
        TEST_CHUNK_REQUEST_ID, KSD_MAX_REQUEST_PAYLOAD - 1u);
    ksd_frame empty_chunk = chunk_frame(TEST_CHUNK_OPCODE, KSD_FLAG_MORE,
        TEST_CHUNK_REQUEST_ID, 0u);
    ksd_frame empty_terminator = chunk_frame(TEST_CHUNK_OPCODE, 0u,
        TEST_CHUNK_REQUEST_ID, 0u);
    ksd_frame response_chunk = chunk_frame(TEST_CHUNK_OPCODE,
        (uint16_t)(KSD_FLAG_MORE | KSD_FLAG_RESPONSE),
        TEST_CHUNK_REQUEST_ID, KSD_MAX_REQUEST_PAYLOAD);
    ksd_frame event_chunk = chunk_frame(TEST_CHUNK_OPCODE, KSD_FLAG_EVENT,
        TEST_CHUNK_REQUEST_ID, KSD_MAX_REQUEST_PAYLOAD);
    ksd_frame other_opcode = chunk_frame(KSD_OP_CLIPBOARD_CONTENT, 0u,
        TEST_CHUNK_REQUEST_ID, 4u);
    ksd_frame other_request = chunk_frame(TEST_CHUNK_OPCODE, 0u,
        TEST_CHUNK_REQUEST_ID + 1u, 4u);
    ksd_frame anonymous_terminator = chunk_frame(TEST_CHUNK_OPCODE, 0u,
        0u, 4u);

    reject_first(&unchunked);
    reject_first(&anonymous);
    reject_first(&short_chunk);
    reject_first(&empty_chunk);
    reject_first(&empty_terminator);
    reject_first(&response_chunk);
    reject_first(&event_chunk);

    reject_second(&short_chunk);
    reject_second(&empty_chunk);
    reject_second(&empty_terminator);
    reject_second(&response_chunk);
    reject_second(&event_chunk);
    reject_second(&other_opcode);
    reject_second(&other_request);
    reject_second(&anonymous_terminator);

    ksd_frame first = full_chunk();
    ksd_frame terminator = chunk_frame(TEST_CHUNK_OPCODE, 0u,
        TEST_CHUNK_REQUEST_ID, 5u);
    ksd_frame full_terminator = chunk_frame(TEST_CHUNK_OPCODE, 0u,
        TEST_CHUNK_REQUEST_ID, KSD_MAX_REQUEST_PAYLOAD);

    ksd_request_assembly_init(&assembly);
    assert(ksd_request_assembly_accept(&assembly, &first)
           == KSD_ASSEMBLY_PENDING);
    assert(ksd_request_assembly_accept(&assembly, &terminator)
           == KSD_ASSEMBLY_COMPLETE);
    assert(ksd_request_assembly_take(&assembly, &taken));
    assert(!ksd_request_assembly_active(&assembly));
    assert(taken.opcode == TEST_CHUNK_OPCODE);
    assert(taken.request_id == TEST_CHUNK_REQUEST_ID);
    assert(taken.flags == 0u);
    assert(ksd_frame_is_request(&taken));
    assert(taken.payload_length == KSD_MAX_REQUEST_PAYLOAD + 5u);
    assert(memcmp(taken.payload, block, KSD_MAX_REQUEST_PAYLOAD) == 0);
    assert(memcmp(taken.payload + KSD_MAX_REQUEST_PAYLOAD, block, 5u) == 0);
    ksd_frame_clear(&taken);
    assert(!ksd_request_assembly_take(&assembly, &taken));

    for (uint32_t sent = KSD_MAX_REQUEST_PAYLOAD;
         sent < KSD_MAX_REQUEST_TOTAL_PAYLOAD;
         sent += KSD_MAX_REQUEST_PAYLOAD)
        assert(ksd_request_assembly_accept(&assembly, &first)
               == KSD_ASSEMBLY_PENDING);
    assert(ksd_request_assembly_accept(&assembly, &full_terminator)
           == KSD_ASSEMBLY_COMPLETE);
    assert(ksd_request_assembly_take(&assembly, &taken));
    assert(taken.payload_length == KSD_MAX_REQUEST_TOTAL_PAYLOAD);
    ksd_frame_clear(&taken);

    for (uint32_t sent = 0u; sent < KSD_MAX_REQUEST_TOTAL_PAYLOAD;
         sent += KSD_MAX_REQUEST_PAYLOAD)
        assert(ksd_request_assembly_accept(&assembly, &first)
               == KSD_ASSEMBLY_PENDING);
    assert(ksd_request_assembly_accept(&assembly, &terminator)
           == KSD_ASSEMBLY_INVALID);
    assert(ksd_request_assembly_accept(&assembly, &first)
           == KSD_ASSEMBLY_INVALID);
    assert(ksd_request_assembly_active(&assembly));
    assert(!ksd_request_assembly_take(&assembly, &taken));
    ksd_request_assembly_clear(&assembly);
    assert(!ksd_request_assembly_active(&assembly));
    return 0;
}
