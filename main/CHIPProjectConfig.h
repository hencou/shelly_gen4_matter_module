/*
 * CHIPProjectConfig.h — Override CHIP SDK defaults for Shelly 1 Gen4
 *
 * Vendor/product-namen die HomeAssistant (en andere controllers)
 * tonen in plaats van "TEST_VENDOR" / "TEST_PRODUCT".
 */

#pragma once

#define CHIP_DEVICE_CONFIG_DEVICE_VENDOR_NAME "Shelly"
#define CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_NAME "1 Gen4 Matter Switch"
#define CHIP_DEVICE_CONFIG_DEFAULT_DEVICE_HARDWARE_VERSION_STRING "v1.0"

/* Software version reported via Matter Basic Information cluster.
 * HA reads these to display the firmware version.  Keep in sync with
 * FW_VERSION in app_config.h. */
#define CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION 154
#define CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION_STRING "1.5.4"
