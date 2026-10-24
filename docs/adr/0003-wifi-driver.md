# ADR 0003 - The WiFi driver: a portable core and a radio

* Status: accepted
* Date: 2026-10-22

> Russian version: [0003-wifi-driver_ru.md](0003-wifi-driver_ru.md)

## Context

The radio of the ESP32 is driven by the stack of ESP-IDF: the driver of the
SoC, the TCP/IP stack of lwIP and the DHCP client. Up to now the project only
had a *health check* of the radio (`drv_wifi` in the module registry): it
started the station and printed the MAC address, which is enough for the
report of the hardware doctor and nothing more.

The next steps of the roadmap need more than that: a client of HTTP or MQTT
and a client of NTP (so that the clock of the board stops being "no clock").
All of them need the same things — a station that is attached to a network,
an address, the signal and the list of the networks around. That is a driver,
not a health check.

The problem is the shape of the code. The component `components/drivers` is
ESP32 only: everything inside it talks to the hardware through ESP-IDF and
cannot be compiled for the host. A driver written that way is untestable: the
cycle "write the code, flash the board, look at the console" takes minutes,
and the logic of the driver (the states, the refusals, the configuration)
cannot be checked at all.

## Decision

The WiFi driver is split in two, the same way the kernel is split from the
port:

* **The core** (`components/wifi/src/wifi.c`, plain C11): the state machine
  of the station, the configuration of the network and the formatting of the
  status. It knows nothing about ESP-IDF and nothing about the hardware.
* **The radio** (`components/wifi/src/wifi_esp32.c`, ESP-IDF): the backend.
  It starts the hardware, associates with the access point, scans and reads
  the address. It keeps the configuration in the NVS.

The two parts talk through a table of functions:

```c
struct wifi_radio {
    int (*start)(void);
    int (*stop)(void);
    int (*connect)(const char *ssid, const char *pass);
    int (*disconnect)(void);
    int (*scan)(struct wifi_scan_entry *out, k_size_t max, k_size_t *count);
    int (*status)(struct wifi_status *out);
};
```

The core owns the state (`stopped`, `idle`, `connecting`, `connected`,
`disconnected`, `failed`); the radio only reports the events
(`WIFI_EVT_CONNECTED`, `WIFI_EVT_DISCONNECTED`, ...). The glue between the
two is `components/drivers/src/drv_wifi.c`: it registers the module of the
hardware doctor and the command of the shell.

The naming follows the other components of the project: the prefix of the
subsystem (`wifi_`) instead of the prefix of the drivers (`drv_`), because
the component is a subsystem of its own, like `kshell`, `mfs` or `procfs`.

### The station comes back on its own

A network that goes away is asked for again by the board: the router was
rebooted, and the station must not wait for a hand every time. The policy
belongs to the core and the timer belongs to the port:

* the core knows how many attempts were made, how long the wait between
  them grows and when the attempts are over. `wifi_reconnect_after_ms()`
  answers the port with the number of milliseconds to wait (0 means "do
  not try"), and `wifi_reconnect()` makes the attempt when the time has
  come. The count of the attempts is not restarted by the attempt itself:
  that is what stops a wrong key from being asked for all day. Only a
  connection asked for by hand (`wifi_connect()`) starts it again, and a
  station that was told to leave (`wifi_disconnect()`) does not come back
  on its own;
* the port makes a one-shot timer and calls `wifi_reconnect()` when it
  rings. A portable core has no clock to sleep on, and the events of the
  radio come from the event loop of ESP-IDF, which is a task of its own;
* the errors that mean that the key does not fit the network
  (`WIFI_REASON_AUTH_FAIL`, `WIFI_REASON_MIC_FAILURE`, the timeouts of the
  handshake) are not retried: the same answer would come back. The
  attempts are few (five) and the wait between them grows from one second
  to half a minute.

### The configuration is kept by the driver, not by ESP-IDF

The records of the network live in the NVS of the driver (`storage_save`),
and the storage of the driver of ESP-IDF is in the working memory
(`WIFI_STORAGE_RAM`). With its storage in the flash, every connection
would be a write to the flash of the board, and the flash has a limited
number of those. The threshold of the authentication of a network with a
key is at least WPA2, so an open point with the same name (the "evil
twin") does not take the station in.

## Consequences

**Good:**

* The logic of the driver is covered by the unit tests of the host
  (`tests/cases/test_wifi.c`): the fake radio plays the part of the hardware
  and the states, the refusals and the configuration are checked in
  milliseconds.
* The backend is thin: it only translates the calls of the core into the
  calls of ESP-IDF and the events into the events of the core.
* The services of the next phase (NTP, HTTP, MQTT) depend on the core, not
  on ESP-IDF: `wifi_get_status()` gives them the address they need.
* The configuration survives the restart of the board: the network is
  remembered in the NVS and the station joins it on its own.

**Bad:**

* The driver is a component of its own and not a part of `components/drivers`,
  so the build knows about it twice: the host build compiles the core, the
  ESP-IDF build compiles the core and the radio.
* The scan blocks the caller until the radio answers, and it has a
  deadline of ten seconds: a radio that went quiet is reported as an error
  instead of holding the shell for ever. A scan in the background would
  need a task and a queue, which is not worth it for a device with a
  135x240 screen.
* The core knows nothing about the details of the network: the roaming, the
  power save of the radio and the channel of the access point are the
  business of the backend.

## Alternatives

* **Everything inside `components/drivers`** — rejected: the driver cannot
  be compiled for the host, so none of its logic can be tested. The
  hardware doctor only tells that the radio answers, not that the state
  machine of the driver is right.
* **The events of ESP-IDF everywhere** — rejected: the state of the station
  would live in the backend, and every user of the driver would have to
  know that the driver of ESP-IDF is there.
* **A netlink-like socket** — rejected: the kernel of the project has no
  sockets yet, and the table of functions is enough for a radio.
