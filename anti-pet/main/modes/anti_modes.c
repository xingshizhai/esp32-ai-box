#include "anti_modes.h"

#include <stddef.h>

const app_role_mode_t *anti_modes_get(int *count)
{
    static app_role_mode_t modes[4];
    static bool initialized;
    if (!initialized) {
        modes[0] = *anti_mode_benchmark();
        modes[1] = *anti_mode_casual();
        modes[2] = *anti_mode_defect_hunter();
        modes[3] = *anti_mode_pressure();
        initialized = true;
    }
    if (count != NULL) {
        *count = 4;
    }
    return modes;
}
