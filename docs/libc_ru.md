# Micro OS — библиотека C

Библиотека системы. Каждая её функция — вызов ABI системных вызовов, поэтому
программа платы доходит до ядра так же, как программа Linux.

Библиотека — переносимый C11, и она ничего не знает о ядре: она знает контракт
вызовов (см. [syscall-abi_ru.md](syscall-abi_ru.md)).

## Имена

Функции сохраняют собственные имена в бинарнике (`microos_printf`,
`microos_open`), а обычные имена — макросы:

```c
#include "libc/stdio.h"
#include "libc/unistd.h"

printf("hello, %s\n", name);   /* microos_printf */
```

Причина — библиотека компилятора: на плате она уже есть, и два `printf` в
одной программе — конфликт, который линкер решает в пользу одного из них, и
решает по порядку файлов. Макросы сохраняют читаемость программ и
однозначность бинарника.

## Форматирование

| Формат | Значение |
|---|---|
| `%d` `%i` | знаковое число |
| `%u` | беззнаковое число |
| `%x` `%X` | число в основании 16 |
| `%o` | число в основании 8 |
| `%c` | символ |
| `%s` | строка |
| `%p` | указатель |
| `%f` `%F` | число с точкой |
| `%e` `%E` | число с экспонентой |
| `%g` `%G` | число, короткая из двух форм |
| `%%` | процент |

Флаги: `-` (влево), `+` (знак у положительных), ` ` (пробел перед
положительными), `0` (нули впереди), `#` (префикс основания). Ширина и
точность пишутся как обычно, `*` берёт их из аргументов. Модификаторы длины
`h`, `l`, `ll`, `z`, `j`, `t` понимаются.

Функции:

```c
int microos_snprintf(char *buf, size_t size, const char *fmt, ...);
int microos_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int microos_printf(const char *fmt, ...);
int microos_vprintf(const char *fmt, va_list ap);
int microos_fprintf(int fd, const char *fmt, ...);
int microos_puts(const char *text);
int microos_putchar(int ch);
```

`snprintf` возвращает число записанных символов, даже если буфер был мал
(как обычно). `printf` и `fprintf` пишут через ABI, в дескриптор 1 или 2 —
консоль системы — и на дисплей платы.

## Файлы

```c
int microos_open(const char *path, int flags, ...);
int microos_close(int fd);
long microos_read(int fd, void *buf, size_t count);
long microos_write(int fd, const void *buf, size_t count);
long microos_lseek(int fd, long offset, int whence);
int microos_unlink(const char *path);
int microos_mkdir(const char *path);
```

Флаги open — числа POSIX: `MICROOS_O_RDONLY`, `MICROOS_O_WRONLY`,
`MICROOS_O_RDWR`, `MICROOS_O_CREAT`, `MICROOS_O_TRUNC`, `MICROOS_O_APPEND`.

## Процессы

```c
int microos_getpid(void);
int microos_getppid(void);
int microos_waitpid(int pid, int *status, int flags);
unsigned microos_sleep(unsigned ms);
int microos_yield(void);
void microos_exit(int code);
```

Флаг waitpid — `MICROOS_WNOHANG`: вызов не ждёт и отвечает 0, когда ребёнок
ещё не готов.

## Ошибки

Коды — числа POSIX. Неудачный вызов возвращает -1, а код хранится для
`microos_errno()`; `microos_strerror()` даёт текст.

```c
int microos_errno(void);
const char *microos_strerror(int code);
```

## Чего библиотека не умеет

* **Память программы не защищена.** Нет `mmap` и `brk`: память задач общая,
  так же, как задачи делят её после `fork`.
* **Стандартные дескрипторы — это консоль.** Дескрипторы 1 и 2, пока они не
  открыты как файл, — это консоль системы. `dup2` пока нет, поэтому программа
  не может отправить свой вывод в файл.
* **Консоль не читается через дескриптор 0.** Программе, которой нужен ввод
  пользователя, следует открыть `/dev/tty0`.
* **У `printf` плавающего числа нет точных цифр системы.** Числа совпадают с
  запрошенной точностью, но округление последней цифры может отличаться.

## Тесты

Тесты библиотеки лежат в `tests/cases/test_libc.c`. Библиотека проверяется
библиотекой системы на настоящих числах корпуса проекта: те же числа — тот же
текст, те же ошибки.
