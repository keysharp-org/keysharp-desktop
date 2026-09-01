#include "permission_domain.h"

#include <assert.h>
#include <dirent.h>
#include <keysharp_permissions/permissions.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void remove_directory(const char *path)
{
    DIR *directory = opendir(path);
    if (directory != NULL) {
        struct dirent *entry;
        while ((entry = readdir(directory)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0
                || strcmp(entry->d_name, "..") == 0)
                continue;
            char child[4096];
            int length = snprintf(child, sizeof(child), "%s/%s",
                                  path, entry->d_name);
            if (length > 0 && (size_t)length < sizeof(child))
                (void)unlink(child);
        }
        closedir(directory);
    }
    (void)rmdir(path);
}

int main(void)
{
    char root[4096];
    const char *home = getenv("HOME");
    assert(home != NULL && home[0] == '/');
    assert(snprintf(root, sizeof(root),
                    "%s/.keysharp-desktop-domain-XXXXXX", home) > 0);
    assert(mkdtemp(root) != NULL);
    char persistent[4096];
    char runtime[4096];
    assert(snprintf(persistent, sizeof(persistent), "%s/store", root) > 0);
    assert(snprintf(runtime, sizeof(runtime), "%s/runtime", root) > 0);

    ksp_store_config desktop_config;
    ksp_store_config_init(&desktop_config, KSD_DESKTOP_MANAGED_SCOPES);
    desktop_config.read_scopes = KSD_DESKTOP_ACCEPTED_SCOPES;
    desktop_config.persistent_directory = persistent;
    desktop_config.runtime_directory = runtime;
    desktop_config.owner_uid = getuid();
    ksp_store *desktop = NULL;
    assert(ksp_store_create(&desktop, &desktop_config) == 0);
    assert(ksp_store_prepare(desktop) == 0);
    assert(ksp_store_read_scopes(desktop) == KSD_DESKTOP_ACCEPTED_SCOPES);
    assert(ksp_store_write_scopes(desktop) == KSD_DESKTOP_MANAGED_SCOPES);

    ksp_identity identity = { 0 };
    identity.uid = getuid();
    identity.pid = getpid();
    identity.start_time = 1u;
    memset(identity.hash, 'a', KSP_HASH_HEX_LENGTH);
    identity.hash[KSP_HASH_HEX_LENGTH] = '\0';
    strcpy(identity.executable, "/opt/apps/domain-test");
    uint64_t generation;
    assert(ksp_store_generation(desktop, getuid(), &generation) == 0);
    assert(ksp_store_grant_if_generation(desktop, &identity,
        KSP_SCOPE_WINDOW_MONITORING, generation) == 0);
    assert(ksp_store_generation(desktop, getuid(), &generation) == 0);
    assert(ksp_store_grant_if_generation(desktop, &identity,
        KSP_SCOPE_INPUT_CONTROL, generation) == 0);

    uint32_t allowed = 0u;
    assert(ksp_store_check(desktop, getuid(), identity.hash,
        KSP_SCOPE_INPUT_CONTROL | KSP_SCOPE_WINDOW_MONITORING,
        &allowed) == 0);
    assert(allowed == (KSP_SCOPE_INPUT_CONTROL
                       | KSP_SCOPE_WINDOW_MONITORING));
    assert(ksp_store_revoke(desktop, getuid(), identity.hash,
                            KSP_SCOPE_INPUT_CONTROL) == 0);
    assert(ksp_store_revoke(desktop, getuid(), identity.hash,
                            KSP_SCOPE_WINDOW_MONITORING) == 0);

    ksp_store_destroy(desktop);
    remove_directory(persistent);
    remove_directory(runtime);
    remove_directory(root);
    return 0;
}
