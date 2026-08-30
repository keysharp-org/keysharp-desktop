#include "polkit_result.h"

ksd_polkit_result ksd_polkit_result_from_exit(int exit_code)
{
    if (exit_code == 0)
        return KSD_POLKIT_GRANTED;
    if (exit_code == 1 || exit_code == 3)
        return KSD_POLKIT_DENIED;
    return KSD_POLKIT_UNAVAILABLE;
}
