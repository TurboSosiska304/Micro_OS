/*
 * Micro OS — the arithmetic of a program with a point in it.
 *
 * The image of a program has no `libm.so` under it and the host has no part
 * in the picture, so every function here is the arithmetic of the machine:
 * the square root and the trigonometric, logarithmic and exponential
 * functions are the instructions of the x87 unit, and the rest (the rounding
 * of a value, the split of an exponent, the power of two) is a handful of
 * integer operations on the bits of a double.
 *
 * The x87 unit is not saved by the scheduler of the port, and this file is
 * the reason the flags of a floating-point program leave `-mgeneral-regs-only`
 * out: the state of the unit and of the XMM registers belongs to the one
 * program that runs at a time, which is what `execve` of this system is
 * (`docs/debts.md`). The kernel and the other programs never touch either.
 *
 * The accuracy is the one of the instruction behind the function. The
 * logarithm of x87, on which `pow`, `exp` and `log` rest, is accurate to the
 * last bit of the 64-bit mantissa of the unit, and the trigonometric
 * functions are accurate for the arguments a program of this machine uses.
 * A correctly rounded library is a debt of the platform, not a promise of
 * this file.
 */
#include "ulibc.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

/* --- the x87 unit ------------------------------------------------------- */

static double x87_sin(double value)
{
    double result;

    __asm__ volatile("fldl %1\n\t"
                     "fsin\n\t"
                     "fstpl %0"
                     : "=m"(result)
                     : "m"(value)
                     : "st");

    return result;
}

static double x87_cos(double value)
{
    double result;

    __asm__ volatile("fldl %1\n\t"
                     "fcos\n\t"
                     "fstpl %0"
                     : "=m"(result)
                     : "m"(value)
                     : "st");

    return result;
}

static double x87_tan(double value)
{
    double result;

    /* fptan leaves the tangent in st(1) and a one in st(0). */
    __asm__ volatile("fldl %1\n\t"
                     "fptan\n\t"
                     "fstp %%st(0)\n\t"
                     "fstpl %0"
                     : "=m"(result)
                     : "m"(value)
                     : "st");

    return result;
}

static double x87_sqrt(double value)
{
    double result;

    __asm__ volatile("fldl %1\n\t"
                     "fsqrt\n\t"
                     "fstpl %0"
                     : "=m"(result)
                     : "m"(value)
                     : "st");

    return result;
}

static double x87_atan2(double y, double x)
{
    double result;

    __asm__ volatile("fldl %1\n\t"
                     "fldl %2\n\t"
                     "fpatan\n\t"
                     "fstpl %0"
                     : "=m"(result)
                     : "m"(y), "m"(x)
                     : "st");

    return result;
}

static double x87_log2(double value)
{
    double result;

    __asm__ volatile("fld1\n\t"
                     "fldl %1\n\t"
                     "fyl2x\n\t"
                     "fstpl %0"
                     : "=m"(result)
                     : "m"(value)
                     : "st");

    return result;
}

static double x87_exp2m1(double value)
{
    double result;

    __asm__ volatile("fldl %1\n\t"
                     "f2xm1\n\t"
                     "fstpl %0"
                     : "=m"(result)
                     : "m"(value)
                     : "st");

    return result;
}

/*
 * The remainder of the division, with the rounding of x87: the instruction
 * reports through C2 that the quotient it could compute in one step was
 * partial, and the loop asks for the rest. The result has the sign of the
 * dividend and an absolute value smaller than the divisor.
 */
static double x87_remainder(double dividend, double divisor)
{
    double result;

    __asm__ volatile("fldl %1\n\t"
                     "fldl %2\n\t"
                     "1:\n\t"
                     "fprem\n\t"
                     "fnstsw %%ax\n\t"
                     "testb $4, %%ah\n\t"
                     "jnz 1b\n\t"
                     "fstpl %0\n\t"
                     "fstp %%st(0)"
                     : "=m"(result)
                     : "m"(divisor), "m"(dividend)
                     : "ax", "st");

    return result;
}

