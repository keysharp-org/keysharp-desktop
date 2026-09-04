/* The persistent display worker's serving loop.
 *
 * A worker that answers one request and exits is easy to check: the answer is
 * either right or it is not. A worker that stays is not, because the thing
 * that makes it worth having -- one display connection serving many requests
 * -- produces answers identical to the one-shot worker's. Every property below
 * is therefore about the loop rather than about the answers: that it keeps
 * going, that it pairs each answer with the request that asked for it, that a
 * bad request does not end it, and that it opens the display once.
 *
 * The loop reads and writes fd 3, so each case hands it one end of a
 * socketpair with every request already queued and the far end closed. It then
 * drains the queue and stops at end-of-file, which is also how the real worker
 * learns the authority has gone.
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "backend.h"
#include "protocol.h"
#include "protocol_io.h"
#include "x11_connect.h"
#include "x11_display.h"

#define WORKER_FD 3

bool ksd_capture_worker_test_serve(uint32_t backend, pid_t session_pid);
extern unsigned ksd_capture_worker_test_opens;

/* Requests are written by hand rather than with the frame packer: the loop
 * reads a header off the wire and trusts nothing about it, so the test has to
 * be able to write headers the packer would refuse -- an over-long payload
 * length above all. */
static void queue_request(int socket_fd, uint16_t opcode, uint64_t request_id,
                          const uint8_t *payload, uint32_t payload_length)
{
    uint8_t header[KSD_FRAME_HEADER_SIZE];

    memset(header, 0, sizeof(header));
    header[0] = KSD_FRAME_MAGIC_0;
    header[1] = KSD_FRAME_MAGIC_1;
    header[2] = KSD_FRAME_MAGIC_2;
    header[3] = KSD_FRAME_MAGIC_3;
    ksd_encode_u16(header + KSD_FRAME_MAJOR_OFFSET, KSD_PROTOCOL_MAJOR);
    ksd_encode_u16(header + KSD_FRAME_MINOR_OFFSET, KSD_PROTOCOL_MINOR);
    ksd_encode_u16(header + KSD_FRAME_OPCODE_OFFSET, opcode);
    ksd_encode_u32(header + KSD_FRAME_PAYLOAD_LENGTH_OFFSET, payload_length);
    ksd_encode_u64(header + KSD_FRAME_REQUEST_ID_OFFSET, request_id);
    assert(write(socket_fd, header, sizeof(header))
           == (ssize_t)sizeof(header));
    if (payload != NULL && payload_length != 0u)
        assert(write(socket_fd, payload, payload_length)
               == (ssize_t)payload_length);
}

/* Reads one answer and reports its status, or false at end of stream. */
static bool take_answer(int socket_fd, uint64_t *request_id, uint32_t *status)
{
    uint8_t header[KSD_FRAME_HEADER_SIZE];
    uint8_t prologue[8];
    uint32_t payload_length;
    ssize_t taken = read(socket_fd, header, sizeof(header));

    if (taken == 0)
        return false;
    /* A worker that refuses a request does not drain the body it refused, and
     * a UNIX socket closed with bytes still unread reports a reset to the
     * other side rather than an orderly end. Both mean the same thing here:
     * nothing came back. */
    if (taken < 0 && errno == ECONNRESET)
        return false;
    assert(taken == (ssize_t)sizeof(header));
    assert(header[0] == KSD_FRAME_MAGIC_0 && header[1] == KSD_FRAME_MAGIC_1
           && header[2] == KSD_FRAME_MAGIC_2
           && header[3] == KSD_FRAME_MAGIC_3);
    payload_length = ksd_decode_u32(header + KSD_FRAME_PAYLOAD_LENGTH_OFFSET);
    /* Every answer carries the status and detail prologue, whatever else it
     * carries: frames have no status field of their own. */
    assert(payload_length >= sizeof(prologue));
    assert(read(socket_fd, prologue, sizeof(prologue))
           == (ssize_t)sizeof(prologue));
    if (payload_length > sizeof(prologue)) {
        uint32_t remaining = payload_length - (uint32_t)sizeof(prologue);
        uint8_t *tail = malloc(remaining);

        assert(tail != NULL);
        assert(read(socket_fd, tail, remaining) == (ssize_t)remaining);
        free(tail);
    }
    *request_id = ksd_decode_u64(header + KSD_FRAME_REQUEST_ID_OFFSET);
    *status = ksd_decode_u32(prologue);
    return true;
}

