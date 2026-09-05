#include "kwin_bus.h"
#include "kwin_relay.h"
#include "protocol.h"
#include "transport.h"

#include <assert.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <unistd.h>

/* The relay is the one part of the KWin channel where several threads share a
 * socket, so it is the one part where an ordering mistake shows up as a caller
 * receiving somebody else's answer. That cannot be argued -- it has to be run.
 *
 * These stand a fake daemon on the far end of a socketpair and drive the relay
 * from real threads. The fake daemon answers OUT OF ORDER on purpose: in
 * production the whole point of the queue is that a cheap verb need not wait
 * behind an enumeration, so answers genuinely do come back reordered, and a
 * relay that assumed otherwise would work in testing and fail on a desktop. */

#define WORKERS 8u

static void check_request_translation(void)
{
    uint8_t payload[24] = { 0 };
    char text[160];
    uint32_t length = 0u;

    ksd_encode_u32(payload, 1u);
    assert(ksd_kwin_request_text(KSD_OP_WINDOW_LIST, payload, 8u, text,
                                 sizeof(text), &length));
    assert(length == 1u && memcmp(text, "1", length) == 0);

    ksd_encode_u64(payload, UINT64_C(4294967295));
    assert(ksd_kwin_request_text(KSD_OP_WINDOW_FOCUS, payload, 8u, text,
                                 sizeof(text), &length));
    assert(length == 10u && memcmp(text, "4294967295", length) == 0);

    ksd_encode_u64(payload, 17u);
    ksd_encode_u32(payload + 8u, (uint32_t)INT32_MIN);
    ksd_encode_u32(payload + 12u, (uint32_t)-24);
    ksd_encode_u32(payload + 16u, 0u);
    ksd_encode_u32(payload + 20u, 640u);
    assert(ksd_kwin_request_text(KSD_OP_WINDOW_MOVE_RESIZE, payload,
                                 sizeof(payload), text, sizeof(text),
                                 &length));
    assert(length == strlen("17 -2147483648 -24 0 640"));
    assert(memcmp(text, "17 -2147483648 -24 0 640", length) == 0);

    memset(payload, 0, sizeof(payload));
    ksd_encode_u64(payload, 17u);
    ksd_encode_u32(payload + 8u, 255u);
    assert(ksd_kwin_request_text(KSD_OP_WINDOW_SET_OPACITY, payload, 16u,
                                 text, sizeof(text), &length));
    assert(length == 6u && memcmp(text, "17 255", length) == 0);

    ksd_encode_u32(payload + 12u, 1u);
    assert(!ksd_kwin_request_text(KSD_OP_WINDOW_SET_OPACITY, payload, 16u,
                                  text, sizeof(text), &length));
    assert(!ksd_kwin_request_text(KSD_OP_CAPTURE_AREA, payload, 16u, text,
                                  sizeof(text), &length));
}

static void check_response_translation(void)
{
    static const char windows[] = "{\"ok\":true,\"windows\":[]}";
    static const char point[] = "{\"x\":-12,\"y\":34}";
    static const char area[] =
        "{\"x\":-1920,\"y\":0,\"width\":1920,\"height\":1080}";
    ksd_buffer payload;

    ksd_buffer_init(&payload, KSD_MAX_TEXT_BYTES + 4u);
    assert(ksd_kwin_response_payload(KSD_OP_WINDOW_LIST,
        (const uint8_t *)windows, (uint32_t)strlen(windows), &payload));
    assert(payload.length == strlen(windows) + 4u);
    assert(ksd_decode_u32(payload.data) == strlen(windows));
    assert(memcmp(payload.data + 4u, windows, strlen(windows)) == 0);
    ksd_buffer_clear(&payload);

    ksd_buffer_init(&payload, 16u);
    assert(ksd_kwin_response_payload(KSD_OP_CURSOR_POSITION,
        (const uint8_t *)point, (uint32_t)strlen(point), &payload));
    assert(payload.length == 8u);
    assert((int32_t)ksd_decode_u32(payload.data) == -12);
    assert((int32_t)ksd_decode_u32(payload.data + 4u) == 34);
    ksd_buffer_clear(&payload);

    ksd_buffer_init(&payload, 16u);
    assert(ksd_kwin_response_payload(KSD_OP_WORK_AREA,
        (const uint8_t *)area, (uint32_t)strlen(area), &payload));
    assert(payload.length == 16u);
    assert((int32_t)ksd_decode_u32(payload.data) == -1920);
    assert(ksd_decode_u32(payload.data + 8u) == 1920u);
    assert(ksd_decode_u32(payload.data + 12u) == 1080u);
    ksd_buffer_clear(&payload);

    ksd_buffer_init(&payload, 1u);
    assert(ksd_kwin_response_payload(KSD_OP_WINDOW_FOCUS, NULL, 0u,
                                     &payload));
    assert(payload.length == 0u);
    assert(!ksd_kwin_response_payload(KSD_OP_WINDOW_FOCUS,
        (const uint8_t *)"x", 1u, &payload));
    assert(!ksd_kwin_response_payload(KSD_OP_CURSOR_POSITION,
        (const uint8_t *)"{\"x\":1}", 7u, &payload));
    ksd_buffer_clear(&payload);
}

