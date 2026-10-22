# 🔥 Отчёт по «прожарке» Micro OS

Проверены все компоненты (~18k строк): ядро, MFS, libc, шелл, драйверы, WiFi,
порты, система сборки, тесты и документация. Ниже — только подтверждённые
находки, отсортированные по критичности. Для каждой: место, суть, последствия и
≥2 варианта исправления.

---

## 🔴 CRITICAL

### C1. `mfs_mkdir()` не сохраняет bitmap на диск → перекрёстная порча данных после ребута
**Где:** `components/mfs/src/mfs.c:1096–1138`.

```c
rc = block_alloc(&block);        /* выделяет блок только в RAM-bitmap */
rc = inode_write(index, &inode); /* inode со ссылкой уходит на флеш */
rc = add_to_parent(...);
return MFS_OK;                   /* bitmap_store() НЕТ */
```

`bitmap_store()` вызывается только в `mfs_close()` (:1439), `mfs_unlink()` (:1439)
и `mfs_format()` (:899). `mfs_mkdir` выделяет блок и пишет inode, ссылаясь на
него, но на диске bitmap всё ещё помечает блок свободным.

**К чему приведёт:** после ребута (без промежуточного close/unlink) `block_alloc`
отдаст тот же блок другому файлу → новый файл перезапишет содержимое каталога.
Тихое перекрёстное согласование блоков, неуловимое без fsck. Тест
персистентности (`test_mfs_persistence.c`) это не покрывает.

**Решения:**
1. Добавить `bitmap_store()` (+ `dev.sync`) в конец `mfs_mkdir()` и в любую
   аллоцирующую операцию — по образцу `mfs_unlink` (:1543).
2. Структурно: ввести `g.bitmap_dirty` (ставится в `bitmap_set`), публичный
   `mfs_sync()`, и заставить VFS-адаптер вызывать его после каждой мутирующей
   операции. Порядок записи «bitmap → inode» гарантирует, что авария утекает
   блоки, но не даёт двойной аллокации.

### C2. `mfs_flash_mount()` автоформатирует том при ЛЮБОЙ ошибке монтирования → мгновенная потеря всех данных
**Где:** `components/mfs/src/mfs_flash.c:140–148`.

```c
rc = mfs_mount(&device);
if (rc != MFS_OK) {
    kinfo("mfs: ... formatting it");
    rc = mfs_format(&device, 256);   /* стирает bitmap/inode/dir */
```

`mfs_mount` возвращает `MFS_EIO` при транзиентной ошибке чтения (бит CRC во
флеш, brownout) и `MFS_EINVAL` при битом суперблоке. Оба пути ведут к
`mfs_format` → том молча уничтожается, пользователь получает пустой volume.

**К чему приведёт:** один brownout = полная потеря ФС. В связке с C6 (суперблок в
одном секторе с inode-таблицей) это гарантированная тотальная утрата.

**Решения:**
1. Форматировать только на `MFS_EINVAL` с проверкой «чистого флеша» (суперблок =
   все 0xFF); прочие ошибки — abort монтирования.
2. Требовать явный маркер «factory fresh» (флаг в NVS/efuse) перед форматом;
   добавить A/B-копии супербкла с CRC и sequence, пробовать вторую копию до
   вывода «том не отформатирован».

### C3. `mfs_vfs` ломает `lseek` → чтения/записи по неверному смещению
**Где:** `components/mfs/src/mfs_vfs.c:55` и `:70`; `kernel/src/ksys.c:235`.

```c
vfs_file->offset = file->file.offset;   /* затирает seek-позицию ВНУТРЕННИМ смещением */
return mfs_read(&file->file, buf, size);/* читает по file->offset, а не по vfs_file->offset */
```

`sys_lseek` пишет в `vfs_file.offset`, но адаптер (а) никогда не переносит это в
inner `mfs_file.offset` перед read/write и (б) активно затирает его устаревшим
внутренним смещением перед каждым вызовом.

