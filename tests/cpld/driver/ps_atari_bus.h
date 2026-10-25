// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>

// GPIO register indices (32-bit words). Callbacks exchange CPU-endian values.
enum PsAtariRegister {
    PS_ATARI_FSEL0 = 0,
    PS_ATARI_FSEL1 = 1,
    PS_ATARI_FSEL2 = 2,
    PS_ATARI_SET = 7,
    PS_ATARI_CLEAR = 10,
    PS_ATARI_LEVEL = 13
};

enum PsAtariError {
    PS_ATARI_OK,
    PS_ATARI_BUS_ERROR,
    PS_ATARI_TIMEOUT,
    PS_ATARI_NOT_READY,
    PS_ATARI_BAD_FIRMWARE,
    PS_ATARI_INVALID_ACCESS
};

enum PsAtariControl {
    PS_ATARI_CONTROL_IDLE = 0,
    PS_ATARI_CONTROL_HALT = 1,
    PS_ATARI_CONTROL_RESET = 2,
    PS_ATARI_CONTROL_INIT = 3
};

struct PsAtariResult {
    uint32_t data;
    uint32_t gpio;
    enum PsAtariError error;
};

struct PsAtariIo {
    void* context;
    uint32_t (*read)(void* context, unsigned int reg);
    void (*write)(void* context, unsigned int reg, uint32_t value);
    uint64_t (*microseconds)(void* context);
    void (*barrier)(void* context);
};

struct PsAtariBus {
    struct PsAtariIo io;
    uint32_t fsel2_keep;
    uint32_t timeout_us;
    unsigned int read_settle;
    uint16_t firmware;
    uint8_t ready;
    uint8_t timed_out;
};

// Callers serialize the whole operation, including all halves of a longword.
// Uses the ST's own clock: GPIO4 is an input, never a Pi-generated clock.
void psAtariSetup(struct PsAtariBus* bus);
struct PsAtariResult psAtariReadStatus(struct PsAtariBus* bus);
enum PsAtariError psAtariWriteStatus(struct PsAtariBus* bus, enum PsAtariControl control);
enum PsAtariError psAtariResetStateMachine(struct PsAtariBus* bus);
enum PsAtariError psAtariCheckFirmware(struct PsAtariBus* bus);
struct PsAtariResult psAtariRead(struct PsAtariBus* bus, uint32_t address, unsigned int size, uint8_t fc);
struct PsAtariResult psAtariWrite(struct PsAtariBus* bus, uint32_t address, uint32_t data,
                                unsigned int size, uint8_t fc);
