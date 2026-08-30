#include "grants.h"

#include "keysharp_desktop/protocol.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define KSP_GRANT_VERSION "keysharp-permission-v1"
#define KSP_MARKER_SUFFIX ".grant"

typedef struct capability_mapping {
    uint32_t local;
    uint32_t shared;
} capability_mapping;

static const capability_mapping capability_mappings[] = {
    { KSD_CAP_SCREEN_CAPTURE, KSP_SCOPE_SCREEN_CAPTURE },
    { KSD_CAP_WINDOW_MONITORING, KSP_SCOPE_WINDOW_MONITORING },
    { KSD_CAP_WINDOW_CONTROL, KSP_SCOPE_WINDOW_CONTROL },
    { KSD_CAP_AUDIO_CAPTURE, KSP_SCOPE_AUDIO_CAPTURE },
    { KSD_CAP_CAMERA_CAPTURE, KSP_SCOPE_CAMERA_CAPTURE },
    { KSD_CAP_CLIPBOARD_MONITORING, KSP_SCOPE_CLIPBOARD_MONITORING },
};

static bool valid_hash(const char *value)
{
    if (value == NULL || strlen(value) != KSD_HASH_HEX_LENGTH)
        return false;
    for (size_t index = 0; index < KSD_HASH_HEX_LENGTH; index++)
        if (!((value[index] >= '0' && value[index] <= '9')
              || (value[index] >= 'a' && value[index] <= 'f')))
            return false;
    return true;
}

static int ensure_runtime_directory(void)
{
    struct stat info;

    if (ksd_make_parent_directories(KSD_RUNTIME_DIRECTORY "/x", 0755) != 0
        || lstat(KSD_RUNTIME_DIRECTORY, &info) != 0)
        return -1;
    if (!S_ISDIR(info.st_mode) || info.st_uid != geteuid()
        || (info.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        errno = EACCES;
        return -1;
    }
    return chmod(KSD_RUNTIME_DIRECTORY, 0755);
}

int ksd_grants_prompt_lock_acquire(uid_t uid, const char *hash)
{
    char path[KSD_PATH_MAX + 128u];
    struct stat info;
    int descriptor;
    int length;

    if (!valid_hash(hash)) {
        errno = EINVAL;
        return -1;
    }
    length = snprintf(path, sizeof(path), KSD_RUNTIME_DIRECTORY
                      "/.prompt-%lu-%s.lock", (unsigned long)uid, hash);
    if (length <= 0 || (size_t)length >= sizeof(path)
        || ensure_runtime_directory() != 0)
        return -1;

    descriptor = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0)
        return -1;
    if (fstat(descriptor, &info) != 0)
        goto error;
    if (!S_ISREG(info.st_mode) || info.st_uid != geteuid()) {
        errno = EACCES;
        goto error;
    }
    if (fchmod(descriptor, 0600) != 0 || fstat(descriptor, &info) != 0)
        goto error;
    if ((info.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO))
        != (S_IRUSR | S_IWUSR)) {
        errno = EACCES;
        goto error;
    }
    while (flock(descriptor, LOCK_EX) != 0) {
        if (errno == EINTR)
            continue;
        int error = errno;
        close(descriptor);
        errno = error;
        return -1;
    }
    return descriptor;

error:
    {
        int error = errno;
        close(descriptor);
        errno = error;
    }
    return -1;
}

void ksd_grants_prompt_lock_release(int descriptor)
{
    if (descriptor < 0)
        return;
    while (flock(descriptor, LOCK_UN) != 0 && errno == EINTR) {
    }
    close(descriptor);
}

static int ensure_store_directory(void)
{
    struct stat info;

    if (ksd_make_parent_directories(KSD_GRANT_DIRECTORY "/x", 0700) != 0
        || lstat(KSD_GRANT_DIRECTORY, &info) != 0
        || !S_ISDIR(info.st_mode) || info.st_uid != geteuid()
        || (info.st_mode & (S_IWGRP | S_IWOTH)) != 0
        || chmod(KSD_GRANT_DIRECTORY, 0700) != 0)
        return -1;
    return 0;
}

static int lock_store(int operation)
{
    int descriptor;

    if (ensure_store_directory() != 0)
        return -1;
    descriptor = open(KSD_GRANT_DIRECTORY "/.lock",
                      O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0 || fchmod(descriptor, 0600) != 0) {
        if (descriptor >= 0)
            close(descriptor);
        return -1;
    }
    if (flock(descriptor, operation) != 0) {
        close(descriptor);
        return -1;
    }
    return descriptor;
}

