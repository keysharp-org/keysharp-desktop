#include "capture_worker.h"
#include "install_mode.h"

int main(int argc, char **argv)
{
    /* Before the worker does anything, and in particular before it drops to the
     * client's credentials. Everything it validates after that drop -- the
     * capture pipe, the spool -- belongs to the installation owner, which is
     * what this reads and what a live check would stop reporting the moment the
     * drop happened. */
    ksd_install_identity_latch();
    return ksd_capture_worker_main(argc, argv);
}
