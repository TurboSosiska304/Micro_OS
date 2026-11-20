# Micro OS — convenience wrapper around the two build systems.
#
#   make help        show the available targets
#   make test        build and run the host unit tests
#   make sim         build and run the host simulator
#   make smoke       boot the simulator and check its answers
#   make integrity   the integrity check of the repository and the bundle
#   make ci          the whole list of checks, before a push
#   make idf-build   build the ESP32 firmware (requires ESP-IDF)
#   make format      reformat the sources with clang-format

BUILD_DIR   ?= build-host
ASAN_DIR    ?= build-asan
# ESP-IDF discovery, in order: $IDF_PATH, a checkout beside the repository
# (../esp-idf), then the conventional ~/esp/esp-idf. Override IDF_EXPORT to
# point at another export.sh.
IDF_CANDIDATES := $(if $(IDF_PATH),$(IDF_PATH)/export.sh) \
                  $(abspath $(CURDIR)/../esp-idf/export.sh) \
                  $(HOME)/esp/esp-idf/export.sh
IDF_EXPORT  ?= $(firstword $(wildcard $(IDF_CANDIDATES)))
IDF_TARGET  ?= esp32
IDF_BUILD   ?= build
IDF_PORT    ?=
IDF_PORT_ARG = $(if $(IDF_PORT),-p $(IDF_PORT))

.PHONY: help all build test test-asan test-tsan smoke integrity ci sim clean format idf-check idf-build idf-flash idf-monitor idf-size bundle x86-build x86-run x86-run-serial x86-smoke x86-vga-smoke x86-run-disk x86-run-disk-serial x86-disk-smoke

help:
	@echo "Micro OS — available targets:"
	@echo "  make build      configure and build the host build"
	@echo "  make test       build and run the unit tests (host)"
	@echo "  make test-asan  the same tests with the address and UB sanitizers"
	@echo "  make test-tsan  the same tests under the race detector"
	@echo "  make smoke      boot the simulator and check its answers"
	@echo "  make integrity  the integrity check of the repository and the bundle"
	@echo "  make ci         build, test, test-asan, test-tsan, smoke, integrity"
	@echo "                  (x86-smoke when qemu-system-x86_64 is installed)"
	@echo "  make sim        build and run the host simulator"
	@echo "  make clean      remove the host build directory"
	@echo "  make format     reformat sources with clang-format"
	@echo "  make idf-build  build the ESP32 firmware"
	@echo "  make idf-flash  flash the ESP32 firmware over USB"
	@echo "  make idf-monitor attach the serial monitor"
	@echo "  make idf-size   show the firmware size report"
	@echo "  make x86-build  build the kernel for x86-64 (a multiboot image)"
	@echo "  make x86-run    boot it in a QEMU window (VGA text, PS/2 keyboard)"
	@echo "  make x86-run-serial  boot it with the serial console (-nographic)"
	@echo "  make x86-smoke  boot it and check what it says"
	@echo "  make x86-vga-smoke   check the screen and the keyboard without a display"
	@echo "  make x86-run-disk   boot the window with an 8 MB disk on the IDE channel"
	@echo "  make x86-run-disk-serial  the same disk in a terminal (-nographic)"
	@echo "  make x86-disk-smoke two boots on one disk image, a file survives both"
	@echo "  make bundle     collect the firmware into dist/ for distribution"

all: build

build:
	cmake -B $(BUILD_DIR) -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug
	cmake --build $(BUILD_DIR)

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

# The board has no debugger: the tests of the host are the place where a
# wrong pointer or undefined behaviour has to be caught.
test-tsan:
	cmake -B build-tsan -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMICRO_OS_SANITIZE_THREAD=ON
	cmake --build build-tsan
	ctest --test-dir build-tsan --output-on-failure

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

# What a contributor runs before pushing: the workflows of the cloud
# repeat this list on every push (and add valgrind and the firmware), so
# `make ci` stays the local half of the same checks.
#
# TSan is a part of the list: the host kernel is one address space and the
# race detector is the closest the tests get to preemption, so a push that
# passes only the plain and ASan builds has not seen the same code the cloud
# sees. The boot of the x86-64 port is not: QEMU is a machine of its own and
# not every laptop has it. The cloud installs it and runs x86-smoke; here it
# joins the list when `qemu-system-x86_64` is on PATH and is skipped with a
# message when it is not. A missing optional machine must not fail a run.
CI_QEMU := $(shell command -v qemu-system-x86_64 2>/dev/null)
CI_TARGETS := build test test-asan test-tsan smoke
ifneq ($(CI_QEMU),)
CI_TARGETS += x86-smoke
endif
CI_TARGETS += integrity

ci: $(CI_TARGETS)
ifeq ($(CI_QEMU),)
	@echo "ci: qemu-system-x86_64 not found, x86-smoke is skipped"
endif

sim: build
	$(BUILD_DIR)/ports/port_host/microos_sim

clean:
	rm -rf $(BUILD_DIR) $(ASAN_DIR)

format:
	@command -v clang-format >/dev/null 2>&1 || { echo "clang-format not found"; exit 1; }
	find components ports main tests -type f \( -name '*.c' -o -name '*.h' \) \
		-exec clang-format -i {} +
	@echo "sources formatted"