static int marker_path(uid_t uid, const char *hash, uint32_t shared_capability,
                       char *path, size_t capacity)
{
    int length;

    if (!valid_hash(hash) || shared_capability == 0u
        || (shared_capability & (shared_capability - 1u)) != 0u)
        return -1;
    length = snprintf(path, capacity, KSD_GRANT_DIRECTORY
                      "/grant-%lu-%s-%08x" KSP_MARKER_SUFFIX,
                      (unsigned long)uid, hash, shared_capability);
    return length > 0 && (size_t)length < capacity ? 0 : -1;
}

static int read_marker(uid_t uid, const char *hash, uint32_t shared_capability,
                       char *executable, size_t executable_capacity)
{
    char path[KSD_PATH_MAX + 128u];
    char line[KSD_PATH_MAX + 256u];
    char record_hash[KSD_HASH_HEX_LENGTH + 1u];
    char version[64];
    unsigned long record_uid;
    unsigned int record_capability;
    unsigned long long record_time;
    struct stat info;
    FILE *file;
    int descriptor;
    int fields;
    int executable_offset = -1;
    bool no_trailing_data;

    if (marker_path(uid, hash, shared_capability, path, sizeof(path)) != 0)
        return -1;
    descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return errno == ENOENT ? 0 : -1;
    if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode)
        || info.st_uid != geteuid()
        || (info.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        close(descriptor);
        errno = EACCES;
        return -1;
    }
    file = fdopen(descriptor, "r");
    if (file == NULL) {
        close(descriptor);
        return -1;
    }
    if (fgets(version, sizeof(version), file) == NULL
        || strcmp(version, KSP_GRANT_VERSION "\n") != 0
        || fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        errno = EPROTO;
        return -1;
    }
    fields = sscanf(line, "%lu\t%64[a-f0-9]\t%x\t%llu%n",
                    &record_uid, record_hash, &record_capability, &record_time,
                    &executable_offset);
    no_trailing_data = fgetc(file) == EOF && feof(file);
    if (fclose(file) != 0)
        return -1;
    if (fields != 4 || record_uid != (unsigned long)uid
        || strcmp(record_hash, hash) != 0
        || record_capability != shared_capability || executable_offset < 0
        || line[executable_offset] != '\t') {
        errno = EPROTO;
        return -1;
    }
    char *record_executable = line + executable_offset + 1;
    size_t executable_length = strcspn(record_executable, "\n");
    if (executable_length == 0u || record_executable[executable_length] != '\n'
        || record_executable[executable_length + 1u] != '\0'
        || (executable != NULL && executable_length >= executable_capacity)
        || !no_trailing_data) {
        errno = EPROTO;
        return -1;
    }
    for (size_t index = 0u; index < executable_length; index++) {
        unsigned char value = (unsigned char)record_executable[index];
        if (value < 0x20u || value == 0x7fu) {
            errno = EPROTO;
            return -1;
        }
    }
    if (executable != NULL) {
        memcpy(executable, record_executable, executable_length);
        executable[executable_length] = '\0';
    }
    return 1;
}