typedef struct fake_daemon {
    int descriptor;
    unsigned answered;
    bool reorder;
} fake_daemon;

/* Reads every request, then answers them all in reverse. */
static void *daemon_thread(void *data)
{
    fake_daemon *daemon = data;
    uint64_t ids[WORKERS];
    uint16_t opcodes[WORKERS];
    unsigned count = 0u;

    while (count < WORKERS) {
        uint8_t header[KSD_FRAME_HEADER_SIZE];
        uint32_t payload_length;

        if (!ksd_read_all(daemon->descriptor, header, sizeof(header)))
            return NULL;
        payload_length =
            ksd_decode_u32(header + KSD_FRAME_PAYLOAD_LENGTH_OFFSET);
        if (payload_length != 0u) {
            uint8_t *body = malloc(payload_length);

            if (body == NULL
                || !ksd_read_all(daemon->descriptor, body, payload_length)) {
                free(body);
                return NULL;
            }
            free(body);
        }
        ids[count] = ksd_decode_u64(header + KSD_FRAME_REQUEST_ID_OFFSET);
        opcodes[count] = ksd_decode_u16(header + KSD_FRAME_OPCODE_OFFSET);
        count++;
    }
    for (unsigned index = 0u; index < count; index++) {
        unsigned pick = daemon->reorder ? count - 1u - index : index;
        ksd_frame frame;
        ksd_buffer payload;
        ksd_buffer packed;
        /* The answer names the opcode it belongs to, so a caller that got
         * somebody else's answer can be told apart from one that got none. */
        uint8_t body[2];

        body[0] = (uint8_t)(opcodes[pick] & 0xffu);
        body[1] = (uint8_t)(opcodes[pick] >> 8);
        ksd_buffer_init(&payload, 32u);
        assert(ksd_buffer_u32(&payload, KSD_STATUS_OK));
        assert(ksd_buffer_u32(&payload, 0u));
        assert(ksd_buffer_bytes(&payload, body, sizeof(body)));

        memset(&frame, 0, sizeof(frame));
        frame.magic[0] = KSD_FRAME_MAGIC_0;
        frame.magic[1] = KSD_FRAME_MAGIC_1;
        frame.magic[2] = KSD_FRAME_MAGIC_2;
        frame.magic[3] = KSD_FRAME_MAGIC_3;
        frame.major = KSD_PROTOCOL_MAJOR;
        frame.minor = KSD_PROTOCOL_MINOR;
        frame.request_id = ids[pick];
        frame.payload = payload.data;
        frame.payload_length = (uint32_t)payload.length;
        ksd_buffer_init(&packed, 128u);
        assert(ksd_frame_pack(&frame, &packed));
        (void)ksd_write_all(daemon->descriptor, packed.data, packed.length);
        ksd_buffer_clear(&packed);
        ksd_buffer_clear(&payload);
        daemon->answered++;
    }
    return NULL;
}

typedef struct caller {
    ksd_kwin_relay *relay;
    uint16_t opcode;
    bool ok;
    uint16_t echoed;
} caller;

