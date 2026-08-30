#include "authority_client.h"

#include "authority_admin.h"
#include "common.h"
#include "keysharp_desktop/protocol.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static int connect_authority(void)
{
    struct sockaddr_un address;
    int descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (descriptor < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    (void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", KSD_AUTHORITY_SOCKET);
    if (connect(descriptor, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        close(descriptor);
        return -1;
    }
    struct timeval receive_timeout = {
        .tv_sec = (time_t)KSD_AUTHORITY_TIMEOUT_SECONDS,
        .tv_usec = 0,
    };
    struct timeval send_timeout = { .tv_sec = 5, .tv_usec = 0 };
    (void)setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO,
                     &receive_timeout, sizeof(receive_timeout));
    (void)setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO,
                     &send_timeout, sizeof(send_timeout));
    return descriptor;
}

static int send_request(const ksd_authority_request *request, int passed_fd)
{
    int descriptor = connect_authority();
    struct iovec iov = { .iov_base = (void *)request, .iov_len = sizeof(*request) };
    struct msghdr message;
    unsigned char control[CMSG_SPACE(sizeof(int))];

    if (descriptor < 0)
        return -1;
    memset(&message, 0, sizeof(message));
    message.msg_iov = &iov;
    message.msg_iovlen = 1u;

    if (passed_fd >= 0) {
        struct cmsghdr *header;
        memset(control, 0, sizeof(control));
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(header), &passed_fd, sizeof(passed_fd));
    }

    if (sendmsg(descriptor, &message, MSG_NOSIGNAL) != (ssize_t)sizeof(*request)) {
        close(descriptor);
        return -1;
    }
    return descriptor;
}

static int receive_response(int descriptor, ksd_authority_response *response)
{
    if (!ksd_read_all(descriptor, response, sizeof(*response)))
        return -1;
    if (response->magic != KSD_AUTH_MAGIC
        || response->major != KSD_PROTOCOL_MAJOR
        || response->minor != KSD_PROTOCOL_MINOR)
        return -1;
    return 0;
}

static int transact(const ksd_authority_request *request, int passed_fd,
                    ksd_authority_response *response)
{
    int descriptor = send_request(request, passed_fd);
    int result;

    if (descriptor < 0)
        return -1;
    result = receive_response(descriptor, response);
    close(descriptor);
    return result;
}

int ksd_authority_check(int client_fd, uint32_t capabilities, bool interactive,
                        uint32_t *granted_capabilities)
{
    ksd_authority_request request = {
        .magic = KSD_AUTH_MAGIC,
        .major = KSD_PROTOCOL_MAJOR,
        .minor = KSD_PROTOCOL_MINOR,
        .operation = KSD_AUTH_OP_CHECK,
        .flags = interactive ? KSD_AUTH_FLAG_INTERACTIVE : 0u,
        .capabilities = capabilities,
    };
    ksd_authority_response response;

    if (granted_capabilities == NULL || client_fd < 0)
        return -1;
    *granted_capabilities = 0u;
    if (transact(&request, client_fd, &response) != 0)
        return -1;
    *granted_capabilities = response.granted_capabilities;
    return response.status == KSD_AUTH_STATUS_GRANTED ? 0
        : response.status == KSD_AUTH_STATUS_DENIED ? 1 : -1;
}

static bool valid_hash(const char *hash)
{
    if (hash == NULL || strlen(hash) != KSD_HASH_HEX_LENGTH)
        return false;
    for (size_t index = 0u; index < KSD_HASH_HEX_LENGTH; index++)
        if (!((hash[index] >= '0' && hash[index] <= '9')
              || (hash[index] >= 'a' && hash[index] <= 'f')))
            return false;
    return true;
}

int ksd_authority_list_current_uid(ksd_authority_grant_info **grants,
                                   size_t *count)
{
    ksd_authority_request request = {
        .magic = KSD_AUTH_MAGIC,
        .major = KSD_PROTOCOL_MAJOR,
        .minor = KSD_PROTOCOL_MINOR,
        .operation = KSD_AUTH_OP_LIST_UID,
    };
    ksd_authority_response response;
    ksd_authority_grant_info *records = NULL;
    int descriptor;

    if (grants == NULL || count == NULL)
        return -1;
    *grants = NULL;
    *count = 0u;
    descriptor = send_request(&request, -1);
    if (descriptor < 0 || receive_response(descriptor, &response) != 0
        || response.status != KSD_AUTH_STATUS_GRANTED
        || response.granted_capabilities > KSD_AUTHORITY_MAX_LIST_RECORDS)
        goto error;
    if (response.granted_capabilities != 0u) {
        records = calloc(response.granted_capabilities, sizeof(*records));
        if (records == NULL)
            goto error;
    }
    for (uint32_t index = 0u; index < response.granted_capabilities; index++) {
        ksd_authority_grant_record_header header;
        static const uint8_t zero[3] = { 0u, 0u, 0u };

        if (!ksd_read_all(descriptor, &header, sizeof(header))
            || header.hash[KSD_HASH_HEX_LENGTH] != '\0'
            || !valid_hash(header.hash)
            || memcmp(header.reserved, zero, sizeof(zero)) != 0
            || header.capabilities == 0u
            || (header.capabilities & ~KSD_CAP_ALL) != 0u
            || header.executable_length == 0u
            || header.executable_length >= KSD_PATH_MAX
            || !ksd_read_all(descriptor, records[index].executable,
                             header.executable_length))
            goto error;
        records[index].executable[header.executable_length] = '\0';
        (void)snprintf(records[index].hash, sizeof(records[index].hash),
                       "%s", header.hash);
        records[index].capabilities = header.capabilities;
    }
    close(descriptor);
    *count = response.granted_capabilities;
    *grants = records;
    return 0;

error:
    if (descriptor >= 0)
        close(descriptor);
    free(records);
    return -1;
}

int ksd_authority_revoke_current_uid(const char *hash, uint32_t capabilities,
                                     bool all)
{
    ksd_authority_request request = {
        .magic = KSD_AUTH_MAGIC,
        .major = KSD_PROTOCOL_MAJOR,
        .minor = KSD_PROTOCOL_MINOR,
        .operation = all ? KSD_AUTH_OP_REVOKE_UID : KSD_AUTH_OP_REVOKE,
        .capabilities = capabilities,
    };
    ksd_authority_response response;
    int descriptor;

    if ((all && (hash != NULL || capabilities != 0u))
        || (!all && (!valid_hash(hash) || capabilities == 0u
                     || (capabilities & ~KSD_CAP_ALL) != 0u)))
        return -1;
    descriptor = send_request(&request, -1);
    if (descriptor < 0)
        return -1;
    if (!all) {
        char selector[KSD_HASH_HEX_LENGTH + 1u] = { 0 };
        (void)snprintf(selector, sizeof(selector), "%s", hash);
        if (!ksd_write_all(descriptor, selector, sizeof(selector))) {
            close(descriptor);
            return -1;
        }
    }
    if (receive_response(descriptor, &response) != 0) {
        close(descriptor);
        return -1;
    }
    close(descriptor);
    return response.status == KSD_AUTH_STATUS_GRANTED ? 0 : -1;
}
