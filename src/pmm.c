#include "pmm.h"

struct efi_memory_descriptor {
    uint32_t type;
    uint32_t padding;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
};

#define EFI_BOOT_SERVICES_CODE 3
#define EFI_BOOT_SERVICES_DATA 4
#define EFI_CONVENTIONAL_MEMORY 7
#define PAGE_SIZE 4096ULL
#define MAX_PHYSICAL_ADDRESS (64ULL * 1024 * 1024 * 1024)
#define MAX_PHYSICAL_PAGES (MAX_PHYSICAL_ADDRESS / PAGE_SIZE)
#define PAGE_STATE_BYTES (MAX_PHYSICAL_PAGES / 4)

enum page_state {
    PAGE_RESERVED,
    PAGE_FREE,
    PAGE_ALLOCATED
};

static uint8_t page_states[PAGE_STATE_BYTES];
static uint64_t free_page_count;
static uint64_t next_search_page = 1;
static int initialized;

static enum page_state get_page_state(uint64_t page_index) {
    uint32_t shift = (uint32_t)((page_index & 3) * 2);
    return (enum page_state)((page_states[page_index >> 2] >> shift) & 3);
}

static void set_page_state(uint64_t page_index, enum page_state state) {
    uint32_t shift = (uint32_t)((page_index & 3) * 2);
    uint8_t mask = (uint8_t)(3U << shift);

    page_states[page_index >> 2] =
        (uint8_t)((page_states[page_index >> 2] & (uint8_t)~mask) |
                  ((uint8_t)state << shift));
}

int pmm_init(const struct boot_info *boot_info) {
    const uint8_t *memory_map;
    uint64_t descriptor_count;

    initialized = 0;
    free_page_count = 0;
    next_search_page = 1;
    for (uint64_t i = 0; i < sizeof(page_states); ++i) {
        page_states[i] = 0;
    }

    if (boot_info == 0 || boot_info->memory_map == 0 ||
        boot_info->descriptor_size < sizeof(struct efi_memory_descriptor) ||
        boot_info->memory_map_size == 0 ||
        boot_info->memory_map_size % boot_info->descriptor_size != 0) {
        return 0;
    }

    memory_map = (const uint8_t *)boot_info->memory_map;
    descriptor_count = boot_info->memory_map_size / boot_info->descriptor_size;
    for (uint64_t i = 0; i < descriptor_count; ++i) {
        const struct efi_memory_descriptor *descriptor =
            (const struct efi_memory_descriptor *)(memory_map +
                i * boot_info->descriptor_size);
        uint64_t first_page;
        uint64_t page_count;
        uint64_t end_page;

        if (descriptor->type != EFI_CONVENTIONAL_MEMORY &&
            descriptor->type != EFI_BOOT_SERVICES_CODE &&
            descriptor->type != EFI_BOOT_SERVICES_DATA) {
            continue;
        }
        if ((descriptor->physical_start & (PAGE_SIZE - 1)) != 0) {
            return 0;
        }

        first_page = descriptor->physical_start / PAGE_SIZE;
        if (first_page >= MAX_PHYSICAL_PAGES) {
            continue;
        }
        page_count = descriptor->number_of_pages;
        if (page_count > MAX_PHYSICAL_PAGES - first_page) {
            page_count = MAX_PHYSICAL_PAGES - first_page;
        }
        end_page = first_page + page_count;

        for (uint64_t page = first_page; page < end_page; ++page) {
            if (page == 0 || get_page_state(page) != PAGE_RESERVED) {
                continue;
            }
            set_page_state(page, PAGE_FREE);
            ++free_page_count;
        }
    }

    initialized = 1;
    return 1;
}

uint64_t pmm_alloc_page(void) {
    if (!initialized || free_page_count == 0) {
        return 0;
    }

    for (uint64_t checked = 0; checked < MAX_PHYSICAL_PAGES - 1; ++checked) {
        uint64_t page = next_search_page;

        ++next_search_page;
        if (next_search_page >= MAX_PHYSICAL_PAGES) {
            next_search_page = 1;
        }
        if (get_page_state(page) == PAGE_FREE) {
            set_page_state(page, PAGE_ALLOCATED);
            --free_page_count;
            return page * PAGE_SIZE;
        }
    }

    return 0;
}

int pmm_free_page(uint64_t physical_address) {
    uint64_t page;

    if (!initialized || physical_address == 0 ||
        (physical_address & (PAGE_SIZE - 1)) != 0) {
        return 0;
    }

    page = physical_address / PAGE_SIZE;
    if (page >= MAX_PHYSICAL_PAGES ||
        get_page_state(page) != PAGE_ALLOCATED) {
        return 0;
    }

    set_page_state(page, PAGE_FREE);
    ++free_page_count;
    if (page < next_search_page) {
        next_search_page = page;
    }
    return 1;
}

uint64_t pmm_free_pages(void) {
    return initialized ? free_page_count : 0;
}