static int fsync_store_directory(void)
{
    int descriptor = open(KSD_GRANT_DIRECTORY,
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    int result;

    if (descriptor < 0)
        return -1;
    result = fsync(descriptor);
    close(descriptor);
    return result;
}

static int write_marker_locked(const ksd_process_identity *identity,
                               uint32_t shared_capability)
{
    char path[KSD_PATH_MAX + 128u];
    char temporary[KSD_PATH_MAX + 128u];
    char executable[KSD_PATH_MAX];
    FILE *file = NULL;
    int descriptor = -1;
    int result = -1;
    int existing = read_marker(identity->uid, identity->hash, shared_capability,
                               NULL, 0u);
    int temporary_length;

    if (existing != 0)
        return existing > 0 ? 0 : -1;
    if (marker_path(identity->uid, identity->hash, shared_capability,
                    path, sizeof(path)) != 0)
        return -1;
    temporary_length = snprintf(temporary, sizeof(temporary),
                                KSD_GRANT_DIRECTORY "/.grant-%lu-XXXXXX",
                                (unsigned long)identity->uid);
    if (temporary_length <= 0
        || (size_t)temporary_length >= sizeof(temporary))
        return -1;

    (void)snprintf(executable, sizeof(executable), "%s", identity->executable);
    for (unsigned char *cursor = (unsigned char *)executable;
         *cursor != '\0'; cursor++)
        if (*cursor < 0x20u || *cursor == 0x7fu)
            *cursor = (unsigned char)'?';

    descriptor = mkstemp(temporary);
    if (descriptor < 0 || fchmod(descriptor, 0600) != 0)
        goto done;
    file = fdopen(descriptor, "w");
    if (file == NULL)
        goto done;
    descriptor = -1;
    if (fprintf(file, KSP_GRANT_VERSION "\n%lu\t%s\t%08x\t%llu\t%s\n",
                (unsigned long)identity->uid, identity->hash,
                shared_capability, (unsigned long long)time(NULL),
                executable) < 0
        || fflush(file) != 0 || fsync(fileno(file)) != 0)
        goto done;
    if (fclose(file) != 0) {
        file = NULL;
        goto done;
    }
    file = NULL;
    if (rename(temporary, path) != 0 || fsync_store_directory() != 0)
        goto done;
    result = 0;

done:
    if (file != NULL)
        fclose(file);
    if (descriptor >= 0)
        close(descriptor);
    if (result != 0)
        (void)unlink(temporary);
    return result;
}

static uint32_t shared_from_local(uint32_t local_capabilities)
{
    uint32_t shared = 0u;

    for (size_t index = 0u;
         index < sizeof(capability_mappings) / sizeof(capability_mappings[0]);
         index++)
        if ((local_capabilities & capability_mappings[index].local) != 0u)
            shared |= capability_mappings[index].shared;
    return shared;
}

static uint32_t local_from_store(uid_t uid, const char *hash, int *error)
{
    uint32_t local = 0u;

    *error = 0;
    for (size_t index = 0u;
         index < sizeof(capability_mappings) / sizeof(capability_mappings[0]);
         index++) {
        int found = read_marker(uid, hash, capability_mappings[index].shared,
                                NULL, 0u);
        if (found < 0) {
            *error = -1;
            return 0u;
        }
        if (found > 0)
            local |= capability_mappings[index].local;
    }
    return local;
}

int ksd_grants_bump_revoke_generation(uid_t uid)
{
    char path[KSD_PATH_MAX];
    char lock_path[KSD_PATH_MAX];
    char temporary[KSD_PATH_MAX];
    uint64_t generation = 0u;
    int lock = -1;
    int descriptor = -1;
    int result = -1;
    int lock_length;
    int temporary_length;

    if (ksd_revoke_generation_path(uid, path, sizeof(path)) != 0)
        return -1;
    lock_length = snprintf(lock_path, sizeof(lock_path), KSD_RUNTIME_DIRECTORY
                           "/.revoke-%lu.lock", (unsigned long)uid);
    temporary_length = snprintf(temporary, sizeof(temporary),
                                KSD_RUNTIME_DIRECTORY "/.revoke-%lu.XXXXXX",
                                (unsigned long)uid);
    if (lock_length <= 0 || (size_t)lock_length >= sizeof(lock_path)
        || temporary_length <= 0
        || (size_t)temporary_length >= sizeof(temporary)
        || ensure_runtime_directory() != 0)
        return -1;
    lock = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || fchmod(lock, 0600) != 0 || flock(lock, LOCK_EX) != 0)
        goto done;
    if (ksd_revoke_generation_read(uid, &generation) != 0)
        goto done;
    generation++;
    if (generation == 0u)
        generation = 1u;
    descriptor = mkstemp(temporary);
    if (descriptor < 0 || fchmod(descriptor, 0644) != 0
        || !ksd_write_all(descriptor, &generation, sizeof(generation))
        || fsync(descriptor) != 0)
        goto cleanup_path;
    if (close(descriptor) != 0) {
        descriptor = -1;
        goto cleanup_path;
    }
    descriptor = -1;
    if (rename(temporary, path) != 0)
        goto cleanup_path;
    int directory = open(KSD_RUNTIME_DIRECTORY,
                         O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0)
        goto done;
    result = fsync(directory);
    close(directory);
    goto done;

cleanup_path:
    (void)unlink(temporary);
done:
    if (descriptor >= 0)
        close(descriptor);
    if (lock >= 0) {
        (void)flock(lock, LOCK_UN);
        close(lock);
    }
    return result;
}

int ksd_grants_check_at_generation(const ksd_process_identity *identity,
                                   uint32_t *capabilities, uint64_t *generation)
{
    int lock;
    int error;

    if (identity == NULL || capabilities == NULL || generation == NULL
        || !valid_hash(identity->hash))
        return -1;
    *capabilities = 0u;
    *generation = 0u;
    lock = lock_store(LOCK_SH);
    if (lock < 0)
        return -1;
    *capabilities = local_from_store(identity->uid, identity->hash, &error);
    if (error == 0)
        error = ksd_revoke_generation_read(identity->uid, generation);
    (void)flock(lock, LOCK_UN);
    close(lock);
    return error;
}

