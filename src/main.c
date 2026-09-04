#include "install_mode.h"
#include "roles.h"
#include <string.h>

int main(int argc, char **argv)
{
    /* Before any subcommand runs, and so before anything can change what this
     * process is. Every ownership check in the service is answered from what is
     * read here. */
    ksd_install_identity_latch();
    if (argc >= 2 && strcmp(argv[1], "daemon") == 0)
        return ksd_daemon_main(argc - 1, argv + 1);
    if (argc >= 2 && strcmp(argv[1], "authority-daemon") == 0)
        return ksd_authority_main(argc - 1, argv + 1);
    if (argc >= 2 && strcmp(argv[1], "session-query") == 0)
        return ksd_session_query_main(argc - 1, argv + 1);
    return ksd_cli_main(argc, argv);
}
