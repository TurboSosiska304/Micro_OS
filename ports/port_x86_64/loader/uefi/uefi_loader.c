/*
 * Micro OS — the loader of a UEFI machine: the firmware starts this, and this
 * boots the kernel.
 *
 * A laptop has no `-kernel` argument. Its firmware reads a file from a
 * FAT-formatted partition, `\EFI\BOOT\BOOTX64.EFI`, and starts it as a program
 * of its own kind: a PE/COFF image written for the 64-bit machine, with no
 * operating system under it, which asks the firmware for the services of the
 * boot and hands the machine to an operating system when it is done. This file
 * is that program for Micro OS.
 *
 * What it does, in the order the firmware allows it:
 *
 *  1. asks the firmware for the picture (EFI_GRAPHICS_OUTPUT_PROTOCOL), picks
 *     the mode the console of the port can draw on - the biggest one within
 *     8 MiB of frame, the mode the firmware is in when it is as big - programs
 *     it and reads back where the pixels are;
 *  2. reads `microos64.bin` (the kernel) and `disk.img` (the volume of the
 *     machine) from the volume the firmware started it from, with
 *     EFI_SIMPLE_FILE_SYSTEM_PROTOCOL: the machine gets a disk without a driver
 *     for the controller that disk is on, because the bytes are in the memory
 *     before the kernel runs;
 *  3. copies the kernel to a 2 MiB-aligned block of the memory the firmware
 *     gave it, writes the map of the memory and the two modules in the
 *     multiboot 1 shape the port already reads, and writes the hand-off of this
 *     machine (the picture, the root of the ACPI tables, the address of the
 *     image) in the structure of `port_x86_64_boot.h`;
 *  4. leaves the boot services (`ExitBootServices`) - after this there is no
 *     memory, no file and no console of the firmware - builds the page tables
 *     of the boot, turns the paging of the long mode on with the image mapped
 *     at PORT_X86_64_KERNEL_BASE, and jumps into the kernel with the arguments
 *     the port's entry expects (`src/boot64.S`).
 *
 * The kernel is the same file the BIOS path boots (`make x86-build`): there is
 * one kernel for the two machines, and the port chooses its screen and its
 * disk from what the boot handed it. That is why the hand-off is a structure
 * of the tree and not a private arrangement of this file.
 *
 * The loader has no libc and links nothing: the strings, the copies and the
 * numbers below are the whole of its runtime.
 */
#include <stdbool.h>
#include <stddef.h>

#include "efi.h"

#include "port_x86_64_boot.h"

/* --- the numbers of the loader -------------------------------------------- */

/* COM1, direct. The console of the firmware is a console of its own screen and
 * of the line of a serial port when the firmware was built with one; the loader
 * writes its lines to both, so a machine whose UEFI speaks to the glass alone
 * still tells a person what it did. */
#define COM1_PORT 0x3F8u

/* The largest frame the port accepts. The console of the kernel keeps a shadow
 * of 16 bits per pixel beside the frame of the firmware when the frame is not
 * already 565, and the heap of the port is the memory between the image and the
 * arena of the programs: 8 MiB is a frame of 2048x1024 of four bytes, which is
 * more than a laptop panel needs and less than the heap can afford. */
#define FRAME_MAX_BYTES (8ull * 1024ull * 1024ull)

/* The smallest picture that is a console: a grid of 80 columns of 8 pixels and
 * 30 lines of 16, which is the room a shell of this machine needs. */
#define FRAME_MIN_WIDTH 640u
#define FRAME_MIN_HEIGHT 480u

/* Where the kernel may be placed. The port keeps its heap between the image and
 * the arena of the programs (32 MiB), so the address of the BIOS loader (16 MiB)
 * is the first choice, and the search walks down from below it in steps of
 * 2 MiB - the chunk the boot's page table maps the image with, and a chunk of
 * that size has to start on a 2 MiB boundary of the physical memory. The image
 * has to end below the arena. */
#define KERNEL_LOAD_FIRST 0x01000000ull
#define KERNEL_LOAD_LAST 0x00E00000ull
#define KERNEL_LOAD_STEP 0x00200000ull
#define KERNEL_LOAD_FLOOR 0x00200000ull
#define KERNEL_LOAD_END 0x02000000ull

/* The memory the loader hands the kernel has to be in the first gigabyte: the
 * identity map the kernel builds for itself (src/paging.c) covers that one, and
 * the port reads the map of the memory, the modules and the hand-off through it
 * for as long as the machine runs - the disk image in particular, which is the
 * volume of the boot (src/blockdev_module.c, IDENTITY_LIMIT).
 *
 * The loader's own code and stack may be higher: the map of *its* jump covers
 * the first 4 GiB, and the copy of the kernel is placed below 32 MiB. */
#define IDENTITY_LIMIT 0x40000000ull

/* The map of the jump of this loader, which is the limit of the loader itself. */
#define LOADER_MAP_LIMIT 0x100000000ull

/* The stack the loader runs on after the boot services are gone: the one the
 * firmware gave this program is memory of the boot services, and the loader
 * leaves them. 64 KiB carries the copy of a file and the call of the jump. */
#define LOADER_STACK_BYTES (64u * 1024u)

/* The regions of the map of the memory the port reads: an entry of its format
 * is 24 bytes and a machine of a person describes a hundred of them. */
#define MB_MMAP_MAX 256u

/* The multiboot 1 information as the port reads it (src/port_x86_64.c): the
 * fields below are the whole of what the port uses, and the two flags are the
 * ones it looks at. */
