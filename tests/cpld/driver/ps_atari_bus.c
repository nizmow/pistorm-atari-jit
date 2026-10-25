// SPDX-License-Identifier: MIT
// Wire protocol reference: gpio/ps_protocol.c and the 1.1b RTL.
#include "ps_atari_bus.h"

#define DATA_PINS 0x00ffff00u
#define COMMAND_PINS 0x0cu
#define STROBE (1u << 1)
#define TXN_BUSY (1u << 0)
#define BERR_N (1u << 7)
#define TXN_END (DATA_PINS | COMMAND_PINS | STROBE)

#define REG_DATA 0u
#define REG_ADDR_LO 4u
#define REG_ADDR_HI 8u
#define REG_STATUS 12u

static uint32_t readRegister(struct PsAtariBus* bus, unsigned int reg)
{
    return bus->io.read(bus->io.context, reg);
}

static void writeRegister(struct PsAtariBus* bus, unsigned int reg, uint32_t value)
{
    bus->io.write(bus->io.context, reg, value);
}

static void barrier(struct PsAtariBus* bus)
{
    bus->io.barrier(bus->io.context);
}

static void setDirection(struct PsAtariBus* bus, int output)
{
    // GPIO0,4,5,6,7 remain inputs in both modes. GPIO1,2,3 are outputs.
    writeRegister(bus, PS_ATARI_FSEL0, 0x00000248u | (output ? 0x09000000u : 0));
    writeRegister(bus, PS_ATARI_FSEL1, output ? 0x09249249u : 0);
    writeRegister(bus, PS_ATARI_FSEL2, bus->fsel2_keep | (output ? 0x00000249u : 0));
    barrier(bus);
}

static void releaseTransaction(struct PsAtariBus* bus)
{
    writeRegister(bus, PS_ATARI_CLEAR, TXN_END);
    barrier(bus);
    setDirection(bus, 0);
}

static void writePhase(struct PsAtariBus* bus, unsigned int command, uint16_t data)
{
    // Select STATUS only with WR already high, so its combinational readback
    // cannot briefly drive PI_D against the Pi while preparing a status write.
    if (command == REG_STATUS) {
        for (unsigned int i = 0; i < 4; i++) {
            writeRegister(bus, PS_ATARI_SET, STROBE);
        }
    }
    writeRegister(bus, PS_ATARI_SET, ((uint32_t)data << 8) | command);
    // Match the reference pulse widths, including the status-write release guard.
    for (unsigned int i = 0; i < 4; i++) {
        writeRegister(bus, PS_ATARI_SET, STROBE);
    }
    writeRegister(bus, PS_ATARI_CLEAR, STROBE);
    writeRegister(bus, PS_ATARI_CLEAR, STROBE);
    writeRegister(bus, PS_ATARI_CLEAR, TXN_END);
}

static enum PsAtariError waitIdle(struct PsAtariBus* bus, uint32_t* pins)
{
    uint64_t start = bus->io.microseconds(bus->io.context);
    do {
        *pins = readRegister(bus, PS_ATARI_LEVEL);
        if (!(*pins & TXN_BUSY)) {
            return PS_ATARI_OK;
        }
    } while (bus->io.microseconds(bus->io.context) - start < bus->timeout_us);

    bus->timed_out = 1;
    return PS_ATARI_TIMEOUT;
}

void psAtariSetup(struct PsAtariBus* bus)
{
    // Preserve GPIO24-29, including the Pi 4 Ethernet PHY's MDIO/MDC pins.
    bus->fsel2_keep = readRegister(bus, PS_ATARI_FSEL2) & ~0x00000fffu;
    bus->timeout_us = 250000;
    bus->read_settle = 4;
    bus->firmware = 0;
    bus->ready = 0;
    bus->timed_out = 0;
    // Make old control functions inputs before clearing latches (GPIO4 may be GPCLK).
    writeRegister(bus, PS_ATARI_FSEL0, 0);
    writeRegister(bus, PS_ATARI_CLEAR, TXN_END);
    setDirection(bus, 0);
    barrier(bus);
}

struct PsAtariResult psAtariReadStatus(struct PsAtariBus* bus)
{
    struct PsAtariResult result = { 0, 0, PS_ATARI_OK };
    // The status readback is combinational even if TXN is stuck. Do not wait here.
    releaseTransaction(bus);
    writeRegister(bus, PS_ATARI_SET, REG_STATUS);
    barrier(bus);
    for (unsigned int i = 0; i < 16; i++) {
        result.gpio = readRegister(bus, PS_ATARI_LEVEL);
    }
    result.data = (result.gpio >> 8) & 0xffff;
    releaseTransaction(bus);
    return result;
}

