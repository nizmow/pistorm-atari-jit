// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "config_file/config_file.h"

void set_hard_drive_image_file_atari(uint8_t i, char *path) { (void)i; (void)path; }
int acsi_attach_at(int id, const char *path) { (void)id; (void)path; return 0; }
int psctrl_settings_config_key(const char *key, const char *value)
{ (void)key; (void)value; return 0; }

int main(void)
{
    const struct { const char *cfg; int present; } cases[] = {
        { "cpu 68000\nmonster\nttram 128M\n", 1 },
        { "cpu 68010\nmonster true\n", 1 },
        { "cpu 68030\nmonster enabled\nttram 32M\n", 1 },
        { "cpu 68000\nmonster false\n", 0 },
        { "cpu 68000\n", 0 },
        { "monster\nvga ET4000AX NOVA\n", 0 },
        { "vga ET4000AX XVDI\nmonster\n", 0 },
        { "monster\nvga ET4000AX FVDI\n", 1 },
        { "monster\nmachine falcon\n", 0 },
        { "falcon_stram 14M\nmonster\n", 0 },
    };
    char path[] = "/tmp/pistorm-monster-cfg-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        FILE *f = fopen(path, "w");
        assert(f);
        fputs(cases[i].cfg, f);
        fclose(f);
        struct emulator_config *c = load_config_file(path);
        assert(c && c->monster == cases[i].present);
        if (c->cpu_type < 2)
            assert(!c->addr32 && !c->ttram);
        free(c);
    }
    unlink(path);
    puts("MonSTer config defaults, CPU addressing and overlap checks passed");
}
