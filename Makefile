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
COVERAGE_DIR ?= build-coverage
# The lines of components/kernel that the host tests and the simulator have
# to reach. The tree met about 80.7% when the gate was added; 70 keeps a margin
# for another compiler and another gcov. docs/wiki/testing.md explains it.
COVERAGE_THRESHOLD ?= 70
# The moment of a reproducible build: one date instead of the clock of the
# machine. Zero is the convention and "unknown" in a MicroFS record.
SOURCE_DATE_EPOCH ?= 0
export SOURCE_DATE_EPOCH
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

.PHONY: help all build test test-asan test-tsan test-smp test-minimal smoke integrity wiki-check ci sim clean format idf-check idf-build idf-flash idf-monitor idf-size bundle x86-build x86-open-build x86-smp-build x86-smp-run x86-run x86-run-serial x86-smoke x86-vga-smoke x86-disk x86-run-disk x86-run-disk-serial x86-disk-smoke x86-net-build x86-net-run x86-net-smoke x86-fat-build coverage reproducible soak soak-image soak-host fuzz-long disk-stress board-soak

help:
	@echo "Micro OS — available targets:"
	@echo "  make build      configure and build the host build"
	@echo "  make test       build and run the unit tests (host)"
	@echo "  make test-asan  the same tests with the address and UB sanitizers"
	@echo "  make test-tsan  the same tests under the race detector (two cores)"
	@echo "  make test-smp   the host tests of the kernel of two cores (KERNEL_SMP)"
	@echo "  make test-minimal  the host tests with the optional features off"
	@echo "  make smoke      boot the simulator and check its answers"
	@echo "  make coverage   the host build with gcov and the threshold of lines"
	@echo "  make reproducible  build the disk image twice and compare the bytes"
	@echo "  make integrity  the integrity check of the repository and the bundle"
	@echo "  make wiki-check the links of the user wiki (docs/wiki)"
	@echo "  make ci         build, test, test-asan, test-tsan, smoke, coverage,"
	@echo "                  x86-smoke when QEMU is there, reproducible, integrity"
	@echo "                  (x86-smoke when qemu-system-x86_64 is installed)"
	@echo "  make sim        build and run the host simulator"
	@echo "  make clean      remove the host build directory"
	@echo "  make format     reformat sources with clang-format"
	@echo "  make idf-build  build the ESP32 firmware"
	@echo "  make idf-flash  flash the ESP32 firmware over USB"
	@echo "  make idf-monitor attach the serial monitor"
	@echo "  make idf-size   show the firmware size report"
	@echo "  make x86-build  build the kernel for x86-64 (a multiboot image)"
	@echo "  make x86-smp-build  the same kernel with KERNEL_SMP on (ADR 0012)"
	@echo "  make x86-run    boot it in a QEMU window (VGA text, PS/2 keyboard)"
	@echo "  make x86-run-serial  boot it with the serial console (-nographic)"
	@echo "  make x86-smoke  boot it and check what it says"
	@echo "  make x86-disk   the image of the disk: /bin and /etc of MicroFS"
	@echo "  make x86-vga-smoke   check the screen and the keyboard without a display"
	@echo "  make x86-run-disk   boot the window with an 8 MB disk on the IDE channel"
	@echo "  make x86-run-disk-serial  the same disk in a terminal (-nographic)"
	@echo "  make x86-disk-smoke two boots on one disk image, a file survives both"
	@echo "  make x86-net-smoke   the network: ping, UDP and HTTP on QEMU user-net"
	@echo "  make soak       the session of 600 seconds on x86-64 (one boot)"
	@echo "  make soak SOAK_MINUTES=1  the same, short, to check the tool"
	@echo "  make soak SOAK_ITERATIONS=5  five short boots, one pass each"
	@echo "  make soak-host  the same ten minutes on the host, two cores, under TSan"
	@echo "  make fuzz-long  the fuzzers with a multiplier and a seed"
	@echo "  make disk-stress  a full volume, fsck -y and two boots (MicroFS, FAT)"
	@echo "  make board-soak the same session on the board (needs the board)"
	@echo "  make bundle     collect the firmware into dist/ for distribution"

all: build

build:
	cmake -B $(BUILD_DIR) -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug
	cmake --build $(BUILD_DIR)

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

# The race detector is a build of its own: it may not share the memory
# of the address sanitizer, and the two answer different questions. The build
# has KERNEL_SMP on (ADR 0012): the kernel keeps a run queue per core and the
# tests run on two of them (MICRO_OS_CPUS=2, tests/CMakeLists.txt), so the
# races of a machine of two cores are what TSan sees. The plain and the ASan
# builds keep the flag off and with it the kernel of 1.0.
test-tsan:
	cmake -B build-tsan -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug \
		-DMICRO_OS_SANITIZE_THREAD=ON -DKERNEL_SMP=ON
	cmake --build build-tsan
	ctest --test-dir build-tsan --output-on-failure

