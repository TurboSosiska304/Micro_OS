/*
 * Micro OS — the UEFI environment of the loader, in its own words.
 *
 * The loader is a PE/COFF application that the firmware of a machine starts
 * before there is an operating system: there is no libc under it, no header
 * of a system, and nothing of this tree. This file is the part of the UEFI
 * specification the loader reads - the system table, the two tables of
 * services, the console, the graphics output, the filesystem of the boot
 * volume and the map of the memory - written by hand and no bigger than that.
 *
 * Every structure here is the layout of the specification and not a
 * convenience: `BootServices` is a table of function pointers whose *order*
 * is the ABI, so a member the loader never calls is still a member of the
 * struct, with a type of its own. The functions of the firmware are called
 * with the convention of the x86-64 Windows ABI (arguments in RCX, RDX, R8,
 * R9), which GCC spells `__attribute__((ms_abi))`: the loader is compiled by
 * the compiler of the host with SysV for its own code and this attribute for
 * the calls into the firmware, so no second toolchain is needed.
 *
 * Types are the widths UEFI names them by: BOOLEAN is one byte, CHAR16 is
 * two, and a status is a machine word whose high bits say whether it is an
 * error.
 */
#ifndef MICRO_OS_LOADER_UEFI_EFI_H
#define MICRO_OS_LOADER_UEFI_EFI_H

typedef unsigned char efi_bool;
typedef unsigned short efi_char16;
typedef unsigned long long efi_u64;
typedef long long efi_i64;
typedef unsigned int efi_u32;
typedef int efi_i32;
typedef unsigned short efi_u16;
typedef unsigned char efi_u8;
typedef void *efi_handle;
typedef efi_u64 efi_status;
typedef efi_u64 efi_phys_addr;
typedef efi_u64 efi_virt_addr;

/* The convention of the firmware: the x86-64 Windows ABI, which is not the one
 * of the C of this machine. */
#define EFIAPI __attribute__((ms_abi))

#define EFI_ERR(status) ((efi_i64)(status) < 0)
#define EFI_ERROR_BIT (1ull << 63)
#define EFI_STATUS_ERROR(mask) (EFI_ERROR_BIT | (mask))

#define EFI_SUCCESS 0ull
#define EFI_LOAD_ERROR 1ull
#define EFI_INVALID_PARAMETER 2ull
#define EFI_UNSUPPORTED 3ull
#define EFI_BAD_BUFFER_SIZE 4ull
#define EFI_BUFFER_TOO_SMALL 5ull
#define EFI_NOT_READY 6ull
#define EFI_DEVICE_ERROR 7ull
#define EFI_WRITE_PROTECTED 8ull
#define EFI_NOT_FOUND 14ull
#define EFI_ABORTED 21ull
#define EFI_SECURITY_VIOLATION 26ull

#define EFI_PAGE_SIZE 4096ull
#define EFI_PAGES(bytes) (((bytes) + EFI_PAGE_SIZE - 1ull) / EFI_PAGE_SIZE)

struct efi_guid {
    efi_u32 data1;
    efi_u16 data2;
    efi_u16 data3;
    efi_u8 data4[8];
};

struct efi_table_header {
    efi_u64 signature;
    efi_u32 revision;
    efi_u32 header_size;
    efi_u32 crc32;
    efi_u32 reserved;
};

/* --- the memory --------------------------------------------------------- */

typedef enum {
    EfiReservedMemoryType = 0,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory,
    EfiUnacceptedMemoryType,
    EfiMaxMemoryType
} efi_memory_type;

typedef enum {
    AllocateAnyPages = 0,
    AllocateMaxAddress,
    AllocateAddress,
    MaxAllocateType
} efi_allocate_type;

struct efi_memory_descriptor {
    efi_u32 type;
    efi_u32 pad;
    efi_phys_addr physical_start;
    efi_virt_addr virtual_start;
    efi_u64 number_of_pages;
    efi_u64 attribute;
};

#define EFI_MEMORY_WB 0x0000000000000008ull

/* --- the tables of the firmware ----------------------------------------- */

struct efi_simple_text_output;

