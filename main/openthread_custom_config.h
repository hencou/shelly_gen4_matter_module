/* Custom OpenThread config (CONFIG_OPENTHREAD_HEADER_CUSTOM).
 *
 * Included at the very top of openthread-core-esp32x-ftd-config.h, ahead of its
 * own `#ifndef ... #define` defaults, so these definitions win.
 *
 * ESP-IDF keeps OPENTHREAD_CONFIG_SRP_SERVER_ENABLE inside an
 * `#if CONFIG_OPENTHREAD_BORDER_ROUTER` block, but the sources themselves
 * (openthread/src/core/net/srp_server.cpp, dnssd_server.cpp) are compiled in
 * every FTD build. Defining the macros here therefore yields the SRP/DNS-SD
 * server without CONFIG_OPENTHREAD_BORDER_ROUTER=y — which is what matters,
 * because that Kconfig also links the prebuilt libopenthread_br.a and the ESP
 * border-router glue, and those hang a Thread-only device (no infra/backbone
 * netif) right after Thread attach.
 *
 * The server's three hard requirements are already met unconditionally by the
 * ESP config: TMF_NETDATA_SERVICE_ENABLE, NETDATA_PUBLISHER_ENABLE (derived
 * from SRP_SERVER_ENABLE) and ECDSA_ENABLE. */
#pragma once

#define OPENTHREAD_CONFIG_SRP_SERVER_ENABLE 1
#define OPENTHREAD_CONFIG_DNSSD_SERVER_ENABLE 1