static void *caller_thread(void *data)
{
    caller *state = data;
    ksd_frame request;
    ksd_operation_result result;

    memset(&request, 0, sizeof(request));
    request.magic[0] = KSD_FRAME_MAGIC_0;
    request.magic[1] = KSD_FRAME_MAGIC_1;
    request.magic[2] = KSD_FRAME_MAGIC_2;
    request.magic[3] = KSD_FRAME_MAGIC_3;
    request.major = KSD_PROTOCOL_MAJOR;
    request.minor = KSD_PROTOCOL_MINOR;
    request.opcode = state->opcode;

    ksd_result_init(&result);
    state->ok = ksd_kwin_relay_call(state->relay, &request,
                                    (uint64_t)-1, &result)
        && result.status == KSD_STATUS_OK && result.tail_length == 2u;
    if (state->ok)
        state->echoed = (uint16_t)(result.tail[0]
                                   | ((uint16_t)result.tail[1] << 8));
    ksd_result_clear(&result);
    return NULL;
}

/* Eight callers at once, answered in reverse. Each must receive the answer to
 * its OWN request, which is the whole reason responses carry a request id
 * rather than being matched by arrival order. */
static void check_out_of_order_demux(void)
{
    int pair[2];
    fake_daemon daemon;
    pthread_t server;
    pthread_t threads[WORKERS];
    caller callers[WORKERS];
    ksd_kwin_relay *relay;

    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
    daemon.descriptor = pair[1];
    daemon.answered = 0u;
    daemon.reorder = true;
    relay = ksd_kwin_relay_create(pair[0]);
    assert(relay != NULL);
    assert(pthread_create(&server, NULL, daemon_thread, &daemon) == 0);

    for (unsigned index = 0u; index < WORKERS; index++) {
        callers[index].relay = relay;
        /* A distinct opcode per caller, so the answer identifies its owner. */
        callers[index].opcode = (uint16_t)(KSD_OP_WINDOW_FOCUS + index);
        callers[index].ok = false;
        callers[index].echoed = 0u;
        assert(pthread_create(&threads[index], NULL, caller_thread,
                              &callers[index]) == 0);
    }
    for (unsigned index = 0u; index < WORKERS; index++)
        assert(pthread_join(threads[index], NULL) == 0);
    assert(pthread_join(server, NULL) == 0);

    for (unsigned index = 0u; index < WORKERS; index++) {
        assert(callers[index].ok);
        assert(callers[index].echoed == callers[index].opcode);
    }
    assert(daemon.answered == WORKERS);
    ksd_kwin_relay_retire(relay);
    close(pair[1]);
}

/* A daemon that never answers. The caller gives up on its own deadline rather
 * than waiting for ever, and reports TIMEOUT and not BUSY: the request went
 * down the socket and may have run, which is what makes close and move-resize
 * unsafe to retry. */
