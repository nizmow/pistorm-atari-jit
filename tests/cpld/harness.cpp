// SPDX-License-Identifier: MIT
#include "Vcpld.h"
#include "verilated_vcd_c.h"
extern "C" {
#include "ps_atari_bus.h"
}
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <limits>

struct Options {
    std::string scenario = "ram", trace = "failure.vcd";
    uint64_t seed = 1, gpio_ns = 80, phase_ns = 0;
    unsigned target_state = 4, offset_ns = 0, pulse_ns = 252, response_ns = 200;
    unsigned settle_ns = 0, jitter_ns = 0, iterations = 32;
    bool transparent_latches = false;
    bool keep = false, inject_contention = false, inject_simulator_failure = false;
};

class Harness {
public:
    VerilatedContext context;
    static VerilatedContext* single_thread(VerilatedContext& c) { c.threads(1); return &c; }
    Vcpld d{single_thread(context)};
    VerilatedVcdC trace;
    PsAtariBus bus{};
    Options opt;
    std::array<uint8_t, 65536> ram{};
    uint64_t now = 0, cycle_at = 0, release_at = 0;
    unsigned cpu_cycles = 0, writes = 0, dma_writes = 0, dma_reads = 0, pending_samples = 0;
    unsigned response_ns = 200, peripheral_delay_ns = 4000;
    unsigned blocked_starts = 0, deferred_grants = 0;
    unsigned dma_tenure_ns = 2500, dma_ack_delay_ns = 0;
    bool suppress_ack = false, suppress_commit = false, succeeded = false;
    std::array<uint64_t,8> state_visits{}, event_states{};
    std::array<uint64_t,10> event_phases{};
    std::string outcome = "none";
    unsigned fired_events = 0, physical_cycles=0;
    std::mt19937 timing_rng;
    std::ofstream events;
    enum class Fault { Request, Ack, Berr, Reset, Dtack };
    struct Event { unsigned state, delay, duration; Fault fault; bool armed=true, fired=false; uint64_t due=0; };
    std::vector<Event> schedule;
    uint16_t peripheral_value = 0x5aa5;
    bool error_response = false;
    std::string scenario;

    explicit Harness(Options options) : opt(std::move(options)), timing_rng(static_cast<unsigned>(opt.seed ^ 0x68a7)), scenario(opt.scenario) {
        response_ns = opt.response_ns;
        std::mt19937 rng(static_cast<unsigned>(opt.seed));
        for (auto& b : ram) b = static_cast<uint8_t>(rng());
        context.traceEverOn(true);
        d.trace(&trace, 32);
        if (auto parent = std::filesystem::path(opt.trace).parent_path(); !parent.empty())
            std::filesystem::create_directories(parent);
        trace.open(opt.trace.c_str());
        events.open(opt.trace + ".events.jsonl");
        d.M68K_CLK = 0; d.M68K_BR_n = 1; d.M68K_BGACK_n = 1;
        d.M68K_RESET_n = 1; d.M68K_HALT_n = 1;
        d.M68K_DTACK_n = 1; d.M68K_BERR_n = 1; d.M68K_VPA_n = 1; d.M68K_IPL_n = 7;
        bus.io = {this, read_cb, write_cb, time_cb, barrier_cb};
    }
    ~Harness() { report(); d.final(); trace.close(); }
    void require(bool ok, std::string_view why) const {
        if (!ok) throw std::runtime_error(std::string(why) + " at " + std::to_string(now) + " ns");
    }
    void report() const {
        std::ofstream f(opt.trace + ".json");
        f << "{\n\"board\":" << BOARD << ",\"scenario\":\"" << scenario << "\",\"seed\":" << opt.seed
          << ",\"success\":" << (succeeded ? "true" : "false") << ",\"cycles\":" << cpu_cycles
          << ",\"writes\":" << writes << ",\"physical_cycles\":" << physical_cycles << ",\"events\":" << fired_events << ",\"outcome\":\"" << outcome << "\",\n"
          << "\"latch_model\":\"" << (opt.transparent_latches ? "16373" : "16374") << "\",\n";
        auto array = [&](const char* key, const auto& values) { f << '\"' << key << "\":["; bool first=true;
            for (auto v: values) { f << (first?"":",") << v; first=false; } f << "]"; };
        array("state_visits", state_visits); f << ",\n"; array("event_states",event_states);
        f << ",\n"; array("event_phases",event_phases);
        f << ",\n\"gpio_ns\":"<<opt.gpio_ns<<",\"phase_ns\":"<<opt.phase_ns<<",\"target_state\":"<<opt.target_state
          <<",\"offset_ns\":"<<opt.offset_ns<<",\"pulse_ns\":"<<opt.pulse_ns<<",\"response_ns\":"<<opt.response_ns
          <<",\"settle_ns\":"<<opt.settle_ns<<",\"jitter_ns\":"<<opt.jitter_ns<<",\"iterations\":"<<opt.iterations;
        std::ifstream manifest(MANIFEST);
        if(manifest) f << ",\n\"manifest\":"<<manifest.rdbuf();
        f << "\n}\n";
    }
    void arm(Fault fault, unsigned state, unsigned delay=0, unsigned duration=252) {
        schedule.push_back({state,delay,duration,fault});
    }
    void configure_access(uint32_t address, unsigned size=2, unsigned fc=5) {
        expected_address=address; expected_size=size; expected_fc=fc;
    }
    void recover() {
        require(psAtariResetStateMachine(&bus)==PS_ATARI_OK,"explicit INIT recovery did not finish");
        require(!bus.ready && !bus.timed_out,"recovery state flags");
        require(psAtariCheckFirmware(&bus)==PS_ATARI_OK,"firmware check after recovery");
    }
    void start() {
        external_reset = true; advance(1000); external_reset = false; advance(2000);
        psAtariSetup(&bus);
        require(psAtariResetStateMachine(&bus) == PS_ATARI_OK, "INIT failed");
        require(psAtariCheckFirmware(&bus) == PS_ATARI_OK, "firmware rejected");
        require(bus.firmware == (BOARD == 240 ? 0x421 : 0x521), "wrong version fixture");
        // Keep timeout failures small in simulated time. There is no wall clock.
        bus.timeout_us = 100;
        capture_trace = true; cancel_observed=false;
        require((fsel[2] & ~0xfffu) == 0x01234000u, "GPIO24-29 changed");
    }
    void advance(uint64_t ns) {
        while (ns--) tick();
    }
    void idle() {
        advance(1000);
        require(!d.PI_TXN_IN_PROGRESS, "transaction still pending");
        require(!pi_mask(), "GPIO data drivers left enabled");
        require(!d.sim_status_oe && d.LTCH_D_RD_OE_n, "Pi readback not released");
        require(d.LTCH_D_WR_OE_n, "ST write data driver not released");
    }
    PsAtariResult read(uint32_t a, unsigned size, unsigned fc = 5) {
        expected_fc = fc; expected_address = a; expected_size = size;
        auto r = psAtariRead(&bus, a, size, static_cast<uint8_t>(fc));
        events << "{\"read_return_ns\":"<<now<<",\"error\":"<<unsigned(r.error)<<",\"data\":"<<r.data<<"}\n";
        if (r.error!=PS_ATARI_TIMEOUT) idle();
        return r;
    }
    PsAtariResult write(uint32_t a, uint16_t v, unsigned size, unsigned fc = 5) {
        expected_fc = fc; expected_address = a; expected_size = size;
        auto r = psAtariWrite(&bus, a, v, size, static_cast<uint8_t>(fc));
        events << "{\"write_return_ns\":"<<now<<",\"error\":"<<unsigned(r.error)<<",\"writes\":"<<writes<<"}\n";
        if (r.error!=PS_ATARI_TIMEOUT) idle();
        return r;
    }
    uint16_t device_word(unsigned a) const { return a == 0xff0000 ? peripheral_value : word(a); }
    uint16_t word(unsigned a) const { return static_cast<uint16_t>((ram.at(a) << 8) | ram.at(a+1)); }
    void arm_arbitration(bool active_cycle) {
        request_on_active = active_cycle;
        request_on_header = !active_cycle;
    }
    bool fault_interrupted() const { return fault_during_request; }
    bool dma_owned() const { return dma_stage == 3; }
    void request_dma() {
        dma_stage = 1; advance(2000);
        require(dma_stage == 3, "DMA failed to acquire bus");
    }
    void check_request_strobe() {
        expected_address = 0x200; expected_size = 2; expected_fc = 5;
        auto expected = word(expected_address);
        constexpr uint32_t end = 0x00ffff0e;
        write_cb(this, PS_ATARI_FSEL0, 0x09000248);
        write_cb(this, PS_ATARI_FSEL1, 0x09249249);
        write_cb(this, PS_ATARI_FSEL2, 0x01234249);
        write_cb(this, PS_ATARI_SET, (0x200u << 8) | 4); // low address
        write_cb(this, PS_ATARI_SET, 2);
        write_cb(this, PS_ATARI_CLEAR, 2);
        write_cb(this, PS_ATARI_CLEAR, end);
        write_cb(this, PS_ATARI_SET, (0xa200u << 8) | 8); // FC5, word read, high address 0
        write_cb(this, PS_ATARI_SET, 2);
        advance(5000);
        require(cpu_cycles == 1 && !d.PI_TXN_IN_PROGRESS, "held header strobe replayed the request");

        // While WR remains high, pass through STATUS/ADDR_LO/DATA back to
        // ADDR_HI. A decoded command edge must not act as a fresh WR edge.
        write_cb(this, PS_ATARI_SET, 4);
        write_cb(this, PS_ATARI_CLEAR, 8);
        write_cb(this, PS_ATARI_CLEAR, 4);
        write_cb(this, PS_ATARI_SET, 8);
        advance(2000);
        require(cpu_cycles == 1 && !d.PI_TXN_IN_PROGRESS, "command decode generated a phantom request");

        external_reset = true; advance(1000);
        external_reset = false; advance(2000);
        require(cpu_cycles == 1 && !d.PI_TXN_IN_PROGRESS, "held strobe replayed across reset");
        write_cb(this, PS_ATARI_CLEAR, 2);
        write_cb(this, PS_ATARI_CLEAR, end);
        write_cb(this, PS_ATARI_FSEL0, 0x248);
        write_cb(this, PS_ATARI_FSEL1, 0);
        write_cb(this, PS_ATARI_FSEL2, 0x01234000);
        auto result = read(expected_address, 2);
        require(result.error == PS_ATARI_OK && result.data == expected && cpu_cycles == 2,
                "fresh strobe did not work after reset/release");
    }

private:
    uint32_t gpio = 0;
    std::array<uint32_t, 3> fsel{0, 0, 0x01234000u};
    uint16_t addr_lo = 0, addr_hi = 0, write_data = 0, read_data = 0;
    std::array<bool, 8> previous{};
    bool external_reset = false, active = false, committed = false, capture_trace = false;
    int override_ack=-1, override_dtack=-1, override_berr=-1;
    bool rogue_drive=false, dma_write_seen=false, dma_read_seen=false, cancel_observed=false;
    uint64_t completed_at=0, fault_at=0;
    bool fault_during_request=false;
    uint16_t captured_read=0;
    uint64_t read_visible_at=0, grant_at=0;
    uint32_t active_address=0, pending_address=0;
    uint16_t pending_data=0;
    bool prev_physical_as=false;
    uint16_t active_data=0;
    struct Undo { uint64_t at; Fault fault; };
    std::vector<Undo> undo;
    bool prev_as = false, prev_go = false;
    bool dma_drive = false, dma_cycle = false, dma_read = false;
    bool request_on_active = false, request_on_header = false;
    unsigned dma_stage = 0, expected_fc = 5, expected_address = 0, expected_size = 2;
    uint16_t pi_value = 0, st_value = 0;

    uint32_t command_mask() const {
        uint32_t mask = 0;
        for (unsigned pin = 0; pin < 8; ++pin) {
            unsigned fn = (fsel[0] >> (pin*3)) & 7;
            if (pin >= 1 && pin <= 3) {
                require(fn <= 1, "alternate function on command GPIO");
                if (fn == 1) mask |= 1u << pin;
            } else require(fn == 0, "Pi driving CPLD status/reset/clock GPIO");
        }
        return mask;
    }
    // Direction is a register property; cache only the decoded FSEL mask.
    // Every FSEL write refreshes it before time or RTL evaluation advances.
    uint16_t data_direction=0;
    uint16_t pi_mask() const { return data_direction; }
    void refresh_data_direction() {
        data_direction=0;
        for(unsigned pin=8;pin<24;++pin) {
            unsigned fn=(fsel[pin/10]>>((pin%10)*3))&7;
            require(fn<=1,"alternate function on shared GPIO");
            if(fn==1) data_direction|=static_cast<uint16_t>(1u<<(pin-8));
        }
    }
    // Enabled drivers conflict regardless of their values. Undriven lines pull
    // high. Partial GPIO direction changes are checked per bit, not per word.
    uint16_t resolve_pi() {
        uint16_t driven = 0, value = 0xffff;
        auto drive = [&](uint16_t mask, uint16_t bits, const char* name) {
            if (driven & mask) require(false, std::string("PI_D contention: ") + name);
            value = static_cast<uint16_t>((value & ~mask) | (bits & mask)); driven |= mask;
        };
        drive(pi_mask(), static_cast<uint16_t>(gpio >> 8), "Pi GPIO");
        if (d.sim_status_oe) drive(0xffff, d.sim_status_data, "CPLD status");
        if (!d.LTCH_D_RD_OE_n) drive(0xffff, read_data, "read latch");
        if (opt.inject_contention && pi_mask()) drive(pi_mask(), static_cast<uint16_t>(gpio >> 8), "injected equal-value driver");
        return value;
    }
    uint32_t address() const { return ((uint32_t(addr_hi & 0xff) << 16) | addr_lo) & 0xfffffe; }
    void resolve_st() {
        bool a = !d.LTCH_A_OE_n, w = !d.LTCH_D_WR_OE_n;
        bool foreign_driver=dma_drive || rogue_drive;
        bool memory = d.sim_bus_drv && !d.M68K_AS_n && d.M68K_RW &&
                      (!d.M68K_UDS_n || !d.M68K_LDS_n) && a;
        memory = memory || (dma_drive && dma_cycle && dma_read);
        bool dma_data = (dma_drive && dma_cycle && !dma_read) || rogue_drive;
        require(!(a && foreign_driver), "ST address contention");
        require(!(d.sim_bus_drv && foreign_driver), "ST control contention");
        require(!(w && (memory || dma_data)), "ST data contention (write latch)");
        require(!(memory && dma_data), "ST data contention (RAM/DMA)");
        if (dma_stage == 3) {
            require(!a && !w && !d.sim_bus_drv, "unreleased ST drivers during DMA");
        }
        st_value = w ? write_data : memory ? device_word(dma_drive ? 0x300 : address()) : rogue_drive ? 0xdead : dma_data ? 0xd00d : 0xffff;
    }
    void settle() {
        d.sim_pi_gpio_mask = pi_mask();
        d.sim_pi_gpio_value = gpio >> 8;
        d.sim_dma_drive = dma_drive || rogue_drive;
        if (read_visible_at && now>=read_visible_at) { read_data=captured_read; read_visible_at=0; }
        // External 16374: rising edge capture. External 16373: transparent
        // while LE is high, retaining the final value when LE falls.
        // The external part is independent of the RTL's LS374 timing mode.
        // Repeat to settle the explicit receiver/driver split and latch output
        // changes. No simulated time passes within these delta evaluations.
        for (unsigned delta = 0; delta < 6; ++delta) {
            pi_value = resolve_pi();
            d.PI_D = pi_value;
            d.M68K_RESET_n = !(external_reset || d.sim_reset_drive);
            d.M68K_HALT_n = !d.sim_halt_drive;
            d.eval();
            resolve_st();
            std::array<bool, 8> edge{bool(d.LTCH_A_0), bool(d.LTCH_A_8), bool(d.LTCH_A_16),
                bool(d.LTCH_A_24), bool(d.LTCH_D_WR_U), bool(d.LTCH_D_WR_L),
                bool(d.LTCH_D_RD_U), bool(d.LTCH_D_RD_L)};
            auto capture = [&](unsigned i, uint16_t& q, uint16_t value, uint16_t mask) {
                if (edge[i] && (opt.transparent_latches || !previous[i]))
                    q = static_cast<uint16_t>((q & ~mask) | (value & mask));
            };
            capture(0, addr_lo, pi_value, 0x00ff); capture(1, addr_lo, pi_value, 0xff00);
            capture(2, addr_hi, pi_value, 0x00ff); capture(3, addr_hi, pi_value, 0xff00);
            capture(4, write_data, pi_value, 0xff00); capture(5, write_data, pi_value, 0x00ff);
            auto old_read = captured_read;
            bool read_edge=(edge[6]&&!previous[6]) || (edge[7]&&!previous[7]);
            capture(6, captured_read, st_value, 0xff00); capture(7, captured_read, st_value, 0x00ff);
            if (read_edge || (opt.transparent_latches && old_read != captured_read)) {
                read_visible_at=now+opt.settle_ns;
                if (!opt.settle_ns) { read_data=captured_read; read_visible_at=0; }
            }
            previous = edge;
        }
        require(d.PI_D == resolve_pi(), "PI_D delta settling failed");
        d.sim_read_latch_value = read_data;
        d.sim_write_latch_value = write_data;
        d.sim_st_data = st_value;
        d.sim_st_address = dma_drive ? 0x300 : address();
        d.sim_ram_drive = d.sim_bus_drv && !d.M68K_AS_n && d.M68K_RW &&
            (!d.M68K_UDS_n || !d.M68K_LDS_n) && !d.LTCH_A_OE_n;
        d.sim_ram_drive |= dma_drive && dma_cycle && dma_read;
        d.eval();
    }
    void tick() {
        // Exercise the same user fatal hook that handles simulator convergence
        // and generated-model errors; the diagnostic test verifies unwinding.
        if (opt.inject_simulator_failure && now == 5000)
            VL_FATAL_MT(__FILE__, __LINE__, "", "injected simulator failure");
        ++now; context.timeInc(1);
        bool was_busy=d.PI_TXN_IN_PROGRESS;
        bool was_clock = d.M68K_CLK;
        d.M68K_CLK = ((now + opt.phase_ns) % 126) >= 63; // 7.9365 MHz, 1 ns ticks
        bool falling = was_clock && !d.M68K_CLK;
        // Directed arbitration integration boundaries: request at the high-address
        // command, or immediately after an own cycle asserts AS. No auto-replay.
        if ((request_on_header && ((gpio >> 2) & 3) == 2 && (gpio & 2)) ||
            (request_on_active && d.sim_as_on)) {
            request_on_header = request_on_active = false; dma_stage = 1;
        }
        for (auto it=undo.begin(); it!=undo.end();) {
            if (now<it->at) { ++it; continue; }
            if (it->fault==Fault::Ack) { override_ack=-1; override_dtack=-1; rogue_drive=false; }
            if (it->fault==Fault::Berr) override_berr=-1;
            if (it->fault==Fault::Reset) external_reset=false;
            if (it->fault==Fault::Dtack) override_dtack=-1;
            it=undo.erase(it);
        }
        for (auto& e:schedule) {
            bool trigger = e.state<8 ? bool(d.sim_states & (1u<<e.state)) && (d.PI_TXN_IN_PROGRESS || e.state<2 || e.state==7) :
                e.state==8 ? bool(d.sim_go) : bool(d.PI_TXN_IN_PROGRESS);
            if (e.armed && trigger) { e.armed=false; e.due=now+e.delay; }
            if (!e.armed && !e.fired && now>=e.due) {
                e.fired=true; ++fired_events;
                fault_at=now;
                fault_during_request=d.PI_TXN_IN_PROGRESS;
                if(e.fault==Fault::Reset || e.fault==Fault::Ack || e.fault==Fault::Berr)
                    outcome=committed?"committed":"indeterminate";
                for(unsigned i=0;i<8;++i) if(d.sim_states&(1u<<i)) ++event_states[i];
                unsigned phase=(now+opt.phase_ns)%126;
                bool rise=phase>=32 && phase<95;
                int distance=rise ? int(phase)-63 : phase>=95 ? int(phase)-126 : int(phase);
                unsigned bin=distance<=-2?0:distance==-1?1:distance==0?2:distance==1?3:4;
                ++event_phases[(rise?5:0)+bin];
                events << "{\"ns\":"<<now<<",\"kind\":"<<unsigned(e.fault)<<",\"target\":"<<e.state
                    <<",\"states\":"<<unsigned(d.sim_states)<<",\"phase\":"<<phase<<",\"duration\":"<<e.duration<<"}\n";
                if(e.fault==Fault::Request) { dma_stage=1; release_at=0; }
                if(e.fault==Fault::Ack) { override_ack=0; override_dtack=0; rogue_drive=true; }
                if(e.fault==Fault::Berr) override_berr=0;
                if(e.fault==Fault::Reset) external_reset=true;
                if(e.fault==Fault::Dtack) override_dtack=0;
                if(e.fault!=Fault::Request) undo.push_back({now+e.duration,e.fault});
            }
        }
        if (dma_stage == 1) {
            d.M68K_BR_n = 0;
            if (!d.M68K_BG_n && !d.sim_as_on) { dma_stage = 2; grant_at=now; }
        }
        if (dma_stage == 2 && now-grant_at>=dma_ack_delay_ns) {
            d.M68K_BGACK_n = 0;
            if (!d.sim_bus_drv && d.LTCH_A_OE_n && d.LTCH_D_WR_OE_n) {
                dma_stage = 3; dma_drive = true; d.M68K_BR_n = 1; dma_write_seen=dma_read_seen=false;
            }
        }
        if (dma_stage == 3 && release_at && now >= release_at) {
            dma_drive = false; d.M68K_BGACK_n = 1; dma_stage = 4;
        }
        uint64_t dma_age = release_at ? now - (release_at - dma_tenure_ns) : 0;
        dma_cycle = dma_drive && release_at && (dma_age < 800 || (dma_age >= 1000 && dma_age < 1800));
        dma_read = dma_age >= 1000;
        bool dma_ack = dma_cycle && (dma_age % 1000 >= 200); // independent DMA device response
        bool accepted = active && d.sim_bus_drv && now - cycle_at >= (address() == 0xff0000 ? peripheral_delay_ns : response_ns);
        bool responded = accepted && !suppress_ack;
        // A final foreign pulse also exercises DTACK outside a DMA data phase.
        d.M68K_DTACK_n = !(dma_ack || (dma_drive && dma_age >= 2000) || (responded && !error_response));
        d.M68K_BERR_n = !(responded && error_response);
        if (override_ack>=0) d.M68K_BGACK_n=override_ack;
        else if (!dma_drive && dma_stage!=2) d.M68K_BGACK_n=1;
        if (override_dtack>=0) d.M68K_DTACK_n=override_dtack;
        if (override_berr>=0) d.M68K_BERR_n=override_berr;
        // Command receivers have modeled pull-downs while Pi pins are inputs.
        auto command_pins = gpio & command_mask();
        d.PI_CMD = (command_pins >> 2) & 3; d.PI_CMD_WR = (command_pins >> 1) & 1;
        try { settle(); }
        catch (...) { trace.dump(context.time()); throw; }
        if (!was_busy && d.PI_TXN_IN_PROGRESS) {
            cancel_observed=false; pending_address=address(); pending_data=write_data;
        }
        if (!d.M68K_RESET_n || d.sim_reset_sm) cancel_observed=true;
        if (was_busy && !d.PI_TXN_IN_PROGRESS) completed_at=now;
        if (capture_trace && d.PI_TXN_IN_PROGRESS && !cancel_observed) {
            require(address()==pending_address,"pending address overwritten");
            require(write_data==pending_data,"pending write latch overwritten");
        }
        auto bits=unsigned(d.sim_states);
        require(!(bits & (bits-1)),"transfer state is not one-hot");
        bool physical_as=d.sim_bus_drv && !d.M68K_AS_n;
        if (physical_as && !prev_physical_as) ++physical_cycles;
        prev_physical_as=physical_as;
        if(capture_trace) for(unsigned i=0;i<8;++i) if(d.sim_states&(1u<<i)) ++state_visits[i];
        if (active && d.sim_as_on && d.sim_bus_drv) {
            require(address()==active_address,"pending/active address overwritten");
            if (!d.M68K_RW && !d.LTCH_D_WR_OE_n) require(write_data==active_data,"pending/active write data overwritten");
        }
        if (dma_ack && !dma_read && !dma_write_seen) {
            ram[0x300] = st_value >> 8; ram[0x301] = st_value & 0xff; ++dma_writes; dma_write_seen=true;
        }
        if (dma_ack && dma_read && !dma_read_seen) {
            require(st_value == 0xd00d, "DMA read did not see its write"); ++dma_reads; dma_read_seen=true;
        }
        if (falling && !d.M68K_BR_n) {
            require(!d.sim_go, "start decision after raw BR at falling-edge sampling point");
            if (d.PI_TXN_IN_PROGRESS && !d.sim_as_on) ++blocked_starts;
            if (d.sim_as_on && d.M68K_BG_n) ++deferred_grants;
        }
        if (d.sim_go && !prev_go) {
            require(d.sim_bus_mine && d.sim_bgack_i && d.M68K_BR_n && d.M68K_BGACK_n,
                    "unsafe cycle start decision");
        }
        if (d.sim_as_on && !prev_as) {
            require(d.sim_bus_mine && d.M68K_BGACK_n && !dma_drive, "cycle started without ownership");
            require(!d.LTCH_A_OE_n, "AS without address driver");
            require(address() == (expected_address & 0xfffffe), "wrong address latch");
            require(d.M68K_FC == expected_fc, "wrong function code");
            cycle_at = now; active = true; committed = false; ++cpu_cycles;
            active_address=address(); active_data=write_data;
        }
        if (active && (!d.M68K_UDS_n || !d.M68K_LDS_n)) {
            bool upper = expected_size == 2 || !(expected_address & 1);
            bool lower = expected_size == 2 || (expected_address & 1);
            require(!d.M68K_UDS_n == upper && !d.M68K_LDS_n == lower, "wrong byte lanes");
        }
        if (active && accepted && !suppress_commit && !error_response && d.sim_bus_drv && !d.M68K_RW && !committed &&
            (!d.M68K_UDS_n || !d.M68K_LDS_n)) {
            require(!d.LTCH_D_WR_OE_n, "RAM write without data driver");
            auto a = address();
            if (a == 0xff0000) {
                if (!d.M68K_UDS_n) peripheral_value = (peripheral_value & 0x00ff) | (st_value & 0xff00);
                if (!d.M68K_LDS_n) peripheral_value = (peripheral_value & 0xff00) | (st_value & 0x00ff);
            } else {
                if (!d.M68K_UDS_n) ram.at(a) = st_value >> 8;
                if (!d.M68K_LDS_n) ram.at(a+1) = st_value & 0xff;
            }
            committed = true; ++writes;
            events << "{\"physical_write_ns\":"<<now<<",\"address\":"<<a<<",\"data\":"<<st_value<<",\"writes\":"<<writes<<"}\n";
        }
        if (!d.sim_as_on && prev_as) active = false;
        require(d.M68K_BG_n || !(d.sim_as_on || d.sim_go), "BG asserted during own cycle/start decision");
        if (dma_stage == 3 && d.PI_TXN_IN_PROGRESS) {
            if (!release_at) release_at = now + dma_tenure_ns;
            ++pending_samples;
        }
        if (dma_stage == 3 && release_at && !cancel_observed) require(d.PI_TXN_IN_PROGRESS, "foreign DTACK completed pending CPU access");
        prev_go = d.sim_go; prev_as = d.sim_as_on;
        if (capture_trace || was_clock!=bool(d.M68K_CLK)) trace.dump(context.time());
    }
    void operation() { advance(opt.gpio_ns + (opt.jitter_ns ? timing_rng()%(opt.jitter_ns+1) : 0)); }
    static uint32_t read_cb(void* p, unsigned reg) {
        auto& s = *static_cast<Harness*>(p); s.operation();
        if (reg <= PS_ATARI_FSEL2) return s.fsel.at(reg);
        s.require(reg == PS_ATARI_LEVEL, "unsupported GPIO read");
        return (uint32_t(s.resolve_pi()) << 8) | s.d.PI_TXN_IN_PROGRESS |
            (uint32_t(s.d.PI_RESET) << 4) | (uint32_t(s.d.PI_IPL1) << 5) |
            (uint32_t(s.d.PI_IPL2) << 6) | (uint32_t(s.d.PI_BERR) << 7) | (s.gpio & s.command_mask());
    }
    static void write_cb(void* p, unsigned reg, uint32_t value) {
        auto& s = *static_cast<Harness*>(p);
        if (reg <= PS_ATARI_FSEL2) { s.fsel.at(reg) = value; s.refresh_data_direction(); }
        else if (reg == PS_ATARI_SET) s.gpio |= value;
        else if (reg == PS_ATARI_CLEAR) s.gpio &= ~value;
        else s.require(false, "unsupported GPIO write");
        s.operation();
    }
    static uint64_t time_cb(void* p) {
        auto& s = *static_cast<Harness*>(p); s.operation(); return s.now / 1000;
    }
    static void barrier_cb(void* p) { static_cast<Harness*>(p)->operation(); }
};

static void scenarios(Harness& s) {
    s.start();
    if (s.scenario == "request-strobe") {
        s.check_request_strobe();
    } else if (s.scenario == "ram") {
        auto before = s.cpu_cycles;
        s.require(s.write(0x100, 0xa55a, 2).error == PS_ATARI_OK, "word write failed");
        s.require(s.word(0x100) == 0xa55a, "word write side effect");
        auto r = s.read(0x100, 2);
        s.require(r.error == PS_ATARI_OK && r.data == 0xa55a, "word read failed");
        s.require(s.cpu_cycles == before+2 && s.writes == 1, "duplicate/missing RAM cycle");
    } else if (s.scenario == "lanes") {
        s.require(s.write(0x120, 0x1234, 2, 1).error == PS_ATARI_OK, "initial word write");
        s.require(s.read(0x120, 1, 2).data == 0x12, "even byte read");
        s.require(s.write(0x121, 0xef, 1, 5).error == PS_ATARI_OK, "odd byte write after read");
        s.require(s.word(0x120) == 0x12ef, "odd byte changed upper lane");
        s.require(s.read(0x121, 1, 6).data == 0xef, "odd byte read");
        s.require(s.write(0x120, 0xab, 1, 5).error == PS_ATARI_OK, "even byte write after read");
        auto r = s.read(0x120, 2);
        s.require(r.error == PS_ATARI_OK && r.data == 0xabef, "even byte changed lower lane");
        s.require(s.writes == 3 && s.cpu_cycles == 6, "lane cycle counts");
    } else if (s.scenario == "slow") {
        auto start_cycles = s.cpu_cycles;
        auto t = s.now; auto r = s.read(0xff0000, 2);
        s.require(r.error == PS_ATARI_OK && r.data == 0x5aa5, "slow peripheral read");
        s.require(s.now - t >= 4000 && s.cpu_cycles == start_cycles+1, "slow response timing/count");
        s.require(s.write(0xff0001, 0x3c, 1).error == PS_ATARI_OK, "slow peripheral byte write");
        s.require(s.peripheral_value == 0x5a3c, "slow peripheral side effect/lanes");
    } else if (s.scenario == "berr") {
        s.error_response = true;
        auto before = s.word(0x100);
        s.require(s.write(0x100, 0xffff, 2).error == PS_ATARI_BUS_ERROR, "BERR not reported");
        s.require(s.word(0x100) == before && s.writes == 0, "BERR write committed");
    } else if (s.scenario == "dma") {
        auto before = s.cpu_cycles;
        auto expected = s.word(0x200);
        s.request_dma();
        auto r = s.read(0x200, 2);
        s.require(r.error == PS_ATARI_OK && r.data == expected, "pending read lost/corrupted by DMA DTACK");
        s.require(s.pending_samples >= 2500 && s.cpu_cycles == before+1, "DMA pending/exactly-once coverage");
        s.require(s.dma_writes == 1 && s.dma_reads == 1 && s.word(0x300) == 0xd00d, "DMA memory side effect");
        s.require(s.read(0x300, 2).data == 0xd00d, "CPU cannot see DMA write");
    } else if (s.scenario == "arbitration-pending") {
        auto expected = s.word(0x200); auto before = s.cpu_cycles;
        s.arm_arbitration(false);
        auto r = s.read(0x200, 2);
        s.require(r.error == PS_ATARI_OK && r.data == expected, "request at ADDR_HI lost pending read");
        s.require(s.blocked_starts > 0 && s.pending_samples >= 2500, "pending/start inhibition not exercised");
        s.require(s.cpu_cycles == before+1 && !s.writes && s.dma_reads == 1 && s.dma_writes == 1,
            "pending access did not complete exactly once after safe return");
    } else if (s.scenario == "arbitration-active") {
        s.response_ns = 1500;
        s.arm_arbitration(true);
        auto w = s.write(0x180, 0xcafe, 2);
        s.require(w.error == PS_ATARI_OK && s.word(0x180) == 0xcafe, "active write did not finish before grant");
        s.require(s.deferred_grants > 0 && s.dma_owned(), "active request did not defer grant then acquire bus");
        auto r = s.read(0x180, 2);
        s.require(r.error == PS_ATARI_OK && r.data == 0xcafe, "pending access did not resume after active-cycle grant");
        s.require(s.cpu_cycles == 2 && s.writes == 1 && s.pending_samples >= 2500 &&
            s.dma_reads == 1 && s.dma_writes == 1, "active/pending transaction side effects/counts");
    } else if (s.scenario == "fault-states") {
        s.response_ns=1000;
        s.arm(Harness::Fault::Request,s.opt.target_state,s.opt.offset_ns);
        auto w=s.write(0x180,0xbeef,2);
        s.require(w.error==PS_ATARI_OK,"state-targeted write failed");
        auto r=s.read(0x180,2);
        s.require(r.error==PS_ATARI_OK && r.data==0xbeef,"pending data/address lost across state-targeted DMA");
        s.require(s.fired_events==1 && s.event_states.at(s.opt.target_state)>0,"requested transfer state not hit");
        s.require(s.cpu_cycles==2 && s.writes==1,"state-targeted request duplicated/lost a cycle");
    } else if (s.scenario == "fault-berr" || s.scenario == "fault-bgack" || s.scenario == "fault-reset" || s.scenario == "fault-berr-write" || s.scenario == "fault-bgack-write") {
        bool writing=s.scenario.find("-write")!=std::string::npos;
        auto fault=s.scenario.find("fault-berr")==0?Harness::Fault::Berr:
            s.scenario.find("fault-bgack")==0?Harness::Fault::Ack:Harness::Fault::Reset;
        s.response_ns=3000;
        s.arm(fault,s.opt.target_state,s.opt.offset_ns,s.opt.pulse_ns);
        auto r=writing?s.write(0x200,0xcafe,2):s.read(0x200,2);
        s.require(s.fired_events==1,"fault injection did not fire");
        s.require(r.error!=PS_ATARI_OK,"injected bus fault/reset was reported as success");
        s.require(s.cpu_cycles<=1 && s.physical_cycles<=1 && s.writes<=unsigned(writing),"fault caused transaction replay");
        s.advance(s.opt.pulse_ns+1000);
        s.outcome=s.writes?"committed":"aborted";
        s.recover();
    } else if (s.scenario == "fault-late" || s.scenario == "fault-commit") {
        s.bus.timeout_us=3;
        s.response_ns=s.scenario=="fault-late"?20000:200;
        s.suppress_ack=s.scenario=="fault-commit";
        auto r=s.write(0x180,0xa55a,2);
        s.require(r.error==PS_ATARI_TIMEOUT && s.bus.timed_out,"missing ACK did not lock out");
        s.require(s.writes==(s.scenario=="fault-commit"?1u:0u),"write acceptance timing did not distinguish late vs committed timeout");
        s.outcome=s.writes?"committed":"indeterminate";
        auto cycles=s.cpu_cycles;
        s.require(psAtariWrite(&s.bus,0x400,0xffff,2,5).error==PS_ATARI_TIMEOUT,"timeout allowed new write");
        auto status=psAtariReadStatus(&s.bus);
        s.require((status.data&0x7ff)==(BOARD==240?0x421:0x521),"status unavailable during timeout");
        s.suppress_ack=false; s.advance(25000);
        s.require(s.cpu_cycles==cycles && s.writes==1 && s.word(0x180)==0xa55a,"late completion was lost/duplicated/corrupted");
        s.outcome="committed";
        s.require(s.bus.timed_out,"late completion silently cleared software lockout");
        s.recover();
        s.require(s.cpu_cycles==cycles && s.writes==1,"recovery replayed completed write");
    } else if (s.scenario == "fault-cancel") {
        s.bus.timeout_us=3; s.dma_tenure_ns=40000;
        s.request_dma();
        auto r=s.write(0x180,0xa55a,2);
        s.require(r.error==PS_ATARI_TIMEOUT && s.cpu_cycles==0 && !s.writes,"pending timeout setup");
        s.outcome="indeterminate";
        s.recover();
        s.advance(50000);
        s.require(s.cpu_cycles==0 && !s.writes,"INIT replayed a pending write after timeout");
        s.outcome="aborted";
    } else if (s.scenario == "fault-active-init") {
        s.bus.timeout_us=3; s.response_ns=100000000;
        auto r=s.write(0x180,0xa55a,2);
        s.require(r.error==PS_ATARI_TIMEOUT && s.cpu_cycles==1 && !s.writes,"active timeout setup");
        s.outcome="indeterminate";
        s.recover();
        s.advance(5000);
        s.require(s.cpu_cycles==1 && !s.writes,"INIT replayed active write");
        s.outcome="aborted";
    } else if (s.scenario == "fault-reset-boundary") {
        s.response_ns=1000;
        auto expected=s.word(0x200);
        s.arm(Harness::Fault::Reset,s.opt.target_state,s.opt.offset_ns,s.opt.pulse_ns);
        auto r=s.read(0x200,2);
        s.require(s.fired_events==1,"reset boundary not hit");
        s.require(r.error!=PS_ATARI_OK || (!s.fault_interrupted() && r.data==expected),"reset before completion accepted/replayed an access");
        auto cycles=s.cpu_cycles;
        s.advance(s.opt.pulse_ns+1000); s.recover();
        s.require(s.cpu_cycles==cycles && !s.writes,"reset boundary recovery replayed access");
        s.outcome="aborted";
    } else if (s.scenario == "fault-go") {
        s.response_ns=1000;
        s.arm(Harness::Fault::Request,8,s.opt.offset_ns);
        s.require(s.write(0x180,0xbeef,2).error==PS_ATARI_OK,"GO-edge write");
        auto r=s.read(0x180,2);
        s.require(r.error==PS_ATARI_OK && r.data==0xbeef && s.cpu_cycles==2 && s.writes==1 && s.fired_events==1,"GO-edge request replay/loss");
    } else if (s.scenario == "fault-stale") {
        s.response_ns=10000;
        s.request_dma();
        s.arm(Harness::Fault::Dtack,2,0,4500);
        auto r=s.read(0x200,2);
        s.require(r.error==PS_ATARI_OK,"stale-DTACK read failed to complete");
        s.require(s.now-s.cycle_at>=s.response_ns,"foreign/stale DTACK completed a new cycle before its device responded");
    } else if (s.scenario == "fault-random") {
        std::mt19937 rng(static_cast<unsigned>(s.opt.seed));
        for(unsigned i=0;i<s.opt.iterations;++i) {
            s.response_ns=200+rng()%1800;
            s.dma_tenure_ns=2500+rng()%4000;
            s.dma_ack_delay_ns=rng()%500;
            unsigned a=0x400+2*(rng()%128), size=(rng()%2)+1;
            if(size==1) a+=rng()%2;
            uint16_t value=rng(); unsigned fc=1+rng()%7;
            s.error_response=rng()%7==0;
            if(i%4==0) s.arm(Harness::Fault::Request,2+rng()%6,rng()%126);
            auto writes=s.writes, cycles=s.cpu_cycles;
            s.events << "{\"iteration\":"<<i<<",\"ns\":"<<s.now<<",\"response_ns\":"<<s.response_ns
                <<",\"dma_ns\":"<<s.dma_tenure_ns<<",\"address\":"<<a<<",\"data\":"<<value
                <<",\"size\":"<<size<<",\"berr\":"<<s.error_response<<"}\n";
            auto r=s.write(a,value,size,fc);
            s.require(r.error==(s.error_response?PS_ATARI_BUS_ERROR:PS_ATARI_OK),"random write result");
            s.require(s.writes==writes+(s.error_response?0:1),"random write side effects duplicated/lost");
            s.require(s.cpu_cycles==cycles+1,"random write cycle duplicated");
            if(!s.error_response) {
                auto read=s.read(a,size,fc);
                s.require(read.error==PS_ATARI_OK && read.data==(value&(size==1?0xff:0xffff)),"random readback");
            }
            s.schedule.erase(std::remove_if(s.schedule.begin(),s.schedule.end(),[](const auto& e){return e.fired;}),s.schedule.end());
        }
    } else if (s.scenario == "controls") {
        s.require(psAtariWriteStatus(&s.bus, PS_ATARI_CONTROL_HALT) == PS_ATARI_OK, "HALT write");
        s.require(!(psAtariReadStatus(&s.bus).data & 0x1000), "HALT readback");
        s.require(psAtariWriteStatus(&s.bus, PS_ATARI_CONTROL_RESET) == PS_ATARI_OK, "RESET write");
        s.require(!(psAtariReadStatus(&s.bus).data & 0x2000), "RESET readback");
        s.require(psAtariWriteStatus(&s.bus, PS_ATARI_CONTROL_IDLE) == PS_ATARI_OK, "IDLE write");
        s.require((psAtariReadStatus(&s.bus).data & 0x3000) == 0x3000, "control lines unreleased");
    } else throw std::runtime_error("unknown scenario: " + s.scenario);
    s.idle();
}

static std::string shell_quote(const std::string& value) {
    std::string quoted = "'";
    for (char c : value) quoted += c == '\'' ? "'\\''" : std::string(1, c);
    return quoted + "'";
}

int main(int argc, char** argv) {
    Options o;
    try {
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            auto value = [&]() { if (++i >= argc) throw std::runtime_error("missing option value"); return std::string(argv[i]); };
            if (a == "--scenario") o.scenario = value();
            else if (a == "--trace") o.trace = value();
            else if (a == "--seed") o.seed = std::stoull(value());
            else if (a == "--gpio-ns") o.gpio_ns = std::stoull(value());
            else if (a == "--phase-ns") o.phase_ns = std::stoull(value());
            else if (a == "--target-state") o.target_state = std::stoul(value());
            else if (a == "--offset-ns") o.offset_ns = std::stoul(value());
            else if (a == "--pulse-ns") o.pulse_ns = std::stoul(value());
            else if (a == "--response-ns") o.response_ns = std::stoul(value());
            else if (a == "--settle-ns") o.settle_ns = std::stoul(value());
            else if (a == "--jitter-ns") o.jitter_ns = std::stoul(value());
            else if (a == "--iterations") o.iterations = std::stoul(value());
            else if (a == "--latch-model") {
                auto model = value();
                if (model != "16373" && model != "16374") throw std::runtime_error("invalid latch model");
                o.transparent_latches = model == "16373";
            }
            else if (a == "--keep-trace") o.keep = true;
            else if (a == "--inject-contention") o.inject_contention = true;
            else if (a == "--inject-simulator-failure") o.inject_simulator_failure = true;
            else throw std::runtime_error("unknown option: " + a);
        }
        if (!o.gpio_ns || o.gpio_ns > 10000 || o.phase_ns >= 126 || o.target_state>9 ||
            o.offset_ns>1000000 || !o.pulse_ns || o.pulse_ns>1000000 || o.response_ns>10000000 ||
            o.settle_ns>1000000 || o.jitter_ns>10000 || !o.iterations || o.iterations>1024) throw std::runtime_error("invalid timing options");
        std::cout << "EPM" << BOARD << " scenario=" << o.scenario << " seed=" << o.seed
                  << " gpio_ns=" << o.gpio_ns << " phase_ns=" << o.phase_ns
                  << " latch_model=" << (o.transparent_latches ? "16373" : "16374")
                  << " simulator=" << Verilated::productVersion() << '\n';
        std::ifstream manifest(MANIFEST);
        if (!manifest) throw std::runtime_error("cannot read fixture manifest");
        std::cout << manifest.rdbuf() << std::flush;
        {
            Harness s(o); scenarios(s); s.succeeded=true;
            std::cout << "PASS cycles=" << s.cpu_cycles << " writes=" << s.writes
                      << " dma_writes=" << s.dma_writes << " dma_reads=" << s.dma_reads << " pending_ns=" << s.pending_samples
                      << " blocked_starts=" << s.blocked_starts << " deferred_grants=" << s.deferred_grants << '\n';
        }
        if (!o.keep) std::filesystem::remove(o.trace);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\nWaveform: " << o.trace << "\nReproduce: "
                  << shell_quote(std::filesystem::absolute(argv[0]).string()) << " --scenario " << shell_quote(o.scenario)
                  << " --seed " << o.seed << " --gpio-ns " << o.gpio_ns << " --phase-ns " << o.phase_ns
                  << " --target-state " << o.target_state << " --offset-ns " << o.offset_ns << " --pulse-ns " << o.pulse_ns
                  << " --response-ns " << o.response_ns << " --settle-ns " << o.settle_ns << " --jitter-ns " << o.jitter_ns << " --iterations " << o.iterations
                  << " --latch-model " << (o.transparent_latches ? "16373" : "16374")
                  << " --trace " << shell_quote(o.trace) << " --keep-trace"
                  << (o.inject_contention ? " --inject-contention" : "")
                  << (o.inject_simulator_failure ? " --inject-simulator-failure" : "") << '\n';
        return 1;
    }
}
