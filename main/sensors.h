#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Callback signatures — called by sensor tasks when a new value is available */
typedef void (*temp_cb_t)(int16_t temp_centi_c);       /* 0.01 °C units, ZCL native */
typedef void (*occupancy_cb_t)(bool occupied);
typedef void (*analog_cb_t)(uint8_t duty_pct);          /* 0-100 % PWM duty cycle */

void sensors_init(temp_cb_t temp_cb, occupancy_cb_t occ_cb, analog_cb_t analog_cb);

/* Latest cached DS18B20 reading (centi-°C). Returns false if no valid reading
 * yet (sensor absent or last read failed). Avoids driving the 1-Wire bus from
 * callers, which would race the sensor task. */
bool sensors_temp_get_centi(int16_t *out);

/* Latest cached Add-on Analog IN duty cycle (0-100 %), or -1 if unavailable. */
int sensors_occupancy_duty(void);

/* Why the last DS18B20 attempt failed, for the management page and the log. */
const char *sensors_temp_error(void);

/* Run the 1-Wire diagnostics (pin ownership, loopback, presence scan, one read)
 * and write a human-readable report into out. Takes the Add-on bus lock and
 * blocks for roughly a second. Returns the number of bytes written. */
size_t sensors_ow_probe(char *out, size_t out_size);

/* Hunt for the Add-on data line: pull every candidate GPIO low and see which
 * one moves the 1-Wire RX pin, then try a single-pin (bidirectional
 * open-drain) DS18B20 read on the TX and RX pins. Takes the Add-on bus lock
 * and blocks for a few seconds. Returns the number of bytes written. */
size_t sensors_ow_pin_scan(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
