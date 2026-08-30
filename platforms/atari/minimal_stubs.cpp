// SPDX-License-Identifier: MIT
/*
 * PISTORM_MINIMAL stub layer — compiled ONLY by `make MINIMAL=1`.
 *
 * The core (emulator.c, pistorm_natmem.cpp, jit_glue.cpp, cpu/newcpu.cpp)
 * references a bounded set of symbols from the platform subsystems. In a
 * minimal build the subsystem sources are excluded from the Makefile and
 * this file provides those symbols in "everything disabled" form. The
 * core's own runtime gates (FDD_enabled, DMA_Sound_enabled,
 * KBD_USB_enabled, ...) then keep every hook inert, so the core source
 * files need NO #ifdefs of their own.
 *
 * Cut subsystems (all resolved here):
 *   FDD emulation        platforms/atari/fdd/
 *   IDE                  platforms/atari/IDE.c, idedriver.c
 *   DMA sound + YM2149   platforms/atari/audio/
 *   USB keyboard         platforms/atari/kbd_usb.c
 *   Networking           platforms/atari/network/ (pistorm_net*)
 *   ET4000 SVGA          platforms/atari/et4000/
 *   Host video/AV record platforms/atari/video/, avrecord.c
 *   STBOX sandbox        platforms/atari/stbox/ + third_party/musashi
 *   NatFeat layer        platforms/atari/network/atari_natfeat.cpp,
 *                        psctrl.cpp, psimg.cpp
 *
 * Linkage notes:
 *  - Everything except atari_natfeat_handle_opcode is declared extern "C"
 *    in the platform headers, so the definitions live in an extern "C"
 *    block (C linkage, unmangled).
 *  - atari_natfeat_handle_opcode is declared as a PLAIN C++ function in
 *    include/newcpu.h and defined as plain C++ in atari_natfeat.cpp, so it
 *    must be defined here as plain C++ to match the mangled reference.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef uint32_t uae_u32;

extern "C" {

/* ---- enable flags the core gates on: all off ----
 * (FDD_enabled, DMA_Sound_enabled, ET4K_emutos_vga are defined in
 *  emulator.c itself, so they are NOT re-defined here) */
bool KBD_USB_enabled = false;
volatile int et4000_thread_ready = 0;

/* ---- FDD emulation (platforms/atari/fdd) ---- */
void *fdd_vbl_thread(void *arg) { (void)arg; return NULL; }
void fdd_vbl(void) {}
int platform_fdd_init(const char *path) { (void)path; return -1; }
int fdd_owns_address(uint32_t a) { (void)a; return 0; }
uint32_t fdd_io_read(uint32_t a, int sz) { (void)a; (void)sz; return 0xFFFFFFFFu; }
void fdd_io_write(uint32_t a, uint32_t v, int sz) { (void)a; (void)v; (void)sz; }
uint8_t fdd_gpip(uint8_t cur) { return cur; }

/* ---- IDE (platforms/atari/IDE.c, idedriver.c) ---- */
void InitIDE(void) {}
uint32_t ide_fold(uint32_t a) { return a; }
int ide_in_regs(uint32_t a) { (void)a; return 0; }
uint16_t readIDE(uint32_t a) { (void)a; return 0xFFFFu; }
uint8_t readIDEB(uint32_t a) { (void)a; return 0xFFu; }
uint32_t readIDEL(uint32_t a) { (void)a; return 0xFFFFFFFFu; }
void writeIDE(uint32_t a, uint16_t v) { (void)a; (void)v; }
void writeIDEL(uint32_t a, uint32_t v) { (void)a; (void)v; }
void writeIDEB(uint32_t a, uint8_t v) { (void)a; (void)v; }
uint8_t IDE_intrq_pending(void) { return 0; }
void set_hard_drive_image_file_atari(uint8_t index, char *filename) { (void)index; (void)filename; }

/* ---- DMA sound + YM2149 (platforms/atari/audio) ---- */
int dmasnd_init(const char *device) { (void)device; return -1; }
int dmasnd_capture_start(void) { return -1; }
void dmasnd_capture_reset(void) {}
int dmasnd_irq_wanted(void) { return 0; }
int dmasnd_irq_pending(void) { return 0; }
uint8_t dmasnd_iack_vector(void) { return 0x40; }
int dmasnd_owns(uint32_t a) { (void)a; return 0; }
uint32_t dmasnd_reg_read32(uint32_t a) { (void)a; return 0xFFFFFFFFu; }
uint16_t dmasnd_reg_read16(uint32_t a) { (void)a; return 0xFFFFu; }
uint8_t dmasnd_reg_read8(uint32_t a) { (void)a; return 0xFFu; }
uint8_t dmasnd_gpip_shim(uint8_t cur) { return cur; }
uint8_t dmasnd_mfp_read_shim(uint32_t addr, uint8_t real) { (void)addr; return real; }
void dmasnd_snoop32(uint32_t a, uint32_t v) { (void)a; (void)v; }
void dmasnd_snoop16(uint32_t a, uint16_t v) { (void)a; (void)v; }
void dmasnd_snoop8(uint32_t a, uint8_t v) { (void)a; (void)v; }
void dmasnd_mfp_snoop(uint32_t a, uint32_t v) { (void)a; (void)v; }
int ym2149_init(void) { return -1; }
void ym2149_reset(void) {}
void ym2149_snoop8(uint32_t addr, uint8_t val) { (void)addr; (void)val; }
void ym2149_snoop16(uint32_t addr, uint16_t val) { (void)addr; (void)val; }
void ym2149_snoop32(uint32_t addr, uint32_t val) { (void)addr; (void)val; }

