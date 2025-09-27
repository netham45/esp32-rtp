#ifndef NETWORK_IN_H
#define NETWORK_IN_H

#include "esp_err.h"
#include <stdint.h>

// Network receiver functions
esp_err_t network_init(void);
esp_err_t restart_network(void);
esp_err_t network_update_port(void);
esp_err_t network_deinit(void);
void get_rtp_statistics(uint32_t *received, uint32_t *lost, float *loss_rate);

#endif // NETWORK_IN_H