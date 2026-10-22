# Phase 6 - The WiFi driver

The radio of the board is no longer only checked: the driver attaches the
station to a network, scans the networks around and remembers the network of
the last time in the NVS, so the board joins it on its own after a restart.

## What is there

* **The core** (`components/wifi/src/wifi.c`): the state machine of the
  station (`stopped`, `idle`, `connecting`, `connected`, `disconnected`,
  `failed`), the configuration of the network and the formatting of the
  status. It is plain C11 and knows nothing about the hardware.
* **The radio** (`components/wifi/src/wifi_esp32.c`): the backend that talks
  to the ESP32 through the driver of ESP-IDF. It starts the radio, sets the
  configuration of the station, scans and reads the address of the network.
* **The configuration** lives in the NVS (the namespace `microos`): the name
  and the key of the network. `wifi connect` remembers the network,
  `wifi forget` forgets it, and the station joins the remembered network
  right after the boot.
* **The command of the shell**: `wifi status`, `wifi scan`,
  `wifi connect <ssid> [key]`, `wifi join`, `wifi disconnect`,
  `wifi forget`.
* **The module of the hardware doctor** stays: the radio is `ok` when the
  network is up, `degraded` when the radio works but there is no network, and
  `fail` when the radio is off.

The decision about the shape of the driver is recorded in
[ADR 0003](../adr/0003-wifi-driver.md).

## The tests are about the properties

* **The state follows the radio.** The station is `stopped` before the start,
  `idle` after it, `connecting` after the request and `connected` when the
  radio reports the network.
* **A disconnection during the connection is a failure**, not a
  disconnection: the network was not there or the key was wrong.
* **The wrong arguments are refused**: an empty name, a name longer than 32
  characters and a connection without a started radio never reach the
  hardware.
* **The network is remembered.** What is saved comes back after a restart of
  the core, and `forget` erases it.
* **The scan reports the networks around** with the signal and the channel.

## The mistakes that cost time

* **The name of the network is not a string.** In the records of the driver
  of ESP-IDF it is 32 bytes without the terminating zero, and it is a
  `uint8_t[]`, not a `char[]`: the first version of the copy took a
  `const char *` and the compiler was right — the warning about the sign
  caught the bug before the flash.
* **The connection is asynchronous.** `esp_wifi_connect()` returns at once,
  the result comes later as an event. A driver that waits for the answer
  inside the call deadlocks the shell.
* **The scan blocks.** The scan of the radio takes a few seconds and the
  caller waits for it. The shell freezes for that time — that is the price
  of the simple implementation.
* **The NVS holds the calibration data of the radio as well.** A partition
  written by an older version of the format has to be erased and initialised
  again, otherwise the configuration of the network cannot be saved either.
* **The address of the network is not the address of the station.** The
  driver reports the address of the network in the format of the IP stack of
  lwIP, and the order of the bytes is the one of that stack, not the one of
  the host.

## What is worth remembering

* The radio is a table of functions: the driver can be tested on the host,
  and the backend stays thin.
* The state belongs to the core, the events belong to the radio. Every
  decision about the state is then testable.
* A driver that only checks the hardware is half a driver. The station that
  joins the network of the last time is what makes the rest of the roadmap
  (NTP, HTTP, MQTT) possible.
* The key of the network is typed on the console: it is visible on the screen
  of the board and in the log of the terminal. The shell has no quotes
  either, so a name with a space in it cannot be used. A network whose key
  must stay secret needs another way in.
