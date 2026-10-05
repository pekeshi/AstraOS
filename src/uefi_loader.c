#include <stdint.h>
#include "boot_info.h"

/* UEFI:n tilakoodit ja käytetyt muistivarauksen asetukset. */
#define EFI_SUCCESS 0
#define EFI_BUFFER_TOO_SMALL 0x8000000000000005ULL
#define EFI_INVALID_PARAMETER 0x8000000000000002ULL
#define EFI_ERROR(status) (((status) >> 63) != 0)

#define EFI_LOADER_DATA 2
#define EFI_ALLOCATE_ANY_PAGES 0
#define EFI_ALLOCATE_ADDRESS 2
#define EFI_FILE_MODE_READ 1
#define EFI_FILE_INFO_GUID \
    {0x09576e92, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}}
#define EFI_SIMPLE_FILE_SYSTEM_GUID \
    {0x964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}}
#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    {0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}}

#define ELF_PT_LOAD 1
#define ELF_PF_X 1
#define ELF_MAX_PROGRAM_HEADERS 128
#define ELF_MAX_FILE_SIZE (64ULL * 1024 * 1024)
#define PAGE_SIZE 4096
#define KERNEL_STACK_PAGES 16
#define MEMORY_MAP_CAPACITY (64 * 1024)

/* UEFI käyttää x86-64:ssä 64-bittisiä osoitteita ja tilakoodeja. */
typedef uint64_t efi_status;
typedef uint64_t efi_uintn;
typedef void *efi_handle;

struct efi_guid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
};

struct efi_configuration_table {
    struct efi_guid vendor_guid;
    void *vendor_table;
};

/* UEFI-taulukoiden ja palveluiden rakenteet seuraavat firmware-rajapintaa. */
struct efi_table_header {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
};

struct efi_simple_text_output_protocol {
    void *reset;
    efi_status (*output_string)(struct efi_simple_text_output_protocol *, const uint16_t *);
};

struct efi_file_protocol {
    uint64_t revision;
    efi_status (*open)(struct efi_file_protocol *, struct efi_file_protocol **,
                       const uint16_t *, uint64_t, uint64_t);
    efi_status (*close)(struct efi_file_protocol *);
    void *delete_file;
    efi_status (*read)(struct efi_file_protocol *, efi_uintn *, void *);
    void *write;
    void *get_position;
    void *set_position;
    efi_status (*get_info)(struct efi_file_protocol *, struct efi_guid *,
                           efi_uintn *, void *);
    void *set_info;
    void *flush;
};

struct efi_simple_file_system_protocol {
    uint64_t revision;
    efi_status (*open_volume)(struct efi_simple_file_system_protocol *,
                              struct efi_file_protocol **);
};

struct efi_boot_services {
    struct efi_table_header header;
    void *raise_tpl;
    void *restore_tpl;
    efi_status (*allocate_pages)(uint32_t, uint32_t, efi_uintn, uint64_t *);
    void *free_pages;
    efi_status (*get_memory_map)(efi_uintn *, void *, uint64_t *, efi_uintn *, uint32_t *);
    efi_status (*allocate_pool)(uint32_t, efi_uintn, void **);
    efi_status (*free_pool)(void *);
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
    efi_status (*exit_boot_services)(efi_handle, uint64_t);
    void *get_next_monotonic_count;
    void *stall;
    void *set_watchdog_timer;
    void *connect_controller;
    void *disconnect_controller;
    void *open_protocol;
    void *close_protocol;
    void *open_protocol_information;
    void *protocols_per_handle;
    void *locate_handle_buffer;
    efi_status (*locate_protocol)(struct efi_guid *, void *, void **);
};

struct efi_pixel_bitmask {
    uint32_t red_mask;
    uint32_t green_mask;
    uint32_t blue_mask;
    uint32_t reserved_mask;
};

