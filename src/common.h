#ifndef KEYSHARP_DESKTOP_COMMON_H
#define KEYSHARP_DESKTOP_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define KSD_HASH_HEX_LENGTH 64u
#define KSD_PATH_MAX 4096u
#ifndef KSD_RUNTIME_DIRECTORY
#define KSD_RUNTIME_DIRECTORY "/run/keysharp-permissions"
#endif
#ifndef KSD_REVOKE_GENERATION_OWNER_UID
#define KSD_REVOKE_GENERATION_OWNER_UID 0u
#endif

typedef struct ksd_process_identity {
    uid_t uid;
    pid_t pid;
    uint64_t start_time;
    char executable[KSD_PATH_MAX];
    char hash[KSD_HASH_HEX_LENGTH + 1u];
} ksd_process_identity;

bool ksd_write_all(int fd, const void *data, size_t length);
bool ksd_read_all(int fd, void *data, size_t length);
int ksd_receive_optional_fd(int fd, void *data, size_t length, int *received_fd);
int ksd_revoke_generation_path(uid_t uid, char *path, size_t capacity);
int ksd_revoke_generation_read(uid_t uid, uint64_t *generation);
uint64_t ksd_process_start_time(pid_t pid);
int ksd_hash_app_identity(const char *kind, const void *identity_bytes,
                          size_t identity_length,
                          char output[KSD_HASH_HEX_LENGTH + 1u]);
bool ksd_executable_path_is_protected(int executable_fd, const char *absolute_path);
int ksd_identify_process(pid_t pid, uid_t expected_uid, uint64_t expected_start_time,
                         ksd_process_identity *identity);
int ksd_make_parent_directories(const char *path, mode_t mode);
void ksd_sanitize_display_text(const char *source, char *destination,
                               size_t capacity);
int ksd_format_capability_names(uint32_t capabilities, char *destination,
                                size_t capacity);

#endif