struct efi_boot_services {
    struct efi_table_header hdr;
    efi_status(EFIAPI *raise_tpl)(efi_u64 new_tpl);
    void(EFIAPI *restore_tpl)(efi_u64 old_tpl);
    efi_status(EFIAPI *allocate_pages)(efi_allocate_type type, efi_memory_type memory_type,
                                       efi_u64 pages, efi_phys_addr *address);
    efi_status(EFIAPI *free_pages)(efi_phys_addr address, efi_u64 pages);
    efi_status(EFIAPI *get_memory_map)(efi_u64 *size, struct efi_memory_descriptor *map,
                                       efi_u64 *key, efi_u64 *descriptor_size,
                                       efi_u32 *descriptor_version);
    efi_status(EFIAPI *allocate_pool)(efi_memory_type pool_type, efi_u64 size, void **buffer);
    efi_status(EFIAPI *free_pool)(void *buffer);
    void *create_event;
    void *set_timer;
    void *wait_for_event;
    void *signal_event;
    void *close_event;
    void *check_event;
    void *install_protocol_interface;
    void *reinstall_protocol_interface;
    void *uninstall_protocol_interface;
    void *handle_protocol;
    void *reserved;
    void *register_protocol_notify;
    void *locate_handle;
    void *locate_device_path;
    void *install_configuration_table;
    void *load_image;
    void *start_image;
    void *exit;
    void *unload_image;
    efi_status(EFIAPI *exit_boot_services)(efi_handle image_handle, efi_u64 map_key);
    void *get_next_monotonic_count;
    efi_status(EFIAPI *stall)(efi_u64 microseconds);
    efi_status(EFIAPI *set_watchdog_timer)(efi_u64 timeout, efi_u64 code, efi_u64 data_size,
                                           efi_char16 *watchdog_code);
    void *connect_controller;
    void *disconnect_controller;
    efi_status(EFIAPI *open_protocol)(efi_handle handle, struct efi_guid *protocol,
                                      void **interface, efi_handle agent_handle,
                                      efi_handle controller_handle, efi_u32 attributes);
    void *close_protocol;
    void *open_protocol_information;
    void *protocols_per_handle;
    void *locate_handle_buffer;
    efi_status(EFIAPI *locate_protocol)(struct efi_guid *protocol, void *registration,
                                        void **interface);
    void *install_multiple_protocol_interfaces;
    void *uninstall_multiple_protocol_interfaces;
    void *calculate_crc32;
    void *copy_mem;
    void *set_mem;
    void *create_event_ex;
};

struct efi_simple_text_output {
    void *reset;
    efi_status(EFIAPI *output_string)(struct efi_simple_text_output *self,
                                      const efi_char16 *string);
    void *test_string;
    void *query_mode;
    void *set_mode;
    void *set_attribute;
    void *clear_screen;
    void *set_cursor_position;
    void *enable_cursor;
    void *mode;
};

struct efi_runtime_services {
    struct efi_table_header hdr;
    void *get_time;
    void *set_time;
    void *get_wakeup_time;
    void *set_wakeup_time;
    void *set_virtual_address_map;
    void *convert_pointer;
    void *get_variable;
    void *get_next_variable_name;
    void *set_variable;
    void *get_next_high_monotonic_count;
    void *reset_system;
};

struct efi_configuration_table {
    struct efi_guid vendor_guid;
    void *vendor_table;
};

struct efi_system_table {
    struct efi_table_header hdr;
    efi_char16 *firmware_vendor;
    efi_u32 firmware_revision;
    efi_handle console_in_handle;
    void *con_in;
    efi_handle console_out_handle;
    struct efi_simple_text_output *console_out;
    efi_handle standard_error_handle;
    struct efi_simple_text_output *standard_error;
    struct efi_runtime_services *runtime_services;
    struct efi_boot_services *boot_services;
    efi_u64 number_of_table_entries;
    struct efi_configuration_table *configuration_table;
};

/* --- the protocols of the loader ---------------------------------------- */

