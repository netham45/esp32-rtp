#ifndef NETWORK_IN_H
#define NETWORK_IN_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// Network receiver functions
esp_err_t network_init(void);
esp_err_t restart_network(void);
esp_err_t network_update_port(void);
esp_err_t network_deinit(void);
void get_rtp_statistics(uint32_t *received, uint32_t *lost, float *loss_rate);

// Multicast functions
esp_err_t network_join_multicast(const char* multicast_ip, uint16_t port, uint32_t ssrc);
esp_err_t network_leave_multicast(void);
bool network_is_multicast_enabled(void);
void network_get_multicast_info(char* ip, uint16_t* port, uint32_t* ssrc);

// Stream configuration function
esp_err_t network_configure_stream(const char* dest_ip, const char* source_ip, uint16_t port);

#endif // NETWORK_IN_H