**К чему приведёт:** `lseek(fd, N, SEEK_SET)` + `write` молча пишет в старую
позицию. `SEEK_CUR`/`SEEK_END` сломаны. Любая программа, делающая
seek-then-write, получает тихое повреждение данных.

**Решения:**
1. Сделать `vfs_file->offset` источником истины: перед операцией
   `file->file.offset = vfs_file->offset;`, после —
   `vfs_file->offset = file->file.offset;` (и в `vfs_close_file`).
2. Убрать дублирование позиции: хранить в priv только `struct mfs_file`,
   экспортировать позицию через пару `mfs_seek()`/`mfs_tell()` — один источник
   истины на открытый файл.

### C4. procfs: один глобальный `s_task_pid` определяет, какую таблицу файлов читать
**Где:** `components/procfs/src/procfs.c:129`, `:297`, `:327`.

```c
static k_u32_t s_task_pid;                                      /* :129 */
s_task_pid = dir_task_pid(dir);                                 /* :297 open */
proc = (s_task_pid != 0) ? find_task_file(name) : find_root_file(name); /* :327 read */
```

Открыть `/proc/1/status` (pid=1), затем `/proc/version` (pid=0) → чтение первого
fd ищет "status" в корневой таблице → возврат `-2` или чужого содержимого. С
двумя задачами — прямая гонка на `s_task_pid`.

**К чему приведёт:** неверное содержимое, смешение файлов, утечка данных между
задачами. Фатально для многоадресности `/proc`.

**Решения:**
1. Разрешать `proc_file*` и pid в момент `open`, складывать оба в 64-байтовый
   `priv` (помещается: указатель + `k_u32_t`); `procfs_read` использует только
   `priv`, глобальную удалить.
2. Пер-open-контекст в куче (в `priv` — указатель), освобождаемый в новом
   `procfs_close`; `s_task_pid` удалить полностью.

### C5. Глобальная таблица priv-данных per-fd общая для всех задач → порча состояния файлов + утечка
**Где:** `components/kernel/src/vfs.c:21`, `:189`; `mfs_vfs.c:28`;
`kernel/src/ksys.c:145–147`.

```c
static uint8_t g_file_data[VFS_MAX_FILES][VFS_FILE_DATA_SIZE];  /* глобально */
out->priv = vfs_file_data(slot);                                 /* slot = per-task fd */
```

`fd` — per-task номер (`TASK_MAX_FDS=4`), но `g_file_data[fd]` — глобальный.
Задача B, открыв fd 0, затирает `g_file_data[0]` — живой `struct mfs_file`
задачи A. Чтение A возвращает содержимое файла B.

**К чему приведёт:** порча данных + межзадачная утечка информации
(низкоприоритетная задача читает чужой открытый файл).

**Решения:**
1. Перенести таблицу priv в `struct task` (per-task), индексировать по fd —
   lifetime совпадает с `files[]`.
2. Оставить глобальный пул, но ключовать по `(pid, fd)` с generation-счётчиком в
   `vfs_file`; на read/write сверять ключ, иначе `KERR_BADF`.

---

## 🟠 HIGH

### H1. Нигде нет блокировок в preemptive-ядре — весь MFS-race
**Где:** `mfs.c:11–19` (`static struct{...} g`), `:356` (`g_dir_buffer`),
`mfs_flash.c:33` (`s_sector` cache). Ни одного мьютекса во всём MFS-стеке при
`TASK_MAX_TASKS 32`.

**Последствие:** гонки на RAM-bitmap (check-then-set → двойная аллокация), на
общем `g_dir_buffer`, на flash-кэше сектора (torn write). `mfs_close` одной
задачи персистит наполовину готовое состояние другой.

**Решения:**
1. Рекурсивный мьютекс MFS-слоя, берётся всеми публичными точками входа; отдельно
   лочить `flash_write_block`/`flash_flush`.
2. Структурно: маршрутизировать все мутации через выделенную FS-задачу/mailbox;
   чтения задокументировать как single-threaded + assert на scheduler lock.

### H2. `%e`/`%g` зависают навсегда на ±INFINITY; `%f/%e/%g` — UB на Inf/NaN
**Где:** `components/libc/src/stdio.c:378–387`, `:464–473`, `:323`.

