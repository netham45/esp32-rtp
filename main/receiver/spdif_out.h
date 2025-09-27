/*

*/
#include <stdint.h>
#include <sys/types.h>
#include "esp_err.h"

/*
 * initialize S/PDIF driver
 *   rate: sampling rate, 44100Hz, 48000Hz etc.
 *   returns ESP_OK on success, or error code on failure
 */ 
esp_err_t spdif_init(int rate);

/*
 * send PCM data to S/PDIF transmitter
 *   src: pointer to 16bit PCM stereo data
 *   size: number of data bytes
 */
void spdif_write(const void *src, size_t size);

/*
 * change sampling rate
 *   rate: sampling rate, 44100Hz, 48000Hz etc.
 *   returns ESP_OK on success, or error code on failure
 */ 
esp_err_t spdif_set_sample_rates(int rate);
