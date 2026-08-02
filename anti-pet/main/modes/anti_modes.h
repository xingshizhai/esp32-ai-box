#pragma once

#include "app_role.h"

/* Registry only: each mode lives in its own translation unit so behavior can
 * grow independently without turning app_role.c into a mode switchboard. */
const app_role_mode_t *anti_modes_get(int *count);

const app_role_mode_t *anti_mode_benchmark(void);
const app_role_mode_t *anti_mode_casual(void);
const app_role_mode_t *anti_mode_defect_hunter(void);
const app_role_mode_t *anti_mode_pressure(void);