```c
while (value >= 10.0) { value /= 10.0; exponent++; }  /* inf/10 == inf → вечный цикл */
ip = (unsigned long long)value;                        /* UB для inf/NaN/≥2^64 */
```

**Последствие:** `printf("%e", 1.0/0.0)` намертво вешает вызывающую задачу (и
ядро, если это shell-задача); Ctrl+C не помогает — ввод опрашивается только в
`shell_read_line`. UB-конверсии дают мусор, утекающий в вывод.

**Решения:**
1. Явные проверки спецзначений в начале `fixed_text`/`exponent_text`/
   `general_text`: `if (value != value) → "nan"; if (value > DBL_MAX) → "inf"`.
2. Ограничить циклы: `for (guard=0; value>=10.0 && guard<400; ...)`, и клампить
   `ip` перед кастом: `if (value >= 18446744073709551615.0) ip = ULLONG_MAX;`.

### H3. `cmd_write()`: `snprintf` накапливает «would-have-written» длину → OOB-записи
**Где:** `components/kshell/src/kshell_files.c:177–183`.

```c
len += (k_size_t)snprintf(text + len, sizeof(text) - len, "%s", argv[i]);
text[len] = '\0';
```

`snprintf` возвращает неразрезанную длину → `len` уходит за `sizeof(text)` →
следующие `text[len++]=' '` и `text[len]='\0'` пишут за пределы `text[128]`.
Сегодня маскируется только потому, что вход урезан до 128.

**Последствие:** переполнение стека ядра; при росте лимита — потенциальное
исполнение кода.

**Решения:**
1. Клампить по реальному остатку: `room = sizeof(text)-len; n = snprintf(...);
   len += (n>=room)? room-1 : n;` убрать отдельный `text[len]='\0'`.
2. Заменить на bounded-append через `memcpy` с проверкой лимита (без printf в
   горячем пути).

### H4. Молчаливо урезанный ввод/пути выполняются как ДРУГАЯ команда → `rm`/`cd` бьют не туда
**Где:** `kshell.c:457–460` (`if (len+1<size) buf[len++]=c;` — избыток просто
теряется) + `kshell_files.c:30–49` (`shell_resolve` урезает путь молча через
snprintf).

**Последствие:** `rm /very/long/path...` удаляет усечённый, возможно
существующий другой файл. `cd` сохраняет урезанный cwd → все последующие
относительные операции бьют не туда. Потеря данных без предупреждения.

**Решения:**
1. В `shell_read_line` отслеживать переполнение: если буфер заполнен без `'\n'` —
   напечатать «line too long», сбросить и НЕ возвращать усечённую строку.
2. `shell_resolve` возвращает `bool` (успех/переполнение); `cd/rm/write/cat/mkdir`
   при `false` печатают «path too long» и возвращают `KSHELL_ERROR`.

### H5. `malloc` «устанавливает errno», вызывая геттер — это no-op
**Где:** `components/libc/src/stdlib.c:18–21` (`microos_errno();` — просто
чтение). Сеттера errno не существует вовсе (`g_errno` — static в `unistd.c`).

**Последствие:** вызывающий код `if (!p && errno==ENO)` работает неверно; мёртвый
код (MISRA 2.2); `realloc(p,0)` — неопределённая семантика (риск утечки).

**Решения:**
1. Добавить `microos_set_errno(int)` в `unistd.c` + заголовок, вызывать в
   fail-путях `malloc/realloc/calloc` (добавить `KERR_NOMEM` в `errno.h`).
2. Переместить обёртки памяти в `unistd.c` и переиспользовать существующий
   `fail()` — одна точка поддержки errno.

### H6. I2C: неатомарная read-последовательность + игнор `rm_device` → неверные данные сенсоров + утечка кучи
**Где:** `components/drivers/src/drv_i2c.c:90–97`.

```c
if (i2c_master_transmit(dev,&reg,1,...) == ESP_OK &&
    i2c_master_receive(dev,buf,len,...) == ESP_OK) {
    i2c_master_bus_rm_device(dev);   /* возврат игнорируется */
```

