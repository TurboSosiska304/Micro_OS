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

.PHONY: ahci-smoke all bench board-soak build bundle bundle-board bundle-x86 ci clean coverage cut-smoke disk-stress format fuzz-long help idf-build idf-check idf-flash idf-monitor idf-size integrity mmu-smoke nvme-smoke ports-smoke power-smoke reproducible sdk sim smoke soak soak-host soak-image soak-smp test test-asan test-minimal test-smp test-tsan tty-smoke uefi uefi-run uefi-smoke uefi-stick usb-smoke usb-stick-smoke wiki-check x86-build x86-cut-smoke x86-disk x86-disk-smoke x86-fat-build x86-fs-smoke x86-iso x86-iso-run x86-iso-smoke x86-iso-uefi-smoke x86-iso-usb-smoke x86-net-build x86-net-run x86-net-smoke x86-open-build x86-root-smoke x86-run x86-run-disk x86-run-disk-serial x86-run-serial x86-smoke x86-smp-build x86-smp-run x86-smp-smoke x86-usb-smoke x86-vga-smoke x86-volume-smoke
help:
	@echo "Micro OS — available targets:"
	@echo "  make build      configure and build the host build"
	@echo "  make test       build and run the unit tests (host)"
	@echo "  make test-asan  the same tests with the address and UB sanitizers"
	@echo "  make test-tsan  the same tests under the race detector (two cores)"
	@echo "  make test-valgrind  the suite under valgrind, as the cloud runs it"
	@echo "  make test-smp   the host tests of the kernel of two cores (KERNEL_SMP)"
	@echo "  make test-minimal  the host tests with the optional features off"
	@echo "  make smoke      boot the simulator and check its answers"
	@echo "  make coverage   the host build with gcov and the threshold of lines"
	@echo "  make reproducible  build the disk image twice and compare the bytes"
	@echo "  make integrity  the integrity check of the repository and the bundle"
	@echo "  make wiki-check the links of the user wiki (docs/wiki)"
	@echo "  make ci         build, test, test-asan, test-tsan, smoke, coverage,"
	@echo "                  x86-smoke and mmu-smoke when QEMU is there, reproducible,"
	@echo "                  integrity"
	@echo "                  (x86-smoke when qemu-system-x86_64 is installed)"
	@echo "  make sim        build and run the host simulator"
	@echo "  make clean      remove the host build directory"
	@echo "  make format     reformat sources with clang-format"
	@echo "  make sdk        the SDK of a program into build-sdk/microos-sdk/<version>/"
	@echo "  make idf-build  build the ESP32 firmware"
	@echo "  make idf-flash  flash the ESP32 firmware over USB"
	@echo "  make idf-monitor attach the serial monitor"
	@echo "  make idf-size   show the firmware size report"
	@echo "  make x86-build  build the kernel for x86-64 (a multiboot image)"
	@echo "  make x86-smp-build  the same kernel with KERNEL_SMP on (ADR 0012)"
	@echo "  make x86-run    boot it in a QEMU window (framebuffer 1280x800, keyboard)"
	@echo "  make x86-run-serial  boot it with the serial console (-nographic)"
	@echo "  make x86-smoke  boot it and check what it says"
	@echo "  make mmu-smoke  the policy of a page: the attacks of ADR 0052"
	@echo "  make ports-smoke  the console set of sbase built by the SDK and run on"
	@echo "                  the machine: the gate of a program nobody rewrote"
	@echo "  make tty-smoke  kilo, a full-screen editor of somebody else, typed at the"
	@echo "                  machine with no line of its source edited (ADR 0051)"
	@echo "  make uefi       the same kernel as an image a firmware starts: a FAT"
	@echo "  make uefi-stick the same wrapped in a table of partitions: a stick of a UEFI machine"
	@echo "                  volume with BOOTX64.EFI, the kernel and disk.img"
	@echo "  make uefi-run   boot that volume in QEMU under OVMF (a window)"
	@echo "  make uefi-smoke boot it without a window and check the machine, the"
	@echo "                  shell and the picture of the frame"
	@echo "  make x86-smp-smoke  boot the kernel of two cores and check both of them"
	@echo "  make x86-disk   the image of the disk: /bin and /etc of MicroFS"
	@echo "  make x86-vga-smoke   check the screen and the keyboard without a display"
	@echo "  make usb-smoke  the keyboard, the stick and the wire of the USB"
	@echo "  make usb-stick-smoke  USB_STICK=/dev/sdc attached read-only, hashed"
	@echo "  make x86-run-disk   boot the window with an 8 MB disk of the machine"
	@echo "  make x86-run-disk-serial  the same disk in a terminal (-nographic)"
	@echo "  make x86-disk-smoke two boots on one disk image, a file survives both:"
	@echo "                  the IDE channel, the SATA controller and NVMe"
	@echo "                  (X86_DISK_BUS=virtio for the virtio device)"
	@echo "  make power-smoke  shutdown goes off with ACPI S5, the volume stays clean,"
	@echo "                  reboot resets the same QEMU process (ADR 0050)"
	@echo "  make ahci-smoke  the same image on the SATA controller of a chipset: a"
	@echo "                  named volume is written and survives a reset, an unnamed"
	@echo "                  one is read-only with the SHA-256 unchanged (ADR 0041)"
	@echo "  make nvme-smoke  the same image on the NVMe controller of a laptop: the"
	@echo "                  same writes, the same rule, the same proof (ADR 0042)"
	@echo "  make x86-fs-smoke  rename, rmdir, fsck ext2 and the SQLite journal"
	@echo "  make x86-net-smoke   the network: DHCP, ping, UDP, HTTP and the e1000e"
	@echo "  make x86-iso    the bootable medium: CD and stick, BIOS and UEFI (K13)"
	@echo "  make x86-iso-run    boot that image in a terminal (-nographic)"
	@echo "  make x86-iso-smoke  boot the image from a CD on a BIOS and check the shell"
	@echo "  make x86-iso-uefi-smoke  the same image on OVMF, from a CD and a stick"
	@echo "  make x86-iso-usb-smoke   the same image on a stick, from a BIOS"
	@echo "  make bench      the numbers of the machine: syscalls, fork, pipe,"
	@echo "                  file, console and latencies, three boots, the median"
	@echo "  make soak       the session of 600 seconds on x86-64 (one boot)"
	@echo "  make soak SOAK_MINUTES=1  the same, short, to check the tool"
	@echo "  make soak SOAK_ITERATIONS=5  five short boots, one pass each"
	@echo "  make soak-smp   the same session on the two-core image (-smp 2)"
	@echo "  make soak-host  the same ten minutes on the host, two cores, under TSan"
	@echo "  make fuzz-long  the fuzzers with a multiplier and a seed"
	@echo "  make disk-stress  a full volume, fsck -y and two boots (MicroFS, FAT)"
	@echo "  make board-soak the same session on the board (needs the board)"
	@echo "  make bundle     collect the firmware and the x86-64 machine into dist/"
	@echo "  make bundle-board  the firmware alone into dist/ (needs ESP-IDF)"
	@echo "  make bundle-x86 the tar of the x86-64 machine alone (no ESP-IDF)"

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

