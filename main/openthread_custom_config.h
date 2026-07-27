/* Custom OpenThread config (CONFIG_OPENTHREAD_HEADER_CUSTOM).
 *
 * Included at the very top of openthread-core-esp32x-ftd-config.h, before its
 * `#ifndef ... #define default` block, so these macros win.
 *
 * We enable CONFIG_OPENTHREAD_BORDER_ROUTER=y only to get the SRP server and
 * DNS-SD server (so a module can answer Thread DNS-SD resolves when no border
 * router is present). But that Kconfig also turns on Border Routing, NAT64 and
 * the Backbone Router — features that require an infrastructure/backbone netif.
 * This device is Thread-only (native 802.15.4 radio, no backbone), so those
 * features have no interface to run on and hang the stack right after the
 * Thread netif attaches. Disable them; keep only SRP + DNS-SD server. */
#pragma once

#define OPENTHREAD_CONFIG_BORDER_ROUTING_ENABLE 0
#define OPENTHREAD_CONFIG_NAT64_BORDER_ROUTING_ENABLE 0
#define OPENTHREAD_CONFIG_BACKBONE_ROUTER_ENABLE 0
#define OPENTHREAD_CONFIG_DNS_UPSTREAM_QUERY_ENABLE 0

/* Kept enabled (esp32x defaults): SRP server + DNS-SD server. */
#define OPENTHREAD_CONFIG_SRP_SERVER_ENABLE 1
#define OPENTHREAD_CONFIG_DNSSD_SERVER_ENABLE 1