/* --- the shape of a double ---------------------------------------------- */

static uint64_t bits_of(double value)
{
    uint64_t bits;

    memcpy(&bits, &value, sizeof(bits));

    return bits;
}

static double value_of(uint64_t bits)
{
    double value;

    memcpy(&value, &bits, sizeof(value));

    return value;
}

static double absolute(double value)
{
    return value_of(bits_of(value) & 0x7FFFFFFFFFFFFFFFull);
}

double fabs(double value)
{
    return absolute(value);
}

/*
 * The rounding of a value to a whole one. A double of 2^52 or more has no
 * fraction left: every bit of it is above the point, and the value is its own
 * floor and ceiling. Below that number the cast is exact and an adjustment of
 * one step in the right direction is the whole of the operation.
 */
double trunc(double value)
{
    if (!isfinite(value) || absolute(value) >= 4503599627370496.0) {
        return value;
    }

    return (double)(long long)value;
}

double floor(double value)
{
    double whole;

    if (!isfinite(value) || absolute(value) >= 4503599627370496.0) {
        return value;
    }

    whole = (double)(long long)value;

    return (whole > value) ? whole - 1.0 : whole;
}

double ceil(double value)
{
    double whole;

    if (!isfinite(value) || absolute(value) >= 4503599627370496.0) {
        return value;
    }

    whole = (double)(long long)value;

    return (whole < value) ? whole + 1.0 : whole;
}

double round(double value)
{
    if (!isfinite(value) || absolute(value) >= 4503599627370496.0) {
        return value;
    }

    return (value >= 0.0) ? floor(value + 0.5) : ceil(value - 0.5);
}

double fmod(double left, double right)
{
    if (right == 0.0 || isnan(left) || isnan(right) || isinf(left)) {
        return NAN;
    }
    if (isinf(right)) {
        return left;
    }

    return x87_remainder(left, right);
}

double modf(double value, double *integer)
{
    double whole = trunc(value);

    if (integer != NULL) {
        *integer = whole;
    }

    return (value == whole) ? 0.0 : value - whole;
}

/*
 * The exponent of a value and its mantissa in [0.5, 1). A subnormal has no
 * exponent in its field, so it is scaled by 2^64 first and the count is taken
 * back from the answer: the two cases are the same arithmetic one step apart.
 */
double frexp(double value, int *exponent)
{
    uint64_t bits = bits_of(value);
    int biased = (int)((bits >> 52) & 0x7FFu);
    int extra = 0;

    if (biased == 0x7FF || value == 0.0) {
        if (exponent != NULL) {
            *exponent = 0;
        }
        return value;
    }

    if (biased == 0) {
        value *= 18446744073709551616.0; /* 2^64 */
        extra = 64;
        bits = bits_of(value);
        biased = (int)((bits >> 52) & 0x7FFu);
    }

    if (exponent != NULL) {
        *exponent = biased - 1022 - extra;
    }

    bits = (bits & 0x800FFFFFFFFFFFFFull) | (1022ull << 52);

    return value_of(bits);
}

/*
 * The value times two to an exponent. The steps are the exponent of the
 * double itself: 1024 at a time, and the last one exactly, so no value that
 * the format can hold is lost to a multiplication of its own.
 */
double ldexp(double value, int exponent)
{
    if (!isfinite(value) || value == 0.0) {
        return value;
    }

    while (exponent > 1023) {
        value *= 0x1p1023;
        exponent -= 1023;
        if (isinf(value)) {
            return value;
        }
    }
    while (exponent < -1022) {
        value *= 0x1p-1022;
        exponent += 1022;
        if (value == 0.0) {
            return value;
        }
    }

    return value * value_of((uint64_t)(1023 + exponent) << 52);
}

double sqrt(double value)
{
    if (value < 0.0) {
        errno = EDOM;
        return NAN;
    }

    return x87_sqrt(value);
}

/* --- the exponential family -------------------------------------------- */

