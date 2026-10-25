// SPDX-License-Identifier: MIT
#ifndef PISTORM_MONSTER_H
#define PISTORM_MONSTER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MONSTER_RAM_BASE 0x00400000u
#define MONSTER_RAM_MAX  0x00800000u

/* RAM-only MonSTer: no flash, RTC or MonSTerbo registers. The callback
 * changes CPU banks; RAM contents survive guest RESET and enable changes. */
void monster_init(bool present, void (*map_ram)(uint32_t bytes));
bool monster_io_owns(uint32_t addr);
/* 1 = handled, -1 = absent register (guest bus error), 0 = not ours. */
int monster_io_read(uint32_t addr, int size, uint32_t *value);
int monster_io_write(uint32_t addr, uint32_t value, int size);
bool monster_ram_contains(uint32_t addr, uint32_t size);
uint32_t monster_ram_size(void);

#ifdef __cplusplus
}
#endif
#endif
