/*
 * Micro OS — the radio of the ESP32: the backend of the WiFi driver.
 *
 * The backend talks to the WiFi hardware of the ESP32 through the driver of
 * ESP-IDF and keeps the configuration in the NVS. It installs itself as the
 * radio of the portable core.
 */
#ifndef MICRO_OS_WIFI_WIFI_ESP32_H
#define MICRO_OS_WIFI_WIFI_ESP32_H

#include "wifi/wifi.h"

/*
 * Install the radio and the storage of the ESP32 and start the radio.
 * Returns 0 on success.
 */
int wifi_esp32_init(void);

/* The MAC address of the station: 6 bytes. */
int wifi_esp32_get_mac(uint8_t *mac);

#endif /* MICRO_OS_WIFI_WIFI_ESP32_H */