# The suite under the memory checker of the cloud, byte for byte what the
# workflow runs. It was the gate of the cloud alone and it was red for two
# days - a checksum over the padding of a record and a `strstr` over the
# bytes a file did not write - without a single failed check of the suite:
# a child of a fork inherits the count and its exit code turns into the 42 of
# `--error-exitcode`, which the case reads as the exit code of its program.
# A gate of the cloud that can be run here is a gate somebody runs.
test-valgrind: test
	valgrind --error-exitcode=42 --track-origins=yes ./$(BUILD_DIR)/tests/microos_tests

# The gate of a release in one command: every smoke, the two sessions, the
# board, the integrity against the tag and a Release from an empty directory,
# with a table of the answers (`tools/release_gate.sh`). It keeps going after a
# failed gate, so one red line does not hide the others.
release-gate:
	sh tools/release_gate.sh $(RELEASE_GATE_ARGS)

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
# sees. The minimal build is a part of it for the same reason: it links a
# subset of the tree and a symbol that only one feature asked for keeps it
# green, and it was red for a release and a half before anybody ran it. The boot of the x86-64 port is not: QEMU is a machine of its own and
# not every laptop has it. The cloud installs it and runs x86-smoke; here it
# joins the list when `qemu-system-x86_64` is on PATH and is skipped with a
# message when it is not. A missing optional machine must not fail a run.
CI_QEMU := $(shell command -v qemu-system-x86_64 2>/dev/null)
CI_TARGETS := build test test-asan test-tsan test-minimal test-valgrind test-smp smoke coverage
ifneq ($(CI_QEMU),)
CI_TARGETS += x86-smoke mmu-smoke x86-root-smoke usb-smoke ports-smoke
endif
# The boot of the firmware needs QEMU and the firmware: a machine with one and
# not the other is a machine of the BIOS gate alone.
ifneq ($(wildcard /usr/share/edk2/x64/OVMF_CODE.4m.fd),)
CI_TARGETS += uefi-smoke
endif
CI_TARGETS += reproducible integrity

ci: $(CI_TARGETS)
ifeq ($(CI_QEMU),)
	@echo "ci: qemu-system-x86_64 not found, x86-smoke, mmu-smoke, x86-root-smoke, usb-smoke and ports-smoke are skipped"
endif

sim: build
	$(BUILD_DIR)/ports/port_host/microos_sim

clean:
	rm -rf $(BUILD_DIR) $(ASAN_DIR) $(MINIMAL_DIR) $(COVERAGE_DIR) $(SMP_DIR) \
		build-tsan build-sdk build-ports

format:
	@command -v clang-format >/dev/null 2>&1 || { echo "clang-format not found"; exit 1; }
	find components ports main tests -type f \( -name '*.c' -o -name '*.h' \) \
		-not -path 'tests/ports/sbase/*' -not -path 'tests/ports/kilo/*' \
		-exec clang-format -i {} +
	@echo "sources formatted"

# The SDK: the headers of a program, the library of a program, the crt and the
# link of the format, so that a program is built outside this tree - a
# repository of its own, a `BUILD` file of a package, a machine with no
# checkout (docs/porting-programs.md, ADR 0025 §6). The result is
# $(DESTDIR)/microos-sdk/<version>/, and the version is the one of
# components/kernel/include/kernel/config.h. The default is a build directory
# and not `dist/`: the SDK is an artifact of a build, and `dist/` is the
# bundle of a release, which `make integrity` reads as one.
DESTDIR ?= build-sdk
SDK_HEAP ?= 262144

sdk:
	sh tools/sdk/make_sdk.sh --destdir "$(DESTDIR)" --heap "$(SDK_HEAP)"

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
	cmake -B $(X86_BUILD) -S ports/port_x86_64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DMICRO_OS_FS_EXT2=ON -DMICRO_OS_FS_FAT32=ON $(X86_SIGN_ARGS)
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
#
# The same tree is written a second time into an ext2 volume, and that image
# carries a FAT32 volume as its second partition: the kernel reads the type of
# the root out of the medium (ADR 0027), so the two disks boot the same shell
# and the second one proves a root that is not MicroFS.
X86_PROGRAMS_IMAGE := $(X86_BUILD)/programs.img
X86_PROGRAMS_EXT2_IMAGE := $(X86_BUILD)/programs-ext2.img
X86_MKFS := $(BUILD_DIR)/tools/microos_mkfs