int ksd_grants_check(const ksd_process_identity *identity, uint32_t *capabilities)
{
    uint64_t generation;
    return ksd_grants_check_at_generation(identity, capabilities, &generation);
}

static int add_locked(const ksd_process_identity *identity, uint32_t capabilities)
{
    uint32_t shared = shared_from_local(capabilities & KSD_CAP_ALL);

    if (shared == 0u)
        return -1;
    for (uint32_t bit = 1u; bit != 0u; bit <<= 1u)
        if ((shared & bit) != 0u && write_marker_locked(identity, bit) != 0)
            return -1;
    return 0;
}

int ksd_grants_add_if_generation(const ksd_process_identity *identity,
                                 uint32_t capabilities,
                                 uint64_t expected_generation)
{
    uint64_t generation;
    int lock;
    int result;

    if (identity == NULL || !valid_hash(identity->hash)
        || capabilities == 0u || (capabilities & ~KSD_CAP_ALL) != 0u)
        return -1;
    lock = lock_store(LOCK_EX);
    if (lock < 0)
        return -1;
    if (ksd_revoke_generation_read(identity->uid, &generation) != 0)
        result = -1;
    else if (generation != expected_generation)
        result = 1;
    else
        result = add_locked(identity, capabilities);
    (void)flock(lock, LOCK_UN);
    close(lock);
    return result;
}

int ksd_grants_add(const ksd_process_identity *identity, uint32_t capabilities)
{
    int lock;
    int result;

    if (identity == NULL || !valid_hash(identity->hash)
        || capabilities == 0u || (capabilities & ~KSD_CAP_ALL) != 0u)
        return -1;
    lock = lock_store(LOCK_EX);
    if (lock < 0)
        return -1;
    result = add_locked(identity, capabilities);
    (void)flock(lock, LOCK_UN);
    close(lock);
    return result;
}

static bool parse_marker_name(const char *name, uid_t uid,
                              char hash[KSD_HASH_HEX_LENGTH + 1u],
                              uint32_t *shared_capability)
{
    char prefix[64];
    const char *bit_text;
    char *end;
    unsigned long bit;
    size_t suffix_length = strlen(KSP_MARKER_SUFFIX);
    size_t length;
    int prefix_length = snprintf(prefix, sizeof(prefix), "grant-%lu-",
                                 (unsigned long)uid);

    if (prefix_length <= 0 || strncmp(name, prefix, (size_t)prefix_length) != 0)
        return false;
    length = strlen(name);
    if (length != (size_t)prefix_length + KSD_HASH_HEX_LENGTH + 1u + 8u
        + suffix_length
        || strcmp(name + length - suffix_length, KSP_MARKER_SUFFIX) != 0)
        return false;
    for (size_t index = (size_t)prefix_length;
         index < (size_t)prefix_length + KSD_HASH_HEX_LENGTH; index++)
        if (!((name[index] >= '0' && name[index] <= '9')
              || (name[index] >= 'a' && name[index] <= 'f')))
            return false;
    if (name[(size_t)prefix_length + KSD_HASH_HEX_LENGTH] != '-')
        return false;
    bit_text = name + length - suffix_length - 8u;
    if (bit_text <= name || bit_text[-1] != '-')
        return false;
    errno = 0;
    bit = strtoul(bit_text, &end, 16);
    if (errno != 0 || end != name + length - suffix_length
        || bit == 0u || bit > UINT32_MAX || (bit & (bit - 1u)) != 0u)
        return false;
    memcpy(hash, name + prefix_length, KSD_HASH_HEX_LENGTH);
    hash[KSD_HASH_HEX_LENGTH] = '\0';
    *shared_capability = (uint32_t)bit;
    return true;
}

static uint32_t local_from_shared(uint32_t shared_capabilities)
{
    uint32_t local = 0u;

    for (size_t index = 0u;
         index < sizeof(capability_mappings) / sizeof(capability_mappings[0]);
         index++)
        if ((shared_capabilities & capability_mappings[index].shared) != 0u)
            local |= capability_mappings[index].local;
    return local;
}

static int compare_stored_grants(const void *left, const void *right)
{
    const ksd_stored_grant *first = left;
    const ksd_stored_grant *second = right;
    return strcmp(first->hash, second->hash);
}

