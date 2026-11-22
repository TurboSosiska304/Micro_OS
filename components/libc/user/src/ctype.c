/*
 * Micro OS — the classes of a byte, in the C locale.
 *
 * A byte above 127 is not a letter of the C locale and not a space, so the
 * table is written for the 128 bytes of ASCII and the functions answer "no"
 * for the rest. The argument is taken as unsigned, at the library does: a
 * `char` of 0xE9 passed as an int is a byte of the program and not a
 * negative number.
 */
#include "ulibc.h"

#include <ctype.h>

/*
 * The classes above 127 are none: a byte of a file is a byte, and this system
 * has one locale. `isascii` is the question of the high bit and not of a
 * class.
 */
int isascii(int byte)
{
    return (unsigned)byte < 128u;
}

int isupper(int byte)
{
    return byte >= 'A' && byte <= 'Z';
}

int islower(int byte)
{
    return byte >= 'a' && byte <= 'z';
}

int isalpha(int byte)
{
    return isupper(byte) || islower(byte);
}

int isdigit(int byte)
{
    return byte >= '0' && byte <= '9';
}

int isalnum(int byte)
{
    return isalpha(byte) || isdigit(byte);
}

int isxdigit(int byte)
{
    return isdigit(byte) || (byte >= 'a' && byte <= 'f') || (byte >= 'A' && byte <= 'F');
}

int isspace(int byte)
{
    return byte == ' ' || (byte >= '\t' && byte <= '\r');
}

int isblank(int byte)
{
    return byte == ' ' || byte == '\t';
}

int iscntrl(int byte)
{
    return (byte >= 0 && byte < 0x20) || byte == 0x7F;
}

int isprint(int byte)
{
    return byte >= 0x20 && byte < 0x7F;
}

int isgraph(int byte)
{
    return byte > 0x20 && byte < 0x7F;
}

int ispunct(int byte)
{
    return isgraph(byte) && !isalnum(byte);
}

int tolower(int byte)
{
    return isupper(byte) ? byte - 'A' + 'a' : byte;
}

int toupper(int byte)
{
    return islower(byte) ? byte - 'a' + 'A' : byte;
}
