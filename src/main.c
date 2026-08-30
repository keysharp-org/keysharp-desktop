#include "roles.h"

#include <stdio.h>
#include <string.h>

static void usage(const char *program)
{
    fprintf(stderr,
        "usage: %s serve|authority|version|probe|status [capability]|grant [capability]|list|revoke ...\n",
        program);
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "serve") == 0)
        return ksd_serve_main(argc - 1, argv + 1);
    if (argc >= 2 && strcmp(argv[1], "authority") == 0)
        return ksd_authority_main(argc - 1, argv + 1);
    if (argc == 2 && strcmp(argv[1], "--info") == 0) {
        char *version_arguments[] = { argv[0], "version", NULL };
        return ksd_cli_main(2, version_arguments);
    }
    if (argc >= 2)
        return ksd_cli_main(argc, argv);
    usage(argv[0]);
    return 2;
}
