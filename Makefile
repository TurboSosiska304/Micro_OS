# Micro OS — convenience wrapper around the two build systems.
#
#   make help        show the available targets
#   make test        build and run the host unit tests
#   make sim         build and run the host simulator
#   make smoke       boot the simulator and check its answers
#   make integrity   the integrity check of the repository and the bundle
#   make ci          what the CI runs: the build, the tests and the checks
#   make idf-build   build the ESP32 firmware (requires ESP-IDF)
#   make format      reformat the sources with clang-format

BUILD_DIR   ?= build-host
ASAN_DIR    ?= build-asan
IDF_EXPORT  ?= $(HOME)/esp/esp-idf/export.sh
IDF_TARGET  ?= esp32
IDF_BUILD   ?= build

.PHONY: help all build test test-asan smoke integrity ci sim clean format idf-build idf-flash idf-monitor idf-size bundle

help:
	@echo "Micro OS — available targets:"
	@echo "  make build      configure and build the host build"
	@echo "  make test       build and run the unit tests (host)"
	@echo "  make test-asan  the same tests with the address and UB sanitizers"
	@echo "  make smoke      boot the simulator and check its answers"
	@echo "  make integrity  the integrity check of the repository and the bundle"
	@echo "  make ci         build, test, smoke and integrity — what the CI runs"
	@echo "  make sim        build and run the host simulator"
	@echo "  make clean      remove the host build directory"
	@echo "  make format     reformat sources with clang-format"
	@echo "  make idf-build  build the ESP32 firmware"
	@echo "  make idf-flash  flash the ESP32 firmware over USB"
	@echo "  make idf-monitor attach the serial monitor"
	@echo "  make idf-size   show the firmware size report"
	@echo "  make bundle     collect the firmware into dist/ for distribution"

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

# The whole simulator at once, not a piece of it: the kernel boots and the
# shell answers. The first line of the kernel after every command is the
# exit status of the command (0 is success) — the smoke test keys on it.
smoke:
	python3 tools/sim_smoke.py --verbose

# The firmware against the sources: the sums of the bundle, the headers of
# the images, the partition table against partitions.csv, the flash size
# against sdkconfig.defaults, the git objects and the hygiene of the sources.
integrity:
	python3 tools/integrity_check.py --verbose

# What the CI runs, and what the branch release_candidate has to pass.
ci: build test test-asan smoke integrity

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
# without the whole project: the binaries, their sums and the flash script.
bundle:
	. $(IDF_EXPORT) && idf.py -D IDF_TARGET=$(IDF_TARGET) build
	tools/bundle.sh $(IDF_BUILD) dist
