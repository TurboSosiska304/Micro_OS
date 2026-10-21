# Phase 4 - The C library over the ABI

The library of the system: the programs of the board talk to the kernel the
way a program of Linux talks to it - `open`, `read`, `write`, `printf` - and
every one of these calls goes through the ABI of the system calls.

## What is there

* `components/libc` - the library itself, portable C11, it knows nothing
  about the kernel, only about the contract of the calls.
* The formatting: `printf`, `snprintf`, `fprintf`, `puts`, `putchar` with
  the flags, the width, the precision and the length modifiers.
* The files: `open`, `close`, `read`, `write`, `lseek`, `unlink`, `mkdir`.
* The processes: `getpid`, `getppid`, `waitpid`, `exit`, `sleep`, `yield`.
* The errors: the codes of POSIX, `microos_errno()` and `microos_strerror()`.
* The console of the kernel (`kernel/console.h`): one place for the output,
  so that the output of the library reaches the serial port **and** the
  display of the board.
* The shell uses the library for `cat` and `write` - the commands of the
  user go through the same calls as the programs of the user.

## The names

The functions keep their own names in the binary (`microos_printf`), and the
usual names are macros. The reason is simple: the compiler of the board
already has a library, and two `printf` in one program is a conflict that
the linker resolves in the favour of one of them - in a way that depends on
the order of the files.

## The tests are about the properties

The library is checked against the library of the system, with the real
numbers of the corpus of the project:

* **The same number, the same text.** Every number of the corpus is printed
  by both libraries, with `%f`, `%e`, `%g` and every precision: the numbers
  that come out of the two texts are the same.
* **The same whole number, the same text.** The whole numbers of the corpus
  are printed with 19 formats (the flags, the width, the precision): the
  texts are equal, character by character.
* **The same string, the same text.** The strings are built from the corpus.
* **The same data, whatever the pieces.** The corpus is written to a file
  through the calls and read back in pieces of 1, 3, 7, 64, 512, 1024 and
  4096 bytes: the bytes come back the same.
* **The same output.** What the library prints into a file is the same as
  what the library of the system prints into a string.
* **The same errors.** The file that is not there, the descriptor that is
  not open, the directory that is not there: the same call gives the same
  code of POSIX, every time.

## The mistakes that cost time

* **The format of the kernel was broken by the macro of the library.** The
  attribute `format(printf, 2, 3)` of `klog()` names the usual `printf`,
  and the library makes that name a macro. The compiler then does not
  accept the attribute: *microos_printf is not a valid format function*.
  The reserved name `__printf__` does the job, and it cannot be renamed.
* **`utoa` is a function of newlib.** The helper of the formatter had the
  same name as the one of the library of the compiler, and the compiler
  was right: the names of the helpers of the library have to be its own.
* **`%g` was not `%g`.** The standard removes the zeros at the end of the
  fraction, and the point goes away when nothing is left after it. The
  first version kept them, and the test caught it on the real numbers.
* **The zero was not zero.** In `%e` the system prints `0.000000e+00` for
  the zero, and the first version printed `1.000000e+00` (the mantissa was
  forced to one).
* **`#x` of the zero.** The prefix `0x` belongs to a number that is not
  zero; the system prints `0` for the zero with `%#x`.
* **The test waited five seconds.** The check "waiting for a child that
  does not exist fails" waited for the whole timeout of the kernel. Now
  the call has the flag `WNOHANG` of POSIX, and the check is immediate.

## What is worth remembering

* The library is checked against the library of the system, not against
  the expectations of the author. The real numbers of the corpus are the
  judge, and they found four mistakes that a test of "is it not zero"
  would have missed.
* The console is in the kernel, not in the shell: the output of the
  programs has to reach the display of the board, and the display is not
  the business of the shell.
* The width of the arguments of the calls is the width of a pointer of the
  target: 32 bits on the board, 64 on the host.
