# ADR 0017 - Pipes and redirections in the shell

* Status: accepted
* Date: 2026-11-21

## Context

ADR 0014 put `pipe()` (27) and `dup2()` (28) in the ABI and left the shell
without a pipeline: the calls were there, the parser and the waits were not,
and the release checklist of 1.0 said so. A person at the console of this
system could not write `ls | wc -l`, `cat /etc/setting > /tmp/f` or
`cat < /etc/setting`, and the contents of `/etc` were read with a command of
the shell rather than with the tools of a Unix-like machine.

The shell is not a program of ring 3: it is a task of the kernel
(`components/kshell`), the same interpreter on the board, in the simulator and
on x86-64. The `fork()` of the ABI copies the address space of a *program*; a
kernel task has none, so the call takes the placeholder of a machine without
an MMU and re-enters the program of the parent - for the shell that is a
second shell reading the same console. A pipeline cannot be built on that
call.

Two more facts decided the shape:

* `execve()` of this system starts a *second task* and waits for it
  (ADR 0016); it does not replace the caller, and the program used to start
  with an empty table of descriptors, so a pipe end could not reach it;
* a descriptor today is a reference to a counted description (ADR 0014), so
  the table of a task can be copied before a child runs.

## Decision

### 1. The grammar

A line is a chain of stages separated by `|`, and every stage may redirect
its input, its output or its errors:

```text
cmd arg... [< file] [> file | >> file] [2> file | 2>> file] [| cmd ...]
```

The words are split on spaces and tabs by the splitter that was already
there, so:

* an operator is a **word of its own**: `a | b`, `a > f`, `echo 2> f`. The
  forms `a|b` and `>f` are arguments and not operators, and the wiki says so;
* the operators of a stage may stand before or after its arguments, as in
  POSIX (`> f ls` and `ls > f` are the same line);
* a redirection of the same descriptor twice in one stage is refused;
* a `|` with no command on one side, a redirect with no file name and a line
  of redirects alone are refused with a sentence and no task is started;
* `&&`, `||`, `;`, `&` and `2>&1` are refused by name. Running `a && b` as the
  command `a` with two arguments would be a different line from the one the
  person typed;
* quotes, escapes, globs and variable expansion are still absent, as they
  were; `--` of the option parser and the two characters `""` of `path` keep
  working because the splitter does not touch them.

At most **three stages**: a boundary is one pipe and both of its ends are
descriptors of the task making the pipeline, which has `TASK_MAX_FDS` (four).
A deeper chain is refused with a sentence rather than failing on a descriptor
in the middle. It is a limit of the descriptor table and not of the parser,
and it is in `docs/debts.md`.

### 2. One task per stage, from `task_fork_shared()`

The runner of `components/kshell/src/kshell_pipe.c` makes one pipe per
boundary with `microos_pipe()`, one task per stage with
`task_fork_shared(parent, ...)`, and puts the ends on 0 and 1 in the child
with `microos_dup2()` before the command runs. The child closes every end it
does not use and every descriptor above 2; the parent closes all of its pipe
ends as soon as the last stage is started, which is what lets the reader see
the end of the file.

`task_fork_shared()` is the honest fork of a kernel task: the task is new,
the descriptors, the working directory and the job are copied, and the task
runs an entry of its own rather than re-entering the shell. A program of the
machine is still started with `execve()`, from the task of its stage.

### 3. A program inherits the descriptors of the stage

`k_port_exec_enter()` of x86-64 creates the program with
`task_fork_shared(caller, ...)` instead of an empty task, so the table of the
stage is copied before the program can run. That is the POSIX rule for
`execve`, and it is what makes `cat < file`, `echo one | /bin/cat` and
`ls /bin | wc -l` work. The same copy carries the working directory, so
`/bin/ls` with no argument lists the directory of the shell.

The change is in the port of a machine that can run a program; the board and
the simulator answer `ENOSYS` for `execve` and their shell pipelines are made
of the commands of the shell.

### 4. A builtin writes descriptor 1, and the shell reads a job

`kshell_printf()` and `kshell_write()` write descriptor 1 through the ABI
instead of calling the console directly, and `kshell_eprintf()` writes
descriptor 2. When no descriptor is open at those numbers the kernel falls
back to the console, so an ordinary line looks exactly as before; when a
stage put a pipe or a file there, the same bytes go into it. This is why a
builtin can be a stage at all.

`struct task` has a `job` (ADR 0015's next step for Ctrl+C): the pipeline
sets its own pid as the job of the stages, `task_fork_shared()` and `fork()`
copy it, and the program of a stage inherits it through `execve`. While the
shell waits for a stage it reads the console itself and one key becomes
`task_signal_job(job, SIGINT)`, which signals every task of the chain. A
stage that is waiting for a program does not poll the console for itself
(`task_get_job(caller) != TASK_JOB_NONE`), so the key is not taken by the
wrong task. The caller of the job is never a target: Ctrl+C ends the work and
not the shell.

The status of the line is the status of the last stage, as POSIX has it. For
a command of the shell it is the answer of its handler; for a program it is
0 when the program ran and 1 when it could not start, because `execve()` of
this system answers 0 for a program that returned and does not hand its code
back yet (`docs/debts.md`).

### 5. Commands that cannot be a stage

`cd` and `path` change the task that runs them. A stage is a task of its own,
so the change would be made to a copy nobody sees; the runner refuses a line
whose stage is one of them, with or without a redirection, and says why. A
redirection of a line of one command is enough to put that command in a task,
so the same rule applies to `cd > f`.

## Consequences

**Good**

* `ls | wc -l`, `cat /etc/setting > /tmp/f`, `cat < file` and
  `echo one | cat` work with the same interpreter and the same commands on
  every machine; the x86-64 smoke test runs them and the host tests run them
  on a MicroFS volume.
* The kernel keeps one notion of a chain: descriptors, counted descriptions
  and the job; `pipe`/`dup2` did not change.
* A program now inherits the descriptors of its caller, which is the POSIX
  rule and a debt that was open since K4.
* Ctrl+C reaches every stage of a pipeline and not only one of them; the
  job is a small field, not a process group, and it is the step ADR 0015
  named.
* A line that cannot run is a sentence; no task is started, so nothing hangs
  on a descriptor that was never made.

**Bad**

* Three stages and two pipes are the ceiling of a four-descriptor task; a
  longer chain is refused.
* The status of a program that is the last stage is 0 or 1, not its code.
* The diagnostics of a command of the shell go to descriptor 1, so a `>` of
  a stage redirects its `usage:` line as well; only the diagnostics of the
  runner (`kshell_eprintf`) and of a program go to descriptor 2.
* Two commands of the shell that keep static buffers (`ls` is one) in the
  same chain share them; the same is already true of two shells (the console
  and telnet). `docs/debts.md` carries the row.
* There is still no `&&`, `||`, `;`, `2>&1`, glob, quote, escape, background
  job, `jobs`, `fg` or `bg`.

## Files

`components/kshell/src/kshell_pipe.c`,
`components/kshell/src/kshell_internal.h`, `components/kshell/src/kshell.c`,
`components/kshell/src/kshell_files.c`, `components/kernel/src/task.c`,
`components/kernel/include/kernel/task.h`,
`ports/port_x86_64/src/port_exec.c`,
`ports/port_x86_64/programs/{cat,ls,wc}.c`,
`components/libc/user/{include/ulibc.h,src/syscall.c}`,
`tests/cases/test_kshell_pipe.c`, `tools/x86_smoke.py`.