static void check_deadline_reports_timeout(void)
{
    int pair[2];
    ksd_kwin_relay *relay;
    ksd_frame request;
    ksd_operation_result result;
    struct timespec now;
    uint64_t deadline;

    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
    relay = ksd_kwin_relay_create(pair[0]);
    assert(relay != NULL);

    memset(&request, 0, sizeof(request));
    request.magic[0] = KSD_FRAME_MAGIC_0;
    request.magic[1] = KSD_FRAME_MAGIC_1;
    request.magic[2] = KSD_FRAME_MAGIC_2;
    request.magic[3] = KSD_FRAME_MAGIC_3;
    request.major = KSD_PROTOCOL_MAJOR;
    request.minor = KSD_PROTOCOL_MINOR;
    request.opcode = KSD_OP_WINDOW_FOCUS;

    clock_gettime(CLOCK_MONOTONIC, &now);
    deadline = (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u
        + 200u;
    ksd_result_init(&result);
    assert(ksd_kwin_relay_call(relay, &request, deadline, &result));
    assert(result.status == KSD_STATUS_TIMEOUT);
    ksd_result_clear(&result);

    ksd_kwin_relay_retire(relay);
    close(pair[1]);
}

/* A channel that has gone is UNAVAILABLE rather than a hang or a crash. */
static void check_closed_channel(void)
{
    int pair[2];
    ksd_kwin_relay *relay;
    ksd_frame request;
    ksd_operation_result result;

    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
    relay = ksd_kwin_relay_create(pair[0]);
    assert(relay != NULL);
    /* The far end goes away before anything is sent. */
    close(pair[1]);

    memset(&request, 0, sizeof(request));
    request.magic[0] = KSD_FRAME_MAGIC_0;
    request.magic[1] = KSD_FRAME_MAGIC_1;
    request.magic[2] = KSD_FRAME_MAGIC_2;
    request.magic[3] = KSD_FRAME_MAGIC_3;
    request.major = KSD_PROTOCOL_MAJOR;
    request.minor = KSD_PROTOCOL_MINOR;
    request.opcode = KSD_OP_WINDOW_FOCUS;

    ksd_result_init(&result);
    assert(ksd_kwin_relay_call(relay, &request, (uint64_t)-1, &result));
    assert(result.status != KSD_STATUS_OK);
    assert(ksd_kwin_relay_is_broken(relay));
    ksd_result_clear(&result);
    ksd_kwin_relay_retire(relay);
}

/* A daemon that writes half a header and then stalls. ksd_read_all has no
 * deadline, so before this was bounded the caller holding the reader role
 * blocked in it for ever -- and because it was the reader, every other caller
 * waited on it too. One stalled write would have wedged the backend for that
 * uid permanently and parked a root thread. */
static void check_partial_write_does_not_wedge(void)
{
    int pair[2];
    ksd_kwin_relay *relay;
    ksd_frame request;
    ksd_operation_result result;
    struct timespec now;
    uint64_t deadline;
    uint8_t half[8] = { 0 };

    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
    relay = ksd_kwin_relay_create(pair[0]);
    assert(relay != NULL);

    memset(&request, 0, sizeof(request));
    request.magic[0] = KSD_FRAME_MAGIC_0;
    request.magic[1] = KSD_FRAME_MAGIC_1;
    request.magic[2] = KSD_FRAME_MAGIC_2;
    request.magic[3] = KSD_FRAME_MAGIC_3;
    request.major = KSD_PROTOCOL_MAJOR;
    request.minor = KSD_PROTOCOL_MINOR;
    request.opcode = KSD_OP_WINDOW_FOCUS;

    /* Eight bytes of a header that needs more, and then silence. */
    assert(write(pair[1], half, sizeof(half)) == (ssize_t)sizeof(half));

    clock_gettime(CLOCK_MONOTONIC, &now);
    deadline = (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u
        + 250u;
    ksd_result_init(&result);
    assert(ksd_kwin_relay_call(relay, &request, deadline, &result));
    assert(result.status != KSD_STATUS_OK);
    ksd_result_clear(&result);

    ksd_kwin_relay_retire(relay);
    close(pair[1]);
}

/* A relay retired while a caller is inside a call must not be freed under
 * them. Before it was reference counted, unregister_backend freed it while a
 * call was in flight, and the window was exactly as wide as an operation is
 * slow -- a use-after-free in the root process. */
static void check_retire_during_call_is_safe(void)
{
    int pair[2];
    ksd_kwin_relay *relay;
    ksd_frame request;
    ksd_operation_result result;
    struct timespec now;
    uint64_t deadline;

    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
    relay = ksd_kwin_relay_create(pair[0]);
    assert(relay != NULL);
    /* The caller's reference, as registered_relay takes under the lock. */
    ksd_kwin_relay_acquire(relay);
    /* The registration ends while that reference is held. */
    ksd_kwin_relay_retire(relay);

    memset(&request, 0, sizeof(request));
    request.magic[0] = KSD_FRAME_MAGIC_0;
    request.magic[1] = KSD_FRAME_MAGIC_1;
    request.magic[2] = KSD_FRAME_MAGIC_2;
    request.magic[3] = KSD_FRAME_MAGIC_3;
    request.major = KSD_PROTOCOL_MAJOR;
    request.minor = KSD_PROTOCOL_MINOR;
    request.opcode = KSD_OP_WINDOW_FOCUS;

    clock_gettime(CLOCK_MONOTONIC, &now);
    deadline = (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u
        + 250u;
    ksd_result_init(&result);
    /* Answers, on memory that is still there, and says the channel is gone. */
    assert(ksd_kwin_relay_call(relay, &request, deadline, &result));
    assert(result.status != KSD_STATUS_OK);
    ksd_result_clear(&result);

    /* And the last reference is what actually frees it. */
    ksd_kwin_relay_release(relay);
    close(pair[1]);
}

static void check_capture_descriptor(bool sealed)
{
    int pair[2];
    uint8_t pixels[24] = { 0 };
    uint8_t payload[12] = { 0 };
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
    int descriptor = memfd_create("relay-capture-test",
                                   MFD_CLOEXEC | MFD_ALLOW_SEALING);
    assert(descriptor >= 0);
    assert(ksd_write_all(descriptor, pixels, sizeof(pixels)));
    if (sealed)
        assert(fcntl(descriptor, F_ADD_SEALS,
            F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE) == 0);
    ksd_encode_u32(payload + 8u, sizeof(pixels));
    ksd_frame answer = {
        .magic = { 'K', 'S', 'D', 'P' }, .major = KSD_PROTOCOL_MAJOR,
        .minor = KSD_PROTOCOL_MINOR, .flags = KSD_RELAY_CAPTURE_FD,
        .request_id = 1u, .payload = payload, .payload_length = sizeof(payload),
    };
    ksd_buffer packed;
    ksd_buffer_init(&packed, KSD_FRAME_HEADER_SIZE + sizeof(payload));
    assert(ksd_frame_pack(&answer, &packed));
    assert(ksd_send_with_fd(pair[1], packed.data, packed.length, descriptor));
    close(descriptor);
    ksd_buffer_clear(&packed);
    ksd_kwin_relay *relay = ksd_kwin_relay_create(pair[0]);
    assert(relay != NULL);
    ksd_frame request = {
        .magic = { 'K', 'S', 'D', 'P' }, .major = KSD_PROTOCOL_MAJOR,
        .minor = KSD_PROTOCOL_MINOR, .opcode = KSD_OP_CAPTURE_AREA,
    };
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t deadline = (uint64_t)now.tv_sec * 1000u
        + (uint64_t)now.tv_nsec / 1000000u + 1000u;
    ksd_operation_result result;
    ksd_result_init(&result);
    assert(ksd_kwin_relay_call(relay, &request, deadline, &result));
    if (sealed) {
        assert(result.status == KSD_STATUS_OK && result.payload_fd >= 0);
        assert(result.tail == NULL && result.tail_length == sizeof(pixels));
        assert(pread(result.payload_fd, pixels, sizeof(pixels), 0)
               == (ssize_t)sizeof(pixels));
    } else {
        assert(result.status != KSD_STATUS_OK && result.payload_fd < 0);
    }
    ksd_result_clear(&result);
    ksd_kwin_relay_retire(relay);
    close(pair[1]);
}

static void check_unavailable_operation_keeps_transport(void)
{
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
    ksd_kwin_relay *relay = ksd_kwin_relay_create(pair[0]);
    assert(relay != NULL);
    ksd_frame request = {
        .magic = { KSD_FRAME_MAGIC_0, KSD_FRAME_MAGIC_1,
                   KSD_FRAME_MAGIC_2, KSD_FRAME_MAGIC_3 },
        .major = KSD_PROTOCOL_MAJOR, .minor = KSD_PROTOCOL_MINOR,
        .opcode = KSD_OP_CLIPBOARD_TEXT, .request_id = 1u,
    };
    for (uint64_t id = 1u; id <= 2u; id++) {
        uint8_t body[8] = { 0 };
        ksd_encode_u32(body, id == 1u ? KSD_STATUS_UNAVAILABLE : KSD_STATUS_OK);
        ksd_frame answer = request;
        answer.request_id = id;
        answer.payload = body;
        answer.payload_length = sizeof(body);
        assert(ksd_frame_write(pair[1], &answer));
    }
    for (unsigned index = 0u; index < 2u; index++) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t deadline = (uint64_t)now.tv_sec * 1000u
            + (uint64_t)now.tv_nsec / 1000000u + 1000u;
        ksd_operation_result result;
        ksd_result_init(&result);
        assert(ksd_kwin_relay_call(relay, &request, deadline, &result));
        assert(result.status == (index == 0u
            ? KSD_STATUS_UNAVAILABLE : KSD_STATUS_OK));
        assert(!ksd_kwin_relay_is_broken(relay));
        ksd_result_clear(&result);
    }
    ksd_kwin_relay_retire(relay);
    close(pair[1]);
}

int main(void)
{
    /* A write to a socket whose far end has closed raises SIGPIPE, which would
     * kill the test rather than returning the error the code handles. */
    signal(SIGPIPE, SIG_IGN);
    check_request_translation();
    check_response_translation();
    check_out_of_order_demux();
    check_deadline_reports_timeout();
    check_closed_channel();
    check_unavailable_operation_keeps_transport();
    check_partial_write_does_not_wedge();
    check_retire_during_call_is_safe();
    check_capture_descriptor(true);
    check_capture_descriptor(false);
    return 0;
}