struct efi_graphics_output_mode_information {
    uint32_t version;
    uint32_t horizontal_resolution;
    uint32_t vertical_resolution;
    uint32_t pixel_format;
    struct efi_pixel_bitmask pixel_information;
    uint32_t pixels_per_scanline;
};

struct efi_graphics_output_protocol_mode {
    uint32_t max_mode;
    uint32_t mode;
    struct efi_graphics_output_mode_information *info;
    efi_uintn size_of_info;
    uint64_t framebuffer_base;
    uint64_t framebuffer_size;
};

struct efi_graphics_output_protocol {
    void *query_mode;
    void *set_mode;
    void *blt;
    struct efi_graphics_output_protocol_mode *mode;
};

/* Käynnistyksen aikana käyttöön annetut UEFI-taulukot. */
struct efi_system_table {
    struct efi_table_header header;
    uint16_t *firmware_vendor;
    uint32_t firmware_revision;
    efi_handle console_in_handle;
    void *console_in;
    efi_handle console_out_handle;
    struct efi_simple_text_output_protocol *console_out;
    efi_handle standard_error_handle;
    void *standard_error;
    void *runtime_services;
    struct efi_boot_services *boot_services;
    uint64_t configuration_table_count;
    struct efi_configuration_table *configuration_table;
};

/* Varmista, että itse määriteltyjen rakenteiden kentät ovat oikeissa kohdissa. */
_Static_assert(__builtin_offsetof(struct efi_simple_text_output_protocol, output_string) == 8,
               "UEFI text-output protocol layout mismatch");
_Static_assert(__builtin_offsetof(struct efi_file_protocol, read) == 32,
               "UEFI file protocol layout mismatch");
_Static_assert(__builtin_offsetof(struct efi_file_protocol, get_info) == 64,
               "UEFI file protocol layout mismatch");
_Static_assert(__builtin_offsetof(struct efi_simple_file_system_protocol, open_volume) == 8,
               "UEFI file-system protocol layout mismatch");
_Static_assert(__builtin_offsetof(struct efi_boot_services, allocate_pages) == 40,
               "UEFI boot-services layout mismatch");
_Static_assert(__builtin_offsetof(struct efi_boot_services, exit_boot_services) == 232,
               "UEFI boot-services layout mismatch");
_Static_assert(__builtin_offsetof(struct efi_boot_services, locate_protocol) == 320,
               "UEFI boot-services layout mismatch");
_Static_assert(__builtin_offsetof(struct efi_system_table, boot_services) == 96,
               "UEFI system-table layout mismatch");
_Static_assert(__builtin_offsetof(struct efi_system_table,
                                  configuration_table_count) == 104,
               "UEFI system-table configuration count layout mismatch");
_Static_assert(__builtin_offsetof(struct efi_system_table,
                                  configuration_table) == 112,
               "UEFI system-table configuration table layout mismatch");
_Static_assert(__builtin_offsetof(struct efi_graphics_output_protocol_mode,
                                  framebuffer_base) == 24,
               "UEFI graphics mode layout mismatch");
_Static_assert(__builtin_offsetof(struct efi_graphics_output_protocol_mode,
                                  framebuffer_size) == 32,
               "UEFI graphics mode layout mismatch");

#define GOP_PIXEL_RED_GREEN_BLUE_RESERVED8 0
#define GOP_PIXEL_BLUE_GREEN_RED_RESERVED8 1

/* ELF64-otsakkeet kuvaavat tiedoston ja ladattavat muistiosiot. */
struct efi_file_info {
    uint64_t size;
    uint64_t file_size;
};

struct elf64_header {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t program_header_offset;
    uint64_t section_header_offset;
    uint32_t flags;
    uint16_t header_size;
    uint16_t program_header_entry_size;
    uint16_t program_header_count;
    uint16_t section_header_entry_size;
    uint16_t section_header_count;
    uint16_t section_name_index;
};

struct elf64_program_header {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t virtual_address;
    uint64_t physical_address;
    uint64_t file_size;
    uint64_t memory_size;
    uint64_t alignment;
};

