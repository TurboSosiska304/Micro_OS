# Micro OS — команды

Все команды выполняются из корня репозитория. Из одних и тех же исходников
собираются две вещи: **хостовая** сборка (юнит-тесты и симулятор, без железа) и
**прошивка** (ESP32 через ESP-IDF v5.3).

> English version: [commands.md](commands.md)

## 1. Что нужно установить

| Нужно | Зачем | Установка (Arch/CachyOS) |
|---|---|---|
| `cmake`, `ninja` | хостовая сборка и ESP-IDF | `sudo pacman -S --needed cmake ninja` |
| `gcc` | хостовая сборка | `sudo pacman -S --needed gcc` |
| `python3` | инструменты в `tools/` и окружение ESP-IDF | `sudo pacman -S --needed python` |
| `clang-format` | `make format` и проверка формата | `sudo pacman -S --needed clang-format` |
| `valgrind` | прогон тестов как в CI (локально не обязателен) | `sudo pacman -S --needed valgrind` |
| `pyserial` | `tools/console.py`, `tools/stream_console.py` | `python3 -m pip install pyserial` (или окружение ESP-IDF) |

ESP-IDF v5.3 нужен только для сборки прошивки. Инструменты ищут его в таком
порядке: `$IDF_PATH`, чекаут рядом с репозиторием (`../esp-idf`), затем
`~/esp/esp-idf`. Переопределяется через `IDF_EXPORT=/path/to/export.sh`.

Установка ESP-IDF с нуля:

```bash
git clone -b v5.3 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32
. ~/esp/esp-idf/export.sh
```

Последовательный порт в большинстве дистрибутивов принадлежит группе `uucp`:

```bash
sudo gpasswd -a $USER uucp    # затем перелогиниться
```

## 2. Хостовая сборка, тесты и симулятор

| Команда | Что делает |
|---|---|
| `make` | то же, что `make build` |
| `make build` | настраивает и собирает `build-host/` через Ninja |
| `make test` | собирает и запускает юнит-тесты через `ctest` |
| `make test-asan` | те же тесты с санитайзерами адреса и UB, в `build-asan/` |
| `make sim` | собирает и запускает симулятор ядра `build-host/ports/port_host/microos_sim` |
| `make smoke` | загружает симулятор и проверяет шелл и подсистемы (нужен `make build`) |
| `make integrity` | проверка целостности: пакет, заголовки образов, таблица разделов, git, исходники, формат |
| `make format` | переформатирует C-исходники через `clang-format` |
| `make clean` | удаляет `build-host/` и `build-asan/` (каталог прошивки `build/` остаётся) |
| `make ci` | `build`, `test`, `test-asan`, `smoke`, `integrity` — прогон перед пушем |

То же вручную:

```bash
cmake -B build-host -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host
ctest --test-dir build-host --output-on-failure
build-host/ports/port_host/microos_sim
```

Полезные опции инструментов:

```bash
python3 tools/sim_smoke.py --verbose
python3 tools/integrity_check.py --verbose
python3 tools/integrity_check.py --skip-format --skip-git
python3 tools/integrity_check.py --expect-version 0.2.0
```

## 3. Сборка прошивки и заливка

| Команда | Что делает |
|---|---|
| `make idf-build` | собирает прошивку ESP32 в `build/` |
| `make idf-flash` | заливает прошивку по USB |
| `make idf-monitor` | подключает последовательный монитор (выход — `Ctrl+]`) |
| `make idf-size` | отчёт о размере прошивки |
| `make bundle` | собирает и складывает готовый пакет в `dist/` |

Makefile передаёт `IDF_TARGET=esp32` (из `sdkconfig.defaults`). Порт `idf.py`
определяет сам; если плат несколько, укажите явно:

```bash
make idf-flash IDF_PORT=/dev/ttyACM0
```

Без Makefile (после `. ../esp-idf/export.sh`):

```bash
idf.py set-target esp32      # один раз для свежего чекаута
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
idf.py size
idf.py menuconfig            # Kconfig Micro OS (components/drivers/Kconfig)
idf.py erase-flash           # стирает всё, включая данные MicroFS
```

## 4. Запуск на плате

```bash
tools/device-test.sh                  # сборка, заливка и чтение консоли
tools/device-test.sh /dev/ttyACM0     # с явным портом
CONSOLE_SECONDS=60 tools/device-test.sh /dev/ttyACM0
```

Скрипт завершается с `RESULT: PASS`, когда поднялись самотест, доктор железа и
шелл.

Прочитать консоль без заливки:

```bash
tools/console.py                          # порт сам, 30 секунд
tools/console.py /dev/ttyACM0 60          # порт и длительность
tools/console.py /dev/ttyACM0 20 --reset --send "modules"
tools/stream_console.py                   # интерактивная консоль, выход — Ctrl+Q
```

### Демоны при загрузке

Каждый файл в `/etc/init.d` — скрипт init-процесса: одна команда шелла на
строку, `#` начинает комментарий, и каждый скрипт работает в своей задаче.
Каталог создаётся при загрузке, если его нет.

```bash
# из шелла платы:
mkdir /etc/init.d
write /etc/init.d/clock date
reboot
```

## 5. Переопределения

| Переменная / аргумент | По умолчанию | Смысл |
|---|---|---|
| `IDF_EXPORT` | автоопределение | путь к `export.sh` другого ESP-IDF |
| `IDF_TARGET` | `esp32` | чип сборки |
| `IDF_BUILD` | `build` | каталог устройства сборки |
| `IDF_PORT` | авто | последовательный порт для `make idf-flash`/`idf-monitor` |
| `BUILD_DIR` | `build-host` | каталог хостовой сборки |
| `ASAN_DIR` | `build-asan` | каталог сборки с санитайзерами |
| `CONSOLE_SECONDS` | `30` | сколько секунд `device-test.sh` читает консоль |

## 6. Релизы

```bash
make ci                                        # локальный список проверок
python3 tools/integrity_check.py --expect-version 0.2.0
git tag v0.2.0-pre && git push origin v0.2.0-pre
```

Тег `vX.Y.Z` запускает в GitHub Actions тесты и сборку прошивки и публикует
релиз с `dist/` (бинарники, их суммы, `flash.sh`). Тег с дефисом
(`vX.Y.Z-pre`) — пререлиз. Версия ядра задаётся `KERNEL_VERSION_*` в
`components/kernel/include/kernel/config.h` и должна совпадать с тегом.

## 7. Два замечания

* `make ci` не собирает прошивку: для неё нужен ESP-IDF, это отдельный
  `make idf-build` (в облаке выполняются оба).
* Сборка прошивки пишет `build/compile_commands.json`; в корне
  `compile_commands.json` — симлинк на него, чтобы clangd и MCP-сервер Serena
  разрешали символы с реальными флагами цели.
