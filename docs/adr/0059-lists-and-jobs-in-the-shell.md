# ADR 0059 - Lists and jobs in the shell

* Status: accepted
* Date: 2026-11-27

## Context

ADR 0017 gave the shell pipes and file redirections, and 2.1 gave a tty a
foreground process group. The grammar still refused `;`, `&&`, `||`, `&` and
`2>&1`, and the signal table had no stop or continue operation. A line could
not express a sequence, a fallback, a background task or a suspended job.

The shell is a kernel task, not a `/bin/sh` process. A pipeline already owns
one task per stage and one job identity shared with the programs those stages
start. The missing pieces are a list parser above the pipeline, a table that
outlives the line for background work, and a scheduler state for a stopped
task.

## Decision

### 1. Lists are parsed above pipelines

The lexer keeps quotes and escapes and emits unquoted `;`, `&&`, `||` and `&`
as operator words even when they touch a neighbour (`a;b`, `a&&b`). Pipes and
redirections keep their spaced-word grammar. The complete line is validated
before the first command runs.

`&&` and `||` have the same precedence and associate left. The status of the
list is the status of the last pipeline that actually ran. A trailing `;` is
accepted; an empty command between operators is refused.

### 2. A pipeline is a job

Every pipeline runs in one process group. The first stage puts itself in that
group before it can start a program, so an SMP fork cannot leak a program into
the shell's group. A foreground pipeline takes the tty; the shell restores its
own group after the pipeline ends or stops. When a program returns, the port's
status is recorded in the kernel task that started it; `execve()` keeps its ABI
answer of 0, and the shell uses the recorded code as the stage status.

`&` starts one pipeline without waiting and prints `[job] pid`. A per-shell
table keeps four entries with the number, leader pid, process group, command,
stage pids and state. Background argv and redirection paths are copied into
the entry: a background stage must not point at the stack buffer of the line
that returned. Finished stages are reaped when the table is read.

### 3. Stop is a scheduler state

`SIGSTOP` (19), `SIGTSTP` (20) and `SIGCONT` (18) are operations, not pending
marks with handlers. Stop removes a task from the ready queue and the sleep
table and marks it `stopped`; continue is the only operation that makes it
ready again. A sleeping task waits in its port while stopped and then resumes
the remainder of its wait.

Ctrl+Z sends `SIGTSTP` to the foreground group. The shell returns the tty to
itself and leaves the job `Stopped`. `fg %N` gives the group the terminal,
sends `SIGCONT` and waits; `bg %N` sends `SIGCONT` without taking the terminal.
Ctrl+C still sends `SIGINT` to the whole group.

### 4. `2>&1` names the stage output

A stage may redirect descriptors 0, 1 and 2 to files or duplicate descriptor 1
onto 2. The output file or pipe is installed before the duplicate. An error
file is installed before the output is opened, so an output-open failure can
still be reported into it. The console fallback has no `kfile` to duplicate;
leaving the target closed preserves the same console fallback.

## Consequences

**Good**

* `a; b`, `a && b` and `a || b` have shell status and short-circuit behaviour;
* a builtin and a program obey the same pipes and redirections;
* a background stage owns its command data for its whole life;
* Ctrl+Z, `jobs`, `fg` and `bg` work for both builtins and programs;
* the host tests check lists, `2>&1` and the table, and
  `make x86-shell-smoke` drives the real terminal keys in QEMU.

**Bad / still open**

* `&` backgrounds one pipeline, not a complete asynchronous AND-OR list;
* there is no SIGKILL, TTIN/TTOU, `wait`, or waitpid WUNTRACED/WCONTINUED;
* globs, variables, substitutions, here-documents and attached pipe or
  redirection forms remain a separate shell-expansion slice;
* `cd`, `path` and the job commands still cannot run in a stage or carry a
  redirection, because a stage is a task of its own.

## Files

`components/kshell/src/kshell_pipe.c`, `components/kshell/src/kshell.c`,
`components/kshell/src/kshell_internal.h`,
`components/kernel/src/{task,sched,tty}.c`,
`components/kernel/include/kernel/{task,sched,signal,port_task,ksys}.h`,
`ports/port_host/src/port_host.c`,
`ports/port_x86_64/src/port_x86_64_task.c`,
`tests/cases/test_kshell_pipe.c`, `tests/cases/test_kshell_jobs.c`,
`tests/cases/test_task.c`, `tests/cases/test_tty.c`,
`tools/x86_shell_smoke.py`.