# The second core of the host (ADR 0012): the same tests, the same binary of
# the kernel, KERNEL_SMP on and MICRO_OS_CPUS=2 (the test property sets it).
# The flag is off by default, so `make test` stays the kernel of 1.0.
SMP_DIR ?= build-smp

test-smp:
	cmake -B $(SMP_DIR) -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug -DKERNEL_SMP=ON
	cmake --build $(SMP_DIR)
	ctest --test-dir $(SMP_DIR) --output-on-failure

test-asan:
	cmake -B $(ASAN_DIR) -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMICRO_OS_SANITIZE=ON
	cmake --build $(ASAN_DIR)
	ctest --test-dir $(ASAN_DIR) --output-on-failure

# The second host configuration (ADR 0010): the optional features off. It is
# a target of its own, not a part of `make ci` at first: it proves that the
# sources still build without the PSRAM, the MOX1 runtime and the network,
# and that the tests which do not need them still pass. The tests of a
# feature that is off are guarded in the source, not skipped by the runner.
MINIMAL_DIR ?= build-minimal
MINIMAL_FLAGS := -DKERNEL_PSRAM=OFF -DKERNEL_EXEC=OFF \
                 -DMICRO_OS_NET=OFF -DMICRO_OS_TELNET=OFF -DMICRO_OS_WIFI=OFF \
                 -DMICRO_OS_FS_FAT32=OFF -DMICRO_OS_FS_EXT2=OFF

test-minimal:
	cmake -B $(MINIMAL_DIR) -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug $(MINIMAL_FLAGS)
	cmake --build $(MINIMAL_DIR)
	ctest --test-dir $(MINIMAL_DIR) --output-on-failure

# The whole simulator at once, not a piece of it: the kernel boots and the
# shell answers. The first line of the kernel after every command is the
# exit status of the command (0 is success) — the smoke test keys on it.
smoke:
	python3 tools/sim_smoke.py --verbose

# The lines of components/kernel that the tests and the simulator reach,
# counted by gcov. The build is a directory of its own: the counters make
# a binary slower and its files are not the ones of a plain run. The tests
# run under ctest and the simulator runs the same smoke test as `make
# smoke`; the report refuses a number below the threshold (70% of lines:
# this tree met about 80.7% when the gate was added, so the threshold keeps a
# margin for another compiler and another gcov).
coverage:
	cmake -B $(COVERAGE_DIR) -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMICRO_OS_COVERAGE=ON
	cmake --build $(COVERAGE_DIR)
	find $(COVERAGE_DIR) -name '*.gcda' -delete
	ctest --test-dir $(COVERAGE_DIR) --output-on-failure
	python3 tools/sim_smoke.py --binary $(COVERAGE_DIR)/ports/port_host/microos_sim
	python3 tools/coverage_report.py --build $(COVERAGE_DIR) --threshold $(COVERAGE_THRESHOLD) \
		--verbose

# The firmware against the sources: the sums of the bundle, the headers of
# the images, the partition table against partitions.csv, the flash size
# against sdkconfig.defaults, the git objects and the hygiene of the sources.
integrity:
	python3 tools/integrity_check.py --verbose
	python3 tools/wiki_check.py

# The links of the user wiki (docs/wiki, the pages are Markdown with relative
# links). The integrity check calls it too; the target is for one page at a
# time while the wiki is edited.
wiki-check:
	python3 tools/wiki_check.py

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
CI_TARGETS := build test test-asan test-tsan test-smp smoke coverage
ifneq ($(CI_QEMU),)
CI_TARGETS += x86-smoke
endif
CI_TARGETS += reproducible integrity

ci: $(CI_TARGETS)
ifeq ($(CI_QEMU),)
	@echo "ci: qemu-system-x86_64 not found, x86-smoke is skipped"
endif

sim: build
	$(BUILD_DIR)/ports/port_host/microos_sim

clean:
	rm -rf $(BUILD_DIR) $(ASAN_DIR) $(MINIMAL_DIR) $(COVERAGE_DIR) $(SMP_DIR) build-tsan

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
	. $(IDF_EXPORT) && MICROOS_SIGN_KEY="$(MICROOS_SIGN_KEY)" \
		idf.py -D IDF_TARGET=$(IDF_TARGET) build

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