`transmit` и `receive` — два отдельных захвата шины; между ними другая задача
может сменить регистр чипа → receive вернёт данные другого регистра. Плюс при
`ESP_ERR_INVALID_STATE` (чужая транзакция в полёте) `rm_device` возвращает ДО
`free()` → утечка `i2c_master_dev_t` на каждый вызов.

**Последствие:** молча неверные значения IMU/RTC; постепенная утечка кучи →
исчерпание heap.

**Решения:**
1. Атомарный `i2c_master_transmit_receive(dev,&reg,1,buf,len,timeout)` + проверка
   `rm_device` c логом/retry.
2. Отказаться от add/rm на каждый вызов: кэшировать `dev_handle_t` per-addr на
   lifetime шины + шина-мьютекс вокруг read/write/probe.

### H7. Шелл: таблица команд молча переполняется — `mount` и `reboot` не регистрируются
**Где:** `kshell.h:15` (`KSHELL_MAX_COMMANDS 24`), `kshell.c:468` (`kshell_init();`
— rc отброшен), `drv_wifi.c:257`, `drivers.c:27`, `main.c:77` (все игнорируют rc).

Считает: `kshell_init` даёт 22 (11 core + memtest + 9 files + reboot) + drivers
(date, i2c, wifi = 3) + display (1) = **26 > 24**.

**Последствие:** `reboot` (последний) и `mount` отвечают «command not found»; одна
новая команда позже вытеснит `wifi`/`date`. Всё молча, rc цепочки отброшены.

**Решения:**
1. Поднять лимит до 32 + `_Static_assert`; сделать `kshell_run` сообщающей об
   ошибке при `kshell_init() != 0`.
2. Проверять и логировать каждую регистрацию; в `drivers_init`/`main` считать
   fail. Компайл-тайм: `K_ARRAY_SIZE(core)+KMOD_MAX+drivers <= MAX`.

### H8. RTC: инвертированная полярность флага валидности → вердикт доктора задом наперёд
**Где:** `components/drivers/src/drv_rtc.c:62–63`.

```c
/* Bit 7 ... "clock integrity" flag. */
s_valid = (raw[0] & 0x80) != 0;
```

На BM8563/PCF8563 бит 7 регистра 02h — это флаг **LV (low voltage)**: 1 = низкое
напряжение, данные НЕ достоверны (подтверждено Linux `rtc-pcf8563.c`:
`PCF8563_SC_LV 0x80`). Код трактует 1 как «валидно».

**Последствие:** с исправной батарейкой (LV=0) `drv_rtc_check()` всегда возвращает
1 («(no clock)»); с мёртвой батарейкой (LV=1) может вернуть 0, если случайные
значения пройдут range-check. Доктор врёт в обе стороны.

**Решения:**
1. Инвертировать: `s_valid = (raw[0] & 0x80) == 0;` + поправить комментарий.
2. Трактовать LV как advisory; валидность по двойному чтению (секунды должны
   продвинуться) + range-check; LV отдельно как «low-voltage».

### H9. WiFi: нет реконнекта + отравленное состояние при частичном init
**Где:** `wifi_esp32.c:113–119` (после `DISCONNECTED` нет `esp_wifi_connect()`),
`:428–487` (7 ранних `return -N` после побочных эффектов, `s_initialised`
ставится только в конце).

**Последствие:** после ребута AP станция молча сидит в DISCONNECTED до ручного
`wifi join`. Retry после частичного init → повторный `esp_wifi_init` (навсегда
`ESP_ERR_INVALID_STATE`) + дублирующийся обработчик (двойной `wifi_notify` на
событие).

**Решения:**
1. Ограниченный авто-реконнект в обработчике событий (без auth-ошибок, счётчик < N,
   exponential backoff в core).
2. Транзакционный init: `s_initialised=true` сразу после `esp_wifi_init`,
   дальнейшие шаги толерантны (`ESP_ERR_INVALID_STATE` = уже сделано) или явный
   rollback (`esp_wifi_deinit`…); хранить два handler-instance.