# The root the machine of `make x86-run` boots: the MicroFS image by default,
# the ext2 one with `make x86-run X86_ROOT=ext2` (ADR 0027 §3).
X86_ROOT ?= mfs
X86_ROOT_IMAGE := $(if $(filter ext2,$(X86_ROOT)),$(X86_PROGRAMS_EXT2_IMAGE),$(X86_PROGRAMS_IMAGE))

x86-disk: x86-build build
	python3 tools/x86_image.py --image $(X86_PROGRAMS_IMAGE) \
		--build $(X86_BUILD) --mkfs $(X86_MKFS)
	python3 tools/x86_image.py --image $(X86_PROGRAMS_EXT2_IMAGE) --fs ext2 \
		--build $(X86_BUILD) --mkfs $(X86_MKFS)

# The gate of a reproducible image: the same inputs, the same bytes. The
# moments of the MicroFS records come from SOURCE_DATE_EPOCH (the tool has
# no clock of its own), the build id of version.c is fixed and the paths of
# the tree are relative. The target builds the image, keeps a copy, builds
# it again from nothing and compares the two files.
reproducible: x86-disk
	@tmp=$$(mktemp -d) || exit 1; trap 'rm -rf "$$tmp"' EXIT; \
	cp $(X86_PROGRAMS_IMAGE) "$$tmp/first.img"; \
	cp $(X86_PROGRAMS_EXT2_IMAGE) "$$tmp/first-ext2.img"; \
	rm -f $(X86_PROGRAMS_IMAGE) $(X86_PROGRAMS_EXT2_IMAGE); \
	$(MAKE) --no-print-directory x86-disk; \
	if cmp "$$tmp/first.img" $(X86_PROGRAMS_IMAGE) && \
		cmp "$$tmp/first-ext2.img" $(X86_PROGRAMS_EXT2_IMAGE); then \
		echo "reproducible: the two images are the same files"; \
	else \
		echo "reproducible: the images differ" >&2; exit 1; \
	fi; \
	sha256sum "$$tmp/first.img" $(X86_PROGRAMS_IMAGE) \
		"$$tmp/first-ext2.img" $(X86_PROGRAMS_EXT2_IMAGE)

# The bus of the disk of the machine: the primary IDE channel of ADR 0008
# (the default, and the disk of every release before 1.6) or the virtio-blk
# device of ADR 0037. The image is one file and the bus is the machine, so
# `make x86-smoke X86_DISK_BUS=virtio` boots the same bytes of the medium on
# the other device and the checks of the run are the checks of the port.
X86_DISK_BUS ?= ide

ifeq ($(X86_DISK_BUS),virtio)
X86_DISK_DRIVE := -drive file=$(X86_ROOT_IMAGE),format=raw,if=virtio,cache=writeback
else
X86_DISK_DRIVE := -drive file=$(X86_ROOT_IMAGE),format=raw,if=ide,index=0,cache=writeback
endif

x86-run: x86-disk
	@qemu-system-x86_64 -display help 2>&1 | grep -qw "$(X86_DISPLAY)" || { \
		echo "QEMU has no '$(X86_DISPLAY)' display: install qemu-ui-gtk" >&2; \
		echo "(Arch: sudo pacman -S qemu-ui-gtk) or use make x86-run-serial" >&2; \
		exit 1; }
	qemu-system-x86_64 -kernel $(X86_BUILD)/loader32.elf \
		-initrd $(X86_BUILD)/microos64.bin -m 256 -no-reboot \
		-display $(X86_DISPLAY) -serial null -vga std \
		$(X86_DISK_DRIVE)

# The same machine with the console of the serial port and no window: the
# way it was run before the VGA and the keyboard, kept for a terminal, a
# script or a machine without a display server.
x86-run-serial: x86-disk
	qemu-system-x86_64 -kernel $(X86_BUILD)/loader32.elf \
		-initrd $(X86_BUILD)/microos64.bin -m 256 -nographic -no-reboot \
		$(X86_DISK_DRIVE)

x86-smoke: x86-disk x86-open-build
	python3 tools/x86_smoke.py --image $(X86_PROGRAMS_IMAGE) --open-build $(X86_OPEN_BUILD) \
		--disk $(X86_DISK_BUS) --timeout 180

# The policy of a page (ADR 0052, lane M1 of 2.0): a program that writes its
# text, executes its stack, writes the kernel, touches a page it does not own,
# asks for a writable and executable mapping and grows past the limit of its
# task - each ends the program with the reason in the log - plus the stack
# that grows by 128 KiB and the W^X audit of every program of the run. The
# machine carries the two bits of 2014 (SMEP and SMAP) so that the kernel side
# of the boundary is exercised too; `make mmu-smoke MMU_CPU=qemu64` runs the
# same suite on the supervisor bit alone.
MMU_CPU ?= qemu64,+smep,+smap

mmu-smoke: x86-disk
	python3 tools/x86_mmu_smoke.py --image $(X86_PROGRAMS_IMAGE) --build $(X86_BUILD) \
		--cpu $(MMU_CPU) --timeout 240

# The gate of a program nobody rewrote (lane M3 of 1.8, docs/ports-table.md):
# the console set of sbase - true, false, echo, basename, dirname, pwd,
# printenv, sleep, cksum, tee, cmp - is built by **the SDK** and not by the
# CMake of this tree, and every file of it is the release of the upstream
# byte for byte (tests/ports/upstream.txt). The volume of the run is the image
# of `make x86-disk` with /bin/ports added to it, so the boot and the shell
# are the machine of the release; the programs are typed at the shell and the
# answers are the answers of the release. The three calls of a command that
# 1.8 added to the library - popen, pclose and system - are the program
# /bin/ports/command of the same gate.
PORTS_BUILD := build-ports