int ksd_grants_list_uid(uid_t uid, ksd_stored_grant **grants, size_t *count)
{
    ksd_stored_grant *records = NULL;
    DIR *directory = NULL;
    int lock;
    int result = -1;

    if (grants == NULL || count == NULL) {
        errno = EINVAL;
        return -1;
    }
    *grants = NULL;
    *count = 0u;
    lock = lock_store(LOCK_SH);
    if (lock < 0)
        return -1;
    directory = opendir(KSD_GRANT_DIRECTORY);
    if (directory == NULL)
        goto done;
    for (;;) {
        char executable[KSD_PATH_MAX];
        char hash[KSD_HASH_HEX_LENGTH + 1u];
        uint32_t shared_capability;
        uint32_t local_capability;
        struct dirent *entry;

        errno = 0;
        entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0)
                goto done;
            break;
        }
        if (!parse_marker_name(entry->d_name, uid, hash, &shared_capability))
            continue;
        local_capability = local_from_shared(shared_capability);
        if (local_capability == 0u)
            continue;
        if (read_marker(uid, hash, shared_capability,
                        executable, sizeof(executable)) <= 0)
            goto done;

        size_t index;
        for (index = 0u; index < *count; index++)
            if (strcmp(records[index].hash, hash) == 0)
                break;
        if (index == *count) {
            if (*count >= 4096u) {
                errno = EOVERFLOW;
                goto done;
            }
            ksd_stored_grant *expanded = realloc(
                records, (*count + 1u) * sizeof(*records));
            if (expanded == NULL)
                goto done;
            records = expanded;
            memset(&records[index], 0, sizeof(records[index]));
            (void)snprintf(records[index].hash, sizeof(records[index].hash),
                           "%s", hash);
            (void)snprintf(records[index].executable,
                           sizeof(records[index].executable), "%s", executable);
            (*count)++;
        }
        records[index].capabilities |= local_capability;
    }
    if (closedir(directory) != 0) {
        directory = NULL;
        goto done;
    }
    directory = NULL;
    if (*count > 1u)
        qsort(records, *count, sizeof(*records), compare_stored_grants);
    *grants = records;
    records = NULL;
    result = 0;

done:
    if (directory != NULL)
        closedir(directory);
    free(records);
    if (result != 0)
        *count = 0u;
    (void)flock(lock, LOCK_UN);
    close(lock);
    return result;
}

static bool marker_matches_revoke(const char *name, uid_t uid,
                                  const char *expected_hash,
                                  uint32_t shared_capabilities)
{
    char hash[KSD_HASH_HEX_LENGTH + 1u];
    uint32_t shared_capability;

    return parse_marker_name(name, uid, hash, &shared_capability)
        && (expected_hash == NULL || strcmp(hash, expected_hash) == 0)
        && (shared_capabilities & shared_capability) == shared_capability;
}

static int revoke_grants(uid_t uid, const char *hash, uint32_t capabilities)
{
    uint32_t shared = shared_from_local(capabilities);
    DIR *directory = NULL;
    int lock;
    int result = -1;

    if ((hash != NULL && !valid_hash(hash)) || capabilities == 0u
        || (capabilities & ~KSD_CAP_ALL) != 0u || shared == 0u) {
        errno = EINVAL;
        return -1;
    }
    lock = lock_store(LOCK_EX);
    if (lock < 0)
        return -1;
    if (ksd_grants_bump_revoke_generation(uid) != 0)
        goto done;
    directory = opendir(KSD_GRANT_DIRECTORY);
    if (directory == NULL)
        goto done;
    for (;;) {
        struct dirent *entry;
        errno = 0;
        entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0)
                goto done;
            break;
        }
        if (!marker_matches_revoke(entry->d_name, uid, hash, shared))
            continue;
        char path[KSD_PATH_MAX + 128u];
        int length = snprintf(path, sizeof(path), "%s/%s",
                              KSD_GRANT_DIRECTORY, entry->d_name);
        if (length <= 0 || (size_t)length >= sizeof(path)
            || (unlink(path) != 0 && errno != ENOENT))
            goto done;
    }
    closedir(directory);
    directory = NULL;
    if (fsync_store_directory() != 0
        || ksd_grants_bump_revoke_generation(uid) != 0)
        goto done;
    result = 0;

done:
    if (directory != NULL)
        closedir(directory);
    (void)flock(lock, LOCK_UN);
    close(lock);
    return result;
}

int ksd_grants_revoke(uid_t uid, const char *hash, uint32_t capabilities)
{
    return revoke_grants(uid, hash, capabilities);
}

int ksd_grants_revoke_uid(uid_t uid)
{
    return revoke_grants(uid, NULL, KSD_CAP_ALL);
}