# The private key of the signed image (ADR 0020). It is not in this tree:
# a release names it with the variable, and a build without one generates a
# key of its own in the build directory.
MICROOS_SIGN_KEY ?=
X86_SIGN_ARGS := -DMICROOS_SIGN_KEY=$(MICROOS_SIGN_KEY)

# The window of `make x86-run`: GTK when QEMU was built with it, SDL for a
# machine without it. `make x86-run X86_DISPLAY=sdl` picks the other one.
X86_DISPLAY ?= gtk

x86-build:
	cmake -B $(X86_BUILD) -S ports/port_x86_64 -G Ninja -DCMAKE_BUILD_TYPE=Release $(X86_SIGN_ARGS)
	cmake --build $(X86_BUILD) --target image

# The other policy of ADR 0020: the loader verifies the image, prints the
# warning and continues when it did not pass. The smoke test boots an unsigned
# kernel on this loader and a good one on the loader of x86-build.
X86_OPEN_BUILD := build-x86-open

x86-open-build:
	cmake -B $(X86_OPEN_BUILD) -S ports/port_x86_64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DMICRO_OS_REQUIRE_SIGNATURE=OFF $(X86_SIGN_ARGS)
	cmake --build $(X86_OPEN_BUILD) --target microos64.bin loader32.elf

# The kernel of two cores on the x86-64 machine (ADR 0012). The port reads the
# processor list of the firmware (the MADT of ACPI) and starts the processor of
# the boot only: the APs wait for the trampoline that is not written yet, so
# the kernel says how many cores the machine has and uses one. The build is the
# half of the gate that can be checked today - the sources of a two-core kernel
# compile and boot on the machine - and `make x86-smoke` boots the same image
# with -smp 2 and reads the two numbers.
X86_SMP_BUILD := build-x86-smp

x86-smp-build:
	cmake -B $(X86_SMP_BUILD) -S ports/port_x86_64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DKERNEL_SMP=ON $(X86_SIGN_ARGS)
	cmake --build $(X86_SMP_BUILD) --target image

x86-smp-run: x86-smp-build x86-disk
	qemu-system-x86_64 -kernel $(X86_SMP_BUILD)/loader32.elf \
		-initrd $(X86_SMP_BUILD)/microos64.bin -m 256 -smp 2 -nographic -no-reboot \
		$(X86_DISK_DRIVE)

# The disk of the machine: an MBR, one partition and a MicroFS volume with
# /bin and /etc already written. The volume is made by the host tool
# (tools/mkfs_mfs.c, which links components/mfs), not by a second
# implementation of the format in a script; the images of the programs are
# the .mox files `make x86-build` leaves in the build directory.
X86_PROGRAMS_IMAGE := $(X86_BUILD)/programs.img
X86_MKFS := $(BUILD_DIR)/tools/microos_mkfs

x86-disk: x86-build build
	python3 tools/x86_image.py --image $(X86_PROGRAMS_IMAGE) \
		--build $(X86_BUILD) --mkfs $(X86_MKFS)

# The gate of a reproducible image: the same inputs, the same bytes. The
# moments of the MicroFS records come from SOURCE_DATE_EPOCH (the tool has
# no clock of its own), the build id of version.c is fixed and the paths of
# the tree are relative. The target builds the image, keeps a copy, builds
# it again from nothing and compares the two files.
reproducible: x86-disk
	@tmp=$$(mktemp -d) || exit 1; trap 'rm -rf "$$tmp"' EXIT; \
	cp $(X86_PROGRAMS_IMAGE) "$$tmp/first.img"; \
	rm -f $(X86_PROGRAMS_IMAGE); \
	$(MAKE) --no-print-directory x86-disk; \
	if cmp "$$tmp/first.img" $(X86_PROGRAMS_IMAGE); then \
		echo "reproducible: the two images are the same file"; \
	else \
		echo "reproducible: the two images differ" >&2; exit 1; \
	fi; \
	sha256sum "$$tmp/first.img" $(X86_PROGRAMS_IMAGE)

X86_DISK_DRIVE := -drive file=$(X86_PROGRAMS_IMAGE),format=raw,if=ide,index=0,cache=writeback

x86-run: x86-disk
	@qemu-system-x86_64 -display help 2>&1 | grep -qw "$(X86_DISPLAY)" || { \
		echo "QEMU has no '$(X86_DISPLAY)' display: install qemu-ui-gtk" >&2; \
		echo "(Arch: sudo pacman -S qemu-ui-gtk) or use make x86-run-serial" >&2; \
		exit 1; }
	qemu-system-x86_64 -kernel $(X86_BUILD)/loader32.elf \
		-initrd $(X86_BUILD)/microos64.bin -m 256 -no-reboot \
		-display $(X86_DISPLAY) -serial null \
		$(X86_DISK_DRIVE)