ports-smoke: x86-disk
	sh tools/ports_build.sh --build $(PORTS_BUILD)
	python3 tools/ports_smoke.py --build $(PORTS_BUILD) --x86 $(X86_BUILD) \
		--disk $(X86_DISK_BUS) --timeout 180

# The gate of the terminal of a program (ADR 0051): the editor of somebody
# else - antirez's kilo, BSD-2-Clause, byte for byte in tests/ports/kilo - is
# typed at the machine, saves a file with Ctrl+S, leaves with Ctrl+Q, and `cat`
# reads the line back. `ports-smoke` builds the same programs with the same
# SDK; this one drives the keyboard that a transcript of lines cannot.
tty-smoke: x86-disk
	sh tools/ports_build.sh --build $(PORTS_BUILD)
	python3 tools/tty_smoke.py --build $(PORTS_BUILD) --x86 $(X86_BUILD) \
		--disk $(X86_DISK_BUS) --timeout 240

# The gate of ADR 0027: one machine boots two roots. The first image is the
# MicroFS volume of `make x86-disk`, the second is the same /bin and /etc in an
# ext2 volume, and the same commands (`ls /bin`, `cat /etc/setting`, `exec
# /bin/hello`, `df /`) have to answer the same thing out of each; the FAT32
# volume of the second partition is mounted at /data and written.
x86-root-smoke: x86-disk
	python3 tools/x86_root_smoke.py --image $(X86_PROGRAMS_IMAGE) \
		--ext2-image $(X86_PROGRAMS_EXT2_IMAGE) --disk $(X86_DISK_BUS) --timeout 180

# The machine of a firmware (UEFI): the same kernel as `make x86-build` - one
# kernel for the two ways of starting it - the loader of the firmware
# (ports/port_x86_64/loader/uefi, a PE/COFF application) and one FAT volume
# that carries the three files a boot needs (tools/uefi_esp.py). No bootloader
# of the distribution is involved: the loader is the project's, it reads the
# kernel and the disk image out of the volume with the file services of the
# firmware and leaves the boot services before the kernel runs.
#
# The loader is built by clang and lld-link, because the format of an
# application of a firmware is the PE/COFF of the 64-bit Windows machine and
# that target is what a compiler of the host knows; the target says what is
# missing instead of building an image that does not boot.
UEFI_BUILD := build-uefi
UEFI_IMAGE := $(UEFI_BUILD)/esp.img

# The firmware of the machine: OVMF, the EDK2 build that QEMU runs. The first
# pair that is installed is the one both targets use.
UEFI_CODE := $(firstword $(wildcard /usr/share/edk2/x64/OVMF_CODE.4m.fd \
	/usr/share/edk2/ovmf/OVMF_CODE.fd /usr/share/OVMF/OVMF_CODE.fd \
	/usr/share/qemu/OVMF_CODE.fd /usr/share/edk2-ovmf/x64/OVMF_CODE.fd))
UEFI_VARS := $(subst CODE,VARS,$(UEFI_CODE))

uefi: x86-build x86-disk
	cmake --build $(X86_BUILD) --target uefi
	python3 tools/uefi_esp.py --image $(UEFI_IMAGE) --build $(X86_BUILD) \
		--disk $(X86_PROGRAMS_IMAGE)

# The stick of a firmware machine: the ESP above, wrapped in the table of the
# partitions a firmware reads before it looks for a file. A stick written with
# `dd` from this image boots on a machine of UEFI only - no BIOS, no CD, no
# optical drive - which is what a laptop of these years is. The size is the
# table and the volume; the rest of a stick stays as it was.
UEFI_STICK := $(UEFI_BUILD)/stick.img
UEFI_STICK_MB := 64

uefi-stick: uefi
	rm -f $(UEFI_STICK)
	truncate -s $(UEFI_STICK_MB)M $(UEFI_STICK)
	printf 'label: gpt\n,48M,uefi\n' | sfdisk -q $(UEFI_STICK)
	dd if=$(UEFI_IMAGE) of=$(UEFI_STICK) bs=512 seek=2048 conv=notrunc status=none
	@echo "$(UEFI_STICK): write it with dd to a stick and start it from the menu of the firmware"

# The machine of a person: the window of QEMU shows the picture of the
# firmware and the keyboard of the window is the PS/2 keyboard of the port.
# The variables of the firmware are a file of the system and a boot writes to
# them: the run takes a copy, the way the smoke test does, and the firmware of
# the machine stays what it was.
uefi-run: uefi
	@test -n "$(UEFI_CODE)" || { echo "uefi-run: no OVMF (install edk2-ovmf)" >&2; exit 2; }
	@test -f "$(UEFI_VARS)" || { echo "uefi-run: $(UEFI_VARS) is not there" >&2; exit 2; }
	cp "$(UEFI_VARS)" $(UEFI_BUILD)/vars.fd
	qemu-system-x86_64 -machine q35 -m 512 -smp 2 \
		-drive if=pflash,format=raw,readonly=on,file=$(UEFI_CODE) \
		-drive if=pflash,format=raw,file=$(UEFI_BUILD)/vars.fd \
		-drive file=$(UEFI_IMAGE),format=raw,if=ide,index=0 \
		-no-reboot

# The gate: the firmware starts the loader, the loader starts the kernel, the
# kernel mounts the disk it was handed and the shell answers on the frame of
# the firmware (tools/uefi_smoke.py reads the picture of the card as well as
# the serial line).
uefi-smoke: uefi
	python3 tools/uefi_smoke.py --image $(UEFI_IMAGE) --timeout 180

# The gate of the two cores (ADR 0012): the kernel of `x86-smp-build` boots on
# a machine of two processors and has to start both of them, keep a task on
# one core, take work on the other and read the clock of the AP. The disk is
# the same image of `x86-disk`; the tool drives the `smp` command of the shell
# and compares the ticks of the core 1 before and after the load. It is not a
# part of `make ci`: the two-core image is not green until the kernel lanes of
# the branch land.
x86-smp-smoke: x86-smp-build x86-disk
	python3 tools/x86_smp_smoke.py --build $(X86_SMP_BUILD) --image $(X86_PROGRAMS_IMAGE)

