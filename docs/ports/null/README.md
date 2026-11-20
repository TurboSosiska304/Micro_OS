# The null port: no machine at all

`ports/port_null` is a port that answers "there is no such thing" to every
call of the contract: no console, no flash, no store, no sockets, no tasks. It
is not a simulator - the host port is the machine the tests run on - and it is
never run for a purpose.

It exists for one check: `microos_null_probe` links the kernel **and every
portable component** against it, so a symbol that portable code takes from a
machine by accident (a driver, a POSIX call of a portable component, a piece
of the ESP-IDF) stops the build on a laptop. It is the proof of ADR 0004,
compiled by `make ci`.

It cannot catch a libc call: the probe is linked with the libc of the host,
and `printf` in portable code would link. What it catches is the contract of
the port itself, which is the part that has to be complete.
