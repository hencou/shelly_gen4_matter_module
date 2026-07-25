#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "script_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the Matter stack with dynamic endpoints based on script slot configs.
 * Each non-NONE slot type creates a corresponding Matter endpoint.
 *
 * @param slot_types  Array of slot types (SCRIPT_MAX_SLOTS elements)
 * @param num_slots   Number of elements in slot_types
 */
esp_err_t matter_start(const script_slot_type_t *slot_types, uint8_t num_slots);

/**
 * Get the Matter endpoint ID assigned to a script slot.
 * Returns 0 if the slot has no endpoint (type == NONE).
 */
uint16_t matter_get_slot_endpoint(uint8_t slot);

/* Command emit to bound nodes/groups (via Binding cluster). */
void matter_send_onoff_toggle(uint16_t local_endpoint_id);
void matter_send_onoff_on(uint16_t local_endpoint_id);
void matter_send_onoff_off(uint16_t local_endpoint_id);
void matter_send_level_move(uint16_t local_endpoint_id, bool up, uint8_t rate);
void matter_send_level_move_to_level(uint16_t local_endpoint_id, uint8_t level, uint16_t transition_ds);
void matter_send_level_stop(uint16_t local_endpoint_id);
void matter_send_color_temp_set(uint16_t local_endpoint_id, uint16_t mireds);
void matter_send_color_temp_move(uint16_t local_endpoint_id, bool warmer, uint16_t rate);
void matter_send_color_temp_stop(uint16_t local_endpoint_id);

/* Sensor attribute updates for Matter server clusters.
 * These iterate all dynamic endpoints to find matching types. */
void matter_update_temperature(int16_t centi_c);
void matter_update_occupancy(bool occupied);

/* Update an Electrical Power Measurement endpoint. ch 0 = 1PM/2PM channel A,
 * ch 1 = 2PM channel B. No-op when that endpoint was not created.
 * Units: volts, amperes, watts, hertz — scaled to the Matter mV/mA/mW/mHz
 * representation internally. */
void matter_update_power_ch(int ch, float voltage_v, float current_a,
                            float power_w, float frequency_hz);

/* Channel-0 convenience wrapper (1PM Gen4). */
void matter_update_power(float voltage_v, float current_a,
                         float power_w, float frequency_hz);

/* Update relay OnOff attribute (report to HA). ch = 0-based relay channel. */
void matter_update_relay_onoff(int ch, bool on);

/* Disable Thread radio so WiFi can use the 2.4 GHz radio exclusively. */
void matter_disable_thread(void);

/* Start the Thread connectivity watchdog. Recovers a node that gets stuck
 * DETACHED (unreachable over Thread) without a manual reboot: soft-toggles the
 * Thread interface after ~2 min, reboots after ~5 min. No-op while runtime
 * WiFi mode is active (Thread is then intentionally off). */
void matter_thread_watchdog_start(void);

/* Start the fallback SRP server controller on the Thread mesh.
 * Runs an SRP server (DNS-SD service discovery) only while no real border
 * router is present, so local device-to-device bindings keep working during a
 * TBR/HA outage. Yields to any border router the moment one appears — a BR has
 * the LAN advertising proxy this node lacks, and hogging the SRP anycast role
 * would otherwise hide the whole mesh from off-mesh controllers. */
esp_err_t matter_srp_server_start(void);

/* Spike: log the device's Thread unicast IPv6 addresses (OMR / mesh-local /
 * link-local) so the management page can be reached over IPv6/Thread from a
 * browser. matter_thread_addr_log_start() also re-logs every 15 s (the OMR
 * address only appears once a border router hands out its prefix). */
void matter_log_thread_addrs(void);
void matter_thread_addr_log_start(void);

/* Factory reset → wipes Matter NVS, leaves the fabric, reboot. */
void matter_factory_reset(void);

/* Commission mode → delete all commissioned fabrics via the CHIP FabricTable
 * API (partition-agnostic). Keeps scripts + WiFi config. Caller reboots after. */
void matter_delete_all_fabrics(void);

/* Diagnostics: dump the current CHIP Binding table into a human-readable
 * string. Returns the number of entries found. */
int matter_binding_dump(char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif
