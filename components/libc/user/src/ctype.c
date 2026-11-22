/*
 * Micro OS — the character classes of a program, in the C locale.
 *
 * A program of this system has no libc of its own under it, so the library
 * carries the few classes its programs ask for. The answer is the one of the
 * C locale and not of a locale of a person: there is no locale on this
 * machine, and a table that changed with an environment would make a regexp
 * of a file mean one thing at the console and another in a test.
 *
 * The argument is an int because the callers are written that way (PCRE2
 * builds its tables from a loop of 0 to 255), and a value outside the byte
 * range - EOF is -1 - belongs to no class.
 */
#include "ulibc.h"

int isalpha(int letter)
{
    return (letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z');
}

int isdigit(int letter)
{
    return letter >= '0' && letter <= '9';
}

int isalnum(int letter)
{
    return isalpha(letter) || isdigit(letter);
}

int isspace(int letter)
{
    return letter == ' ' || letter == '\t' || letter == '\n' || letter == '\v' || letter == '\f' ||
           letter == '\r';
}

int isupper(int letter)
{
    return letter >= 'A' && letter <= 'Z';
}

int islower(int letter)
{
    return letter >= 'a' && letter <= 'z';
}

int isprint(int letter)
{
    return letter >= 0x20 && letter <= 0x7e;
}

int isgraph(int letter)
{
    return letter >= 0x21 && letter <= 0x7e;
}

int ispunct(int letter)
{
    return isgraph(letter) && !isalnum(letter);
}

int iscntrl(int letter)
{
    return (letter >= 0 && letter < 0x20) || letter == 0x7f;
}

int isxdigit(int letter)
{
    return isdigit(letter) || (letter >= 'a' && letter <= 'f') || (letter >= 'A' && letter <= 'F');
}

int isblank(int letter)
{
    return letter == ' ' || letter == '\t';
}

int tolower(int letter)
{
    return isupper(letter) ? letter - 'A' + 'a' : letter;
}

int toupper(int letter)
{
    return islower(letter) ? letter - 'a' + 'A' : letter;
}
