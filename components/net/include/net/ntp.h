/*
 * Micro OS — the NTP client, as arithmetic.
 *
 * A request is 48 bytes: the first one carries the version and the mode, the
 * transmit timestamp is left at zero (we only want the clock of the server).
 * The answer carries the time of the server in its transmit timestamp, in
 * seconds since 1900; the epoch of Unix is 2208988800 seconds later.
 *
 * The conversion to a calendar date is the same for every era, so the parser
 * accepts the stamps of this century and of the next one until the year
 * 2036, when the 32 bits of the seconds field turn over.
 */
#ifndef MICRO_OS_NET_NTP_H
#define MICRO_OS_NET_NTP_H

#include "kernel/types.h"

#define NTP_PACKET_SIZE 48
#define NTP_PORT 123

/* The seconds from the epoch of NTP (1900) to the epoch of Unix (1970). */
#define NTP_UNIX_OFFSET 2208988800ULL

/* The fields of the first byte of the request: version 4, mode 3 (client). */
#define NTP_FIRST_BYTE 0x23

/* The request: a header and three empty timestamps. */
void ntp_build_request(k_u8_t packet[NTP_PACKET_SIZE]);

/*
 * The time of the server from its answer. The last eight bytes of the
 * answer are the moment the server sent it; a kiss-o'-death (stratum 0) or
 * a zero stamp is not a time, and the answer is a negative code then.
 */
int ntp_parse_response(const k_u8_t packet[NTP_PACKET_SIZE], k_u64_t *unix_seconds);

/*
 * The calendar of the moment. Years outside 1970..2099 are refused: the
 * clock of the board keeps a century and a year byte, and a date outside
 * that window would be a lie.
 */
int ntp_unix_to_datetime(k_u64_t unix_seconds, int *year, int *month, int *day, int *hour,
                         int *minute, int *second);

#endif /* MICRO_OS_NET_NTP_H */
