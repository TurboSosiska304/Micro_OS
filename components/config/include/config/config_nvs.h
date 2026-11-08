/*
 * Micro OS — the configuration in the NVS of the ESP32.
 *
 * The store keeps the configuration in its own namespace of the NVS, apart
 * from the records of the radio: the keys of the network are secrets of the
 * driver and are not files of /etc.
 */
#ifndef MICRO_OS_CONFIG_CONFIG_NVS_H
#define MICRO_OS_CONFIG_CONFIG_NVS_H

/*
 * Initialise the NVS and install it as the store of the configuration.
 * Returns 0 on success, a negative error when the NVS cannot be used.
 */
int config_nvs_register(void);

#endif /* MICRO_OS_CONFIG_CONFIG_NVS_H */
