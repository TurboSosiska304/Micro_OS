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
| 1 | Планировщик, задачи, таймеры, PID | ⬜ план |
| 2 | VFS, `/proc`, `/dev`, шелл | ⬜ план |
| 3 | Менеджер памяти, ABI системных вызовов, libc | ⬜ план |
| 4 | Драйверы: TFT, кнопки, буззер, IMU, RTC, IR, микрофон | ⬜ план |
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
idf.py -p /dev/ttyUSB0 flash monitor
```

Либо через Makefile: `make idf-build`, `make idf-flash`, `make idf-monitor`.

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
