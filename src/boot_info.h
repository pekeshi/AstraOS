#ifndef ASTRAOS_BOOT_INFO_H
#define ASTRAOS_BOOT_INFO_H

#include <stdint.h>

struct boot_info {
    uint64_t memory_map_size;
    uint64_t memory_map_key;
    uint64_t descriptor_size;
    uint32_t descriptor_version;
    uint32_t reserved;
    void *memory_map;
    uint64_t kernel_base;
    uint64_t kernel_size;
    uint64_t framebuffer_base;
    uint64_t framebuffer_size;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t pixels_per_scanline;
    uint32_t framebuffer_format;
};

#endif
