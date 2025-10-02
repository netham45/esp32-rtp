#pragma once
/*
 * Centralized build-time configuration shim.
 * - Always includes sdkconfig.h.
 * - Provides safe fallback defaults when CONFIG_* may be missing for static analyzers.
 * - Do NOT override values when sdkconfig.h defines them.
 */
#include "sdkconfig.h"

/* Audio core */
#ifndef CONFIG_PCM_CHUNK_SIZE
#define CONFIG_PCM_CHUNK_SIZE 1152
#endif

/* Networking (RTP/SAP) */
#ifndef CONFIG_RTP_PORT
#define CONFIG_RTP_PORT 4010
#endif
#ifndef CONFIG_SAP_MULTICAST_ADDR
#define CONFIG_SAP_MULTICAST_ADDR "224.2.127.254"
#endif
#ifndef CONFIG_SAP_PULSEAUDIO_ADDR
#define CONFIG_SAP_PULSEAUDIO_ADDR "224.0.0.56"
#endif
#ifndef CONFIG_SAP_PORT
#define CONFIG_SAP_PORT 9875
#endif
#ifndef CONFIG_SAP_MAX_ANNOUNCEMENTS
#define CONFIG_SAP_MAX_ANNOUNCEMENTS 10
#endif
#ifndef CONFIG_SAP_ANNOUNCEMENT_TIMEOUT_SEC
#define CONFIG_SAP_ANNOUNCEMENT_TIMEOUT_SEC 120
#endif
#ifndef CONFIG_SAP_CLEANUP_INTERVAL_SEC
#define CONFIG_SAP_CLEANUP_INTERVAL_SEC 30
#endif
#ifndef CONFIG_SAP_BUFFER_SIZE
#define CONFIG_SAP_BUFFER_SIZE 1024
#endif

/* mDNS */
#ifndef CONFIG_MDNS_MAX_DEVICES
#define CONFIG_MDNS_MAX_DEVICES 16
#endif

/* Visualizer (commonly referenced sizes) */
#ifndef CONFIG_VIZ_CHUNK_SIZE
#define CONFIG_VIZ_CHUNK_SIZE 1152
#endif
#ifndef CONFIG_VIZ_RING_SIZE
#define CONFIG_VIZ_RING_SIZE (CONFIG_VIZ_CHUNK_SIZE * 4)
#endif