/* ---- USB keyboard (platforms/atari/kbd_usb.c) ---- */
int kbd_usb_init(int grab) { (void)grab; return -1; }
void kbd_usb_shutdown(void) {}
int kbd_usb_irq_wanted(void) { return 0; }
volatile uint32_t kbd_usb_stat_virtual_iacks = 0;
int kbd_usb_force_mode = 0;
int kbd_usb_mouse_div = 1;
void kbd_usb_ctrl_snoop(uint8_t v) { (void)v; }
uint8_t kbd_usb_ctrl_filter(uint8_t v) { return v; }
int kbd_native_mouse_enabled(void) { return 0; }
void kbd_native_tx_snoop(uint8_t v) { (void)v; }
void kbd_usb_tx_snoop(uint8_t v) { (void)v; }
void kbd_usb_mfp_snoop(uint32_t addr, uint32_t value, int is_word) { (void)addr; (void)value; (void)is_word; }
uint8_t kbd_usb_acia_data_shim(void) { return 0xFF; }
uint8_t kbd_usb_acia_status_shim(uint8_t real) { return real; }
uint8_t kbd_usb_gpip_shim(uint8_t real) { return real; }
uint8_t kbd_native_rx_filter(uint8_t v) { return v; }

/* ---- FDD DMA snoop (platforms/atari/fdd/dma_snoop.c) ---- */
int dma_snoop_active(void) { return 0; }
int dma_snoop_owns(uint32_t addr) { (void)addr; return 0; }
void dma_snoop_write(uint32_t addr, uint32_t val, int size) { (void)addr; (void)val; (void)size; }
void dma_snoop_read(uint32_t addr, uint32_t val, int size) { (void)addr; (void)val; (void)size; }

/* ---- networking (platforms/atari/network) ---- */
int platform_network_init_from_config(void *cfg) { (void)cfg; return -1; }
void platform_network_shutdown(void) {}
void pistorm_net_reset(void) {}

/* ---- ET4000 display thread (platforms/atari/et4000/et4000.c) ---- */
void *render_frame(void *vptr) { (void)vptr; return NULL; }

/* ---- ET4000 SVGA (platforms/atari/et4000) ---- */
void *g_et4000 = NULL;   /* ET4000State *; NULL keeps every gated call inert */
uint8_t *et4000_engine_vram_ptr(void) { return NULL; }
void et4000_io_write8(void *s, uint32_t port, uint8_t val) { (void)s; (void)port; (void)val; }
uint8_t et4000_io_read8(void *s, uint32_t port) { (void)s; (void)port; return 0xFF; }
void et4000_engine_vram_write8(uint32_t off, uint8_t v) { (void)off; (void)v; }
uint8_t et4000_engine_vram_read8(uint32_t off) { (void)off; return 0xFF; }
void et4000_engine_vram_write16(uint32_t off, uint16_t v) { (void)off; (void)v; }
void et4000_engine_vram_write32(uint32_t off, uint32_t v) { (void)off; (void)v; }
uint16_t et4000_engine_vram_read16(uint32_t off) { (void)off; return 0xFFFF; }
uint32_t et4000_engine_vram_read32(uint32_t off) { (void)off; return 0xFFFFFFFFu; }

/* ---- STBOX sandbox (platforms/atari/stbox) ---- */
int stbox_core_armed(void) { return 0; }
void stbox_slice(uint64_t now) { (void)now; }
void stbox_errand_pump(void) {}   /* CPU-thread spcflags window; referenced by newcpu.cpp */

/* ---- PSCTRL counters (platforms/atari/psctrl) - referenced by the core ---- */
volatile uint32_t psctrl_ctr_stop_iters = 0;
volatile uint32_t psctrl_ctr_interp_cycles = 0;
volatile uint32_t psctrl_ctr_interp_calls = 0;
volatile uint32_t psctrl_ctr_compiles = 0;
volatile uint32_t psctrl_ctr_flushes = 0;
volatile uint32_t psctrl_ctr_smc_inv = 0;

} /* extern "C" */

/* ---- NatFeat dispatch hook: NOT extern "C" (see header note) ----
 * Returning false makes $7300/$7301 fall through to op_illg, i.e. the
 * guest sees the exact behaviour of real hardware: an illegal instruction.
 * nf_get_id() then never answers, so guest NatFeat probes fail clean. */
bool atari_natfeat_handle_opcode(uae_u32 opcode, uae_u32 *cycles)
{
    (void)opcode;
    (void)cycles;
    return false;
}