# The same machine with the console of the serial port and no window: the
# way it was run before the VGA and the keyboard, kept for a terminal, a
# script or a machine without a display server.
x86-run-serial: x86-disk
	qemu-system-x86_64 -kernel $(X86_BUILD)/loader32.elf \
		-initrd $(X86_BUILD)/microos64.bin -m 256 -nographic -no-reboot \
		$(X86_DISK_DRIVE)

x86-smoke: x86-disk x86-open-build
	python3 tools/x86_smoke.py --image $(X86_PROGRAMS_IMAGE) --open-build $(X86_OPEN_BUILD)

# The window without a display: the monitor of QEMU reads the text memory of
# the VGA and presses the keys of the keyboard, so a build farm can check the
# screen and IRQ1. Not a part of `make ci`: the monitor is a tool of its own.
x86-vga-smoke: x86-build
	python3 tools/x86_vga_smoke.py

# The disk of the port (ADR 0008): the same image of `make x86-disk`, copied
# once to a disk of its own. A person boots the machine, writes a file, boots
# it again and finds the file there, which is the whole point of a disk.
# `make x86-disk-smoke` works on an image of its own and removes it, so a run
# of the test never disturbs the disk of a person.
X86_DISK_IMAGE := $(X86_BUILD)/disk.img

# An order-only prerequisite: the image is written only when it is not there,
# so the file a person wrote survives the next `make x86-run-disk`.
$(X86_DISK_IMAGE): | x86-disk
	cp $(X86_PROGRAMS_IMAGE) $(X86_DISK_IMAGE)

x86-run-disk: x86-build $(X86_DISK_IMAGE)
	@qemu-system-x86_64 -display help 2>&1 | grep -qw "$(X86_DISPLAY)" || { \
		echo "QEMU has no '$(X86_DISPLAY)' display: install qemu-ui-gtk" >&2; \
		echo "(Arch: sudo pacman -S qemu-ui-gtk) or use make x86-run-disk-serial" >&2; \
		exit 1; }
	qemu-system-x86_64 -kernel $(X86_BUILD)/loader32.elf \
		-initrd $(X86_BUILD)/microos64.bin -m 256 -no-reboot \
		-display $(X86_DISPLAY) -serial null \
		-drive file=$(X86_DISK_IMAGE),format=raw,if=ide,index=0,cache=writeback

x86-run-disk-serial: x86-build $(X86_DISK_IMAGE)
	qemu-system-x86_64 -kernel $(X86_BUILD)/loader32.elf \
		-initrd $(X86_BUILD)/microos64.bin -m 256 -nographic -no-reboot \
		-drive file=$(X86_DISK_IMAGE),format=raw,if=ide,index=0,cache=writeback

x86-disk-smoke: x86-disk
	python3 tools/x86_disk_smoke.py --image $(X86_BUILD)/disk-smoke.img

# The network of the port (K6): a build of its own, with the HTTP client, the
# page of the state and the clock of NTP of components/net
# (-DMICRO_OS_X86_NET=ON) and a QEMU machine on its user-mode network. Not a
# part of `make ci`: the network of the test is the loopback of the host, and
# a build farm is not a place to open a port. The default image keeps the flag
# off, so `make x86-smoke` is the same machine it always was.
X86_NET_BUILD := build-x86-net

# The machine of `make x86-net-run` forwards 8080 of the loopback to 8080 of
# the guest, so `httpd` on the console and `curl http://127.0.0.1:8080/` on
# the host are the same page. The smoke test makes its own forward, on a port
# it found free.
X86_NET_RUN_DEVICE := -netdev user,id=n0,hostfwd=tcp:127.0.0.1:8080-:8080 \
	-device virtio-net-pci,netdev=n0,disable-modern=on

x86-net-build:
	cmake -B $(X86_NET_BUILD) -S ports/port_x86_64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DMICRO_OS_X86_NET=ON $(X86_SIGN_ARGS)
	cmake --build $(X86_NET_BUILD) --target image

x86-net-run: x86-net-build
	qemu-system-x86_64 -kernel $(X86_NET_BUILD)/loader32.elf \
		-initrd $(X86_NET_BUILD)/microos64.bin -m 256 -nographic -no-reboot \
		$(X86_NET_RUN_DEVICE)

x86-net-smoke: x86-net-build
	X86_BUILD=$(X86_NET_BUILD) python3 tools/x86_net_smoke.py

