#include "polkit_result.h"

#include <assert.h>

int main(void)
{
    assert(ksd_polkit_result_from_exit(0) == KSD_POLKIT_GRANTED);
    assert(ksd_polkit_result_from_exit(1) == KSD_POLKIT_DENIED);
    assert(ksd_polkit_result_from_exit(3) == KSD_POLKIT_DENIED);
    assert(ksd_polkit_result_from_exit(2) == KSD_POLKIT_UNAVAILABLE);
    assert(ksd_polkit_result_from_exit(126) == KSD_POLKIT_UNAVAILABLE);
    assert(ksd_polkit_result_from_exit(127) == KSD_POLKIT_UNAVAILABLE);
    return 0;
}