/* The image that was started: its device is the volume of the boot. */
struct efi_loaded_image {
    efi_u32 revision;
    efi_handle parent_handle;
    struct efi_system_table *system_table;
    efi_handle device_handle;
    void *file_path;
    void *reserved;
    efi_u32 load_options_size;
    void *load_options;
    void *image_base;
    efi_u64 image_size;
    efi_memory_type image_code_type;
    efi_memory_type image_data_type;
    void *unload;
};

/* The filesystem of the volume: one call, and the root directory of it. */
struct efi_file_protocol;

struct efi_simple_file_system {
    efi_u64 revision;
    efi_status(EFIAPI *open_volume)(struct efi_simple_file_system *self,
                                    struct efi_file_protocol **root);
};

#define EFI_FILE_MODE_READ 0x0000000000000001ull

struct efi_file_protocol {
    efi_u64 revision;
    efi_status(EFIAPI *open)(struct efi_file_protocol *self, struct efi_file_protocol **file,
                             const efi_char16 *name, efi_u64 mode, efi_u64 attributes);
    efi_status(EFIAPI *close)(struct efi_file_protocol *self);
    void *delete_file;
    efi_status(EFIAPI *read)(struct efi_file_protocol *self, efi_u64 *size, void *buffer);
    void *write;
    void *get_position;
    void *set_position;
    efi_status(EFIAPI *get_info)(struct efi_file_protocol *self, struct efi_guid *type,
                                 efi_u64 *size, void *buffer);
    void *set_info;
    void *flush;
};

/* What `get_info` of EFI_FILE_INFO answers: the size of the file, which the
 * loader reads before it allocates the memory of one. */
struct efi_file_info {
    efi_u64 size;
    efi_u64 file_size;
    efi_u64 physical_size;
    efi_u64 create_time[2];
    efi_u64 last_access_time[2];
    efi_u64 modification_time[2];
    efi_u64 attribute;
    efi_char16 file_name[1];
};

/* The picture of the firmware. */
typedef enum {
    PixelRedGreenBlueReserved8BitPerColor = 0,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax
} efi_graphics_pixel_format;

struct efi_pixel_bitmask {
    efi_u32 red;
    efi_u32 green;
    efi_u32 blue;
    efi_u32 reserved;
};

struct efi_graphics_output_mode_information {
    efi_u32 version;
    efi_u32 horizontal_resolution;
    efi_u32 vertical_resolution;
    efi_graphics_pixel_format pixel_format;
    struct efi_pixel_bitmask pixel_information;
    efi_u32 pixels_per_scan_line;
};

struct efi_graphics_output_mode {
    efi_u32 max_mode;
    efi_u32 mode;
    struct efi_graphics_output_mode_information *info;
    efi_u64 size_of_info;
    efi_phys_addr frame_buffer_base;
    efi_u64 frame_buffer_size;
};

struct efi_graphics_output_protocol {
    efi_status(EFIAPI *query_mode)(struct efi_graphics_output_protocol *self, efi_u32 mode,
                                   efi_u64 *size_of_info,
                                   struct efi_graphics_output_mode_information **info);
    efi_status(EFIAPI *set_mode)(struct efi_graphics_output_protocol *self, efi_u32 mode);
    void *blt;
    struct efi_graphics_output_mode *mode;
};

/* --- the identifiers ---------------------------------------------------- */

#define EFI_LOADED_IMAGE_PROTOCOL_GUID                                                             \
    {0x5B1B31A1, 0x9562, 0x11D2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}}

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID                                                       \
    {0x964E5B22, 0x6459, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}}

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID                                                          \
    {0x9042A9DE, 0x23DC, 0x4A38, {0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A}}

#define EFI_FILE_INFO_GUID                                                                         \
    {0x09576E92, 0x6D3F, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}}

/* The pointer to the root of the ACPI tables: what the kernel's MADT reader
 * needs and what a machine of UEFI does not keep in the extended BIOS area. */
#define EFI_ACPI_20_TABLE_GUID                                                                     \
    {0x8868E871, 0xE4F1, 0x11D3, {0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81}}

#define EFI_ACPI_10_TABLE_GUID                                                                     \
    {0xEB9D2D30, 0x2D88, 0x11D3, {0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D}}

#endif /* MICRO_OS_LOADER_UEFI_EFI_H */
