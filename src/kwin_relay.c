#include "kwin_relay.h"

#include "protocol.h"
#include "transport.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* One per connection thread is the ceiling, the same bound the worker pool
 * uses: a thread blocks on the request it sent, so it cannot have two. */
#define KSD_KWIN_RELAY_PENDING 128u

typedef struct relay_pending {
    bool active;
    bool done;
    uint64_t request_id;
    uint32_t status;
    uint32_t detail;
    uint8_t *tail;
    uint32_t tail_length;
} relay_pending;

struct ksd_kwin_relay {
    int descriptor;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    /* Serialises writes only. A write is short and never waits on the far end,
     * so holding this across one is not the bottleneck holding a lock across
     * the whole exchange would be. */
    pthread_mutex_t write_mutex;
    bool reading;
    bool broken;
    uint64_t next_request_id;
    relay_pending pending[KSD_KWIN_RELAY_PENDING];
};

static uint64_t relay_monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0u;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

ksd_kwin_relay *ksd_kwin_relay_create(int descriptor)
{
    ksd_kwin_relay *relay;

    if (descriptor < 0)
        return NULL;
    relay = calloc(1u, sizeof(*relay));
    if (relay == NULL)
        return NULL;
    relay->descriptor = descriptor;
    relay->next_request_id = 1u;
    if (pthread_mutex_init(&relay->mutex, NULL) != 0
        || pthread_mutex_init(&relay->write_mutex, NULL) != 0
        || pthread_cond_init(&relay->changed, NULL) != 0) {
        free(relay);
        return NULL;
    }
    return relay;
}

void ksd_kwin_relay_destroy(ksd_kwin_relay *relay)
{
    if (relay == NULL)
        return;
    pthread_mutex_lock(&relay->mutex);
    relay->broken = true;
    for (size_t index = 0u; index < KSD_KWIN_RELAY_PENDING; index++)
        free(relay->pending[index].tail);
    pthread_cond_broadcast(&relay->changed);
    pthread_mutex_unlock(&relay->mutex);
    if (relay->descriptor >= 0)
        close(relay->descriptor);
    pthread_mutex_destroy(&relay->mutex);
    pthread_mutex_destroy(&relay->write_mutex);
    pthread_cond_destroy(&relay->changed);
    free(relay);
}

/* Reads one response frame and hands it to whoever was waiting for it. Called
 * with the lock NOT held, by whichever caller took the reader role. */
static bool read_one(ksd_kwin_relay *relay)
{
    uint8_t header[KSD_FRAME_HEADER_SIZE];
    uint8_t *body = NULL;
    ksd_frame frame;
    bool matched = false;

    if (!ksd_read_all(relay->descriptor, header, sizeof(header)))
        return false;
    uint32_t payload_length =
        ksd_decode_u32(header + KSD_FRAME_PAYLOAD_LENGTH_OFFSET);
    /* Every answer carries the eight-byte status and detail prologue, so one
     * shorter than that is not an answer this daemon wrote. */
    if (payload_length < 8u || payload_length > KSD_MAX_TEXT_BYTES + 8u)
        return false;
    if (payload_length != 0u) {
        body = malloc(payload_length);
        if (body == NULL || !ksd_read_all(relay->descriptor, body,
                                          payload_length)) {
            free(body);
            return false;
        }
    }
    memset(&frame, 0, sizeof(frame));
    frame.request_id = ksd_decode_u64(header + KSD_FRAME_REQUEST_ID_OFFSET);
    uint32_t status = ksd_decode_u32(body);
    uint32_t detail = ksd_decode_u32(body + 4u);
    uint32_t tail_length = payload_length - 8u;

    pthread_mutex_lock(&relay->mutex);
    for (size_t index = 0u; index < KSD_KWIN_RELAY_PENDING; index++) {
        relay_pending *slot = relay->pending + index;

        if (!slot->active || slot->done
            || slot->request_id != frame.request_id)
            continue;
        slot->status = status;
        slot->detail = detail;
        /* The prologue is dropped here rather than carried further: what the
         * caller wants is the tail, and every reader past this point would
         * otherwise have to know to skip eight bytes. */
        if (tail_length != 0u) {
            slot->tail = malloc(tail_length);
            if (slot->tail != NULL) {
                memcpy(slot->tail, body + 8u, tail_length);
                slot->tail_length = tail_length;
            }
        }
        slot->done = true;
        matched = true;
        break;
    }
    pthread_mutex_unlock(&relay->mutex);
    /* A response nobody is waiting for is dropped rather than kept. It names a
     * request that has already timed out, and holding it would grow without
     * bound on a daemon that answers late every time. */
    free(body);
    (void)matched;
    return true;
}

