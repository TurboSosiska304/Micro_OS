/*
 * Micro OS — the NTP client as arithmetic.
 *
 * The packet and the calendar are pure numbers, so they are tested on the
 * host: the answer of a server is a canned set of bytes, and the date of
 * that moment is known in advance.
 */
#include "net/ntp.h"

#include <string.h>

void ntp_build_request(k_u8_t packet[NTP_PACKET_SIZE])
{
    memset(packet, 0, NTP_PACKET_SIZE);

    /* Leap indicator 0, version 4, mode 3 (the client of the server). */
    packet[0] = NTP_FIRST_BYTE;
}

static k_u32_t read_be32(const k_u8_t *p)
{
    return ((k_u32_t)p[0] << 24) | ((k_u32_t)p[1] << 16) | ((k_u32_t)p[2] << 8) | (k_u32_t)p[3];
}

int ntp_parse_response(const k_u8_t packet[NTP_PACKET_SIZE], k_u64_t *unix_seconds)
{
    int mode;
    k_u32_t seconds;

    if (packet == NULL || unix_seconds == NULL) {
        return -1;
    }

    mode = packet[0] & 0x07;
    if (mode != 4 && mode != 5) { /* a server or a broadcast, not a request */
        return -1;
    }

    /* Stratum 0 is a kiss-o'-death: the server says no, not the time. */
    if (packet[1] == 0) {
        return -1;
    }

    seconds = read_be32(packet + 40);
    if (seconds == 0) {
        return -1;
    }
    if ((k_u64_t)seconds < NTP_UNIX_OFFSET) {
        return -1;
    }

    *unix_seconds = (k_u64_t)seconds - NTP_UNIX_OFFSET;

    return 0;
}

/* The date of a day counted from 1970-01-01, the algorithm of Hinnant. */
static void civil_from_days(k_u64_t days, int *year, int *month, int *day)
{
    /* The algorithm counts the days from 0000-03-01. */
    k_u64_t z = days + 719468;
    k_u64_t era = z / 146097;
    k_u32_t doe = (k_u32_t)(z - era * 146097);
    k_u32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    k_u32_t y = (k_u32_t)(yoe + era * 400);
    k_u32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    k_u32_t mp = (5 * doy + 2) / 153;
    k_u32_t d = doy - (153 * mp + 2) / 5 + 1;
    k_u32_t m = mp < 10 ? mp + 3 : mp - 9;

    *year = (int)(y + (m <= 2));
    *month = (int)m;
    *day = (int)d;
}

int ntp_unix_to_datetime(k_u64_t unix_seconds, int *year, int *month, int *day, int *hour,
                         int *minute, int *second)
{
    k_u64_t days;
    k_u32_t rem;

    if (year == NULL || month == NULL || day == NULL || hour == NULL || minute == NULL ||
        second == NULL) {
        return -1;
    }

    days = unix_seconds / 86400;
    rem = (k_u32_t)(unix_seconds % 86400);

    civil_from_days(days, year, month, day);

    /* The clock of the board keeps the years of 1900..2099. */
    if (*year < 1970 || *year > 2099) {
        return -1;
    }

    *hour = (int)(rem / 3600);
    *minute = (int)((rem % 3600) / 60);
    *second = (int)(rem % 60);

    return 0;
}