#define MB_FLAG_MODS 0x00000008u
#define MB_FLAG_MMAP 0x00000040u
#define MB_MMAP_USABLE 1u
#define MB_MMAP_RESERVED 2u

struct mb_info {
    efi_u32 flags;
    efi_u32 mem_lower;
    efi_u32 mem_upper;
    efi_u32 boot_device;
    efi_u32 cmdline;
    efi_u32 mods_count;
    efi_u32 mods_addr;
    efi_u32 syms[4];
    efi_u32 mmap_length;
    efi_u32 mmap_addr;
};

struct mb_module {
    efi_u32 start;
    efi_u32 end;
    efi_u32 string;
    efi_u32 reserved;
};

struct mb_mmap_entry {
    efi_u32 size;
    efi_u32 base_low;
    efi_u32 base_high;
    efi_u32 length_low;
    efi_u32 length_high;
    efi_u32 type;
};

/* The two modules of the boot: the kernel and the disk image. The port reads
 * the first for the end of the heap and the second as the volume of the
 * machine (src/blockdev_module.c). */
#define MODULE_COUNT 2u
#define MODULE_KERNEL 0u
#define MODULE_DISK 1u

/* The text of a line: the line of the loader is built in one buffer, because
 * the loader has no printf. 192 bytes hold a sentence and four numbers. */
struct text {
    char bytes[192];
    efi_u32 length;
};

struct loader {
    struct efi_system_table *st;
    struct efi_boot_services *bs;
    efi_handle image;

    struct efi_graphics_output_protocol *gop;
    struct efi_graphics_output_mode_information *gop_info;

    struct mb_info *mb;
    struct mb_module *modules;
    struct mb_mmap_entry *mmap;
    efi_u32 mmap_entries;

    struct port_x86_64_boot_info *handoff;

    efi_phys_addr kernel_phys;
    efi_u64 kernel_bytes;
    efi_phys_addr disk_phys;
    efi_u64 disk_bytes;

    efi_phys_addr stack_phys;
};

static struct loader g_loader;

/* --- the runtime the loader needs and does not have ----------------------- */

/* The compiler emits a call to these for a large struct that is zeroed or
 * copied, and a loader links no libc. The four are the whole of it. */
void *memset(void *destination, int value, unsigned long long count)
{
    unsigned char *bytes = (unsigned char *)destination;
    unsigned long long i;

    for (i = 0; i < count; i++) {
        bytes[i] = (unsigned char)value;
    }

    return destination;
}

void *memcpy(void *destination, const void *source, unsigned long long count)
{
    unsigned char *to = (unsigned char *)destination;
    const unsigned char *from = (const unsigned char *)source;
    unsigned long long i;

    for (i = 0; i < count; i++) {
        to[i] = from[i];
    }

    return destination;
}

int memcmp(const void *left, const void *right, unsigned long long count)
{
    const unsigned char *a = (const unsigned char *)left;
    const unsigned char *b = (const unsigned char *)right;
    unsigned long long i;

    for (i = 0; i < count; i++) {
        if (a[i] != b[i]) {
            return (int)a[i] - (int)b[i];
        }
    }

    return 0;
}

/* --- the line of the loader ----------------------------------------------- */

static void port_out(efi_u16 port, efi_u8 value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "d"(port));
}

static efi_u8 port_in(efi_u16 port)
{
    efi_u8 value;

    __asm__ volatile("inb %1, %0" : "=a"(value) : "d"(port));

    return value;
}

static void serial_init(void)
{
    /* 115200 8N1, the line COM1 of the port and the serial port of QEMU speak:
     * the divisor is 1 at the highest rate a UART of the PC has. */
    port_out(COM1_PORT + 1u, 0x00u); /* no interrupt of the UART */
    port_out(COM1_PORT + 3u, 0x80u); /* the divisor latch is open */
    port_out(COM1_PORT + 0u, 0x01u);
    port_out(COM1_PORT + 1u, 0x00u);
    port_out(COM1_PORT + 3u, 0x03u); /* 8N1 */
    port_out(COM1_PORT + 2u, 0xC7u); /* the FIFO, emptied */
}

static void serial_putc(char character)
{
    efi_u32 spins = 0;

    while (spins < 1000000u) {
        if ((port_in(COM1_PORT + 5u) & 0x20u) != 0u) {
            break;
        }

        spins++;
    }

    port_out(COM1_PORT, (efi_u8)character);
}

/* One line: the console of the firmware, and COM1 when the firmware has no
 * console at all. The console of a firmware of a laptop *is* a serial port on
 * a machine whose firmware was built that way (the one of QEMU is), so writing
 * to both would print every line twice; the line of a machine with no console
 * is the one of a port that has nothing else to say it on.
 *
 * The text is ASCII, which is the subset of the UTF-16 of a console that a
 * firmware of every language reads. */
static void console_line(const char *line)
{
    efi_char16 wide[192];
    efi_u32 length = 0;
    efi_u32 i;

    while (line[length] != '\0' && length < 190u) {
        length++;
    }

    for (i = 0; i < length; i++) {
        wide[i] = (efi_char16)(unsigned char)line[i];
    }

    wide[length] = 0;

    if (g_loader.st != NULL && g_loader.st->console_out != NULL) {
        g_loader.st->console_out->output_string(g_loader.st->console_out, wide);
        g_loader.st->console_out->output_string(g_loader.st->console_out, u"\r\n");
        return;
    }

    for (i = 0; i < length; i++) {
        serial_putc(line[i]);
    }

    serial_putc('\r');
    serial_putc('\n');
}

