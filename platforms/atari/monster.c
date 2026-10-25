// SPDX-License-Identifier: MIT
#include "monster.h"

#define MONSTER_REG 0x00fffe00u

static bool installed;
static uint32_t ram_bytes;
static int write_phase;
static uint16_t pending;
static void (*remap)(uint32_t);

void monster_init(bool present, void (*map_ram)(uint32_t))
{
    installed = present;
    ram_bytes = 0;
    write_phase = 0;
    remap = map_ram;
    if (present && remap)
        remap(0);
}

uint32_t monster_ram_size(void) { return ram_bytes; }

bool monster_ram_contains(uint32_t addr, uint32_t size)
{
    return size && addr >= MONSTER_RAM_BASE &&
           addr - MONSTER_RAM_BASE < ram_bytes &&
           size <= ram_bytes - (addr - MONSTER_RAM_BASE);
}

bool monster_io_owns(uint32_t addr)
{
    addr &= 0x00ffffffu;
    return installed && addr >= MONSTER_REG && addr < MONSTER_REG + 12;
}

int monster_io_read(uint32_t addr, int size, uint32_t *value)
{
    if (!monster_io_owns(addr))
        return 0;
    addr &= 0x00ffffffu;
    /* $FFFE08 is the optional firmware register; $FFFE0A identifies
     * MonSTerbo. BERR means an original MonSTer to M_ALTRAM.PRG. */
    if (size < 1 || size > 4 || addr + (uint32_t)size > MONSTER_REG + 8)
        return -1;
    *value = 0;
    for (int i = 0; i < size; i++) {
        uint32_t off = addr + (uint32_t)i - MONSTER_REG;
        uint16_t reg;
        if (off < 2) {
            reg = (uint16_t)(ram_bytes >> 20);
            write_phase = 1; /* read - write - write */
        } else {
            /* Floating SDA reads high: EmuTOS's DS1307 probe sees $FF,
             * including after a write, and correctly finds no RTC. */
            reg = off < 4 ? 3 : 1;
        }
        *value = (*value << 8) | ((off & 1) ? reg & 0xffu : reg >> 8);
    }
    return 1;
}

int monster_io_write(uint32_t addr, uint32_t value, int size)
{
    if (!monster_io_owns(addr))
        return 0;
    addr &= 0x00ffffffu;
    if (size < 1 || size > 4 || addr + (uint32_t)size > MONSTER_REG + 8)
        return -1;
    if (addr != MONSTER_REG || size != 2)
        return 1; /* I2C writes, and unsupported byte control writes */

    uint16_t mb = (uint16_t)value;
    if (mb > 8 || (mb & 1)) {
        write_phase = 0;
        return 1;
    }
    if (write_phase == 1) {
        pending = mb;
        write_phase = 2;
    } else if (write_phase == 2) {
        write_phase = 0;
        if (mb == pending && ram_bytes != (uint32_t)mb << 20) {
            ram_bytes = (uint32_t)mb << 20;
            if (remap)
                remap(ram_bytes);
        }
    }
    return 1;
}