# The gate of ADR 0029: a volume of 5 GiB is an ordinary volume. The tool
# writes the two images of the plan itself - ext2 and MicroFS, both of 5 GiB -
# with `/bin/mkbig` of the build, boots each as the root of the machine, writes
# a file (128 MiB of ext2, 16 MiB of MicroFS) and reads it back, and then the
# tools of the world read the image of the session: e2fsck -fn finds nothing
# to repair, debugfs sees the file, and the fsck of the machine says the
# MicroFS volume is consistent. Not a part of `make ci`: a file of 128 MiB
# through the disk of QEMU is about two minutes of emulation and the images
# are gigabytes of the host.
x86-volume-smoke: x86-build build
	python3 tools/x86_volume_smoke.py --build $(X86_BUILD) --mkfs $(X86_MKFS) \
		--disk $(X86_DISK_BUS)

# The power cut of ADR 0038: a machine that writes a file of 16 MiB on a
# MicroFS volume and dies in the middle of the write (the process is taken
# away, not asked to stop - `-no-reboot` already means a reset is the end of
# the machine), then the same image boots again and the volume is asked to
# answer: `fsck` walks it, `fsck -y` leaves it consistent, and the file that
# was whole before the reset comes back byte for byte. Ten checks, about five
# seconds of the host; not a part of `make ci` because it needs the disk of
# QEMU and the image of `make x86-build`.
x86-mfs-smoke: x86-build build
	python3 tools/x86_mfs_smoke.py --build $(X86_BUILD) --mkfs $(X86_MKFS)

# The gate of ADR 0047: twenty cuts of one write. The image is copied, the
# copy is booted and killed in the middle of a write of a file (the process
# goes, not the machine: `-no-reboot` makes a reset the end of it), and the
# same blocks are booted again. The mount replays the journal or discards
# the draft, `fsck` finds nothing, the file written before the cut is whole,
# and the file the cut interrupted is either whole or absent - never a
# prefix. Twenty points of the write, about a minute of the host.
x86-cut-smoke: x86-build build
	python3 tools/x86_cut_smoke.py --build $(X86_BUILD) --mkfs $(X86_MKFS)

# The name of the lane: the same gate.
cut-smoke: x86-cut-smoke

# The numbers of the machine (tools/x86_bench.py, docs/bench.md): three boots
# of the x86-64 port, `/bin/bench` of the build in the image for each run, and
# a table of fixed sizes - the call of the door and its latency, a fork and an
# exec, 4 MiB through a pipe and the latency of one byte of one, a file of
# 16 MiB written and read, and the console. The value of a row is the median of
# the runs, which is what two releases are compared by; the sizes are the
# constants of the tool and do not change with the release.
#
# Not a part of `make ci`: the pipe of 1.6 holds 256 bytes and the file is
# 16 MiB, so three runs are about half a minute of emulation.
# `make bench BENCH_ARGS="--runs 1"` is one boot for a look and
# `make bench BENCH_ARGS="--json bench.json"` keeps the table in a file.
BENCH_ARGS ?=

bench: x86-build build
	python3 tools/x86_bench.py --build $(X86_BUILD) --mkfs $(X86_MKFS) $(BENCH_ARGS)

# The screen without a display: the monitor of QEMU reads the framebuffer of
# the new screen with screendump (a picture has no text memory) and the text
# mode of the fallback with pmemsave, and presses the keys of the keyboard, so
# a build farm can check both screens of ADR 0023 and IRQ1. Not a part of
# `make ci`: the monitor is a tool of its own.
x86-vga-smoke: x86-build x86-disk
	python3 tools/x86_vga_smoke.py --image $(X86_PROGRAMS_IMAGE)

# The keyboard of the USB (ADR 0040): the machine of this run has no PS/2
# controller at all (`-machine pc,i8042=off`) and the only keyboard on it is a
# device of the xHCI controller, so a line typed through the monitor of QEMU
# reaches the kernel through the rings of the controller and the reports of the
# HID device, and nothing else. The same image of the disk as the smoke test =
# the shell, /bin and /etc are the files of the release.
usb-smoke: x86-build x86-disk x86-net-build
	python3 tools/x86_usb_smoke.py --image $(X86_PROGRAMS_IMAGE) \
		--ext2-image $(X86_PROGRAMS_EXT2_IMAGE) --net-build $(X86_NET_BUILD) --timeout 180

# The same gate under the name of the other machines of the port.
x86-usb-smoke: usb-smoke

# The stick of a person, attached to the controller of the USB read-only: the
# test mounts its first partition, lists it and compares the SHA-256 of the
# whole device before and after. The device is the one a person names and the
# run needs the permission to read it (`USB_STICK=/dev/sdb`, or sudo).
USB_STICK ?= /dev/sdc

usb-stick-smoke: x86-build x86-disk
	python3 tools/x86_usb_smoke.py --image $(X86_PROGRAMS_IMAGE) --stick $(USB_STICK)

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

X86_DISK_IMAGE_DRIVE := -drive file=$(X86_DISK_IMAGE),format=raw,if=ide,index=0,cache=writeback
ifeq ($(X86_DISK_BUS),virtio)
X86_DISK_IMAGE_DRIVE := -drive file=$(X86_DISK_IMAGE),format=raw,if=virtio,cache=writeback
endif