/* --- the lines the loader builds ------------------------------------------ */

static void text_reset(struct text *out, const char *line)
{
    out->length = 0;

    while (*line != '\0' && out->length + 1u < (efi_u32)sizeof(out->bytes)) {
        out->bytes[out->length++] = *line++;
    }

    out->bytes[out->length] = '\0';
}

static void text_char(struct text *out, char character)
{
    if (out->length + 1u < (efi_u32)sizeof(out->bytes)) {
        out->bytes[out->length++] = character;
        out->bytes[out->length] = '\0';
    }
}

static void text_unsigned(struct text *out, efi_u64 value)
{
    char digits[24];
    efi_u32 count = 0;

    do {
        digits[count++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0u && count < sizeof(digits));

    while (count > 0u) {
        text_char(out, digits[--count]);
    }
}

static void text_hex(struct text *out, efi_u64 value)
{
    static const char digits[] = "0123456789abcdef";
    char number[16];
    efi_u32 count = 0;
    efi_u32 i;

    /* The leading zeros of a word are dropped, and the number is the one a
     * person reads in a map: 0x100000 is 16 MiB. */
    for (i = 0; i < 16u; i++) {
        efi_u32 digit = (efi_u32)((value >> ((15u - i) * 4u)) & 0xFu);

        if (digit != 0u || count > 0u) {
            number[count] = digits[digit];
            count++;
        }
    }

    if (count == 0u) {
        text_char(out, '0');
    }

    for (i = 0; i < count; i++) {
        text_char(out, number[i]);
    }
}

static void text_say(struct text *out, const char *suffix)
{
    efi_u32 i;

    for (i = 0; suffix[i] != '\0'; i++) {
        text_char(out, suffix[i]);
    }
}

static void line_number(const char *prefix, efi_u64 value, const char *suffix)
{
    struct text out;

    text_reset(&out, prefix);
    text_unsigned(&out, value);
    text_say(&out, suffix);
    console_line(out.bytes);
}

static void line_hex(const char *prefix, efi_u64 value)
{
    struct text out;

    text_reset(&out, prefix);
    text_hex(&out, value);
    console_line(out.bytes);
}

/* --- the memory of the firmware ------------------------------------------- */

/* A block of the firmware's memory below `max`: the kernel reads it through the
 * identity map of the first 4 GiB, so a block above the map is a block the
 * kernel cannot name. The allocation is a page and its alignment is the one of
 * a page, which is the alignment every reader of it wants. */
static void *alloc_pages(efi_u64 bytes, efi_u64 max, efi_memory_type type)
{
    efi_phys_addr address = max;
    efi_status status;

    if (bytes == 0u) {
        return NULL;
    }

    status = g_loader.bs->allocate_pages(AllocateMaxAddress, type, EFI_PAGES(bytes), &address);

    if (EFI_ERR(status)) {
        return NULL;
    }

    return (void *)(unsigned long long)address;
}

/* --- the picture of the firmware ------------------------------------------ */

static bool masks_are(const efi_u32 *masks, efi_u32 red, efi_u32 green, efi_u32 blue)
{
    return masks[0] == red && masks[1] == green && masks[2] == blue;
}

/* The format of a pixel as the port names it. A mode whose masks are none of
 * the formats the console can convert is refused: a picture is worth more than
 * a colour nobody can name. */
static bool pixel_format(const struct efi_graphics_output_mode_information *info, efi_u32 *format,
                         efi_u32 *masks)
{
    masks[0] = 0u;
    masks[1] = 0u;
    masks[2] = 0u;

    switch (info->pixel_format) {
    case PixelRedGreenBlueReserved8BitPerColor:
        *format = PORT_X86_64_PIXEL_RGBX32;
        return true;
    case PixelBlueGreenRedReserved8BitPerColor:
        *format = PORT_X86_64_PIXEL_BGRX32;
        return true;
    case PixelBitMask:
        masks[0] = info->pixel_information.red;
        masks[1] = info->pixel_information.green;
        masks[2] = info->pixel_information.blue;

        /* The one 16-bit mode the console of the kernel draws on as it is:
         * 5-6-5, the format of the renderer (kernel/render.h). */
        if (masks_are(masks, 0xF800u, 0x07E0u, 0x001Fu)) {
            *format = PORT_X86_64_PIXEL_RGB565;
            return true;
        }

        /* The two orders of 8-8-8 that the specification calls by name, in
         * case a firmware built them from masks instead. */
        if (masks_are(masks, 0x00FF0000u, 0x0000FF00u, 0x000000FFu)) {
            *format = PORT_X86_64_PIXEL_RGBX32;
            return true;
        }

        if (masks_are(masks, 0x000000FFu, 0x0000FF00u, 0x00FF0000u)) {
            *format = PORT_X86_64_PIXEL_BGRX32;
            return true;
        }

        return false;
    default:
        /* A buffer the firmware blits itself: there is no frame memory to
         * write a console into. */
        return false;
    }
}

/* Whether the console of the port can put a picture of this mode on the glass:
 * a pixel of 16 or 32 bits in a format it converts, and a frame the shadow of
 * the console can afford. */
static bool mode_usable(const struct efi_graphics_output_mode_information *info, efi_u32 *format,
                        efi_u32 *masks)
{
    efi_u64 bytes;

    if (info == NULL) {
        return false;
    }

    if (info->horizontal_resolution < FRAME_MIN_WIDTH ||
        info->vertical_resolution < FRAME_MIN_HEIGHT) {
        return false;
    }

    if (!pixel_format(info, format, masks)) {
        return false;
    }

    bytes = (efi_u64)info->pixels_per_scan_line * info->vertical_resolution *
            (*format == PORT_X86_64_PIXEL_RGB565 ? 2u : 4u);

    return bytes <= FRAME_MAX_BYTES;
}

/* The mode the loader programs: the one the firmware is in when the console can
 * draw on it, and otherwise the biggest one of the list - the picture of a laptop
 * is its panel, and a mode that is smaller than another the firmware offers is a
 * picture of less room for the shell. A tie goes to the mode the firmware listed
 * first, which is its own preference. */
static bool choose_mode(void)
{
    struct efi_graphics_output_protocol *gop = g_loader.gop;
    efi_u32 masks[3];
    efi_u32 format;
    efi_u32 best = 0;
    efi_u64 best_area = 0;
    efi_status status;
    efi_u32 max = gop->mode->max_mode;
    efi_u32 i;

    status = gop->query_mode(gop, gop->mode->mode, &gop->mode->size_of_info, &gop->mode->info);

    if (!EFI_ERR(status) && mode_usable(gop->mode->info, &format, masks)) {
        best = gop->mode->mode;
        best_area =
            (efi_u64)gop->mode->info->horizontal_resolution * gop->mode->info->vertical_resolution;
    }

    for (i = 0; i < max; i++) {
        struct efi_graphics_output_mode_information *info = NULL;
        efi_u64 size = 0;
        efi_u64 area;

        if (EFI_ERR(gop->query_mode(gop, i, &size, &info)) || info == NULL) {
            continue;
        }

        if (!mode_usable(info, &format, masks)) {
            continue;
        }

        area = (efi_u64)info->horizontal_resolution * info->vertical_resolution;

        if (area > best_area) {
            best_area = area;
            best = i;
        }
    }

    if (best_area == 0u) {
        return false;
    }

    if (best != gop->mode->mode) {
        if (EFI_ERR(gop->set_mode(gop, best))) {
            return false;
        }

        if (EFI_ERR(gop->query_mode(gop, best, &gop->mode->size_of_info, &gop->mode->info))) {
            return false;
        }
    }

    g_loader.gop_info = gop->mode->info;

    return true;
}

/* The name of a format, for the line of the boot. */
static const char *format_name(efi_u32 format)
{
    switch (format) {
    case PORT_X86_64_PIXEL_RGB565:
        return "rgb565";
    case PORT_X86_64_PIXEL_RGBX32:
        return "rgbx";
    case PORT_X86_64_PIXEL_BGRX32:
        return "bgrx";
    default:
        return "unknown";
    }
}

/* --- the volume of the boot ------------------------------------------------ */

static bool open_volume(struct efi_file_protocol **root)
{
    struct efi_guid loaded_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    struct efi_guid fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    struct efi_loaded_image *image = NULL;
    struct efi_simple_file_system *fs = NULL;

    /* EFI_OPEN_PROTOCOL_GET_PROTOCOL: the interface is not the loader's to
     * close, it only asks for it. */
    if (EFI_ERR(g_loader.bs->open_protocol(g_loader.image, &loaded_guid, (void **)&image,
                                           g_loader.image, NULL, 0x02u)) ||
        image == NULL) {
        return false;
    }

    if (EFI_ERR(g_loader.bs->open_protocol(image->device_handle, &fs_guid, (void **)&fs,
                                           g_loader.image, NULL, 0x02u)) ||
        fs == NULL) {
        return false;
    }

    return !EFI_ERR(fs->open_volume(fs, root));
}

/* The whole file `name`, in a block of pages below `max`. False when the volume
 * has no such file, the file is empty, or the memory of it cannot be had. */
static bool read_file(struct efi_file_protocol *root, const efi_char16 *name, efi_u64 max,
                      efi_phys_addr *address, efi_u64 *bytes)
{
    struct efi_guid info_guid = EFI_FILE_INFO_GUID;
    struct efi_file_protocol *file = NULL;
    struct efi_file_info *info;
    efi_u8 buffer[512];
    efi_u64 size = sizeof(buffer);
    efi_u64 length;
    efi_status status;
    void *memory;

    if (EFI_ERR(root->open(root, &file, name, EFI_FILE_MODE_READ, 0u)) || file == NULL) {
        return false;
    }

    info = (struct efi_file_info *)buffer;
    status = file->get_info(file, &info_guid, &size, buffer);

    if (EFI_ERR(status) || info->file_size == 0u) {
        file->close(file);
        return false;
    }

    length = info->file_size;
    memory = alloc_pages(length, max, EfiLoaderData);

    if (memory == NULL) {
        file->close(file);
        return false;
    }

    size = length;
    status = file->read(file, &size, memory);
    file->close(file);

    if (EFI_ERR(status) || size != length) {
        return false;
    }

    *address = (efi_phys_addr)(unsigned long long)memory;
    *bytes = length;

    return true;
}

/* --- the map of the memory and the hand-off ------------------------------- */

/* Whether the bytes of a region of the firmware are memory the kernel may be
 * handed. What the boot services owned is free once they are gone, and the
 * conventional memory was always free; a region the firmware keeps for its
 * runtime, for the ACPI tables it lends and for its own use is not. The blocks
 * of this loader and of the kernel are EfiLoaderData and stay out as well -
 * they are the modules of the boot. */
static bool region_usable(efi_u32 type)
{
    switch (type) {
    case EfiBootServicesCode:
    case EfiBootServicesData:
    case EfiConventionalMemory:
        return true;
    default:
        return false;
    }
}

/* The map of the memory in the multiboot 1 shape: the 32-bit addresses of the
 * port's reader, one entry a region, and the regions past the first gigabyte
 * left out - the identity map of the kernel is that gigabyte, and a region the
 * port cannot name is not memory it may hand out (the heap of the port takes
 * the largest region of this map). The one entry that crosses the end of the
 * gigabyte is cut there. */
static bool build_memory_map(void)
{
    efi_u64 size = 0;
    efi_u64 key = 0;
    efi_u64 descriptor_size = 0;
    efi_u32 version = 0;
    struct efi_memory_descriptor *map;
    efi_u64 cursor;
    efi_u64 lower = 0;
    efi_u64 upper = 0;

    g_loader.bs->get_memory_map(&size, NULL, &key, &descriptor_size, &version);

    /* The map may grow between the call that measures it and the call that
     * reads it: the buffer is the size it asked for and one descriptor more. */
    map = alloc_pages(size + descriptor_size, IDENTITY_LIMIT, EfiLoaderData);

    if (map == NULL ||
        EFI_ERR(g_loader.bs->get_memory_map(&size, map, &key, &descriptor_size, &version))) {
        return false;
    }

    g_loader.mmap_entries = 0;

    for (cursor = 0; cursor + descriptor_size <= size && g_loader.mmap_entries < MB_MMAP_MAX;
         cursor += descriptor_size) {
        const struct efi_memory_descriptor *entry =
            (const struct efi_memory_descriptor *)(const void *)((const efi_u8 *)map + cursor);
        struct mb_mmap_entry *out = &g_loader.mmap[g_loader.mmap_entries];
        efi_u64 base = entry->physical_start;
        efi_u64 length = entry->number_of_pages * EFI_PAGE_SIZE;
        efi_u32 type;

        if (length == 0u || base >= IDENTITY_LIMIT) {
            continue;
        }

        if (base + length > IDENTITY_LIMIT) {
            length = IDENTITY_LIMIT - base;
        }

        type = region_usable(entry->type) ? MB_MMAP_USABLE : MB_MMAP_RESERVED;

        out->size = sizeof(*out) - 4u;
        out->base_low = (efi_u32)base;
        out->base_high = 0u;
        out->length_low = (efi_u32)length;
        out->length_high = 0u;
        out->type = type;
        g_loader.mmap_entries++;

        if (type != MB_MMAP_USABLE) {
            continue;
        }

        if (base < 0x100000ull) {
            efi_u64 end = base + length < 0x100000ull ? base + length : 0x100000ull;

            if (end > lower) {
                lower = end;
            }
        } else if (base + length > upper) {
            upper = base + length;
        }
    }

    /* The two numbers of a machine of the PC: the memory below 1 MiB a program
     * may use, and the memory above it. The port reads the map itself; these
     * two are for the reader of the multiboot information. */
    g_loader.mb->mem_lower = (efi_u32)(lower / 1024u);
    g_loader.mb->mem_upper = (efi_u32)(upper > 0x100000ull ? (upper - 0x100000ull) / 1024u : 0u);
    g_loader.mb->mmap_addr = (efi_u32)(unsigned long long)(efi_phys_addr)g_loader.mmap;
    g_loader.mb->mmap_length = g_loader.mmap_entries * (efi_u32)sizeof(struct mb_mmap_entry);
    g_loader.mb->flags |= MB_FLAG_MMAP;

    return g_loader.mmap_entries > 0u;
}

/* The root of the ACPI tables: the pointer of ACPI 2.0 first, the one of 1.0
 * for a machine whose firmware publishes only that. A machine with neither
 * keeps 0 and the port falls back to its own search of the extended BIOS
 * area. */
static efi_u64 find_rsdp(void)
{
    const struct efi_guid acpi20 = EFI_ACPI_20_TABLE_GUID;
    const struct efi_guid acpi10 = EFI_ACPI_10_TABLE_GUID;
    efi_u64 rsdp = 0;
    efi_u64 i;

    for (i = 0; i < g_loader.st->number_of_table_entries; i++) {
        const struct efi_configuration_table *table = &g_loader.st->configuration_table[i];

        if (table->vendor_table == NULL) {
            continue;
        }

        if (memcmp(&table->vendor_guid, &acpi20, sizeof(acpi20)) == 0) {
            rsdp = (efi_u64)(efi_phys_addr)table->vendor_table;
            break;
        }

        if (memcmp(&table->vendor_guid, &acpi10, sizeof(acpi10)) == 0) {
            rsdp = (efi_u64)(efi_phys_addr)table->vendor_table;
        }
    }

    /*
     * The kernel reads the tables through the identity map of the boot, which
     * is the first 4 GiB: a root pointer above that map is not one the kernel
     * can follow, and the port's own search of the extended BIOS area is the
     * fallback of a machine of UEFI whose tables are out of reach (it finds
     * nothing there and the machine runs on one processor). Saying so is
     * better than a page fault before the kernel has an IDT.
     */
    if (rsdp >= LOADER_MAP_LIMIT) {
        console_line("uefi: the ACPI tables are above the map of the boot, not handed over");
        return 0;
    }

    return rsdp;
}

static void build_handoff(void)
{
    g_loader.mb->mods_count = MODULE_COUNT;
    g_loader.mb->mods_addr = (efi_u32)(unsigned long long)(efi_phys_addr)g_loader.modules;
    g_loader.mb->flags |= MB_FLAG_MODS;

    g_loader.modules[MODULE_KERNEL].start = (efi_u32)g_loader.kernel_phys;
    g_loader.modules[MODULE_KERNEL].end = (efi_u32)(g_loader.kernel_phys + g_loader.kernel_bytes);

    if (g_loader.disk_bytes > 0u) {
        g_loader.modules[MODULE_DISK].start = (efi_u32)g_loader.disk_phys;
        g_loader.modules[MODULE_DISK].end = (efi_u32)(g_loader.disk_phys + g_loader.disk_bytes);
    } else {
        /* A machine without a disk: the module is empty and the port reads no
         * volume of the boot, which is the boot of `ram0`. */
        g_loader.mb->mods_count = MODULE_COUNT - 1u;
    }

    g_loader.handoff->magic = PORT_X86_64_BOOT_MAGIC;
    g_loader.handoff->version = PORT_X86_64_BOOT_VERSION;
    g_loader.handoff->mb_info = (unsigned long long)(efi_phys_addr)g_loader.mb;
    g_loader.handoff->kernel_phys = (unsigned long long)g_loader.kernel_phys;
    g_loader.handoff->rsdp = find_rsdp();

    if (g_loader.gop_info != NULL) {
        efi_u32 masks[3];
        efi_u32 format;
        efi_u32 bytes;

        if (!pixel_format(g_loader.gop_info, &format, masks)) {
            return;
        }

        bytes = format == PORT_X86_64_PIXEL_RGB565 ? 2u : 4u;

        g_loader.handoff->fb_phys = g_loader.gop->mode->frame_buffer_base;
        g_loader.handoff->fb_size = g_loader.gop->mode->frame_buffer_size;
        g_loader.handoff->fb_width = g_loader.gop_info->horizontal_resolution;
        g_loader.handoff->fb_height = g_loader.gop_info->vertical_resolution;
        g_loader.handoff->fb_pitch = g_loader.gop_info->pixels_per_scan_line * bytes;
        g_loader.handoff->fb_bpp = bytes * 8u;
        g_loader.handoff->fb_format = format;
        g_loader.handoff->fb_mask_red = masks[0];
        g_loader.handoff->fb_mask_green = masks[1];
        g_loader.handoff->fb_mask_blue = masks[2];
    }
}

/* --- the last two steps --------------------------------------------------- */

/* The function the firmware calls: it is named before it is written because
 * the check of the address of the image below asks where this program runs. */
EFIAPI efi_status efi_main(efi_handle image, struct efi_system_table *system_table);

void port_x86_64_enter_kernel(efi_u64 stack, efi_u64 boot_info, efi_u64 magic, efi_u64 pml4)
    __attribute__((noreturn));

/* --- the tables of the boot ------------------------------------------------ */

/*
 * The map of the boot, in 2 MiB pages, in the bss of this image:
 *
 *   PML4[0]   -> the identity of the first 4 GiB (the memory of the kernel,
 *                the modules, the map of the memory, the hand-off and the code
 *                of this loader, wherever the firmware put it);
 *   PML4[511] -> PDPT[510] -> the image at PORT_X86_64_KERNEL_BASE.
 *
 * The kernel builds its own map as the first thing its C does (src/paging.c)
 * and gives the text its read-only bit; these tables carry the one jump and
 * the first lines of the boot.
 *
 * The tables are aligned to a 4 KiB page because CR3 holds an address and not
 * a page number, and the bss of an image of the firmware is zero when the
 * firmware loads it: every entry not written below is "not present".
 */
#define IDENTITY_GIB 4u
#define KERNEL_MAP_2MIB 8u
#define PTE_PRESENT_WRITABLE 0x83ull
#define PTE_DIRECTORY 0x03ull

static efi_u64 g_pml4[512] __attribute__((aligned(4096)));
static efi_u64 g_pdpt_low[512] __attribute__((aligned(4096)));
static efi_u64 g_pd_low[IDENTITY_GIB][512] __attribute__((aligned(4096)));
static efi_u64 g_pdpt_high[512] __attribute__((aligned(4096)));
static efi_u64 g_pd_high[512] __attribute__((aligned(4096)));

static void build_tables(efi_u64 kernel_phys)
{
    efi_u32 table;
    efi_u32 entry;

    for (table = 0; table < IDENTITY_GIB; table++) {
        g_pdpt_low[table] = (efi_u64)(unsigned long long)&g_pd_low[table][0] | PTE_DIRECTORY;

        for (entry = 0; entry < 512u; entry++) {
            efi_u64 page = (efi_u64)table * 512ull * 0x200000ull + (efi_u64)entry * 0x200000ull;

            g_pd_low[table][entry] = page | PTE_PRESENT_WRITABLE;
        }
    }

    g_pml4[0] = (efi_u64)(unsigned long long)g_pdpt_low | PTE_DIRECTORY;
    g_pml4[511] = (efi_u64)(unsigned long long)g_pdpt_high | PTE_DIRECTORY;
    g_pdpt_high[510] = (efi_u64)(unsigned long long)g_pd_high | PTE_DIRECTORY;

    for (entry = 0; entry < KERNEL_MAP_2MIB; entry++) {
        g_pd_high[entry] = (kernel_phys + (efi_u64)entry * 0x200000ull) | PTE_PRESENT_WRITABLE;
    }
}

/* The image has to be at the top of the address space in chunks of 2 MiB that
 * do not cross its base: the copy is made into a 2 MiB-aligned block, and the
 * kernel reads its own physical address from the hand-off. */
static bool place_kernel(efi_phys_addr source, efi_u64 bytes)
{
    efi_u64 pages = EFI_PAGES(bytes);
    efi_phys_addr address = KERNEL_LOAD_FIRST;
    efi_u32 attempt = 0;

    /* The address of the BIOS loader first: a machine of that one and a machine
     * of UEFI then keep the image at the same place, and the numbers of the
     * two boots are the numbers of one kernel. */
    while (EFI_ERR(g_loader.bs->allocate_pages(AllocateAddress, EfiLoaderData, pages, &address))) {
        attempt++;

        if (attempt == 1u) {
            address = KERNEL_LOAD_LAST;
            continue;
        }

        if (address < KERNEL_LOAD_FLOOR + KERNEL_LOAD_STEP) {
            return false;
        }

        address -= KERNEL_LOAD_STEP;
    }

    if (address + bytes > KERNEL_LOAD_END) {
        console_line("uefi: the image does not fit below the arena of the programs");
        return false;
    }

    memcpy((void *)(unsigned long long)address, (const void *)(unsigned long long)source, bytes);

    g_loader.kernel_phys = address;
    g_loader.kernel_bytes = bytes;

    return true;
}

/* The exit of the boot services and the jump. The exit has to be given the key
 * of the map that is current, and every allocation of the firmware between the
 * two calls changes it: the map is read and the exit is taken in a loop, and the
 * retry is the rule of the specification and not a hope. */
static void leave_and_jump(void)
{
    efi_u64 size = 0;
    efi_u64 key = 0;
    efi_u64 descriptor_size = 0;
    efi_u32 version = 0;
    struct efi_memory_descriptor *map = NULL;
    efi_u32 attempt;

    for (attempt = 0; attempt < 8u; attempt++) {
        g_loader.bs->get_memory_map(&size, NULL, &key, &descriptor_size, &version);
        map = alloc_pages(size + descriptor_size, IDENTITY_LIMIT, EfiLoaderData);

        if (map == NULL) {
            break;
        }

        if (EFI_ERR(g_loader.bs->get_memory_map(&size, map, &key, &descriptor_size, &version))) {
            break;
        }

        if (!EFI_ERR(g_loader.bs->exit_boot_services(g_loader.image, key))) {
            break;
        }
    }

    /* The console of the firmware is a service of the boot and it is gone
     * with them: the one line of the machine after this one is the line of
     * COM1, which is hardware and not a service. */
    serial_putc('\r');
    serial_putc('\n');

    {
        const char *line = "uefi: the boot services are gone, the kernel takes the machine";
        efi_u32 i;

        for (i = 0; line[i] != '\0'; i++) {
            serial_putc(line[i]);
        }

        serial_putc('\r');
        serial_putc('\n');
    }

    /*
     * The map of the boot is the identity of the first 4 GiB and the image at
     * the top. A loader the firmware put above the identity map would stop
     * executing the instruction after the CR3 write, so it says so instead:
     * the firmware of a machine of this size loads an application below
     * 4 GiB, and a machine that does not is a machine of a later lane.
     */
    build_tables((efi_u64)g_loader.kernel_phys);

    line_hex("uefi: the map of the boot is at 0x", (efi_u64)(unsigned long long)g_pml4);
    line_hex("uefi: the image is at the top of 0x", (efi_u64)g_loader.kernel_phys);

    port_x86_64_enter_kernel((efi_u64)g_loader.stack_phys + LOADER_STACK_BYTES,
                             (efi_u64)(unsigned long long)g_loader.handoff, PORT_X86_64_BOOT_MAGIC,
                             (efi_u64)(unsigned long long)g_pml4);
}

/* --- the boot ------------------------------------------------------------- */

static void say_picture(void)
{
    struct text out;
    efi_u32 masks[3];
    efi_u32 format = PORT_X86_64_PIXEL_RGB565;

    if (g_loader.gop == NULL || g_loader.gop_info == NULL) {
        return;
    }

    if (!pixel_format(g_loader.gop_info, &format, masks)) {
        return;
    }

    text_reset(&out, "uefi: the picture is ");
    text_unsigned(&out, g_loader.gop_info->horizontal_resolution);
    text_char(&out, 'x');
    text_unsigned(&out, g_loader.gop_info->vertical_resolution);
    text_say(&out, ", mode ");
    text_unsigned(&out, g_loader.gop->mode->mode);
    text_say(&out, " of ");
    text_unsigned(&out, g_loader.gop->mode->max_mode);
    text_say(&out, ", ");
    text_unsigned(&out, format == PORT_X86_64_PIXEL_RGB565 ? 16u : 32u);
    text_say(&out, " bpp ");
    text_say(&out, format_name(format));
    console_line(out.bytes);

    text_reset(&out, "uefi: the frame is ");
    text_unsigned(&out, g_loader.gop->mode->frame_buffer_size / 1024u);
    text_say(&out, " KB at ");
    text_char(&out, '0');
    text_char(&out, 'x');
    text_hex(&out, g_loader.gop->mode->frame_buffer_base);
    console_line(out.bytes);
}

static void say_file(const char *what, efi_u64 bytes)
{
    struct text out;

    text_reset(&out, "uefi: the ");
    text_say(&out, what);
    text_say(&out, " is ");
    text_unsigned(&out, bytes / 1024u);
    text_say(&out, " KB");
    console_line(out.bytes);
}

EFIAPI efi_status efi_main(efi_handle image, struct efi_system_table *system_table)
{
    struct efi_guid gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    struct efi_file_protocol *root = NULL;
    efi_phys_addr kernel_file = 0;
    efi_u64 kernel_bytes = 0;
    efi_u32 format = PORT_X86_64_PIXEL_RGB565;
    efi_u32 masks[3] = {0u, 0u, 0u};
    efi_status status;

    memset(&g_loader, 0, sizeof(g_loader));

    g_loader.st = system_table;
    g_loader.bs = system_table->boot_services;
    g_loader.image = image;

    serial_init();

    /* The watchdog of the firmware would reset a loader that reads a file
     * slowly; the machine of a person is not the firmware's to reset. */
    g_loader.bs->set_watchdog_timer(0u, 0u, 0u, NULL);

    console_line("");
    console_line("uefi: Micro OS, the loader of a firmware machine");

    /* 1. the picture: where the console of the kernel will draw. */
    status = g_loader.bs->locate_protocol(&gop_guid, NULL, (void **)&g_loader.gop);

    if (EFI_ERR(status) || g_loader.gop == NULL || g_loader.gop->mode == NULL) {
        g_loader.gop = NULL;
        g_loader.gop_info = NULL;
        console_line("uefi: the firmware has no picture, the kernel keeps the VGA");
    } else if (!choose_mode() || !pixel_format(g_loader.gop_info, &format, masks)) {
        g_loader.gop = NULL;
        g_loader.gop_info = NULL;
        console_line("uefi: no mode of the firmware is one the console can draw on");
    } else {
        say_picture();
    }

    /* 2. the files of the volume this program was started from. */
    if (!open_volume(&root)) {
        console_line("uefi: the volume of the boot cannot be read");
        goto fail;
    }

    if (!read_file(root, u"\\microos64.bin", IDENTITY_LIMIT, &kernel_file, &kernel_bytes)) {
        console_line("uefi: the volume has no microos64.bin");
        goto fail;
    }

    say_file("kernel", kernel_bytes);

    if (read_file(root, u"\\disk.img", IDENTITY_LIMIT, &g_loader.disk_phys, &g_loader.disk_bytes)) {
        say_file("disk", g_loader.disk_bytes);
    } else {
        /* A machine without a volume still boots: the kernel mounts the RAM
         * disk of the port and says that it has no /bin. */
        g_loader.disk_phys = 0;
        g_loader.disk_bytes = 0;
        console_line("uefi: the volume has no disk.img, the machine boots without one");
    }

    /* 3. the address of the image. */
    if (!place_kernel(kernel_file, kernel_bytes)) {
        console_line("uefi: the memory below the arena has no room for the kernel");
        goto fail;
    }

    line_hex("uefi: the kernel is placed at 0x", g_loader.kernel_phys);

    /* The copy of the kernel is memory the kernel may have: the map of the
     * memory is read after it is given back, and the heap of the port is not
     * told that the file it already copied is a hole. */
    g_loader.bs->free_pages(kernel_file, EFI_PAGES(kernel_bytes));

    /* 4. the structures of the hand-off and the stack of the jump. */
    g_loader.mb = alloc_pages(sizeof(struct mb_info), IDENTITY_LIMIT, EfiLoaderData);
    g_loader.modules =
        alloc_pages(sizeof(struct mb_module) * MODULE_COUNT, IDENTITY_LIMIT, EfiLoaderData);
    g_loader.mmap =
        alloc_pages(sizeof(struct mb_mmap_entry) * MB_MMAP_MAX, IDENTITY_LIMIT, EfiLoaderData);
    g_loader.handoff =
        alloc_pages(sizeof(struct port_x86_64_boot_info), IDENTITY_LIMIT, EfiLoaderData);
    g_loader.stack_phys = (efi_phys_addr)(unsigned long long)alloc_pages(
        LOADER_STACK_BYTES, IDENTITY_LIMIT, EfiLoaderData);

    if (g_loader.mb == NULL || g_loader.modules == NULL || g_loader.mmap == NULL ||
        g_loader.handoff == NULL || g_loader.stack_phys == 0u) {
        console_line("uefi: the loader has no memory for its own structures");
        goto fail;
    }

    memset(g_loader.mb, 0, sizeof(struct mb_info));
    memset(g_loader.modules, 0, sizeof(struct mb_module) * MODULE_COUNT);
    memset(g_loader.mmap, 0, sizeof(struct mb_mmap_entry) * MB_MMAP_MAX);
    memset(g_loader.handoff, 0, sizeof(struct port_x86_64_boot_info));

    if (!build_memory_map()) {
        console_line("uefi: the firmware does not describe its memory");
        goto fail;
    }

    build_handoff();

    line_number("uefi: the map of the memory has ", g_loader.mmap_entries, " region(s)");

    if (g_loader.handoff->rsdp == 0u) {
        console_line("uefi: the firmware publishes no root of the ACPI tables");
    } else {
        line_hex("uefi: the ACPI tables are at 0x", g_loader.handoff->rsdp);
    }

    /*
     * 5. the boot services, the tables of the boot, the kernel. The map of the
     * boot is the identity of the first 4 GiB and the image at the top: a
     * loader the firmware put above that map would stop executing at the
     * instruction after the write of CR3, so it says so instead. A firmware of
     * a machine of this size loads an application below 4 GiB; one that does
     * not is a machine of a later lane.
     */
    if ((efi_u64)(unsigned long long)&efi_main >= LOADER_MAP_LIMIT) {
        console_line("uefi: this loader runs above the 4 GiB of its own identity map");
        goto fail;
    }

    leave_and_jump();

fail:
    console_line("uefi: the boot stops here, the firmware takes the machine back");
    g_loader.bs->stall(10000000u);

    return EFI_LOAD_ERROR;
}
