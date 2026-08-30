#ifndef KEYSHARP_DESKTOP_POLKIT_RESULT_H
#define KEYSHARP_DESKTOP_POLKIT_RESULT_H

typedef enum ksd_polkit_result {
    KSD_POLKIT_GRANTED = 0,
    KSD_POLKIT_DENIED = 1,
    KSD_POLKIT_UNAVAILABLE = -1,
} ksd_polkit_result;

ksd_polkit_result ksd_polkit_result_from_exit(int exit_code);

#endif
