/*
 * Micro OS — the non-local exit of a program.
 *
 * The library of Lua is built out of long jumps: a syntax error, a runtime
 * error and the end of a `pcall` are one `longjmp` into the state of the
 * interpreter. The C standard leaves the shape of `jmp_buf` to the machine,
 * and this one is the x86-64 port of the window: the six callee-saved
 * registers, the stack pointer and the return address.
 *
 * The registers are the ones the System V ABI calls callee-saved - rbx, rbp,
 * r12, r13, r14 and r15 - and nothing else has to survive: a caller-saved
 * register belongs to the caller by definition, and the compiler has already
 * spilled what it needs across the call of `setjmp`.
 */
#ifndef MICRO_OS_LIBC_USER_SETJMP_H
#define MICRO_OS_LIBC_USER_SETJMP_H

/* rbx, rbp, r12, r13, r14, r15, rsp, rip */
typedef unsigned long jmp_buf[8];

/*
 * The declaration is the one the compiler knows: `returns_twice` tells it
 * that the function may come back from a second place, so a variable of the
 * caller that is kept in a register is written back to its slot before the
 * call and a `longjmp` sees the value it had.
 */
int setjmp(jmp_buf environment) __attribute__((returns_twice));
void longjmp(jmp_buf environment, int value) __attribute__((__noreturn__));

#endif /* MICRO_OS_LIBC_USER_SETJMP_H */
