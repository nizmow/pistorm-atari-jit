// SPDX-License-Identifier: MIT
/* Link against the complete emulator, replacing only its main entry point.
 * Runs on AArch64 (or qemu-aarch64), without opening GPIO or display devices. */
#include "sysconfig.h"
#include "sysdeps.h"
#include "options.h"
#include "memory.h"
#include "newcpu.h"
#include "config_file/config_file.h"
#include "platforms/atari/monster.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
void jit_mem_init(void);
int pistorm_pc_executable(unsigned int);
uint32_t pistorm_stram_top(void);
void pistorm_blit_write16(uint32_t, uint16_t);
uint16_t pistorm_blit_read16(uint32_t);
void psctrl_guest_write(uint32_t, const void *, uint32_t);
void psctrl_guest_read(uint32_t, char *, uint32_t);
extern int pistorm_addr24;
}
extern addrbank pistorm_dummy_bank;
extern void pistorm_mark_code(uaecptr);
extern uint32_t g_jp_smc;

static void select_ram(unsigned mb)
{
    addrbank *hw = mem_banks[bankindex(0xfffffe00u)];
    (void)hw->wget(0xfffffe00u);
    hw->wput(0xfffffe00u, mb);
    hw->wput(0xfffffe00u, mb);
}

int main(void)
{
    setenv("PISTORM_CART", "0", 1);
    setenv("PISTORM_HUGEPAGE", "off", 1);
    emulator_config cfg = {};
    cfg.monster = true;
    emulator_config_set_current(&cfg);
    pistorm_addr24 = 1;
    jit_mem_init();
    assert(pistorm_stram_top() == MONSTER_RAM_BASE);
    assert(!pistorm_pc_executable(MONSTER_RAM_BASE));
    assert(mem_banks[bankindex(MONSTER_RAM_BASE)] == &pistorm_dummy_bank);
    natmem_offset[0x20] = 0x5a;
    select_ram(8);
    assert(monster_ram_size() == MONSTER_RAM_MAX);
    assert(pistorm_pc_executable(0x400000));
    assert(pistorm_pc_executable(0x09400000));
    assert(!pistorm_pc_executable(0xc00000));
    pistorm_mark_code(0x400020);
    uint32_t invalidations = g_jp_smc;
    for (unsigned hi = 0; hi < 256; hi++) {
        uint32_t a = (hi << 24) | 0x400020u;
        addrbank *bank = mem_banks[bankindex(a)];
        assert(bank != &pistorm_dummy_bank);
        assert(baseaddr[bankindex(a)] + a == natmem_offset + 0x400020);
        bank->lput(a, 0x12345678);
        assert(bank->lget(a) == 0x12345678);
        assert(mem_banks[bankindex(0x400020)]->lget(0x400020) == 0x12345678);
    }
    assert(g_jp_smc > invalidations);
    assert(natmem_offset[0x20] == 0x5a); /* direct base must not alias ST-RAM */
    pistorm_blit_write16(0x400020, 0xabcd);
    assert(pistorm_blit_read16(0x400020) == 0xabcd);
    assert(mem_banks[bankindex(0x400020)]->wget(0x400020) == 0xabcd);
    char text[8];
    invalidations = g_jp_smc;
    psctrl_guest_write(0x400040, "monster", 8);
    assert(g_jp_smc > invalidations);
    psctrl_guest_read(0x400040, text, sizeof text);
    assert(!strcmp(text, "monster"));
    select_ram(2);
    assert(pistorm_pc_executable(0x5ffffe));
    assert(!pistorm_pc_executable(0x600000));
    assert(mem_banks[bankindex(0x09600000)] == &pistorm_dummy_bank);
    select_ram(0);
    for (unsigned hi = 0; hi < 256; hi++) {
        uint32_t a = (hi << 24) | MONSTER_RAM_BASE;
        assert(mem_banks[bankindex(a)] == &pistorm_dummy_bank);
        assert(!pistorm_pc_executable(a));
    }
    select_ram(8);
    assert(pistorm_blit_read16(0x400020) == 0xabcd); /* RAM is retained */
    assert(pistorm_stram_top() == MONSTER_RAM_BASE);
    puts("MonSTer CPU banks, 256 aliases, blitter, NatFeats and disable/re-enable passed");
}
