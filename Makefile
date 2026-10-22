# Micro OS — convenience wrapper around the two build systems.
#
#   make help        show the available targets
#   make test        build and run the host unit tests
#   make sim         build and run the host simulator
#   make idf-build   build the ESP32 firmware (requires ESP-IDF)
#   make format      reformat the sources with clang-format

BUILD_DIR   ?= build-host
ASAN_DIR    ?= build-asan
IDF_EXPORT  ?= $(HOME)/esp/esp-idf/export.sh
IDF_TARGET  ?= esp32

.PHONY: help all build test test-asan sim clean format idf-build idf-flash idf-monitor idf-size bundle

help:
	@echo "Micro OS — available targets:"
	@echo "  make build      configure and build the host build"
	@echo "  make test       build and run the unit tests (host)"
	@echo "  make test-asan  the same tests with the address and UB sanitizers"
	@echo "  make sim        build and run the host simulator"
	@echo "  make clean      remove the host build directory"
	@echo "  make format     reformat sources with clang-format"
	@echo "  make idf-build  build the ESP32 firmware"
	@echo "  make idf-flash  flash the ESP32 firmware over USB"
	@echo "  make idf-monitor attach the serial monitor"
	@echo "  make idf-size   show the firmware size report"
	@echo "  make bundle     copy the firmware into dist/ for distribution"

all: build

build:
	cmake -B $(BUILD_DIR) -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug
	cmake --build $(BUILD_DIR)

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

# The board has no debugger: the tests of the host are the place where a
# wrong pointer or undefined behaviour has to be caught.
test-asan:
	cmake -B $(ASAN_DIR) -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMICRO_OS_SANITIZE=ON
	cmake --build $(ASAN_DIR)
	ctest --test-dir $(ASAN_DIR) --output-on-failure

sim: build
	$(BUILD_DIR)/ports/port_host/microos_sim

clean:
	rm -rf $(BUILD_DIR) $(ASAN_DIR)

format:
	@command -v clang-format >/dev/null 2>&1 || { echo "clang-format not found"; exit 1; }
	find components ports main tests -type f \( -name '*.c' -o -name '*.h' \) \
		-exec clang-format -i {} +
	@echo "sources formatted"

idf-build:
	. $(IDF_EXPORT) && idf.py -D IDF_TARGET=$(IDF_TARGET) build

idf-flash:
	. $(IDF_EXPORT) && idf.py -D IDF_TARGET=$(IDF_TARGET) flash

idf-monitor:
	. $(IDF_EXPORT) && idf.py -D IDF_TARGET=$(IDF_TARGET) monitor

idf-size:
	. $(IDF_EXPORT) && idf.py -D IDF_TARGET=$(IDF_TARGET) size

# Collect the firmware into dist/ so that it can be downloaded and flashed
# without the whole project.
bundle:
	. $(IDF_EXPORT) && idf.py -D IDF_TARGET=$(IDF_TARGET) build
	mkdir -p dist
	cp build/micro_os.bin dist/micro_os.bin
	cp build/bootloader/bootloader.bin dist/bootloader.bin
	cp build/partition_table/partition-table.bin dist/partition-table.bin
	cd dist && sha256sum micro_os.bin bootloader.bin partition-table.bin > checksums.txt
	@echo "the firmware is in dist/, flash it with: dist/flash.sh"