# The campaign of 1.0 (docs/wiki/testing.md). The session is one boot of the
# x86-64 machine, ten minutes long by default, under the mixed load of
# tools/soak.py. It is not a part of `make ci`: it takes ten minutes and it
# is run for a release candidate, not for a push.
SOAK_BUILD   ?= $(X86_NET_BUILD)
SOAK_IMAGE   ?= $(SOAK_BUILD)/soak.img
SOAK_MINUTES ?= 10
SOAK_ITERATIONS ?=
SOAK_ARGS    ?=

# The image of the session has no free block (tools/soak_image.py): the
# ENOSPC of a full volume is part of the load. The tool keeps the image
# while it is newer than the kernel.
soak-image: x86-net-build build
	python3 tools/soak_image.py --build $(SOAK_BUILD) --image $(SOAK_IMAGE) \
		--mkfs $(X86_MKFS) --if-needed

soak: soak-image
	python3 tools/soak.py --build $(SOAK_BUILD) --image $(SOAK_IMAGE) \
		--mkfs $(X86_MKFS) \
		$(if $(SOAK_ITERATIONS),--iterations $(SOAK_ITERATIONS),--minutes $(SOAK_MINUTES)) \
		$(SOAK_ARGS)

# The other half of the same ten minutes: the portable kernel over the host
# port, several tasks at once on two cores (ADR 0012, KERNEL_SMP and
# MICRO_OS_CPUS=2), under TSan. A warning of TSan, a deadlock, a leaked block
# or a definite leak of valgrind fails it. The plain half is the same two-core
# build without the sanitizer, so valgrind sees the kernel of the gate and not
# the one-core kernel beside it.
HOST_SOAK_MINUTES ?= 10
HOST_SOAK_ARGS    ?=

soak-host:
	cmake -B $(SMP_DIR) -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug -DKERNEL_SMP=ON
	cmake --build $(SMP_DIR) --target microos_stress
	cmake -B build-tsan -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug \
		-DMICRO_OS_SANITIZE_THREAD=ON -DKERNEL_SMP=ON
	cmake --build build-tsan --target microos_stress
	python3 tools/soak_host.py \
		--plain $(SMP_DIR)/tools/microos_stress \
		--tsan build-tsan/tools/microos_stress --cpus 2 \
		--minutes $(HOST_SOAK_MINUTES) $(HOST_SOAK_ARGS)

# The fuzz of the campaign: the same suite as `make test`, in the sanitizer
# build, with the iterations of tests/cases/test_fuzz.c multiplied and a
# seed of its own (KTEST_FUZZ_ITERATIONS and KTEST_FUZZ_SEED). The binary
# covers the ABI edge, MOX1, MicroFS, FAT, ext2 and the network with the
# same run, because the suite runs every case.
FUZZ_ITERATIONS ?= 20
FUZZ_SEED       ?= 0

fuzz-long:
	cmake -B $(ASAN_DIR) -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMICRO_OS_SANITIZE=ON
	cmake --build $(ASAN_DIR) --target microos_tests
	KTEST_FUZZ_ITERATIONS=$(FUZZ_ITERATIONS) KTEST_FUZZ_SEED=$(FUZZ_SEED) \
		./$(ASAN_DIR)/tests/microos_tests

# The disk stress of the campaign: MicroFS and FAT, a volume filled to
# ENOSPC, fsck -y and two boots of the same image. The FAT half needs
# dosfstools and mtools; when they are absent it is reported as skipped.
DISK_STRESS_FS ?= both

x86-fat-build:
	cmake -B build-x86-fat -S ports/port_x86_64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DMICRO_OS_FS_FAT32=ON
	cmake --build build-x86-fat --target image

disk-stress: x86-disk x86-fat-build build
	python3 tools/disk_stress.py --fs $(DISK_STRESS_FS)

# The board half of the session: build, flash and ten minutes with one
# boot. It is a target of the campaign and not of `make ci` (no runner has
# the board).
BOARD_SOAK_MINUTES ?= 10

board-soak:
	tools/board_soak.sh --minutes $(BOARD_SOAK_MINUTES)

bundle: idf-check
	. $(IDF_EXPORT) && MICROOS_SIGN_KEY="$(MICROOS_SIGN_KEY)" \
		idf.py -D IDF_TARGET=$(IDF_TARGET) build && \
		cmake -B $(X86_BUILD) -S ports/port_x86_64 -G Ninja -DCMAKE_BUILD_TYPE=Release $(X86_SIGN_ARGS) && \
		cmake --build $(X86_BUILD) --target image && \
		MICROOS_SIGN_KEY="$(MICROOS_SIGN_KEY)" tools/bundle.sh $(IDF_BUILD) dist
