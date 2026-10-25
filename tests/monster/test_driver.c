// SPDX-License-Identifier: MIT
/* Execute the original M_ALTRAM.PRG control routine on a 68000, without
 * redistributing the utility. Accept only the version inspected here. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m68k.h"
#include "platforms/atari/monster.h"

void stbox_pc_hook(unsigned int pc) { (void)pc; }

static uint8_t mem[0x10000];
static unsigned reg_reads, reg_writes;
unsigned int m68k_read_memory_8(unsigned int a)
{
    uint32_t v;
    int r = monster_io_read(a, 1, &v);
    assert(r >= 0);
    if (r) { reg_reads++; return v; }
    assert(a < sizeof mem);
    return mem[a];
}
unsigned int m68k_read_memory_16(unsigned int a)
{
    uint32_t v;
    int r = monster_io_read(a, 2, &v);
    assert(r >= 0);
    if (r) { reg_reads++; return v; }
    return (m68k_read_memory_8(a) << 8) | m68k_read_memory_8(a + 1);
}
unsigned int m68k_read_memory_32(unsigned int a)
{
    return (m68k_read_memory_16(a) << 16) | m68k_read_memory_16(a + 2);
}
void m68k_write_memory_8(unsigned int a, unsigned int v)
{
    int r = monster_io_write(a, v, 1);
    assert(r >= 0);
    if (r) { reg_writes++; return; }
    assert(a < sizeof mem);
    mem[a] = (uint8_t)v;
}
void m68k_write_memory_16(unsigned int a, unsigned int v)
{
    int r = monster_io_write(a, v, 2);
    assert(r >= 0);
    if (r) { reg_writes++; return; }
    m68k_write_memory_8(a, v >> 8);
    m68k_write_memory_8(a + 1, v);
}
void m68k_write_memory_32(unsigned int a, unsigned int v)
{
    m68k_write_memory_16(a, v >> 16);
    m68k_write_memory_16(a + 2, v);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *f = fopen(argv[1], "rb");
    assert(f);
    uint8_t prg[17876];
    size_t n = fread(prg, 1, sizeof prg, f);
    fclose(f);
    const uint8_t expected[] = {
        0x48,0xe7,0xc0,0x00, 0x32,0x39,0x00,0xff,0xfe,0x00,
        0x33,0xc0,0x00,0xff,0xfe,0x00, 0x33,0xc0,0x00,0xff,0xfe,0x00,
        0x4c,0xdf,0x00,0x03, 0x4e,0x75
    };
    if (n != 17875 || memcmp(prg + 28 + 0xa42, expected, sizeof expected)) {
        fputs("Unsupported M_ALTRAM.PRG version (control routine differs)\n", stderr);
        return 1;
    }
    memcpy(mem + 0x1000, prg + 28 + 0xa42, sizeof expected);
    /* RTS returns to STOP #$2700. */
    mem[0x2000] = 0x4e; mem[0x2001] = 0x72; mem[0x2002] = 0x27;
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    monster_init(true, NULL);
    for (unsigned mb = 0; mb <= 8; mb += 2) {
        m68k_write_memory_32(0, 0x8000);
        m68k_write_memory_32(4, 0x1000);
        m68k_pulse_reset();
        m68k_set_reg(M68K_REG_SP, 0x7ffc);
        m68k_write_memory_32(0x7ffc, 0x2000);
        m68k_set_reg(M68K_REG_D0, mb);
        reg_reads = reg_writes = 0;
        m68k_execute(512);
        assert(monster_ram_size() == mb * 0x100000u);
        assert(reg_reads == 1 && reg_writes == 2);
        assert(m68k_get_reg(NULL, M68K_REG_D0) == mb);
        assert(m68k_get_reg(NULL, M68K_REG_SP) == 0x8000);
    }
    puts("Original M_ALTRAM.PRG routine passed on emulated 68000 (0/2/4/6/8MB)");
}
