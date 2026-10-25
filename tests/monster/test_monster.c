// SPDX-License-Identifier: MIT
/* Register contract shared by EmuTOS and the original MonSTer utility. */
#include <assert.h>
#include <stdio.h>
#include "platforms/atari/monster.h"

static unsigned maps;
static uint32_t mapped;
static void remap(uint32_t n) { mapped = n; maps++; }
static uint32_t read_reg(uint32_t addr, int size)
{
    uint32_t v;
    assert(monster_io_read(addr, size, &v) == 1);
    return v;
}
static void write_control(unsigned n)
{
    assert(monster_io_write(0xfffffe00u, n, 2) == 1);
}
int main(void)
{
    uint32_t v;
    monster_init(false, remap);
    assert(!monster_io_owns(0xfffe00));
    assert(monster_io_read(0xfffe00, 1, &v) == 0);
    assert(!monster_ram_contains(MONSTER_RAM_BASE, 1));
    monster_init(true, remap);
    assert(mapped == 0 && maps == 1);
    /* EmuTOS detects with a byte access, then reads and writes words. */
    assert(read_reg(0xfffffe00u, 1) == 0);
    assert(read_reg(0xfffe00, 2) == 0);
    write_control(8);
    assert(mapped == 0); /* first write alone must not expose RAM */
    write_control(8);
    assert(mapped == MONSTER_RAM_MAX && maps == 2);
    assert(monster_ram_contains(0x400000, 4));
    assert(monster_ram_contains(0xbffffc, 4));
    assert(!monster_ram_contains(0xbffffe, 4));
    assert(!monster_ram_contains(0xc00000, 1));
    assert(!monster_ram_contains(0x3fffff, 2));
    assert(!monster_ram_contains(0x400000, 0xffffffffu));
    assert(!monster_ram_contains(0x01400000, 1)); /* only CPU folds aliases */
    assert(read_reg(0xfffe00, 2) == 8);
    write_control(6); write_control(4); /* mismatched pair ignored */
    assert(mapped == MONSTER_RAM_MAX);
    write_control(0); write_control(0); /* no preceding read: ignored */
    assert(mapped == MONSTER_RAM_MAX);
    for (unsigned mb = 0; mb <= 8; mb += 2) {
        (void)read_reg(0xfffffe00u, 2);
        write_control(mb); write_control(mb);
        assert(mapped == mb * 0x100000u);
        assert(read_reg(0xfffe01, 1) == mb);
    }
    write_control(9); write_control(9);
    assert(mapped == MONSTER_RAM_MAX);
    /* Optional firmware and MonSTerbo probes must fail rather than identify
     * features whose memory mapping/software we do not implement. */
    assert(monster_io_read(0xfffe08, 2, &v) == -1);
    assert(monster_io_read(0xfffe0a, 2, &v) == -1);
    /* DS1307 DAY register always reads $FF, even after an attempted write. */
    for (int i = 0; i < 16; i++) {
        assert(monster_io_write(0xfffe06, i & 1, 2) == 1);
        assert(read_reg(0xfffe06, 2) == 1);
    }
    assert(monster_io_read(0xfffe07, 2, &v) == -1);
    assert(!monster_io_owns(0xfffe0c));
    puts("MonSTer register, RAM boundary and absent-device checks passed");
}