idf-check:
	@test -n "$(IDF_EXPORT)" || { \
		echo "ESP-IDF not found: set IDF_PATH or IDF_EXPORT"; \
		exit 1; \
	}

idf-build: idf-check
	. $(IDF_EXPORT) && idf.py -D IDF_TARGET=$(IDF_TARGET) build

idf-flash: idf-check
	. $(IDF_EXPORT) && idf.py $(IDF_PORT_ARG) -D IDF_TARGET=$(IDF_TARGET) flash

idf-monitor: idf-check
	. $(IDF_EXPORT) && idf.py $(IDF_PORT_ARG) -D IDF_TARGET=$(IDF_TARGET) monitor

idf-size: idf-check
	. $(IDF_EXPORT) && idf.py -D IDF_TARGET=$(IDF_TARGET) size

# Collect the firmware into dist/ so that it can be downloaded and flashed
# without the whole project: the binaries, their sums and the flash script.
# The x86-64 port: a project of its own, with no libc under the kernel.
# The loader is a multiboot image that QEMU loads with -kernel; it copies
# the kernel to its address and turns the long mode on.
X86_BUILD := build-x86

# The window of `make x86-run`: GTK when QEMU was built with it, SDL for a
# machine without it. `make x86-run X86_DISPLAY=sdl` picks the other one.
X86_DISPLAY ?= gtk

x86-build:
	cmake -B $(X86_BUILD) -S ports/port_x86_64 -G Ninja -DCMAKE_BUILD_TYPE=Release
	cmake --build $(X86_BUILD) --target image

x86-run: x86-build
	@qemu-system-x86_64 -display help 2>&1 | grep -qw "$(X86_DISPLAY)" || { \
		echo "QEMU has no '$(X86_DISPLAY)' display: install qemu-ui-gtk" >&2; \
		echo "(Arch: sudo pacman -S qemu-ui-gtk) or use make x86-run-serial" >&2; \
		exit 1; }
	qemu-system-x86_64 -kernel $(X86_BUILD)/loader32.elf \
		-initrd $(X86_BUILD)/microos64.bin -m 256 -no-reboot \
		-display $(X86_DISPLAY) -serial null

# The same machine with the console of the serial port and no window: the
# way it was run before the VGA and the keyboard, kept for a terminal, a
# script or a machine without a display server.
x86-run-serial: x86-build
	qemu-system-x86_64 -kernel $(X86_BUILD)/loader32.elf \
		-initrd $(X86_BUILD)/microos64.bin -m 256 -nographic -no-reboot

x86-smoke: x86-build
	python3 tools/x86_smoke.py

# The window without a display: the monitor of QEMU reads the text memory of
# the VGA and presses the keys of the keyboard, so a build farm can check the
# screen and IRQ1. Not a part of `make ci`: the monitor is a tool of its own.
x86-vga-smoke: x86-build
	python3 tools/x86_vga_smoke.py

# The disk of the port (ADR 0008): a raw image of 8 MB with the MBR of one
# partition. The image is made once and then kept: a person boots the machine,
# writes a file, boots it again and finds the file there, which is the whole
# point of a disk. `make x86-disk-smoke` works on an image of its own and
# removes it, so a run of the test never disturbs the disk of a person.
X86_DISK_IMAGE := $(X86_BUILD)/disk.img

$(X86_DISK_IMAGE):
	python3 tools/x86_disk_smoke.py --mkimage $(X86_DISK_IMAGE)

x86-run-disk: x86-build $(X86_DISK_IMAGE)
	@qemu-system-x86_64 -display help 2>&1 | grep -qw "$(X86_DISPLAY)" || { \
		echo "QEMU has no '$(X86_DISPLAY)' display: install qemu-ui-gtk" >&2; \
		echo "(Arch: sudo pacman -S qemu-ui-gtk) or use make x86-run-disk-serial" >&2; \
		exit 1; }
	qemu-system-x86_64 -kernel $(X86_BUILD)/loader32.elf \
		-initrd $(X86_BUILD)/microos64.bin -m 256 -no-reboot \
		-display $(X86_DISPLAY) -serial null \
		-drive file=$(X86_DISK_IMAGE),format=raw,if=ide,index=0,cache=writethrough

x86-run-disk-serial: x86-build $(X86_DISK_IMAGE)
	qemu-system-x86_64 -kernel $(X86_BUILD)/loader32.elf \
		-initrd $(X86_BUILD)/microos64.bin -m 256 -nographic -no-reboot \
		-drive file=$(X86_DISK_IMAGE),format=raw,if=ide,index=0,cache=writethrough

x86-disk-smoke: x86-build
	python3 tools/x86_disk_smoke.py

bundle: idf-check
	. $(IDF_EXPORT) && idf.py -D IDF_TARGET=$(IDF_TARGET) build && \
		cmake -B $(X86_BUILD) -S ports/port_x86_64 -G Ninja -DCMAKE_BUILD_TYPE=Release && \
		cmake --build $(X86_BUILD) --target image && \
		tools/bundle.sh $(IDF_BUILD) dist
