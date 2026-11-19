# Micro OS — ESP32 platform port (ESP-IDF component).

set(MICRO_OS_PORT_ESP32_SRCS
    "src/port_esp32.c"
    "src/blockdev_flash.c"
    "src/port_kv_nvs.c"
    "src/port_net_lwip.c"
    "src/wifi_radio.c"
)

idf_component_register(
    SRCS ${MICRO_OS_PORT_ESP32_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES kernel esp_timer freertos esp_driver_uart vfs esp_partition nvs_flash lwip
             esp_wifi esp_netif esp_event
    # The radio is the backend of the portable core: it fills the table of
    # `wifi` and reports its events back to it, and nothing of that appears
    # in the header of the port.
    PRIV_REQUIRES wifi
    # The port is the other half of the kernel contract: the kernel calls
    # k_port_* and the port calls back into the kernel (the scheduler, the
    # log). ESP-IDF links the components in dependency order and repeats
    # them to break cycles, but a member of an archive is only pulled when
    # something already needs it - so the files that only the kernel calls
    # (the store, the sockets) would be dropped and the link would fail.
    # The port is small; it is linked whole.
    WHOLE_ARCHIVE
)

    # The discipline of the host build: the sources of the project are
    # compiled with the extra warnings, so a mistake is a message at
    # build time and not a fault on the board.
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra)