x86-run-disk: x86-build $(X86_DISK_IMAGE)
	@qemu-system-x86_64 -display help 2>&1 | grep -qw "$(X86_DISPLAY)" || { \
		echo "QEMU has no '$(X86_DISPLAY)' display: install qemu-ui-gtk" >&2; \
		echo "(Arch: sudo pacman -S qemu-ui-gtk) or use make x86-run-disk-serial" >&2; \
		exit 1; }
	qemu-system-x86_64 -kernel $(X86_BUILD)/loader32.elf \
		-initrd $(X86_BUILD)/microos64.bin -m 256 -no-reboot \
		-display $(X86_DISPLAY) -serial null -vga std \
		$(X86_DISK_IMAGE_DRIVE)

x86-run-disk-serial: x86-build $(X86_DISK_IMAGE)
	qemu-system-x86_64 -kernel $(X86_BUILD)/loader32.elf \
		-initrd $(X86_BUILD)/microos64.bin -m 256 -nographic -no-reboot \
		$(X86_DISK_IMAGE_DRIVE)

x86-disk-smoke: x86-disk
	python3 tools/x86_disk_smoke.py --image $(X86_BUILD)/disk-smoke.img --disk $(X86_DISK_BUS)
	python3 tools/x86_disk_smoke.py --image $(X86_BUILD)/disk-smoke-ahci.img --disk ahci
	python3 tools/x86_disk_smoke.py --image $(X86_BUILD)/disk-smoke-nvme.img --disk nvme

# The machine that goes off (ADR 0050): a boot writes a file and says
# `shutdown`, the S5 of ACPI ends the QEMU process, the same image starts
# again on a clean volume with the file still there, `reboot` resets the
# guest in the same process (so this one does not pass -no-reboot) and the
# file survives the reset. The negative - a disk that does not answer the
# flush keeps the machine alive - is `test_power.c` of the host tests.
power-smoke: x86-disk
	python3 tools/x86_power_smoke.py --image $(X86_BUILD)/power-smoke.img --disk $(X86_DISK_BUS)

# The disk of a laptop (ADR 0041): the same image of `make x86-disk` on the
# SATA controller of a chipset, in a machine of the q35 family that has no IDE
# channel at all. The kernel finds the controller (class 01/06), reads the
# identify data, the table and the volume of the first partition, and mounts
# it read-write: the volume carries the header of MicroFS and `/etc/volumes`
# names `ahci0p1`, so the rule lets the writes through and the root mount
# asks for them. The tool writes a file, boots the same image again and reads
# it out of the medium - the file survives a reset. Two more boots prove the
# refusals: a copy whose `/etc/volumes` names nothing is mounted read-only and
# a write is refused with the sentence of the rule, and a copy with the magic
# of btrfs is refused at the mount. The SHA-256 of each negative image is the
# same before and after the boot.
ahci-smoke: x86-build x86-disk
	python3 tools/x86_ahci_smoke.py --image $(X86_PROGRAMS_IMAGE) --build $(X86_BUILD)

# The disk of a modern laptop (ADR 0042): the same image of `make x86-disk` on
# the NVMe controller of the PCI bus - the admin queue, the two identifiers and
# one I/O queue pair created through the admin queue - in a machine of the
# i440fx family, where the function sits on the bus 0. The same four boots as
# the SATA gate: the volume of `nvme0p1` named in `/etc/volumes` is written and
# its file survives the reset, the volume the list does not name is read-only
# with the hash of the image unchanged, and a volume of btrfs is refused at the
# mount.
nvme-smoke: x86-build x86-disk
	python3 tools/x86_nvme_smoke.py --image $(X86_PROGRAMS_IMAGE) --build $(X86_BUILD)

# The filesystem of 1.4: rename and rmdir through the programs of /bin,
# `fsck ext2 -y` on a broken image (and e2fsck of the host after it), and the
# journal of SQLite rolled back after a reset of the machine.
x86-fs-smoke: x86-disk
	python3 tools/x86_fs_smoke.py --image $(X86_PROGRAMS_IMAGE) \
		--ext2-image $(X86_PROGRAMS_EXT2_IMAGE)

# The network of the port (K6): a build of its own, with the HTTP client, the
# page of the state and the clock of NTP of components/net
# (-DMICRO_OS_X86_NET=ON) and a QEMU machine on its user-mode network. Not a
# part of `make ci`: the network of the test is the loopback of the host, and
# a build farm is not a place to open a port. The default image keeps the flag
# off, so `make x86-smoke` is the same machine it always was.
#
# Since 2.0 the smoke test boots three machines of that image (ADR 0055): the
# card of virtio of K6, the e1000e of a laptop on the same user network, and
# the e1000e on a wire whose other end never answers - where the client of
# DHCP has to fall back to the fixed address in its budget and say so.
X86_NET_BUILD := build-x86-net

# The machine of `make x86-net-run` forwards 8080 of the loopback to 8080 of
# the guest, so `httpd` on the console and `curl http://127.0.0.1:8080/` on
# the host are the same page. The smoke test makes its own forward, on a port
# it found free.
X86_NET_RUN_DEVICE := -netdev user,id=n0,hostfwd=tcp:127.0.0.1:8080-:8080 \
	-device virtio-net-pci,netdev=n0,disable-modern=on

x86-net-build:
	cmake -B $(X86_NET_BUILD) -S ports/port_x86_64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DMICRO_OS_X86_NET=ON -DMICRO_OS_TELNET=ON $(X86_SIGN_ARGS)
	cmake --build $(X86_NET_BUILD) --target image

x86-net-run: x86-net-build
	qemu-system-x86_64 -kernel $(X86_NET_BUILD)/loader32.elf \
		-initrd $(X86_NET_BUILD)/microos64.bin -m 256 -nographic -no-reboot \
		$(X86_NET_RUN_DEVICE)

x86-net-smoke: x86-net-build
	X86_BUILD=$(X86_NET_BUILD) python3 tools/x86_net_smoke.py

