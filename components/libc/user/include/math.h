/*
 * Micro OS — the numbers of a program.
 *
 * The functions are the ones of the C library and the values are doubles: the
 * library of a program always has floating point on x86-64, because the
 * compiler of the guest does not use `-mgeneral-regs-only` for a program that
 * asks for it (a program of a machine with no saved XMM state is one program
 * at a time, see docs/debts.md).
 *
 * The implementation is arithmetic of the machine and the x87 unit of the
 * processor, not the library of the host: the image of a program has no
 * `libm.so` under it. The accuracy of a function is the accuracy of the
 * instruction behind it: the square root of the hardware and the logarithm,
 * the exponential and the trigonometry of the x87 unit.
 */
#ifndef MICRO_OS_LIBC_USER_MATH_H
#define MICRO_OS_LIBC_USER_MATH_H

#include "ulibc.h"

#define HUGE_VAL (__builtin_huge_val())
#define INFINITY (__builtin_inff())
#define NAN (__builtin_nanf(""))

#define M_E 2.7182818284590452354
#define M_LOG2E 1.4426950408889634074
#define M_LOG10E 0.43429448190325182765
#define M_LN2 0.69314718055994530942
#define M_LN10 2.30258509299404568402
#define M_PI 3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#define M_PI_4 0.78539816339744830962
#define M_1_PI 0.31830988618379067154
#define M_2_PI 0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT2 1.41421356237309504880
#define M_SQRT1_2 0.70710678118654752440

#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)

double fabs(double value);
double floor(double value);
double ceil(double value);
double trunc(double value);
double round(double value);
double fmod(double left, double right);
double modf(double value, double *integer);
double frexp(double value, int *exponent);
double ldexp(double value, int exponent);
double sqrt(double value);
double pow(double base, double exponent);
double exp(double value);
double exp2(double value);
double log(double value);
double log2(double value);
double log10(double value);
double sin(double value);
double cos(double value);
double tan(double value);
double asin(double value);
double acos(double value);
double atan(double value);
double atan2(double y, double x);
double sinh(double value);
double cosh(double value);
double tanh(double value);

#endif /* MICRO_OS_LIBC_USER_MATH_H */