typedef void (*kernel_entry)(struct boot_info *) __attribute__((sysv_abi, noreturn));

extern void enter_kernel(kernel_entry entry, struct boot_info *boot_info, void *stack_top);

/* Säilytä UEFI-taulukot, jotta apufunktiot voivat käyttää niiden palveluita. */
static struct efi_system_table *system_table;
static struct efi_boot_services *boot_services;

/* Hae ACPI 2.0 RSDP ja säilytä ACPI 1.0 varavaihtoehtona. */
static void *find_acpi_root_pointer(void) {
    static const struct efi_guid acpi_20_guid = {
        0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}
    };
    static const struct efi_guid acpi_10_guid = {
        0xeb9d2d30, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}
    };
    void *acpi_10_table = 0;

    if (system_table->configuration_table == 0) {
        return 0;
    }
    for (uint64_t i = 0; i < system_table->configuration_table_count; ++i) {
        const struct efi_guid *guid =
            &system_table->configuration_table[i].vendor_guid;
        int matches_acpi_20 = guid->data1 == acpi_20_guid.data1 &&
            guid->data2 == acpi_20_guid.data2 &&
            guid->data3 == acpi_20_guid.data3;
        int matches_acpi_10 = guid->data1 == acpi_10_guid.data1 &&
            guid->data2 == acpi_10_guid.data2 &&
            guid->data3 == acpi_10_guid.data3;

        for (uint32_t j = 0; j < sizeof(guid->data4); ++j) {
            if (guid->data4[j] != acpi_20_guid.data4[j]) {
                matches_acpi_20 = 0;
            }
            if (guid->data4[j] != acpi_10_guid.data4[j]) {
                matches_acpi_10 = 0;
            }
        }
        if (matches_acpi_20) {
            void *acpi_20_table = system_table->configuration_table[i].vendor_table;
            if (acpi_20_table != 0) {
                return acpi_20_table;
            }
        }
        if (matches_acpi_10) {
            acpi_10_table = system_table->configuration_table[i].vendor_table;
        }
    }
    return acpi_10_table;
}

/* UEFI-konsoli tulostaa nämä UTF-16-merkit. */
static const uint16_t message_loading[] = {
    'A', 's', 't', 'r', 'a', 'O', 'S', ':', ' ', 'l', 'o', 'a', 'd', 'i', 'n', 'g', ' ',
    'k', 'e', 'r', 'n', 'e', 'l', '.', '.', '.', '\r', '\n', 0
};
static const uint16_t message_read_error[] = {
    'A', 's', 't', 'r', 'a', 'O', 'S', ':', ' ', 'c', 'a', 'n', 'n', 'o', 't', ' ',
    'r', 'e', 'a', 'd', ' ', 'k', 'e', 'r', 'n', 'e', 'l', '.', '\r', '\n', 0
};
static const uint16_t message_elf_error[] = {
    'A', 's', 't', 'r', 'a', 'O', 'S', ':', ' ', 'i', 'n', 'v', 'a', 'l', 'i', 'd', ' ',
    'E', 'L', 'F', ' ', 'k', 'e', 'r', 'n', 'e', 'l', '.', '\r', '\n', 0
};
static const uint16_t message_load_error[] = {
    'A', 's', 't', 'r', 'a', 'O', 'S', ':', ' ', 'c', 'o', 'u', 'l', 'd', ' ',
    'n', 'o', 't', ' ', 'l', 'o', 'a', 'd', ' ', 'k', 'e', 'r', 'n', 'e', 'l', '.', '\r', '\n', 0
};
static const uint16_t message_exit_error[] = {
    'A', 's', 't', 'r', 'a', 'O', 'S', ':', ' ', 'c', 'o', 'u', 'l', 'd', ' ',
    'n', 'o', 't', ' ', 'e', 'x', 'i', 't', ' ', 'U', 'E', 'F', 'I', '.', '\r', '\n', 0
};

