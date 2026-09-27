/* lwIP IPv6 route hook: keep Thread traffic on the Thread netif while WiFi
 * runs next to it.
 *
 * When the WiFi STA gets an address, esp_netif makes it the lwIP default
 * netif (route_prio 100 vs 15 for OpenThread). Any IPv6 destination without
 * an explicit route then goes to WiFi -- including the Matter group multicast
 * (ff35::/16 with the fabric prefix) and off-mesh Thread peers reached
 * through a border router (ULA OMR prefix). WiFi has no source address for
 * those, so udp_sendto() fails with ERR_RTE (CHIP 0x3000004) and both group
 * bindings and CASE replies silently die as soon as WiFi is on.
 *
 * lwIP consults this hook before its own prefix/router/default lookups (only
 * for destinations that are not link-local scoped, those are already bound to
 * a netif by lwIP itself). Multicast beyond link scope and ULA (fc00::/7)
 * destinations are steered to the Thread netif when it is up; everything else
 * (global unicast, IPv4) keeps following the normal default route. */
#include "sdkconfig.h"

#include "lwip/netif.h"
#include "lwip/ip6_addr.h"
#include "lwip_default_hooks.h"

#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_openthread_netif_glue.h"

struct netif *lwip_hook_ip6_route(const ip6_addr_t *src, const ip6_addr_t *dest)
{
    (void)src;

    bool thread_dest = ip6_addr_ismulticast(dest) ||
                       (ip6_addr_isuniquelocal(dest));
    if (!thread_dest) return NULL;

    esp_netif_t *ot_esp_netif = esp_openthread_get_netif();
    if (ot_esp_netif == NULL) return NULL;

    struct netif *ot = (struct netif *)esp_netif_get_netif_impl(ot_esp_netif);
    if (ot == NULL || !netif_is_up(ot) || !netif_is_link_up(ot)) return NULL;

    return ot;
}