/* Both ends are moved clear of the worker fd before anything else happens.
 * Each case closes fd 3 when it finishes, so the next socketpair is handed it
 * straight back -- as the FAR end, whereupon dup2 of the near end onto fd 3
 * closes the far end and leaves both variables naming the same side of the
 * socket. The failure that produces is a short read a long way from its cause.
 */
static int clear_of_worker_fd(int descriptor)
{
    int moved;

    if (descriptor > WORKER_FD)
        return descriptor;
    moved = fcntl(descriptor, F_DUPFD, WORKER_FD + 1);
    assert(moved > WORKER_FD);
    close(descriptor);
    return moved;
}

static void make_pair(int *far_end, int *near_end)
{
    int pair[2];

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    *far_end = clear_of_worker_fd(pair[0]);
    *near_end = clear_of_worker_fd(pair[1]);
}

/* Runs the loop against whatever the case has queued, with the writing end
 * already closed. Returns the loop's own verdict; the caller reads the answers
 * off the far end afterwards. */
static bool serve_queued(int far_end, int near_end, uint32_t backend,
                         pid_t session_pid)
{
    bool ok;

    assert(dup2(near_end, WORKER_FD) == WORKER_FD);
    close(near_end);
    /* The loop stops at end-of-file, so the queue has to be complete before it
     * starts. Everything queued fits the socket buffer many times over. */
    assert(shutdown(far_end, SHUT_WR) == 0);
    ok = ksd_capture_worker_test_serve(backend, session_pid);
    close(WORKER_FD);
    return ok;
}

/* Many requests, one display. This is the whole reason the persistent worker
 * exists, and it is the one property the answers cannot show. */
static void check_connection_is_reused(void)
{
    int far_end;
    int near_end;
    uint64_t id;
    uint32_t status;
    unsigned answers = 0u;

    make_pair(&far_end, &near_end);
    queue_request(far_end, KSD_OP_CURSOR_POSITION, 11u, NULL, 0u);
    queue_request(far_end, KSD_OP_WORK_AREA, 22u, NULL, 0u);
    queue_request(far_end, KSD_OP_WINDOW_HANDLES, 33u, NULL, 0u);

    ksd_capture_worker_test_opens = 0u;
    assert(serve_queued(far_end, near_end, KSD_BACKEND_X11, getpid()));

    /* The display belongs to this very process, so the first two have to
     * genuinely succeed: answers alone would prove the loop replies, not that
     * it works. */
    assert(take_answer(far_end, &id, &status));
    assert(id == 11u && status == KSD_STATUS_OK);
    answers++;
    assert(take_answer(far_end, &id, &status));
    assert(id == 22u && status == KSD_STATUS_OK);
    answers++;
    /* The third cannot be served: the X server this runs against has no window
     * manager, so there is no _NET_CLIENT_LIST to read. That is the valuable
     * case rather than an awkward one -- an operation the display cannot serve
     * must not be read as a display that has gone, and the count below is what
     * says the loop kept the connection instead of dropping it to reconnect. */
    assert(take_answer(far_end, &id, &status));
    assert(id == 33u && status == KSD_STATUS_UNAVAILABLE);
    answers++;
    assert(!take_answer(far_end, &id, &status));
    assert(answers == 3u);
    assert(ksd_capture_worker_test_opens == 1u);
    close(far_end);
}

/* A request the backend refuses is answered, and the loop carries on. A worker
 * that exited here would take the session's display connection down every time
 * a client sent something malformed. */
static void check_bad_request_does_not_end_the_loop(void)
{
    int far_end;
    int near_end;
    uint64_t id;
    uint32_t status;
    /* CURSOR_POSITION takes no payload at all, so this is refused by the
     * backend's own validation rather than by the frame layer. */
    uint8_t junk[4] = { 1u, 0u, 0u, 0u };

    make_pair(&far_end, &near_end);
    queue_request(far_end, KSD_OP_CURSOR_POSITION, 1u, junk, sizeof(junk));
    queue_request(far_end, 0xfffeu, 2u, NULL, 0u);
    queue_request(far_end, KSD_OP_CURSOR_POSITION, 3u, NULL, 0u);

    ksd_capture_worker_test_opens = 0u;
    assert(serve_queued(far_end, near_end, KSD_BACKEND_X11, getpid()));

    assert(take_answer(far_end, &id, &status));
    assert(id == 1u && status == KSD_STATUS_INVALID_REQUEST);
    assert(take_answer(far_end, &id, &status));
    assert(id == 2u && status != KSD_STATUS_OK);
    assert(take_answer(far_end, &id, &status));
    assert(id == 3u && status == KSD_STATUS_OK);
    assert(!take_answer(far_end, &id, &status));
    /* The two refusals cost no reconnection, and did not stop the third from
     * being served on the connection already open. */
    assert(ksd_capture_worker_test_opens == 1u);
    close(far_end);
}