/* Kirjoita viesti UEFI:n tekstikonsoliin, jos konsoli on käytettävissä. */
static void print(const uint16_t *message) {
    if (system_table != 0 && system_table->console_out != 0) {
        system_table->console_out->output_string(system_table->console_out, message);
    }
}

/* Hae näytön kuvapuskurin tiedot ennen UEFI-palveluista poistumista. */
static void get_framebuffer_info(struct boot_info *boot_info) {
    static struct efi_guid graphics_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    struct efi_graphics_output_protocol *graphics = 0;
    struct efi_graphics_output_protocol_mode *mode;
    struct efi_graphics_output_mode_information *info;

    if (EFI_ERROR(boot_services->locate_protocol(&graphics_guid, 0, (void **)&graphics)) ||
        graphics == 0 || graphics->mode == 0 || graphics->mode->info == 0) {
        return;
    }

    mode = graphics->mode;
    info = mode->info;
    if ((info->pixel_format != GOP_PIXEL_RED_GREEN_BLUE_RESERVED8 &&
         info->pixel_format != GOP_PIXEL_BLUE_GREEN_RED_RESERVED8) ||
        info->horizontal_resolution == 0 || info->vertical_resolution == 0 ||
        info->pixels_per_scanline < info->horizontal_resolution ||
        info->pixels_per_scanline > UINT64_MAX / info->vertical_resolution ||
        (uint64_t)info->pixels_per_scanline * info->vertical_resolution >
            mode->framebuffer_size / sizeof(uint32_t)) {
        return;
    }

    boot_info->framebuffer_base = mode->framebuffer_base;
    boot_info->framebuffer_size = mode->framebuffer_size;
    boot_info->framebuffer_width = info->horizontal_resolution;
    boot_info->framebuffer_height = info->vertical_resolution;
    boot_info->pixels_per_scanline = info->pixels_per_scanline;
    boot_info->framebuffer_format = info->pixel_format;
}

