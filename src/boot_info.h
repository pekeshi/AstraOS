#ifndef ASTRAOS_BOOT_INFO_H
#define ASTRAOS_BOOT_INFO_H

#include <stdint.h>

struct boot_info {
    uint64_t memory_map_size;          /* Kartan koko tavuina. */
    uint64_t memory_map_key;           /* UEFI:n kartalle antama avain. */
    uint64_t descriptor_size;          /* Yhden muistialueen kuvauksen koko. */
    uint32_t descriptor_version;       /* Muistikuvauksen versio. */
    uint32_t reserved;                 /* Varattu tulevaa käyttöä varten. */
    void *memory_map;                  /* Osoite UEFI:n muistialuekarttaan. */
    void *acpi_root_pointer;           /* ACPI:n RSDP-osoitin. */
    uint64_t kernel_base;              /* Ytimen latausalueen alku. */
    uint64_t kernel_size;              /* Ytimen latausalueen koko tavuina. */
    uint64_t framebuffer_base;         /* Näytön kuvapuskurin osoite. */
    uint64_t framebuffer_size;         /* Kuvapuskurin koko tavuina. */
    uint32_t framebuffer_width;        /* Kuvan leveys pikseleinä. */
    uint32_t framebuffer_height;       /* Kuvan korkeus pikseleinä. */
    uint32_t pixels_per_scanline;      /* Kuvapuskurin pikselit yhdellä rivillä. */
    uint32_t framebuffer_format;       /* GOP:n käyttämä pikselimuoto. */
};

#endif
