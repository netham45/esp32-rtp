#pragma once

/* Provide a minimal POSIX socket shim for ESP-IDF builds so live555 can use
 * its existing Unix-oriented includes without modification.
 */

#include <stdint.h>
#include "lwip/opt.h"

#if !defined(LWIP_IPV6) || !LWIP_IPV6
#undef LWIP_IPV6
#define LWIP_IPV6 1
#endif

#if !defined(LWIP_IPV6_MLD) || !LWIP_IPV6_MLD
#undef LWIP_IPV6_MLD
#define LWIP_IPV6_MLD 1
#endif

#ifndef CONFIG_LWIP_IPV6_NUM_ADDRESSES
#define CONFIG_LWIP_IPV6_NUM_ADDRESSES 3
#endif

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"

#ifndef INET6_ADDRSTRLEN
#define INET6_ADDRSTRLEN 46
#endif

#ifndef IPPROTO_IPV6
#define IPPROTO_IPV6 41
#endif
#ifndef IPV6_V6ONLY
#define IPV6_V6ONLY 27
#endif
#ifndef IPV6_JOIN_GROUP
#define IPV6_JOIN_GROUP 20
#endif
#ifndef IPV6_LEAVE_GROUP
#define IPV6_LEAVE_GROUP 21
#endif