### H10. Порт ESP32: гонка переиспользования слота задачи → NULL task pointer
**Где:** `port_esp32.c:296–304` (`slot->handle=NULL; slot->task=NULL;` перед
`vTaskDelete`), `:235–250` (незалоченный скан слотов).

Между `handle=NULL` и `task=NULL` создание новой задачи может занять тот же
слот, а старый поток затем затрёт `slot->task=NULL` → новый поток выполнит
`entry->entry(entry->task)` с NULL → LoadProhibited / крах.

**Последствие:** device-only crash (на хосте этой гонки нет — там утечка, см.
Medium). Реалистичный триггер: fork из шелла при завершающемся предыдущем ребёнке.

**Решения:**
1. Снапшотить `fn`/`t` в начале потока до wait; в `exit` чистить `task/entry` ДО
   `handle`; освобождать слот после `fn(t)`.
2. Скан+записи+`xTaskCreate` под `k_port_enter_critical()`; или не переиспользовать
   слоты (`state=DEAD`, переклейка только подтверждённо умерших).

### H11. Дисплей/SPI без мьютекса + общий DMA-буфер
**Где:** `drv_display.c:61–87`, `:174–207`, `:49–59` (статичный `s_chunk[4092]`),
`spics_io_num=-1`.

Последовательность DC/CS, `lcd_set_window`, `lcd_flush` и `s_chunk` не под
блокировкой. Вывод консоли идёт из shell-задачи и из любой задачи через
`write(1,…)`.

**Последствие:** DC неверен для транзакции, CS отпущен между set_window и записью
пикселей → рассинхрон панели; гонка в `s_chunk` → рваная картинка; `s_offset_x/y`
пишутся пока `lcd_set_window` их читает.

**Решения:**
1. Рекурсивный мьютекс в `drv_display.c` на `lcd_write_command/data`, `flush`,
   `text/clear`, `set_offset`.
2. Единый «display server» (одна задача + очередь) — единственный владелец SPI;
   заодно чинит цену перерисовки (M-14).
3. Минимум: `spi_device_acquire_bus()` для сквозной атомарности
   polling-последовательности.

### H12. Хост: паника = вечный завис; CTest без таймаута
**Где:** `ports/port_host/src/port_host.c:96–101` (`for(;;) sleep(1);`),
`panic.c:27`, `tests/CMakeLists.txt:42` (нет TIMEOUT), `tests/src/main.c:62–64`
(busy-wait без дедлайна).

**Последствие:** любая регрессия, вызывающая панику, превращает CI/`make test` в
25-минутный завис вместо мгновенного красного. Негативный путь загрузки не
тестируем (`test_boot.c` это честно признаёт).

**Решения:**
1. `k_port_halt()` на хосте: `_exit(89);` (не 0); в `tests/CMakeLists.txt`
   `TIMEOUT 60`; затем удалить escape-hatch в `test_boot` и реально тестировать
   aborting boot.
2. Опция `port_host_set_panic_exits(bool)` (включается раннером) + watchdog в
   раннере `for(i<60000 && !g_done)` → self-terminate.

### H13. Нет санитайзеров в хост-сборке — самый рискованный код не проверен ASan/UBSan
**Где:** grep `fsanitize` → 0 совпадений; флаги хоста (`CMakeLists.txt:38–48`)
только `-Wall -Wextra -Werror`.

**Последствие:** OOB/UB в slab-аллокаторе (kmm), блок-цепочках MFS, ручном
форматтере проходят молча и проявляются только на устройстве, где инструментов
нет.

**Решения:**
1. `option(MICRO_OS_SANITIZE ...)` + `add_compile_options(-fsanitize=address,undefined)`
   в хост-ветке.
2. Цель `make test-asan` отдельным `build-asan` + прогон в CI; опционально
   `-fsanitize=thread` для тестов планировщика.

---

## 🟡 MEDIUM

