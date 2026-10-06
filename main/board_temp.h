#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Onboard NTC (board temperature) on the profile's ntc_gpio. Samples every
 * 10 s and switches every relay off when the board stays above
 * BOARD_TEMP_MAX_C for two readings while a relay is on. No-op on models
 * without an NTC. Call after matter_start(). */
#define BOARD_TEMP_MAX_C 95.0f

void board_temp_init(void);

/* Latest board temperature in °C. False when the model has no NTC or no valid
 * reading is available yet. */
bool board_temp_read(float *out);

/* True after the overheat protection switched the relays off, until the board
 * has cooled down 10 °C below the limit. */
bool board_temp_overheated(void);

#ifdef __cplusplus
}
#endif