/* A payload larger than the worker will ever be sent ends the loop with no
 * answer, rather than being trusted into a malloc of the sender's choosing.
 *
 * The body is genuinely sent, all of it. Declaring a huge length and sending
 * nothing would be refused whether the cap existed or not -- the read would
 * simply hit end-of-file -- so the cap would go untested and its removal would
 * not show up here. */
static void check_oversized_payload_is_refused(void)
{
    int far_end;
    int near_end;
    uint64_t id;
    uint32_t status;
    uint8_t oversized[4096];

    memset(oversized, 0, sizeof(oversized));
    make_pair(&far_end, &near_end);
    queue_request(far_end, KSD_OP_CURSOR_POSITION, 7u, oversized,
                  (uint32_t)sizeof(oversized));
    assert(!serve_queued(far_end, near_end, KSD_BACKEND_X11, getpid()));
    assert(!take_answer(far_end, &id, &status));
    close(far_end);
}

/* The authority going away is the ordinary way a worker ends, so it is not a
 * failure -- and nothing is opened on the way out. */
static void check_immediate_eof_is_clean(void)
{
    int far_end;
    int near_end;
    uint64_t id;
    uint32_t status;

    make_pair(&far_end, &near_end);
    ksd_capture_worker_test_opens = 0u;
    assert(serve_queued(far_end, near_end, KSD_BACKEND_X11, getpid()));
    assert(!take_answer(far_end, &id, &status));
    assert(ksd_capture_worker_test_opens == 0u);
    close(far_end);
}

/* A session with no display of its own is answered rather than crashed into,
 * and the loop stays up for the next request. pid 1 is used because it is
 * certain to exist and certain not to be running this test's X client. */
static void check_unreachable_display_is_answered(void)
{
    int far_end;
    int near_end;
    uint64_t id;
    uint32_t status;

    make_pair(&far_end, &near_end);
    queue_request(far_end, KSD_OP_CURSOR_POSITION, 5u, NULL, 0u);
    queue_request(far_end, KSD_OP_CURSOR_POSITION, 6u, NULL, 0u);

    ksd_capture_worker_test_opens = 0u;
    assert(serve_queued(far_end, near_end, KSD_BACKEND_X11, 1));

    assert(take_answer(far_end, &id, &status));
    assert(id == 5u && status != KSD_STATUS_OK);
    assert(take_answer(far_end, &id, &status));
    assert(id == 6u && status != KSD_STATUS_OK);
    assert(!take_answer(far_end, &id, &status));
    /* It kept trying. A failed open is retried on the next request rather than
     * remembered as a permanent verdict, because the display may simply not
     * have come up yet. */
    assert(ksd_capture_worker_test_opens == 0u);
    close(far_end);
}

int main(void)
{
    const char *display = getenv("KSD_TEST_DISPLAY");

    /* The wrapper polls with this to decide the server is up. */
    if (display != NULL && getenv("KSD_TEST_PROBE") != NULL) {
        char canonical[KSD_X11_DISPLAY_CAPACITY];
        ksd_x11 *probe = NULL;

        if (!ksd_x11_display_parse(display, canonical, sizeof(canonical)))
            return 1;
        if (ksd_x11_open(canonical, NULL, &probe) != KSD_STATUS_OK)
            return 1;
        ksd_x11_close(probe);
        return 0;
    }
    if (display == NULL) {
        fputs("KSD_TEST_DISPLAY unset: skipping persistent worker tests\n",
              stderr);
        return 77;
    }
    /* The loop resolves the display from the session's own environment, so the
     * wrapper has to export DISPLAY and not only KSD_TEST_DISPLAY -- and it is
     * read back out of /proc, so setting it from here would be too late. */
    if (getenv("DISPLAY") == NULL) {
        fputs("DISPLAY unset: skipping persistent worker tests\n", stderr);
        return 77;
    }

    check_immediate_eof_is_clean();
    check_oversized_payload_is_refused();
    check_unreachable_display_is_answered();
    check_connection_is_reused();
    check_bad_request_does_not_end_the_loop();
    return 0;
}