**M1. `sdkconfig.defaults:31` — неизвестный Kconfig-символ.**
`CONFIG_ESP_INT_WDT_TIMEOUT_S=10` — в IDF 5.x ключ переименован в `..._MS`; в
логе сборки «unknown kconfig symbol», реальное значение 800 мс. Фикс: строка 31 →
`CONFIG_ESP_INT_WDT_TIMEOUT_MS=10000`; либо CI greppит лог на «unknown kconfig
symbol» и падает.

**M2. `bild/` — опечатка в имени + 836 КБ бинарей в git, не в `.gitignore`.**
`.git` уже 41 МБ. Фикс: переименовать в `dist/`+gitignore+`git rm -r --cached`,
публиковать через GitHub Releases; либо минимально — `git rm --cached` бинарей +
`checksums.txt` с git-коммитом.

**M3. `k_port_enter_critical()` — блокирующий мьютекс, «выключенный» при позднем
init, ложный контракт.** `port_esp32.c:98–110`: не запрещает прерывания,
`xSemaphoreTakeRecursive` из ISR — illegal, при неинициализированном `s_lock` —
молча no-op (только на устройстве, хост всегда лочит). Фикс: настоящий
`portENTER_CRITICAL` + `configASSERT(!xPortInIsrContext())`; либо разделить API на
spinlock(ISR-safe) и mutex.

**M4. Ошибка чтения errno в `microos_open` и игнор `mode`.** `unistd.c:97–113`:
variadic `mode` не читается, `flags&3==3` молча мапится в RDONLY. Фикс: читать
`mode` через `va_arg` и пробрасывать (обновить ABI) **либо** убрать `...` и
валидировать `flags&3==3` → `KERR_INVAL`.

**M5. `%hd`/`%hhd` не сужают аргумент.** `stdio.c:687–693`: `printf("%hhu",300)`
печатает 300, а не 44. Атрибут `format(__printf__)` скрывает это от компилятора.
Фикс: сузить после `va_arg` по знаковости (развести `h`/`hh` в парсере длин); либо
отказаться от claims и явно отклонять `%h`.

**M6. Флаг `'0'` не подавляется precision (int) и не применяется к float.**
`stdio.c:223–228`, `:286–303`. `%08.3d` даёт `00000007` вместо `     007`. Фикс:
`if (precision>=0) sp.zero=0;` для целых + zero-pad вариант в `emit_text`; либо
задокументировать отклонение + закрепить тестом.

**M7. `strtoul/strtol` не реализованы в libc — молчаливая зависимость от newlib +
`sleep` DoS.** `kshell.c:306`: `sleep 4294967295` вешает shell на ~49.7 дней (Ctrl+C
не опрашивается во время сна). Фикс: реализовать `microos_strto*` с
overflow/endptr + валидация и кап в `cmd_sleep`; либо ручной bounded-парсинг с
диапазоном.

**M8. `cmd_write` игнорирует результат write/close и всегда рапортует успех; `cat`
принимает I/O-ошибку за EOF.** `kshell_files.c:192–196`, `:148–159`. Фикс:
проверять каждый вызов и печатать `strerror`; либо хелпер `write_all()` + проброс
`close`.

**M9. `cmd_ls` кладёт ~3 КБ массив dirent на стек ядра.** `kshell_files.c:95`
(64×48B ≈ 3072 + printf-sinks ≈ 4 КБ peak). Фикс: `static`/heap; либо visitor-форма
`vfs_list_each` или пейджинг со смещением.

**M10. WiFi: NVS-запись на каждый connect + `threshold.authmode=OPEN` + `scan`
блокирует ~2–5 с.** `wifi_esp32.c:213`, `:171`, дефолт `WIFI_STORAGE_FLASH`. Износ
флеша, evil-twin AP. Фикс: `esp_wifi_set_storage(WIFI_STORAGE_RAM)` + явный
persist; async-скан с таймаутом; pin BSSID / authmode из сохранённой конфигурации.

**M11. Порт-хост утекает TCB при выходе задачи; ESP32 перерабатывает слот —
дивергенция.** `port_host.c:267` (calloc) vs `:334` (`pthread_exit` без free).
Фикс: `pthread_cleanup_push`; либо фиксированный registry как на ESP32 (лечит и
утечку, и дивергенцию).

