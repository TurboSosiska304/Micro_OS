# Micro OS

> English version: [README.md](README.md)

Linux-подобное ядро для **M5Stack StickC Plus2** (ESP32-PICO-V3-02, 240 МГц, 2 МБ PSRAM).

Цель — не «прошить ещё один ESP32-проект», а написать настоящее ядро: планировщик задач,
системные вызовы, VFS с `/proc` и `/dev`, менеджер памяти, драйверы и init-процесс,
который запускает шелл. Всё это — на голом железе без MMU, с одним ядром ESP32,
200 мА·ч батареей и экраном 135×240.

## Состояние проекта

| Фаза | Содержание | Статус |
|------|------------|--------|
| 0 | Каркас, сборка, лог, описание платы, юнит-тесты | ✅ готово |
| 1 | Планировщик, задачи, таймеры, PID | ✅ готово |
| 1F | Доктор железа (модули и проверки) | ✅ готово |
| 2 | VFS, MicroFS, шелл | 🔄 в работе |
| 3 | ABI системных вызовов, подмножество libc | ⬜ план |
| 4 | Драйвер микрофона, консоль на TFT | ⬜ план |
| 5 | init, конфигурация, приложения, WiFi/MQTT | ⬜ план |
| 6 | OTA, релиз, документация | ⬜ план |

Подробный план — в [docs/roadmap_ru.md](docs/roadmap_ru.md), архитектура — в
[docs/architecture_ru.md](docs/architecture_ru.md).

## Ключевая идея: переносимое ядро

Ядро **не знает про железо**. Вся платформенная часть вынесена в порт:

```
components/kernel/    ← чистый C11, ноль зависимостей от ESP-IDF
components/board/     ← описание платы
components/port_esp32/← реализация порта для ESP32 (ESP-IDF)
ports/port_host/      ← реализация порта для Linux (симулятор + тесты)
```

Следствие: ядро компилируется и тестируется на обычном компьютере за секунды.
Цикл «правка → тест → запуск» не требует прошивки устройства. А сборка самой
прошивки — обычный ESP-IDF проект.

## Сборка и запуск

### Хост (симулятор + тесты) — работает сразу

```bash
make test     # сборка и юнит-тесты
make sim      # запустить симулятор ядра на Linux
```

Или напрямую:

```bash
cmake -B build-host -S . -G Ninja
cmake --build build-host
ctest --test-dir build-host --output-on-failure
./build-host/ports/port_host/microos_sim
```

### Устройство (ESP-IDF)

Требуется установленный ESP-IDF v5.x:

```bash
git clone -b v5.3 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32
. ~/esp/esp-idf/export.sh

idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Либо через Makefile: `make idf-build`, `make idf-flash`, `make idf-monitor`.

Порт должен быть доступен текущему пользователю. В большинстве дистрибутивов
устройство принадлежит группе `uucp`:

```bash
sudo gpasswd -a $USER uucp   # затем перелогиниться
```

### Проверка ядра на устройстве

init-процесс запускает самотест на реальном железе: три задачи с разными
приоритетами спят, отдают управление и просыпаются, а init проверяет результат
и печатает `ps`-подобную таблицу списка задач.

```bash
tools/device-test.sh              # собрать, залить и проверить автоматически
tools/device-test.sh /dev/ttyACM0 # с явным указанием порта
tools/console.py                  # просто читать консоль
tools/console.py /dev/ttyACM0 20 --reset --send "modules"
```

Скрипт завершается с `RESULT: PASS`, если самотест прошёл.

### Доктор железа

После самотеста init-процесс обходит зарегистрированные модули и проверяет
их по очереди. Результат — отчёт:

```
module health report:
  module     state     detail
  psram      ok        2048 KB
  flash      ok        8 MB
  imu        ok        acc -16,-2952,16728 (raw)
  rtc        degraded  01:00:00 (no clock)
  display    ok        135x240, fb 63 KB
  input      ok        A=up B=up C=up
  buzzer     ok        passive, 2 kHz via LEDC
  led/ir     ok        G19, shared with the IR emitter
  battery    ok        4176 mV (raw 2088 mV)
  wifi       degraded  f4:65:0b:32:00:c0 idle
  mic        skip      SPM1423 MEMS microphone (I2S)
11 module(s), 0 problem(s)
```

`ok` — работает, `degraded` — работает с ограничениями (нет точки доступа, нет
часов без батарейки), `fail` — не работает, `skip` — пока не реализовано.
Команда `modules` повторяет проверку в любой момент.

### Шелл

init-процесс запускает шелл — небольшое подмножество команд Linux:

| Команда | Смысл |
|---|---|
| `help` | список команд |
| `ps` | список задач |
| `uptime` | время с момента загрузки |
| `uname` | имя и версия ядра |
| `free` | использование памяти |
| `dmesg` | журнал ядра |
| `modules` | повторная проверка модулей |
| `memtest` | проверка памяти и её скорости |
| `i2c` | сканирование шины I2C |
| `date` | время с RTC |
| `echo`, `sleep`, `clear`, `reboot` | как в жизни |

### Готовые бинарники

В папке `bild/` лежит собранная прошивка: приложение, загрузчик, таблица
разделов и скрипт прошивки. Скачиваешь папку, подключаешь плату и запускаешь
`bild/flash.sh` — исходники, ESP-IDF и тулчейн не нужны.

## Структура репозитория

```
Micro_OS/
├── components/
│   ├── kernel/         переносимое ядро: boot, log, panic, version
│   ├── board/          описание железа
│   └── port_esp32/     порт для ESP32
├── ports/port_host/    порт для Linux + симулятор
├── main/               точка входа прошивки (app_main)
├── tests/              юнит-тесты (свой минимальный фреймворк, без зависимостей)
├── docs/               архитектура, roadmap, железо, ADR
├── CMakeLists.txt      двухрежимная сборка: ESP-IDF или хост
└── Makefile            удобные цели: test, sim, format, idf-*
```

## Стиль разработки

* C11, `-Wall -Wextra -Werror`, 4 пробела, стиль Linux/BSD-mix (см. `.clang-format`).
* Один коммит — одна логическая change; сообщения в стиле Linux:
  `subsys: add vfs mount table`, `drivers: fix ir carrier frequency`.
* Любое новое поведение ядра покрывается тестом в `tests/`.
* Форматирование: `make format`.

Подробнее — [CONTRIBUTING_ru.md](CONTRIBUTING_ru.md).

## Ограничения, о которых стоит знать сразу

* **Нет MMU.** Разделение ядра и пользовательского кода логическое, а не аппаратное:
  нет аппаратной защиты памяти и полноценных адресных пространств.
* **8 МБ flash / 2 МБ PSRAM.** Ядро, приложения и данные делят одну флешку
  (OTA-разделы `app0`/`app1` + `spiffs`).
* **ESP32-PICO-V3-02 без PMIC.** Питанием и сном управляет сам ESP32:
  вывод `HOLD` (G4) надо выставить в HIGH, иначе плата выключится.
* **Одна кнопка питания + две пользовательские** (A, B) и третья (C) у Plus2.

## Лицензия

MIT — см. [LICENSE](LICENSE).