# The whole machine in one command: the kernel with the full feature set (the
# network of K6 is the one optional block), the disk with /bin and /etc, and a
# run.sh that boots the two with a card on the user network. `x86-all` is not
# `make bundle` (that packs a release); it is the machine a person can start
# without knowing which of the builds above it is made of.
X86_ALL_BUILD := build-x86-all
X86_ALL_IMAGE := $(X86_ALL_BUILD)/x86-all.img
# The same tree in an ext2 volume, with the FAT32 volume of /data after it:
# the second root of the release (ADR 0027 §3).
X86_ALL_EXT2_IMAGE := $(X86_ALL_BUILD)/x86-ext2.img
X86_ALL_RUN := $(X86_ALL_BUILD)/run.sh

x86-all-build:
	cmake -B $(X86_ALL_BUILD) -S ports/port_x86_64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DMICRO_OS_X86_NET=ON -DMICRO_OS_TELNET=ON $(X86_SIGN_ARGS)
	cmake --build $(X86_ALL_BUILD) --target image

x86-all: x86-all-build build
	python3 tools/x86_image.py --image $(X86_ALL_IMAGE) \
		--build $(X86_ALL_BUILD) --mkfs $(X86_MKFS)
	python3 tools/x86_image.py --image $(X86_ALL_EXT2_IMAGE) --fs ext2 \
		--build $(X86_ALL_BUILD) --mkfs $(X86_MKFS)
	@printf '%s\n' \
		'#!/bin/sh' \
		'# Boot the complete Micro OS x86-64 machine: the kernel of `make x86-all`,' \
		'# its disks and a virtio-net card on the user network (8080 forwarded).' \
		'# X86_SERIAL=1 gives the serial console and no window; X86_ROOT=ext2' \
		'# boots the ext2 volume of the same tree instead of the MicroFS one.' \
		'cd "$$(dirname "$$0")" || exit 1' \
		'root="$${X86_ROOT:-mfs}"' \
		'if [ "$$root" = ext2 ]; then image=x86-ext2.img; else image=x86-all.img; fi' \
		'if [ "$${X86_SERIAL:-0}" = 1 ]; then' \
		'    exec qemu-system-x86_64 -kernel loader32.elf -initrd microos64.bin \' \
		'        -m 256 -nographic -no-reboot \' \
		'        -drive "file=$$image,format=raw,if=ide,index=0,cache=writeback" \' \
		'        -netdev user,id=n0,hostfwd=tcp:127.0.0.1:8080-:8080 \' \
		'        -device virtio-net-pci,netdev=n0,disable-modern=on' \
		'fi' \
		'exec qemu-system-x86_64 -kernel loader32.elf -initrd microos64.bin \' \
		'    -m 256 -no-reboot -display "$${X86_DISPLAY:-gtk}" -serial null -vga std \' \
		'    -drive "file=$$image,format=raw,if=ide,index=0,cache=writeback" \' \
		'    -netdev user,id=n0,hostfwd=tcp:127.0.0.1:8080-:8080 \' \
		'    -device virtio-net-pci,netdev=n0,disable-modern=on' > $(X86_ALL_RUN)
	@chmod +x $(X86_ALL_RUN)
	@echo "x86-all: the machine is ready"
	@echo "  kernel    $(X86_ALL_BUILD)/microos64.bin"
	@echo "  loader    $(X86_ALL_BUILD)/loader32.elf"
	@echo "  disk      $(X86_ALL_IMAGE)   (X86_ROOT=ext2: $(X86_ALL_EXT2_IMAGE))"
	@echo "  run       $(X86_ALL_RUN)   (X86_SERIAL=1 for the serial console)"

x86-all-run: x86-all
	X86_DISPLAY=$(X86_DISPLAY) $(X86_ALL_RUN)

x86-all-run-serial: x86-all
	X86_SERIAL=1 $(X86_ALL_RUN)

# The artifact a person downloads: the x86-64 tar of the release, unpacked
# and started as it is. The target builds the complete machine and its tar
# first (both are incremental), then runs the tar and not the tree: what a
# person gets is what this starts. The tar is the x86-64 half of the bundle
# alone, so this target needs no ESP-IDF and no board.
X86_USER_DIR := build-x86-user
X86_SERIAL ?= 0

x86-user: x86-all bundle-x86
	@tar=$$(ls -t dist/MicroOS-x86_64-*.tar.gz 2>/dev/null | head -1); \
	if [ -z "$$tar" ]; then \
		echo "x86-user: dist/ has no x86-64 tar" >&2; exit 1; \
	fi; \
	echo "x86-user: $$tar -> $(X86_USER_DIR)"; \
	rm -rf $(X86_USER_DIR); mkdir -p $(X86_USER_DIR); \
	tar -xzf "$$tar" -C $(X86_USER_DIR); \
	cd $(X86_USER_DIR) && X86_SERIAL=$(X86_SERIAL) X86_DISPLAY=$(X86_DISPLAY) ./run.sh

x86-user-serial: X86_SERIAL=1
x86-user-serial: x86-user

# The bootable medium of the port (K13, ADR 0026): one image with the loader,
# the kernel and a 12 MB root image, which boots in four machines - a BIOS and
# a firmware of UEFI, each from a CD drive and from a USB stick written with
# `dd`. The three files are the ones of the builds above - nothing is built
# twice - and tools/x86_iso.py stages them and writes the image: the El Torito
# record of the BIOS, the one of the EFI, a GPT whose ESP is the FAT image of
# the EFI record, and the boot sector of a stick (`limine bios-install`). The
# root image is the disk of `make x86-disk`: /bin and /etc written by the
# tool, mounted read-only by the port because a CD and a stick written from
# the image are not places to write.
X86_ISO_BUILD := build-x86-iso
X86_ISO_IMAGE := $(X86_ISO_BUILD)/microos.iso
X86_ISO_BOOTLOADER ?= auto

