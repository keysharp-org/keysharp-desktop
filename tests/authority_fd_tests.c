#include "common.h"

#include <assert.h>
#include <dirent.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static size_t open_fd_count(void)
{
    DIR *directory = opendir("/proc/self/fd");
    struct dirent *entry;
    size_t count = 0u;

    assert(directory != NULL);
    while ((entry = readdir(directory)) != NULL)
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
            count++;
    closedir(directory);
    return count;
}

static void send_fds(int descriptor, const int *fds, size_t fd_count)
{
    char byte = 'x';
    unsigned char ancillary[CMSG_SPACE(sizeof(int) * 20u)];
    struct iovec iov = { .iov_base = &byte, .iov_len = 1u };
    struct msghdr message;

    assert(fd_count > 0u && fd_count <= 20u);
    memset(&message, 0, sizeof(message));
    memset(ancillary, 0, sizeof(ancillary));
    message.msg_iov = &iov;
    message.msg_iovlen = 1u;
    message.msg_control = ancillary;
    message.msg_controllen = CMSG_SPACE(sizeof(int) * fd_count);
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int) * fd_count);
    memcpy(CMSG_DATA(header), fds, sizeof(int) * fd_count);
    assert(sendmsg(descriptor, &message, 0) == 1);
}

int main(void)
{
    int client_pair[2];
    int control_pair[2];
    int received = -1;
    char byte = 'x';
    struct ucred credentials;
    socklen_t credentials_length = sizeof(credentials);

    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, client_pair) == 0);
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, control_pair) == 0);
    send_fds(control_pair[0], &client_pair[1], 1u);
    assert(ksd_receive_optional_fd(control_pair[1], &byte, sizeof(byte), &received) == 0);
    assert(received >= 0);
    assert(getsockopt(received, SOL_SOCKET, SO_PEERCRED, &credentials, &credentials_length) == 0);
    assert(credentials.pid == getpid());
    assert(credentials.uid == getuid());
    close(received);

    size_t before = open_fd_count();
    send_fds(control_pair[0], client_pair, 2u);
    assert(ksd_receive_optional_fd(control_pair[1], &byte, sizeof(byte), &received) != 0);
    assert(received == -1);
    assert(open_fd_count() == before);

    int excessive[20];
    for (size_t index = 0u; index < 20u; index++)
        excessive[index] = client_pair[0];
    send_fds(control_pair[0], excessive, 20u);
    assert(ksd_receive_optional_fd(control_pair[1], &byte, sizeof(byte), &received) != 0);
    assert(received == -1);
    assert(open_fd_count() == before);

    assert(ksd_write_all(control_pair[0], &byte, sizeof(byte)));
    assert(ksd_receive_optional_fd(control_pair[1], &byte, sizeof(byte), &received) == 0);
    assert(received == -1);

    close(client_pair[0]);
    close(client_pair[1]);
    close(control_pair[0]);
    close(control_pair[1]);
    return 0;
}
