/*
 * Micro OS — the ASCII classes of a program.
 *
 * A table would be a kilobyte of rodata per program for a test of six
 * comparisons; the functions are the table, written out. The classes are the
 * ones of the C locale and nothing else: this system has no locale, and a
 * byte of the console is a letter only when ASCII says so.
 */
#include <ctype.h>

int isascii(int byte)
{
    return (unsigned)byte < 128u;
}

int isdigit(int byte)
{
    return byte >= '0' && byte <= '9';
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

int isalnum(int byte)
{
    return isalpha(byte) || isdigit(byte);
}

int isspace(int byte)
{
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r' || byte == '\v' ||
           byte == '\f';
}

int isblank(int byte)
{
    return byte == ' ' || byte == '\t';
}

int iscntrl(int byte)
{
    return ((unsigned)byte < 32u) || byte == 127;
}

int isprint(int byte)
{
    return byte >= 32 && byte < 127;
}

int isgraph(int byte)
{
    return byte > 32 && byte < 127;
}

int ispunct(int byte)
{
    return isgraph(byte) && !isalnum(byte);
}

int isxdigit(int byte)
{
    return isdigit(byte) || (byte >= 'a' && byte <= 'f') || (byte >= 'A' && byte <= 'F');
}

int tolower(int byte)
{
    return isupper(byte) ? byte + ('a' - 'A') : byte;
}

int toupper(int byte)
{
    return islower(byte) ? byte - ('a' - 'A') : byte;
}