enum PsAtariError psAtariWriteStatus(struct PsAtariBus* bus, enum PsAtariControl control)
{
    uint32_t pins;
    if ((unsigned int)control > PS_ATARI_CONTROL_INIT) {
        return PS_ATARI_INVALID_ACCESS;
    }
    // INIT must be possible with a stuck transaction; it is never an automatic retry.
    if (control != PS_ATARI_CONTROL_INIT && bus->timed_out) {
        return PS_ATARI_TIMEOUT;
    }
    if (control != PS_ATARI_CONTROL_INIT && waitIdle(bus, &pins) != PS_ATARI_OK) {
        releaseTransaction(bus);
        return PS_ATARI_TIMEOUT;
    }
    releaseTransaction(bus);
    setDirection(bus, 1);
    writePhase(bus, REG_STATUS, (uint16_t)control);
    releaseTransaction(bus);
    return PS_ATARI_OK;
}

enum PsAtariError psAtariResetStateMachine(struct PsAtariBus* bus)
{
    bus->ready = 0;
    psAtariWriteStatus(bus, PS_ATARI_CONTROL_INIT);
    uint64_t start = bus->io.microseconds(bus->io.context);
    while (bus->io.microseconds(bus->io.context) - start < 1000) {}
    bus->timed_out = 0;
    enum PsAtariError error = psAtariWriteStatus(bus, PS_ATARI_CONTROL_IDLE);
    uint32_t pins;
    if (error == PS_ATARI_OK) {
        error = waitIdle(bus, &pins);
    }
    return error;
}

enum PsAtariError psAtariCheckFirmware(struct PsAtariBus* bus)
{
    struct PsAtariResult result = psAtariReadStatus(bus);
    uint16_t firmware = result.data & 0x07ff;
    uint16_t kind = firmware & 0x0600;
    bus->firmware = firmware;
    bus->ready = firmware != 0x07ff && firmware != 0 &&
        ((kind == 0x0200 && (firmware & 0x00ff) >= 0x0053) || kind == 0x0400 || kind == 0x0600);
    return bus->ready ? PS_ATARI_OK : PS_ATARI_BAD_FIRMWARE;
}

static struct PsAtariResult transfer(struct PsAtariBus* bus, uint32_t address, uint32_t data,
                                    unsigned int size, uint8_t fc, int reading)
{
    struct PsAtariResult result = { 0, 0, PS_ATARI_OK };
    if ((size != 1 && size != 2) || fc > 7 || (size == 2 && (address & 1))) {
        result.error = PS_ATARI_INVALID_ACCESS;
        return result;
    }
    if (!bus->ready || bus->timed_out) {
        result.error = bus->timed_out ? PS_ATARI_TIMEOUT : PS_ATARI_NOT_READY;
        return result;
    }
    result.error = waitIdle(bus, &result.gpio);
    if (result.error != PS_ATARI_OK) {
        releaseTransaction(bus);
        return result;
    }
    address &= 0x00ffffff;
    setDirection(bus, 1);
    if (!reading) {
        uint16_t word = size == 1 ? (data & 0xff) * 0x0101u : data;
        writePhase(bus, REG_DATA, word);
    }
    writePhase(bus, REG_ADDR_LO, address & 0xffff);
    uint16_t high = (address >> 16) | ((uint16_t)fc << 13) |
        (reading ? 0x0200 : 0) | (size == 1 ? 0x0100 : 0);
    writePhase(bus, REG_ADDR_HI, high);
    if (reading) {
        setDirection(bus, 0);
        writeRegister(bus, PS_ATARI_SET, REG_DATA);
        writeRegister(bus, PS_ATARI_SET, STROBE);
    }
    barrier(bus);
    result.error = waitIdle(bus, &result.gpio);
    if (result.error == PS_ATARI_OK) {
        if (reading) {
            // TXN and latch data have different propagation delays. Sample after settling.
            for (unsigned int i = 0; i < bus->read_settle; i++) {
                result.gpio = readRegister(bus, PS_ATARI_LEVEL);
            }
            result.data = (result.gpio >> 8) & 0xffff;
            if (size == 1) {
                result.data = (result.data >> ((address & 1) ? 0 : 8)) & 0xff;
            }
        }
        if (!(result.gpio & BERR_N)) {
            result.error = PS_ATARI_BUS_ERROR;
        }
    }
    releaseTransaction(bus);
    return result;
}

struct PsAtariResult psAtariRead(struct PsAtariBus* bus, uint32_t address, unsigned int size, uint8_t fc)
{
    return transfer(bus, address, 0, size, fc, 1);
}

struct PsAtariResult psAtariWrite(struct PsAtariBus* bus, uint32_t address, uint32_t data,
                                unsigned int size, uint8_t fc)
{
    return transfer(bus, address, data, size, fc, 0);
}
