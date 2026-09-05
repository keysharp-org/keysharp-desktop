#include <keysharp_desktop/client.h>
#include <stdio.h>

int main(void)
{
    ksd_connect_options options;
    ksd_service_info info;
    ksd_error error;
    ksd_connection *connection = NULL;
    ksd_string state;
    int result = 1;

    ksd_connect_options_init(&options);
    ksd_service_info_init(&info);
    ksd_error_init(&error);
    ksd_string_init(&state);
    if (ksd_connect(&options, &connection, &info, &error) != KSD_STATUS_OK) {
        fprintf(stderr, "connect: %s\n", error.message);
        return 1;
    }
    if ((info.available_operations & KSD_OPERATION_KEYBOARD_STATE) == 0u) {
        fputs("this backend does not expose its keyboard keymap\n", stderr);
    } else if (ksd_keyboard_state_json(connection, &state, &error)
               == KSD_STATUS_OK) {
        puts(state.data);
        result = 0;
    } else {
        fprintf(stderr, "keyboard state: %s\n", error.message);
    }
    ksd_string_clear(&state);
    ksd_disconnect(connection);
    return result;
}