**M12. `selftest` создаёт 3 zombie-задачи, никогда не reaping; счётчики на
`volatile` RMW.** `selftest.c:72–98`. 3 из 32 слотов `g_tasks[]` сгорают на каждый
boot. Фикс: `task_wait`/`task_reap` после цикла; счётчики `_Atomic` или под
critical; `count+8` → `TASK_MAX_TASKS`.

**M13. IMU: неверный регистр для заявленной конфигурации.** `drv_imu.c:18`:
`MPU_REG_ACCEL_CONFIG 0x1A` — это CONFIG (DLPF), а не 0x1C. Гиро реально ±250 dps
вместо заявленных ±2000 (×8 ошибка масштаба). Фикс: дефайн → 0x1C + писать полный
набор (ACCEL/GYRO/CONFIG) с проверкой rc; либо至少 чинить коммент и проверять rc.

**M14. `tft_console` перерисовывает весь экран (clear + 64 КБ flush) на каждый
`\n` — синхронно в печатающей задаче.** `tft_console.c:135–144`. `help`/`ls` ≈
0.5–1 с зависания. Фикс: dirty-флаг + debounce-flush от низкоприоритетной задачи
(≤10 Гц); либо отрисовка только изменённых строк.

**M15. `mfs_open(O_TRUNC)`: ошибка игнорируется, блоки утекают.** `mfs.c:1211–1214`:
`inode_write` без проверки; `size=0` без освобождения цепочки → блоки заняты
навсегда. Фикс: проброс rc + освободить цепочку (как unlink) + `bitmap_store`; либо
Ленивый truncate с корректным порядком записи.

**M16. `mfs_unlink` глотает ошибки записи родителя → несогласованность на диске.**
`mfs.c:1527`, `:1539–1540`. Фикс: проверять и упорядочивать записи для
crash-safety (dirent → bitmap → нулевой inode); либо хотя бы логировать первую
ошибку.

**M17. Частичный сбой `mfs_write` утекает аллоцированные блоки навсегда.**
`mfs.c:1354–1362`, `:1411`, `:1439`. Фикс: ставить `dirty=true` сразу после первого
успешного `file_grow`; либо rollback по цепочке в error-пути.

**M18. Нет CRC на диске; суперблок физически в одном erase-секторе с
inode-таблицей.** `mfs.c:811–823`, `mfs_flash.c:44–54`. Стирание сектора
уничтожает суперблок → в связке с C2 автоформат. Фикс: INFO на отдельной секторной
границе + A/B суперблоки с CRC32/sequence; либо минимум — писать суперблок
последним + заголовок bitmap с magic/seq/crc.

**M19. Несколько ошибок mount без валидации.** `mfs.c:919` (нет проверки
`write_block`), `:929` (`blocks==0` → underflow), `:967` (`meta_blocks <
bitmap_blocks` → underflow `inode_capacity`). Фикс: расширить guard +
`sb_validate()`; либо отдельные проверки каждой арифметики.

**M20. `mfs_format` без верхнего предела размера тома → OOB в `g.bitmap`.**
`mfs.c:829` (guard есть только в `bitmap_load:70`). Для томов >16 МБ `bitmap_store`
пишет за 2-КБ массив. Фикс: тот же guard в format; либо перенести guard в
`bitmap_store` + `_Static_assert`.

---

## 🟢 LOW (кратко)

- **`mfs.h` MFS-errno коллизует с POSIX/KERR с другими смыслами**
  (`MFS_ENOTEMPTY=-9 == KERR_BADF`) — маппинг в адаптере или переиспользование
  `kernel/errno.h`.
- **Дисковый формат — native-endian raw struct + нет layout-asserts**
  (`mfs.c:182,242,491`) — encode/decode LE-хелперы + `_Static_assert(sizeof(inode)==16)`.
- **`MFS_O_APPEND` объявлен, но не реализован** (`mfs.c:1220` — всегда `offset=0`) —
  перезапись головы файла.
