#ifndef ASTRAOS_PMM_H
#define ASTRAOS_PMM_H

#include "boot_info.h"

#include <stdint.h>

int pmm_init(const struct boot_info *boot_info);
uint64_t pmm_alloc_page(void);
int pmm_free_page(uint64_t physical_address);
uint64_t pmm_free_pages(void);

#endif