# The root of the ISO is a module of the boot, and the loader refuses a
# module that runs over the 16 MiB of KERNEL_PHYS where the kernel is copied:
# a limine module starts low and a root image of 16 MiB reaches the mark, an
# image of 8 MiB does not. The CD is read-only and the whole set of 65
# objects fits in 8 MiB, so the medium carries the smaller one and the disk
# of the machine keeps its 16.
X86_ISO_ROOT := $(X86_ISO_BUILD)/root.img

x86-iso: x86-disk build
	# The root image must end below KERNEL_PHYS (16 MiB): a module that lies
	# over the copy of the kernel is refused by the loader, because the copy
	# would destroy the volume exactly when the kernel is about to read it.
	# The boot loader of the machine places a module right after the kernel
	# module, and twelve megabytes leave the room for the programs of the
	# release and the store of roots with three megabytes to spare.
	python3 tools/x86_image.py --image $(X86_ISO_ROOT) --build $(X86_BUILD) \
		--mkfs build-host/tools/microos_mkfs --size-mb 12
	python3 tools/x86_iso.py --loader $(X86_BUILD)/loader32.elf \
		--kernel $(X86_BUILD)/microos64.bin --root $(X86_ISO_ROOT) \
		--out $(X86_ISO_IMAGE) --stage $(X86_ISO_BUILD)/iso \
		--bootloader $(X86_ISO_BOOTLOADER)

x86-iso-run: x86-iso
	qemu-system-x86_64 -cdrom $(X86_ISO_IMAGE) -m 256 -nographic -no-reboot -boot d

x86-iso-smoke: x86-iso
	python3 tools/x86_iso_smoke.py --iso $(X86_ISO_IMAGE)

# The other three boots of the same image: the firmware of a UEFI (OVMF) from
# a DVD drive and from a USB stick, and a BIOS from a stick. The stick is the
# proof that the image is a medium a person can carry: the firmware is not
# told which device it has, it finds the ESP of the GPT (UEFI) or the boot
# sector of the MBR (BIOS) by itself. OVMF is a package of the distribution
# (/usr/share/edk2/x64), and a machine without it skips the two boots of the
# firmware with a message, the way `make ci` treats a missing QEMU.
x86-iso-uefi-smoke: x86-iso
	python3 tools/x86_iso_uefi_smoke.py --iso $(X86_ISO_IMAGE) --firmware uefi --media both

x86-iso-usb-smoke: x86-iso
	python3 tools/x86_iso_uefi_smoke.py --iso $(X86_ISO_IMAGE) --firmware bios --media usb

# The campaign of 1.0 (docs/wiki/testing.md). The session is one boot of the
# x86-64 machine, ten minutes long by default, under the mixed load of
# tools/soak.py. It is not a part of `make ci`: it takes ten minutes and it
# is run for a release candidate, not for a push.
SOAK_BUILD   ?= $(X86_NET_BUILD)
SOAK_IMAGE   ?= $(SOAK_BUILD)/soak.img
SOAK_MINUTES ?= 10
SOAK_ITERATIONS ?=
SOAK_ARGS    ?=
# The image of the two-core session is its own: the programs are the current
# build of `x86-disk` (the kernel is the SMP one, the images of the programs
# do not depend on it) and the volume is filled for the session alone.
SOAK_SMP_IMAGE ?= build-x86-smp/soak.img

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

# The same session on the kernel of two cores: `--smp` boots the build of
# `x86-smp-build` with -smp 2 and the load asserts the two-core invariants on
# top of the ones of 1.0 - the report of `smp`, the ticks of both cores
# moving, the queues consistent and no exception in any answer. The image is
# the full volume of the session, made from the current programs of
# `x86-disk` (the images of the programs do not depend on the kernel), and
# the boot is the two-core kernel on that image.
soak-smp: x86-smp-build x86-disk
	rm -f $(SOAK_SMP_IMAGE) $(SOAK_SMP_IMAGE).full
	python3 tools/soak_image.py --build $(X86_BUILD) --image $(SOAK_SMP_IMAGE) \
		--mkfs $(X86_MKFS)
	python3 tools/soak.py --build $(X86_SMP_BUILD) --image $(SOAK_SMP_IMAGE) \
		--mkfs $(X86_MKFS) --minutes $(SOAK_MINUTES) --smp --no-image $(SOAK_ARGS)

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

# The board half of the session: build, flash and three minutes with one
# boot. It is a target of the campaign and not of `make ci` (no runner has
# the board).
#
# Three minutes, not ten: from 1.2 the board is the compatibility port of
# ADR 0018 and a release gives it small changes - the volume, the flash map
# and the boot path do not move. A release that does touch them (1.1.0 moved
# the volume) asks for the long session on purpose: BOARD_SOAK_MINUTES=10.
BOARD_SOAK_MINUTES ?= 3

board-soak:
	tools/board_soak.sh --minutes $(BOARD_SOAK_MINUTES)

bundle: bundle-board bundle-x86
# The two halves write the same dist/checksums.txt one after the other, so a
# `make -j` bundle must not run them at the same time.
.NOTPARALLEL: bundle

# The board: the firmware, esptool and the partition table. It is flashed,
# not run, and the ESP-IDF build is the one thing it cannot do without.
bundle-board: idf-check
	. $(IDF_EXPORT) && MICROOS_SIGN_KEY="$(MICROOS_SIGN_KEY)" \
		idf.py -D IDF_TARGET=$(IDF_TARGET) build && \
		MICROOS_SIGN_KEY="$(MICROOS_SIGN_KEY)" tools/bundle.sh --board-only $(IDF_BUILD) dist

# The other machine: the complete machine and its tar. It is run, not
# flashed, so nothing of the board is needed here - not ESP-IDF, not esptool,
# not the build/ of the firmware.
bundle-x86: x86-all
	tools/bundle.sh --x86-only $(X86_ALL_BUILD) dist