/*
 * Two to a real power. x87 computes 2^f - 1 for a fraction f of [-1, 1] and
 * scales by a whole power of two, so the value is split into the two parts
 * the instructions ask for.
 */
static double exp2_value(double exponent)
{
    double whole;
    double fraction;
    double result;

    if (isnan(exponent)) {
        return exponent;
    }
    if (exponent > 1024.0) {
        errno = ERANGE;
        return HUGE_VAL;
    }
    if (exponent < -1075.0) {
        errno = ERANGE;
        return 0.0;
    }

    whole = (exponent >= 0.0) ? floor(exponent + 0.5) : ceil(exponent - 0.5);
    fraction = exponent - whole;

    result = 1.0 + x87_exp2m1(fraction);

    return ldexp(result, (int)whole);
}

double exp2(double value)
{
    return exp2_value(value);
}

double exp(double value)
{
    return exp2_value(value * M_LOG2E);
}

double log2(double value)
{
    if (value < 0.0) {
        errno = EDOM;
        return NAN;
    }
    if (value == 0.0) {
        errno = ERANGE;
        return -HUGE_VAL;
    }

    return x87_log2(value);
}

double log(double value)
{
    return log2(value) * M_LN2;
}

double log10(double value)
{
    return log2(value) * M_LOG10E;
}

double pow(double base, double exponent)
{
    int negative = 0;

    if (exponent == 0.0) {
        return 1.0;
    }
    if (isnan(base) || isnan(exponent)) {
        return NAN;
    }
    if (base == 0.0) {
        if (exponent < 0.0) {
            errno = ERANGE;
            return HUGE_VAL;
        }
        return 0.0;
    }
    if (base < 0.0) {
        /* A negative base has a real power only for a whole exponent, and
         * the sign of the answer follows the parity of that exponent. */
        if (exponent != floor(exponent)) {
            errno = EDOM;
            return NAN;
        }
        negative = (((long long)exponent) % 2) != 0;
        base = -base;
    }

    {
        double result = exp2_value(exponent * log2(base));

        return negative ? -result : result;
    }
}

/* --- the trigonometric family ------------------------------------------ */

/* The argument brought into [-pi, pi]: x87's own reduction is exact for the
 * numbers of the format, and the unit is asked for the rest of the turn. */
static double reduce_turn(double value)
{
    return x87_remainder(value, 2.0 * M_PI);
}

double sin(double value)
{
    if (!isfinite(value)) {
        return NAN;
    }

    return x87_sin(reduce_turn(value));
}

double cos(double value)
{
    if (!isfinite(value)) {
        return NAN;
    }

    return x87_cos(reduce_turn(value));
}

double tan(double value)
{
    double reduced;

    if (!isfinite(value)) {
        return NAN;
    }

    reduced = reduce_turn(value);
    if (absolute(reduced - M_PI_2) < 1e-15) {
        return HUGE_VAL;
    }

    return x87_tan(reduced);
}

double atan2(double y, double x)
{
    if (x == 0.0 && y == 0.0) {
        return (signbit(x) && signbit(y)) ? -M_PI : 0.0;
    }

    return x87_atan2(y, x);
}

double atan(double value)
{
    return x87_atan2(value, 1.0);
}

double asin(double value)
{
    if (value < -1.0 || value > 1.0) {
        errno = EDOM;
        return NAN;
    }

    return x87_atan2(value, sqrt(1.0 - value * value));
}

double acos(double value)
{
    if (value < -1.0 || value > 1.0) {
        errno = EDOM;
        return NAN;
    }

    return x87_atan2(sqrt(1.0 - value * value), value);
}

double sinh(double value)
{
    double positive = exp(value);

    return (positive - 1.0 / positive) / 2.0;
}

double cosh(double value)
{
    double positive = exp(value);

    return (positive + 1.0 / positive) / 2.0;
}

double tanh(double value)
{
    double square;

    if (value > 20.0) {
        return 1.0;
    }
    if (value < -20.0) {
        return -1.0;
    }

    square = exp(2.0 * value);

    return (square - 1.0) / (square + 1.0);
}
