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

/* Software version the OTA image is tagged with (tools/make-matter-ota.py).
 * On ESP32 the Basic Information cluster itself reports PROJECT_VER /
 * PROJECT_VER_NUMBER from CMakeLists.txt, so these must match that version --
 * the build fails if they don't. */
#define CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION 10603
#define CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION_STRING "1.6.3"