/* Pysähdy virhetilanteessa sen sijaan, että jatkaisit rikkinäisillä tiedoilla. */
static void halt(void) __attribute__((noreturn));
static void halt(void) {
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

/* Varaa muistia UEFI:n hallitsemasta muistista. */
static void *allocate_pool(efi_uintn size) {
    void *buffer = 0;
    if (EFI_ERROR(boot_services->allocate_pool(EFI_LOADER_DATA, size, &buffer))) {
        return 0;
    }
    return buffer;
}

/* Tarkista tiedoston rajat ylivuodon välttämiseksi. */
static int file_range_valid(uint64_t offset, uint64_t size, uint64_t file_size) {
    return offset <= file_size && size <= file_size - offset;
}

/* Avaa EFI-osion juuressa oleva kernel.elf ja lue se muistiin. */
static efi_status read_kernel(uint8_t **file_buffer, uint64_t *file_size) {
    static struct efi_guid file_system_guid = EFI_SIMPLE_FILE_SYSTEM_GUID;
    static struct efi_guid file_info_guid = EFI_FILE_INFO_GUID;
    static const uint16_t kernel_path[] = {
        '\\', 'k', 'e', 'r', 'n', 'e', 'l', '.', 'e', 'l', 'f', 0
    };
    struct efi_simple_file_system_protocol *file_system = 0;
    struct efi_file_protocol *root = 0;
    struct efi_file_protocol *kernel_file = 0;
    struct efi_file_info file_info;
    efi_uintn info_size = 0;
    efi_uintn bytes_read;
    void *info_buffer = 0;
    efi_status status;

    status = boot_services->locate_protocol(&file_system_guid, 0, (void **)&file_system);
    if (EFI_ERROR(status)) {
        return status;
    }
    status = file_system->open_volume(file_system, &root);
    if (EFI_ERROR(status)) {
        return status;
    }
    status = root->open(root, &kernel_file, kernel_path, EFI_FILE_MODE_READ, 0);
    root->close(root);
    if (EFI_ERROR(status)) {
        return status;
    }

    status = kernel_file->get_info(kernel_file, &file_info_guid, &info_size, 0);
    if (status != EFI_BUFFER_TOO_SMALL || info_size < sizeof(file_info)) {
        kernel_file->close(kernel_file);
        return EFI_ERROR(status) ? status : EFI_INVALID_PARAMETER;
    }
    info_buffer = allocate_pool(info_size);
    if (info_buffer == 0) {
        kernel_file->close(kernel_file);
        return EFI_INVALID_PARAMETER;
    }
    status = kernel_file->get_info(kernel_file, &file_info_guid, &info_size, info_buffer);
    if (EFI_ERROR(status)) {
        boot_services->free_pool(info_buffer);
        kernel_file->close(kernel_file);
        return status;
    }
    file_info = *(struct efi_file_info *)info_buffer;
    boot_services->free_pool(info_buffer);

    if (file_info.file_size < sizeof(struct elf64_header) ||
        file_info.file_size > ELF_MAX_FILE_SIZE) {
        kernel_file->close(kernel_file);
        return EFI_INVALID_PARAMETER;
    }
    *file_buffer = allocate_pool(file_info.file_size);
    if (*file_buffer == 0) {
        kernel_file->close(kernel_file);
        return EFI_INVALID_PARAMETER;
    }

    bytes_read = file_info.file_size;
    status = kernel_file->read(kernel_file, &bytes_read, *file_buffer);
    kernel_file->close(kernel_file);
    if (EFI_ERROR(status) || bytes_read != file_info.file_size) {
        boot_services->free_pool(*file_buffer);
        *file_buffer = 0;
        return EFI_ERROR(status) ? status : EFI_INVALID_PARAMETER;
    }
    *file_size = file_info.file_size;
    return EFI_SUCCESS;
}

/* Tarkista ELF ennen kuin sen sisältämää koodia suoritetaan. */
static int validate_elf(const uint8_t *file, uint64_t file_size,
                        struct elf64_header **header_out,
                        struct elf64_program_header **program_headers_out,
                        uint64_t *kernel_base_out, uint64_t *kernel_end_out) {
    struct elf64_header *header = (struct elf64_header *)file;
    struct elf64_program_header *program_headers;
    uint64_t header_bytes;
    uint64_t kernel_base = UINT64_MAX;
    uint64_t kernel_end = 0;
    int has_load_segment = 0;
    int entry_is_executable = 0;

    /* Hyväksy vain vähänpääinen 64-bittinen x86-64-suoritettava ELF. */
    if (header->ident[0] != 0x7F || header->ident[1] != 'E' ||
        header->ident[2] != 'L' || header->ident[3] != 'F' ||
        header->ident[4] != 2 || header->ident[5] != 1 ||
        header->ident[6] != 1 || header->type != 2 ||
        header->machine != 62 || header->version != 1 ||
        header->header_size != sizeof(*header) ||
        header->program_header_entry_size != sizeof(*program_headers) ||
        header->program_header_count == 0 ||
        header->program_header_count > ELF_MAX_PROGRAM_HEADERS) {
        return 0;
    }

    header_bytes = (uint64_t)header->program_header_count *
                   header->program_header_entry_size;
    if (!file_range_valid(header->program_header_offset, header_bytes, file_size)) {
        return 0;
    }
    program_headers = (struct elf64_program_header *)(file + header->program_header_offset);

    /* Tarkista jokaisen ladattavan osion rajat ja etsi ytimen muistialue. */
    for (uint16_t i = 0; i < header->program_header_count; ++i) {
        struct elf64_program_header *segment = &program_headers[i];
        uint64_t segment_end;

        if (segment->type != ELF_PT_LOAD) {
            continue;
        }
        if (segment->file_size > segment->memory_size ||
            !file_range_valid(segment->offset, segment->file_size, file_size) ||
            segment->virtual_address > UINT64_MAX - segment->memory_size) {
            return 0;
        }
        if (segment->alignment > 1 &&
            ((segment->alignment & (segment->alignment - 1)) != 0 ||
             segment->virtual_address % segment->alignment != segment->offset % segment->alignment)) {
            return 0;
        }
        if (segment->memory_size == 0) {
            continue;
        }
        segment_end = segment->virtual_address + segment->memory_size;
        if (segment->virtual_address < 0x100000 || segment_end > 0x80000000ULL) {
            return 0;
        }
        if (segment->virtual_address < kernel_base) {
            kernel_base = segment->virtual_address;
        }
        if (segment_end > kernel_end) {
            kernel_end = segment_end;
        }
        if ((segment->flags & ELF_PF_X) != 0 &&
            header->entry >= segment->virtual_address && header->entry < segment_end) {
            entry_is_executable = 1;
        }
        has_load_segment = 1;
    }

    if (!has_load_segment || !entry_is_executable || kernel_base == UINT64_MAX ||
        (kernel_base & (PAGE_SIZE - 1)) != 0) {
        return 0;
    }
    /* Ladattavat osiot eivät saa mennä päällekkäin. */
    for (uint16_t i = 0; i < header->program_header_count; ++i) {
        struct elf64_program_header *left = &program_headers[i];
        if (left->type != ELF_PT_LOAD || left->memory_size == 0) {
            continue;
        }
        for (uint16_t j = i + 1; j < header->program_header_count; ++j) {
            struct elf64_program_header *right = &program_headers[j];
            if (right->type == ELF_PT_LOAD && right->memory_size != 0 &&
                left->virtual_address < right->virtual_address + right->memory_size &&
                right->virtual_address < left->virtual_address + left->memory_size) {
                return 0;
            }
        }
    }

    *header_out = header;
    *program_headers_out = program_headers;
    *kernel_base_out = kernel_base;
    *kernel_end_out = kernel_end;
    return 1;
}

/* Varaa ytimen osoitealue, nollaa se ja kopioi ELF-osiot paikalleen. */
static efi_status load_segments(const uint8_t *file,
                               struct elf64_header *header,
                               struct elf64_program_header *program_headers,
                               uint64_t kernel_base, uint64_t kernel_end) {
    uint64_t address = kernel_base;
    uint64_t span = kernel_end - kernel_base;
    efi_uintn pages = (span + PAGE_SIZE - 1) / PAGE_SIZE;
    efi_status status;

    status = boot_services->allocate_pages(EFI_ALLOCATE_ADDRESS, EFI_LOADER_DATA,
                                           pages, &address);
    if (EFI_ERROR(status) || address != kernel_base) {
        return EFI_ERROR(status) ? status : EFI_INVALID_PARAMETER;
    }

    /* Nollaus täyttää myös ELF:n BSS-alueen. */
    for (uint64_t i = 0; i < span; ++i) {
        ((uint8_t *)kernel_base)[i] = 0;
    }
    for (uint16_t i = 0; i < header->program_header_count; ++i) {
        struct elf64_program_header *segment = &program_headers[i];
        if (segment->type != ELF_PT_LOAD || segment->memory_size == 0) {
            continue;
        }
        uint8_t *destination = (uint8_t *)(uintptr_t)segment->virtual_address;
        const uint8_t *source = file + segment->offset;
        for (uint64_t j = 0; j < segment->file_size; ++j) {
            destination[j] = source[j];
        }
    }
    return EFI_SUCCESS;
}

/* Hae lopullinen muistialuekartta, sulje UEFI-palvelut ja käynnistä ydin. */
static efi_status exit_boot_services_and_enter(efi_handle image,
                                               uint64_t entry_address,
                                               uint64_t kernel_base,
                                               uint64_t kernel_end) {
    struct boot_info *boot_info = 0;
    void *memory_map = 0;
    uint64_t stack_address = 0;
    efi_uintn map_capacity = MEMORY_MAP_CAPACITY;
    efi_uintn map_size;
    efi_uintn map_key;
    efi_uintn descriptor_size;
    uint32_t descriptor_version;
    efi_status status;

    status = boot_services->allocate_pool(EFI_LOADER_DATA, sizeof(*boot_info),
                                          (void **)&boot_info);
    if (EFI_ERROR(status)) {
        return status;
    }
    status = boot_services->allocate_pages(EFI_ALLOCATE_ANY_PAGES, EFI_LOADER_DATA,
                                           KERNEL_STACK_PAGES, &stack_address);
    if (EFI_ERROR(status)) {
        return status;
    }
    status = boot_services->allocate_pool(EFI_LOADER_DATA, map_capacity, &memory_map);
    if (EFI_ERROR(status)) {
        return status;
    }

    boot_info->framebuffer_base = 0;
    boot_info->framebuffer_size = 0;
    boot_info->framebuffer_width = 0;
    boot_info->framebuffer_height = 0;
    boot_info->pixels_per_scanline = 0;
    boot_info->framebuffer_format = 0;
    get_framebuffer_info(boot_info);

    /* UEFI voi muuttaa karttaa; yritä uudestaan, jos avain vanheni. */
    for (;;) {
        map_size = map_capacity;
        status = boot_services->get_memory_map(&map_size, memory_map, &map_key,
                                               &descriptor_size, &descriptor_version);
        if (status == EFI_BUFFER_TOO_SMALL) {
            boot_services->free_pool(memory_map);
            map_capacity *= 2;
            status = boot_services->allocate_pool(EFI_LOADER_DATA, map_capacity, &memory_map);
            if (EFI_ERROR(status)) {
                return status;
            }
            continue;
        }
        if (EFI_ERROR(status)) {
            return status;
        }

        boot_info->memory_map_size = map_size;
        boot_info->memory_map_key = map_key;
        boot_info->descriptor_size = descriptor_size;
        boot_info->descriptor_version = descriptor_version;
        boot_info->reserved = 0;
        boot_info->memory_map = memory_map;
        boot_info->acpi_root_pointer = find_acpi_root_pointer();
        boot_info->kernel_base = kernel_base;
        boot_info->kernel_size = kernel_end - kernel_base;

        status = boot_services->exit_boot_services(image, map_key);
        if (status == EFI_INVALID_PARAMETER) {
            continue;
        }
        if (EFI_ERROR(status)) {
            return status;
        }
        enter_kernel((kernel_entry)(uintptr_t)entry_address, boot_info,
                     (void *)(uintptr_t)(stack_address + KERNEL_STACK_PAGES * PAGE_SIZE));
    }
}

/* UEFI-ohjelman aloituskohta: lue, tarkista ja käynnistä kernel.elf. */
efi_status efi_main(efi_handle image, struct efi_system_table *table) {
    uint8_t *kernel_file = 0;
    uint64_t kernel_file_size = 0;
    uint64_t kernel_entry_address;
    uint64_t kernel_base;
    uint64_t kernel_end;
    struct elf64_header *header;
    struct elf64_program_header *program_headers;
    efi_status status;

    system_table = table;
    boot_services = table->boot_services;
    print(message_loading);

    status = read_kernel(&kernel_file, &kernel_file_size);
    if (EFI_ERROR(status)) {
        print(message_read_error);
        halt();
    }
    if (!validate_elf(kernel_file, kernel_file_size, &header, &program_headers,
                      &kernel_base, &kernel_end)) {
        boot_services->free_pool(kernel_file);
        print(message_elf_error);
        halt();
    }
    kernel_entry_address = header->entry;
    status = load_segments(kernel_file, header, program_headers, kernel_base, kernel_end);
    boot_services->free_pool(kernel_file);
    if (EFI_ERROR(status)) {
        print(message_load_error);
        halt();
    }

    status = exit_boot_services_and_enter(image, kernel_entry_address, kernel_base, kernel_end);
    if (EFI_ERROR(status)) {
        print(message_exit_error);
        halt();
    }
    halt();
}
