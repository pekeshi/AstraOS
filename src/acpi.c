#include "acpi.h"

#include <stdint.h>

#define ACPI_HEADER_SIZE 36
#define ACPI_MAX_TABLE_SIZE (1024 * 1024)
#define ACPI_SCI_ENABLED 0x0001
#define ACPI_SLEEP_TYPE_MASK (7U << 10)
#define ACPI_SLEEP_ENABLE (1U << 13)
#define ACPI_HW_REDUCED (1U << 20)
#define ACPI_SHUTDOWN_WAIT 20000000U

struct acpi_register {
    uint8_t address_space;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
};

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline uint16_t inw(uint16_t port) {
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline uint32_t inl(uint16_t port) {
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outl(uint16_t port, uint32_t value) {
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static uint32_t read_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static uint64_t read_u64(const uint8_t *bytes) {
    return (uint64_t)read_u32(bytes) |
           ((uint64_t)read_u32(bytes + 4) << 32);
}

static uint32_t table_length(const uint8_t *table) {
    return read_u32(table + 4);
}

static int checksum_valid(const uint8_t *bytes, uint32_t length) {
    uint8_t sum = 0;

    for (uint32_t i = 0; i < length; ++i) {
        sum = (uint8_t)(sum + bytes[i]);
    }
    return sum == 0;
}

static int table_valid(const uint8_t *table, const char signature[4]) {
    uint32_t length;

    if (table == 0 || table[0] != (uint8_t)signature[0] ||
        table[1] != (uint8_t)signature[1] ||
        table[2] != (uint8_t)signature[2] ||
        table[3] != (uint8_t)signature[3]) {
        return 0;
    }
    length = table_length(table);
    return length >= ACPI_HEADER_SIZE && length <= ACPI_MAX_TABLE_SIZE &&
           checksum_valid(table, length);
}

static const uint8_t *find_fadt_in_root(const uint8_t *root,
                                        uint32_t entry_size,
                                        int *invalid_table_found) {
    uint32_t length = table_length(root);

    if ((length - ACPI_HEADER_SIZE) % entry_size != 0) {
        *invalid_table_found = 1;
        return 0;
    }
    for (uint32_t offset = ACPI_HEADER_SIZE; offset < length;
         offset += entry_size) {
        const uint8_t *candidate =
            (const uint8_t *)(uintptr_t)(entry_size == 8
                ? read_u64(root + offset)
                : read_u32(root + offset));

        if (table_valid(candidate, "FACP")) {
            return candidate;
        }
        if (candidate != 0 && candidate[0] == 'F' && candidate[1] == 'A' &&
            candidate[2] == 'C' && candidate[3] == 'P') {
            *invalid_table_found = 1;
        }
    }
    return 0;
}

static int decode_aml_integer(const uint8_t **cursor, const uint8_t *end,
                              uint64_t *value) {
    uint8_t opcode;

    if (*cursor >= end) {
        return 0;
    }
    opcode = *(*cursor)++;
    if (opcode == 0x00) {
        *value = 0;
        return 1;
    }
    if (opcode == 0x01) {
        *value = 1;
        return 1;
    }
    if (opcode == 0x0A) {
        if ((uint64_t)(end - *cursor) < 1) {
            return 0;
        }
        *value = *(*cursor)++;
        return 1;
    }
    if (opcode == 0x0B) {
        if ((uint64_t)(end - *cursor) < 2) {
            return 0;
        }
        *value = (uint64_t)(*cursor)[0] |
                 ((uint64_t)(*cursor)[1] << 8);
        *cursor += 2;
        return 1;
    }
    if (opcode == 0x0C) {
        if ((uint64_t)(end - *cursor) < 4) {
            return 0;
        }
        *value = read_u32(*cursor);
        *cursor += 4;
        return 1;
    }
    if (opcode == 0x0E) {
        if ((uint64_t)(end - *cursor) < 8) {
            return 0;
        }
        *value = read_u64(*cursor);
        *cursor += 8;
        return 1;
    }
    return 0;
}

static int find_s5_sleep_types(const uint8_t *dsdt,
                               uint8_t *sleep_type_a,
                               uint8_t *sleep_type_b) {
    uint32_t length = table_length(dsdt);
    const uint8_t *end = dsdt + length;

    for (const uint8_t *cursor = dsdt + ACPI_HEADER_SIZE;
         (uint64_t)(end - cursor) >= 7; ++cursor) {
        const uint8_t *name;
        const uint8_t *package;
        const uint8_t *package_end;
        uint8_t first_length;
        uint8_t follow_bytes;
        uint32_t package_length;
        uint32_t package_length_bytes;
        uint64_t first_type;
        uint64_t second_type;

        if (*cursor != 0x08) {
            continue;
        }
        name = cursor + 1;
        if (*name == 0x5C) {
            ++name;
        }
        if ((uint64_t)(end - name) < 5 ||
            name[0] != '_' || name[1] != 'S' ||
            name[2] != '5' || name[3] != '_' || name[4] != 0x12) {
            continue;
        }

        package = name + 5;
        if (package >= end) {
            continue;
        }
        first_length = *package;
        follow_bytes = first_length >> 6;
        package_length_bytes = (uint32_t)follow_bytes + 1;
        if (follow_bytes > 3 ||
            (uint64_t)(end - package) < package_length_bytes) {
            continue;
        }
        if (follow_bytes == 0) {
            package_length = first_length & 0x3F;
        } else {
            package_length = first_length & 0x0F;
            for (uint32_t i = 0; i < follow_bytes; ++i) {
                package_length |= (uint32_t)package[i + 1] <<
                                  (4 + 8 * i);
            }
        }
        if (package_length < package_length_bytes ||
            package_length > (uint64_t)(end - package)) {
            continue;
        }
        package_end = package + package_length;
        package += package_length_bytes;
        if (package >= package_end || *package < 2) {
            continue;
        }
        ++package;
        if (!decode_aml_integer(&package, package_end, &first_type) ||
            !decode_aml_integer(&package, package_end, &second_type) ||
            first_type > 7 || second_type > 7) {
            continue;
        }
        *sleep_type_a = (uint8_t)first_type;
        *sleep_type_b = (uint8_t)second_type;
        return 1;
    }
    return 0;
}

static int gas_to_register(const uint8_t *gas, struct acpi_register *reg) {
    reg->address_space = gas[0];
    reg->bit_width = gas[1];
    reg->bit_offset = gas[2];
    reg->access_size = gas[3];
    reg->address = read_u64(gas + 4);
    return reg->address != 0 && reg->bit_offset == 0 &&
           reg->address_space <= 1 && reg->access_size <= 4;
}

static int legacy_io_register(uint32_t address, uint8_t length,
                             struct acpi_register *reg) {
    if (address == 0 || address > 0xFFFF || length < 2) {
        return 0;
    }
    reg->address_space = 1;
    reg->bit_width = 16;
    reg->bit_offset = 0;
    reg->access_size = 2;
    reg->address = address;
    return 1;
}

static int get_power_register(const uint8_t *fadt, uint32_t length,
                              uint32_t legacy_offset, uint32_t extended_offset,
                              uint8_t register_length,
                              struct acpi_register *reg) {
    if (length >= extended_offset + 12 &&
        gas_to_register(fadt + extended_offset, reg) &&
        reg->bit_width >= 16) {
        return 1;
    }
    return legacy_io_register(read_u32(fadt + legacy_offset),
                              register_length, reg);
}

static uint32_t register_access_bits(const struct acpi_register *reg) {
    if (reg->access_size == 0) {
        return reg->bit_width <= 8 ? 8 :
               reg->bit_width <= 16 ? 16 :
               reg->bit_width <= 32 ? 32 : 64;
    }
    return 8U << (reg->access_size - 1);
}

static int read_register(const struct acpi_register *reg, uint64_t *value) {
    uint32_t access_bits = register_access_bits(reg);

    if (reg->bit_width > access_bits ||
        (reg->address_space == 1 && access_bits > 32)) {
        return 0;
    }
    if (reg->address_space == 1) {
        if (reg->address > 0xFFFF) {
            return 0;
        }
        if (access_bits == 8) {
            *value = inb((uint16_t)reg->address);
        } else if (access_bits == 16) {
            *value = inw((uint16_t)reg->address);
        } else if (access_bits == 32) {
            *value = inl((uint16_t)reg->address);
        } else {
            return 0;
        }
    } else {
        volatile const uint8_t *address =
            (volatile const uint8_t *)(uintptr_t)reg->address;
        if (access_bits == 8) {
            *value = *(volatile const uint8_t *)address;
        } else if (access_bits == 16) {
            *value = *(volatile const uint16_t *)address;
        } else if (access_bits == 32) {
            *value = *(volatile const uint32_t *)address;
        } else {
            *value = *(volatile const uint64_t *)address;
        }
    }
    return 1;
}

static int write_register(const struct acpi_register *reg, uint64_t value) {
    uint32_t access_bits = register_access_bits(reg);

    if (reg->bit_width > access_bits ||
        (reg->address_space == 1 && access_bits > 32)) {
        return 0;
    }
    if (reg->address_space == 1) {
        if (reg->address > 0xFFFF) {
            return 0;
        }
        if (access_bits == 8) {
            outb((uint16_t)reg->address, (uint8_t)value);
        } else if (access_bits == 16) {
            outw((uint16_t)reg->address, (uint16_t)value);
        } else if (access_bits == 32) {
            outl((uint16_t)reg->address, (uint32_t)value);
        } else {
            return 0;
        }
    } else {
        volatile uint8_t *address = (volatile uint8_t *)(uintptr_t)reg->address;
        if (access_bits == 8) {
            *(volatile uint8_t *)address = (uint8_t)value;
        } else if (access_bits == 16) {
            *(volatile uint16_t *)address = (uint16_t)value;
        } else if (access_bits == 32) {
            *(volatile uint32_t *)address = (uint32_t)value;
        } else {
            *(volatile uint64_t *)address = value;
        }
    }
    return 1;
}

static const uint8_t *find_fadt(const uint8_t *rsdp, int *invalid_table_found) {
    uint32_t rsdp_length = 20;
    uint64_t xsdt_address = 0;
    uint32_t rsdt_address = read_u32(rsdp + 16);
    const uint8_t *root;

    if (rsdp[15] >= 2) {
        rsdp_length = read_u32(rsdp + 20);
        if (rsdp_length < 36 || rsdp_length > 4096 ||
            !checksum_valid(rsdp, rsdp_length)) {
            *invalid_table_found = 1;
            return 0;
        }
        xsdt_address = read_u64(rsdp + 24);
    }
    if (xsdt_address != 0) {
        root = (const uint8_t *)(uintptr_t)xsdt_address;
        if (table_valid(root, "XSDT")) {
            const uint8_t *fadt =
                find_fadt_in_root(root, 8, invalid_table_found);
            if (fadt != 0) {
                return fadt;
            }
        } else {
            *invalid_table_found = 1;
        }
    }
    if (rsdt_address != 0) {
        root = (const uint8_t *)(uintptr_t)rsdt_address;
        if (table_valid(root, "RSDT")) {
            return find_fadt_in_root(root, 4, invalid_table_found);
        }
        *invalid_table_found = 1;
    }
    return 0;
}

enum acpi_shutdown_result acpi_request_shutdown(const void *rsdp_address) {
    static const uint8_t rsdp_signature[8] = {
        'R', 'S', 'D', ' ', 'P', 'T', 'R', ' '
    };
    const uint8_t *rsdp = (const uint8_t *)rsdp_address;
    const uint8_t *fadt;
    const uint8_t *dsdt;
    uint32_t fadt_length;
    uint64_t dsdt_address;
    uint8_t sleep_type_a;
    uint8_t sleep_type_b;
    int invalid_table_found = 0;

    if (rsdp == 0) {
        return ACPI_SHUTDOWN_NO_TABLES;
    }
    for (uint32_t i = 0; i < sizeof(rsdp_signature); ++i) {
        if (rsdp[i] != rsdp_signature[i]) {
            return ACPI_SHUTDOWN_INVALID_TABLES;
        }
    }
    if (!checksum_valid(rsdp, 20)) {
        return ACPI_SHUTDOWN_INVALID_TABLES;
    }
    fadt = find_fadt(rsdp, &invalid_table_found);
    if (fadt == 0) {
        return invalid_table_found ? ACPI_SHUTDOWN_INVALID_TABLES
                                   : ACPI_SHUTDOWN_NO_TABLES;
    }

    fadt_length = table_length(fadt);
    if (fadt_length < 44) {
        return ACPI_SHUTDOWN_INVALID_TABLES;
    }
    dsdt_address = read_u32(fadt + 40);
    if (fadt_length >= 148 && read_u64(fadt + 140) != 0) {
        dsdt_address = read_u64(fadt + 140);
    }
    if (dsdt_address == 0) {
        return ACPI_SHUTDOWN_INVALID_TABLES;
    }
    dsdt = (const uint8_t *)(uintptr_t)dsdt_address;
    if (!table_valid(dsdt, "DSDT")) {
        return ACPI_SHUTDOWN_INVALID_TABLES;
    }
    if (!find_s5_sleep_types(dsdt, &sleep_type_a, &sleep_type_b)) {
        return ACPI_SHUTDOWN_NO_S5;
    }

    if (fadt_length >= 116 &&
        (read_u32(fadt + 112) & ACPI_HW_REDUCED) != 0) {
        struct acpi_register sleep_control;
        if (fadt_length < 256 ||
            !gas_to_register(fadt + 244, &sleep_control) ||
            sleep_control.bit_width < 6) {
            return ACPI_SHUTDOWN_UNSUPPORTED;
        }
        if (!write_register(&sleep_control,
                            (uint64_t)(sleep_type_a & 7) | (1U << 5))) {
            return ACPI_SHUTDOWN_UNSUPPORTED;
        }
    } else {
        struct acpi_register pm1a_control;
        struct acpi_register pm1b_control;
        uint8_t pm1_control_length;
        uint64_t pm1a_value;
        uint64_t pm1b_value = 0;
        int has_pm1b;

        if (fadt_length < 90) {
            return ACPI_SHUTDOWN_UNSUPPORTED;
        }
        pm1_control_length = fadt[89];
        if (!get_power_register(fadt, fadt_length, 64, 172,
                                pm1_control_length, &pm1a_control)) {
            return ACPI_SHUTDOWN_UNSUPPORTED;
        }
        has_pm1b = get_power_register(fadt, fadt_length, 68, 184,
                                      pm1_control_length, &pm1b_control);
        if (!read_register(&pm1a_control, &pm1a_value) ||
            (has_pm1b && !read_register(&pm1b_control, &pm1b_value))) {
            return ACPI_SHUTDOWN_UNSUPPORTED;
        }
        if ((pm1a_value & ACPI_SCI_ENABLED) == 0) {
            uint32_t smi_command;
            uint8_t acpi_enable;
            uint32_t attempts;

            if (fadt_length < 54) {
                return ACPI_SHUTDOWN_UNSUPPORTED;
            }
            smi_command = read_u32(fadt + 48);
            acpi_enable = fadt[52];
            if (smi_command == 0 || smi_command > 0xFFFF ||
                acpi_enable == 0) {
                return ACPI_SHUTDOWN_UNSUPPORTED;
            }
            outb((uint16_t)smi_command, acpi_enable);
            for (attempts = 0; attempts < 10000000U; ++attempts) {
                if (!read_register(&pm1a_control, &pm1a_value)) {
                    return ACPI_SHUTDOWN_UNSUPPORTED;
                }
                if ((pm1a_value & ACPI_SCI_ENABLED) != 0) {
                    break;
                }
                __asm__ volatile ("pause");
            }
            if ((pm1a_value & ACPI_SCI_ENABLED) == 0) {
                return ACPI_SHUTDOWN_UNSUPPORTED;
            }
            if (has_pm1b && !read_register(&pm1b_control, &pm1b_value)) {
                return ACPI_SHUTDOWN_UNSUPPORTED;
            }
        }

        pm1a_value = (pm1a_value & ~(ACPI_SLEEP_TYPE_MASK | ACPI_SLEEP_ENABLE)) |
                     ((uint64_t)sleep_type_a << 10) | ACPI_SLEEP_ENABLE;
        if (!write_register(&pm1a_control, pm1a_value)) {
            return ACPI_SHUTDOWN_UNSUPPORTED;
        }
        if (has_pm1b) {
            pm1b_value =
                (pm1b_value & ~(ACPI_SLEEP_TYPE_MASK | ACPI_SLEEP_ENABLE)) |
                ((uint64_t)sleep_type_b << 10) | ACPI_SLEEP_ENABLE;
            if (!write_register(&pm1b_control, pm1b_value)) {
                return ACPI_SHUTDOWN_UNSUPPORTED;
            }
        }
    }

    for (uint32_t i = 0; i < ACPI_SHUTDOWN_WAIT; ++i) {
        __asm__ volatile ("pause");
    }
    return ACPI_SHUTDOWN_DID_NOT_POWER_OFF;
}
