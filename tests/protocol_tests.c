#include "common.h"
#include "keysharp_desktop/protocol.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    char capability_names[160];
    char display_text[32];
    static const char zero_sha256[] =
        "0000000000000000000000000000000000000000000000000000000000000000";
    char app_hash[KSD_HASH_HEX_LENGTH + 1u];
    ksd_process_identity identity;
    uint64_t start_time = ksd_process_start_time(getpid());
    int system_executable = open("/usr/bin/env", O_RDONLY | O_CLOEXEC);

    assert(KSD_PROTOCOL_MAJOR == 1u);
    assert(KSD_PROTOCOL_MINOR == 2u);
    assert(strcmp(KSD_PROTOCOL_LABEL, "keysharp-desktop/session-v1") == 0);
    assert(KSD_CAP_SCREEN_CAPTURE == 1u);
    assert(KSD_CAP_WINDOW_MONITORING == 2u);
    assert(KSD_CAP_WINDOW_CONTROL == 4u);
    assert(KSD_CAP_AUDIO_CAPTURE == 8u);
    assert(KSD_CAP_CAMERA_CAPTURE == 16u);
    assert(KSD_CAP_CLIPBOARD_MONITORING == 32u);
    assert(KSD_CAP_ALL == 63u);
    assert((KSD_CAP_ALL & 64u) == 0u);
    assert(KSP_SCOPE_INPUT_MONITORING == 1u);
    assert(KSP_SCOPE_INPUT_CONTROL == 2u);
    assert(KSP_SCOPE_WINDOW_MONITORING == 4u);
    assert(KSP_SCOPE_WINDOW_CONTROL == 8u);
    assert(KSP_SCOPE_SCREEN_CAPTURE == 16u);
    assert(KSP_SCOPE_AUDIO_CAPTURE == 32u);
    assert(KSP_SCOPE_CAMERA_CAPTURE == 64u);
    assert(KSP_SCOPE_CLIPBOARD_MONITORING == 128u);
    assert(ksd_format_capability_names(KSD_CAP_ALL, capability_names,
                                       sizeof(capability_names)) == 0);
    assert(strcmp(capability_names,
        "Screen Capture, Window Monitoring, Window Control, Audio Capture, "
        "Camera Capture, Clipboard Monitoring") == 0);
    assert(ksd_format_capability_names(KSD_CAP_WINDOW_MONITORING
                                       | KSD_CAP_CAMERA_CAPTURE,
                                       capability_names,
                                       sizeof(capability_names)) == 0);
    assert(strcmp(capability_names,
                  "Window Monitoring, Camera Capture") == 0);
    assert(ksd_format_capability_names(0u, capability_names,
                                       sizeof(capability_names)) < 0);
    assert(ksd_format_capability_names(0x80000000u, capability_names,
                                       sizeof(capability_names)) < 0);
    assert(ksd_format_capability_names(KSD_CAP_SCREEN_CAPTURE,
                                       capability_names, 4u) < 0);
    assert(strcmp(KSD_APP_IDENTITY_DOMAIN, "org.keysharp.app-identity-v1") == 0);
    assert(sizeof(ksd_authority_request) == 16u);
    assert(sizeof(ksd_authority_response) == 16u);
    assert(KSD_AUTH_OP_LIST_UID == 4u);
    assert(KSD_AUTH_OP_REVOKE == 5u);
    assert(system_executable >= 0);
    assert(ksd_executable_path_is_protected(system_executable, "/usr/bin/env"));
    assert(!ksd_executable_path_is_protected(system_executable, "/tmp/env"));
    close(system_executable);

    ksd_sanitize_display_text("/tmp/line\nname\177", display_text,
                              sizeof(display_text));
    assert(strcmp(display_text, "/tmp/line?name?") == 0);
    assert(ksd_hash_app_identity(KSD_APP_IDENTITY_PATH_KIND,
                                 "/usr/bin/example-app", strlen("/usr/bin/example-app"),
                                 app_hash) == 0);
    assert(strcmp(app_hash,
        "a39558ae92a7f5227560ccf6e10e2941aeeceb8a3b608b4fea274f98dc41f1ae") == 0);
    assert(ksd_hash_app_identity(KSD_APP_IDENTITY_SHA256_KIND,
                                 zero_sha256, strlen(zero_sha256), app_hash) == 0);
    assert(strcmp(app_hash,
        "73cd7ab5e10d259a782b6e021af8326514447477af0358481ee31fc5fee7d434") == 0);
    assert(start_time != 0u);
    assert(ksd_identify_process(getpid(), getuid(), start_time, &identity) == 0);
    assert(identity.uid == getuid());
    assert(identity.pid == getpid());
    assert(identity.start_time == start_time);
    assert(strlen(identity.hash) == KSD_HASH_HEX_LENGTH);
    assert(identity.executable[0] == '/');
    puts("protocol tests passed");
    return 0;
}