- **`mfs_read` игнорирует флаги open** — чтение с O_WRONLY-дескриптора успешно.
- **`g_errno` — один глобал на все задачи** (`unistd.c:13`) — привязать к
  `struct task` или `_Thread_local`.
- **`#define printf/open/...` hijacking стандартных имён** (`stdio.h:65–71`,
  `unistd.h:50–63`) — C11 7.1.4, коллизии с newlib; gate за `#ifndef` или
  shim-заголовки.
- **`mfs_unmount` не flush-ит и не guarded по открытым файлам** (`mfs.c:975–978`).
- **`vfs_stat.inode` в MFS заполняется flags, а не номером inode** (`mfs_vfs.c:144`).
- **Дубли include:** `main/src/main.c:14/19` (`kernel/vfs.h`), `tft_console.c:13/15`
  (`drivers/display.h`).
- **README/CONTRIBUTING заявляют `-Wall -Wextra -Werror` и «no GNU extensions» —
  правда только для хоста** (`CMAKE_C_EXTENSIONS ON` → gnu11); ESP32-ветка не
  добавляет `-Wextra`.
- **`CONFIG_ESP32_DEFAULT_CPU_FREQ_240`** (`sdkconfig.defaults:24`) — deprecated
  alias 4.x.
- **Документация противоречит коду:** `bild/README.md:20–30` и `flash.sh:14–17`
  дают неверные адреса прошивки (bootloader 0x0 vs реальный 0x1000, MicroFS
  0x21000 vs 0x210000) — можно «кирпичить» прошивкой вручную; README claims
  OTA-слоты `app0/app1+spiffs`, которых нет; три разные нумерации фаз; roadmap
  противоречит себе и коду (драйверы помечены нетick, хотя реализованы).
- **`NOTES-FOR-ME.md` закоммичен**, хотя объявлен «не для репозитория», и уже
  устарел («номера 0..19» vs реальные 0..22); ссылается на несуществующие
  `.github/`, `tools/*.py`.
- **Утечки `tmpfile()` FILE* в тестах** (~15 на прогон) — упадут при включении LSan.
- **Тест `boot: a failing subsystem aborts the boot` не тестирует отказ**
  (`test_boot.c:90–106`) — только успешный путь.
- **Нет CI на этой ветке**, хотя CONTRIBUTING рекламирует «automated checks».

---

## ✅ Что проверено и чисто (без действий)

- Хост-сборка с нуля чистая под `-Wall -Wextra -Werror`; `ctest`: 1 тест,
  **28 258 проверок, 0 падений, 0.25 с**.
- Assertions в тестах реальные (`ktest.h` инкрементирует счётчик, раннер возвращает
  exit-code).
- Нет credential'ов в дереве; WiFi-ключ идёт только CLI → NVS. `sdkconfig`
  untracked, без абсолютных путей.
- Целостность бандла: `bild/checksums.txt` совпадает с `sha256sum`, байты
  partition-таблицы = `partitions.csv`.
- Все 39 Markdown-ссылок разрешаются. `clang-format --dry-run -Werror` проходит.
- `syscall-abi.md` совпадает с кодом (0..22, `KSYS_COUNT 23`, маппинг open-флагов
  верен).

---

## 🎯 Порядок исправления (есолько несколько)

1. **C1 + C2 + H5/M18** — crash-safe персист метаданных MFS (один проход: bitmap в
   mkdir, отмена автоформата, A/B суперблок). Это защита от потери ВСЕХ данных.
2. **C3 + C5** — один источник правды для offset'а файла и per-task priv. Чинит
   молчаливую порчу записей.
3. **C4** — procfs per-open контекст вместо глобала.
4. **H2 + H3 + H4** — Inf/NaN-гарды + дисциплина границ для всего, что вводит
   пользователь (зависания и потеря данных от `rm`/`write`).
5. **H8** — инверсия LV-флага RTC (строчное, чинит вралящий доктор).
6. **H13 + H12** — санитайзеры + таймауты, чтобы регрессии ловились на хосте.
7. **M1 + M2** — Kconfig-ключ и вычищение `bild/` из git.