bool ksd_kwin_relay_call(ksd_kwin_relay *relay, const ksd_frame *request,
                         uint64_t deadline_ms, ksd_operation_result *result)
{
    ksd_buffer packed;
    relay_pending *slot = NULL;
    uint64_t request_id;
    bool sent;
    bool answered = false;

    if (relay == NULL || request == NULL || result == NULL)
        return false;

    pthread_mutex_lock(&relay->mutex);
    if (relay->broken) {
        pthread_mutex_unlock(&relay->mutex);
        ksd_result_error(result, KSD_STATUS_UNAVAILABLE, 0u,
                         "the compositor channel is closed");
        return true;
    }
    for (size_t index = 0u; index < KSD_KWIN_RELAY_PENDING; index++) {
        if (!relay->pending[index].active) {
            slot = relay->pending + index;
            break;
        }
    }
    if (slot == NULL) {
        pthread_mutex_unlock(&relay->mutex);
        /* Nobody is waiting on a free slot, so this is BUSY rather than a
         * timeout: the request provably never left this process. */
        ksd_result_error(result, KSD_STATUS_BUSY, 0u,
                         "too many requests in flight for this compositor");
        return true;
    }
    memset(slot, 0, sizeof(*slot));
    slot->active = true;
    request_id = relay->next_request_id++;
    slot->request_id = request_id;
    pthread_mutex_unlock(&relay->mutex);

    /* Packed with the caller's opcode and payload but OUR request id, because
     * the id is what demultiplexes the shared socket and the client's own ids
     * are only unique within its own connection. */
    ksd_frame outgoing = *request;
    outgoing.request_id = request_id;
    ksd_buffer_init(&packed, KSD_FRAME_HEADER_SIZE
                    + request->payload_length + 16u);
    sent = ksd_frame_pack(&outgoing, &packed);
    if (sent) {
        pthread_mutex_lock(&relay->write_mutex);
        sent = ksd_write_all(relay->descriptor, packed.data, packed.length);
        pthread_mutex_unlock(&relay->write_mutex);
    }
    ksd_buffer_clear(&packed);

    pthread_mutex_lock(&relay->mutex);
    while (sent && !slot->done && !relay->broken) {
        uint64_t now = relay_monotonic_ms();
        struct timespec until;

        if (now == 0u || now >= deadline_ms)
            break;
        if (!relay->reading) {
            /* Nobody is reading, so this caller does it. The lock is dropped
             * across the read: a read blocks, and holding the lock through one
             * would stop every other caller from even noticing its own answer
             * had arrived. */
            relay->reading = true;
            pthread_mutex_unlock(&relay->mutex);
            bool ok = read_one(relay);
            pthread_mutex_lock(&relay->mutex);
            relay->reading = false;
            if (!ok)
                relay->broken = true;
            pthread_cond_broadcast(&relay->changed);
            continue;
        }
        /* Someone else is reading; wait to be woken, but not past the
         * deadline, because the reader may be waiting on a daemon that has
         * stopped answering. */
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += 20 * 1000000L;
        if (until.tv_nsec >= 1000000000L) {
            until.tv_sec += 1;
            until.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&relay->changed, &relay->mutex, &until);
    }
    if (slot->done) {
        if (slot->status == KSD_STATUS_OK && slot->tail_length != 0u) {
            answered = ksd_result_take(result, slot->tail, slot->tail_length);
            if (answered)
                slot->tail = NULL;
        } else if (slot->status == KSD_STATUS_OK) {
            answered = ksd_result_take(result, NULL, 0u);
        } else {
            ksd_result_error(result, slot->status, slot->detail,
                             "the compositor refused the operation");
            answered = true;
        }
    }
    free(slot->tail);
    memset(slot, 0, sizeof(*slot));
    pthread_cond_broadcast(&relay->changed);
    pthread_mutex_unlock(&relay->mutex);

    if (answered)
        return true;
    if (!sent) {
        /* Never written, so it provably never reached the compositor. */
        ksd_result_error(result, KSD_STATUS_BUSY, 0u,
                         "could not reach the compositor");
        return true;
    }
    /* Written and unanswered. Its outcome is unknown, which is TIMEOUT and not
     * BUSY: the operation may well have happened. */
    ksd_result_error(result, KSD_STATUS_TIMEOUT, 0u,
                     "the compositor did not answer in time");
    return true;
}
