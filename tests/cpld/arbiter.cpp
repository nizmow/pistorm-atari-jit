// SPDX-License-Identifier: MIT
#include "Varb.h"
#include "verilated_vcd_c.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr unsigned OWNED = 0, GRANT = 1, FOREIGN = 2, SETTLE = 3;
constexpr std::array<const char*, 4> names{"OWNED", "GRANT", "FOREIGN", "SETTLE"};
// Reachability requirements, not a next-state implementation.
constexpr bool reachable[4][4] = {
    {true, true, true, false}, {false, true, true, true},
    {false, false, true, true}, {true, true, true, true}
};
struct Coverage {
    std::array<uint64_t, 4> states{}, resets{};
    std::array<std::array<uint64_t, 4>, 4> transitions{};
    std::array<uint64_t, 2> interrupted_br{}, interrupted_ack{};
    uint64_t falling_edges = 0, assertions = 0, returns = 0, deferred_grants = 0, held_grants = 0;
    uint64_t sampled_pulses = 0, missed_pulses = 0, cases = 0;
    std::vector<std::string> completed_cases;
};
struct Snapshot {
    unsigned state, br, ack;
    bool quiet, mine, bg, clear;
};

class Arbiter {
    VerilatedContext context;
    static VerilatedContext* configured(VerilatedContext& c) { c.threads(1); c.traceEverOn(true); return &c; }
    Varb d{configured(context)};
    VerilatedVcdC trace;
    Coverage& coverage;
    unsigned released_edges = 0, clean_settle_edges = 0;
public:
    uint64_t now = 0;
    std::string label;
    Arbiter(Coverage& c, std::string name, const std::filesystem::path& path) : coverage(c), label(std::move(name)) {
        d.clk = 0; d.reset_n = 1; d.br_n = 1; d.bgack_n = 1; d.as_busy = 0;
        d.trace(&trace, 8); trace.open(path.c_str());
        d.eval(); trace.dump(context.time());
        // Explicit reset edge; do not use Verilator's zero initialization as reset.
        tick([&] { d.reset_n = 0; });
        cycle(1, 1, 0, 0); cycle(1, 1, 0); cycles(3);
        ++coverage.cases;
    }
    ~Arbiter() { d.final(); trace.close(); }
    Snapshot snapshot() const { return {d.sim_state, d.sim_br_s, d.sim_bgk_s,
        bool(d.sim_quiet), bool(d.bus_mine), bool(d.bg_n), bool(d.br_clear)}; }
    void check(bool ok, const std::string& why) {
        ++coverage.assertions;
        if (!ok) throw std::runtime_error(label + ": " + why + " at " + std::to_string(now) + " ns");
    }
    void expect(unsigned state, bool mine, bool bg) {
        check(d.sim_state == state && bool(d.bus_mine) == mine && bool(d.bg_n) == bg,
            std::string("expected ") + names[state] + " mine=" + std::to_string(mine) + " bg_n=" + std::to_string(bg));
    }
    void tick(const std::function<void()>& action = {}) {
        auto before = snapshot(); bool was_clock = d.clk, was_reset = d.reset_n;
        ++now; context.timeInc(1);
        if (action) action();
        d.clk = now % 126 >= 63;
        bool falling = was_clock && !d.clk;
        try {
            d.eval();
            auto after = snapshot();
            if (!d.reset_n) {
                check(after.state == OWNED && after.mine && after.bg && !after.quiet &&
                    after.br == 3 && after.ack == 3 && after.clear, "asynchronous reset values");
                if (was_reset) ++coverage.resets[before.state];
                released_edges = clean_settle_edges = 0;
            } else if (falling) {
                ++coverage.falling_edges;
                ++coverage.states[after.state];
                check(after.state < 4 && reachable[before.state][after.state], "illegal state transition");
                ++coverage.transitions[before.state][after.state];
                // Independently check the synchronizer pipeline and registered
                // boundary; do not infer its latency from the FSM outputs.
                check(after.br == ((before.br << 1 | unsigned(d.br_n)) & 3), "BR synchronizer");
                check(after.ack == ((before.ack << 1 | unsigned(d.bgack_n)) & 3), "BGACK synchronizer");
                check(after.clear == bool(d.br_n), "one-flop BR-clear sampling");
                check(after.mine == (after.state == OWNED), "ownership/state disagreement");
                check(after.bg || (!after.mine && after.state == GRANT), "unsafe grant ownership/state");
                if (!(before.br & 2) || !(before.ack & 2))
                    check(!after.mine, "ownership allowed after synchronized request/acknowledge");
                if (!(before.ack & 2))
                    check(after.state == FOREIGN && after.bg, "BGACK must have priority and negate grant");
                if (before.bg && !after.bg) check(!d.as_busy, "new grant while own cycle busy");
                if (before.state == GRANT && !before.bg && !(before.br & 2) && (before.ack & 2)) {
                    check(!after.bg && !after.mine, "grant withdrawn without synchronized acknowledgement/withdrawal");
                    ++coverage.held_grants;
                }
                if (before.state == GRANT && before.bg && d.as_busy &&
                    (before.ack & 2) && !(before.br & 2)) {
                    check(after.bg && !after.mine, "active cycle failed to defer grant");
                    ++coverage.deferred_grants;
                }
                bool clean = (before.br & 2) && (before.ack & 2);
                if (before.state == SETTLE && clean) ++clean_settle_edges;
                else clean_settle_edges = 0;
                if (!before.mine && after.mine) {
                    check(before.state == SETTLE && before.quiet && clean && clean_settle_edges >= 2,
                        "ownership returned without two clean SETTLE samples");
                    ++coverage.returns;
                }
                if (before.state == SETTLE && after.state == GRANT)
                    ++coverage.interrupted_br[before.quiet];
                if (before.state == SETTLE && after.state == FOREIGN)
                    ++coverage.interrupted_ack[before.quiet];
                // Fairness is explicit: both raw requests stay released. No
                // promise of progress is made while a master holds either low.
                if (d.br_n && d.bgack_n) ++released_edges; else released_edges = 0;
                if (released_edges >= 6) check(after.mine && after.bg, "no ownership return within six released falling edges");
            } else {
                check(after.state == before.state && after.quiet == before.quiet &&
                    after.mine == before.mine && after.bg == before.bg && after.clear == before.clear &&
                    after.br == before.br && after.ack == before.ack, "registered outputs changed away from falling edge/reset");
            }
        } catch (...) { trace.dump(context.time()); throw; }
        trace.dump(context.time());
    }
    void until(uint64_t target) { check(target >= now, "time went backwards"); while (now < target) tick(); }
    // Directed vectors update halfway before the next falling edge. One call
    // consumes one sampled edge when called at a falling-edge boundary.
    void cycle(bool br, bool ack, bool busy = false, bool reset = true) {
        uint64_t fall = (now / 126 + 1) * 126;
        if (fall - 62 <= now) fall += 126;
        until(fall - 63); tick([&] { d.br_n = br; d.bgack_n = ack; d.as_busy = busy; d.reset_n = reset; });
        until(fall);
    }
    void cycles(unsigned count) { while (count--) cycle(d.br_n, d.bgack_n, d.as_busy, d.reset_n); }
    void grant(bool busy = false) { cycle(0, 1, busy); cycles(4); expect(GRANT, false, busy); }
    void foreign() { grant(); cycle(0, 0); cycles(4); expect(FOREIGN, false, true); }
    void release() { cycle(1, 1); cycles(5); expect(OWNED, true, true); }
    void reset_here() {
        // Reset between edges, including with asserted raw BR/BGACK.
        tick([&] { d.reset_n = 0; });
        check(d.sim_state == OWNED && d.bus_mine && d.bg_n, "reset did not act asynchronously");
        cycle(1, 1, 0, 0); cycle(1, 1, 0); cycles(5); expect(OWNED, true, true);
    }
    void phase(bool rising, int offset, bool br, bool ack, bool busy = false) {
        uint64_t target = (now / 126 + 1) * 126 + (rising ? 63 : 0) + offset;
        until(target - 1); tick([&] { d.br_n = br; d.bgack_n = ack; d.as_busy = busy; });
    }
    void pulse(bool rising, int offset) {
        phase(rising, offset, 0, 1);
        // A two-nanosecond pulse may hit or miss the defined sampling edge.
        tick(); bool sampled = !d.br_clear;
        tick([&] { d.br_n = 1; });
        cycles(8); expect(OWNED, true, true);
        if (sampled) ++coverage.sampled_pulses; else ++coverage.missed_pulses;
    }
};

using Case = std::pair<std::string, std::function<void(Arbiter&)>>;
std::vector<Case> directed() {
    std::vector<Case> cases{
        {"idle-request", [](Arbiter& a) { a.grant(); a.cycles(8); a.expect(GRANT, false, false); a.cycle(0, 0); a.cycles(4); a.expect(FOREIGN, false, true); a.release(); }},
        {"active-request", [](Arbiter& a) { a.grant(true); a.cycles(8); a.expect(GRANT, false, true); a.cycle(0, 1, 0); a.expect(GRANT, false, false); a.cycle(0, 0); a.cycles(4); a.release(); }},
        {"withdraw-before-grant", [](Arbiter& a) { a.grant(true); a.cycle(1, 1, 1); a.cycles(5); a.expect(OWNED, true, true); }},
        {"withdraw-after-grant", [](Arbiter& a) { a.grant(); a.release(); }},
        {"early-ack", [](Arbiter& a) { a.grant(true); a.cycle(0, 0, 1); a.cycles(4); a.expect(FOREIGN, false, true); a.cycle(1, 0); a.cycles(8); a.expect(FOREIGN, false, true); a.release(); }},
        {"unexpected-ack", [](Arbiter& a) { a.cycle(1, 0); a.cycles(8); a.expect(FOREIGN, false, true); a.release(); }},
        {"ack-request-priority", [](Arbiter& a) { a.cycle(0, 0); a.cycles(4); a.expect(FOREIGN, false, true); a.release(); }},
        {"consecutive-tenures", [](Arbiter& a) { a.foreign(); a.cycle(0, 1); a.cycles(5); a.expect(GRANT, false, false); a.cycle(0, 0); a.cycles(5); a.expect(FOREIGN, false, true); a.release(); }},
        {"late-ack-after-withdrawal", [](Arbiter& a) { a.grant(); a.cycle(1, 1); a.cycle(1, 0); a.cycles(5); a.expect(FOREIGN, false, true); a.release(); }},
        {"reset-owned", [](Arbiter& a) { a.reset_here(); }},
        {"reset-grant", [](Arbiter& a) { a.grant(); a.reset_here(); }},
        {"reset-foreign", [](Arbiter& a) { a.foreign(); a.reset_here(); }},
        {"reset-settle", [](Arbiter& a) { a.foreign(); a.cycle(1, 1); a.cycles(2); a.expect(SETTLE, false, true); a.reset_here(); }},
        {"reset-quiet", [](Arbiter& a) { a.foreign(); a.cycle(1, 1); a.cycles(3); a.expect(SETTLE, false, true); a.check(a.snapshot().quiet, "quiet SETTLE not reached"); a.reset_here(); }},
        {"reset-asserted-inputs", [](Arbiter& a) { a.foreign(); a.cycle(0, 0, 0, 0); a.cycle(0, 0); a.cycles(4); a.expect(FOREIGN, false, true); a.release(); }}
    };
    for (bool ack : {false, true}) for (unsigned quiet = 0; quiet < 2; ++quiet) {
        cases.emplace_back(std::string("settle-") + (ack ? "ack-" : "request-") + std::to_string(quiet),
            [=](Arbiter& a) {
                a.foreign();
                a.cycle(1, 1); if (quiet) a.cycles(1);
                // Pipeline the new assertion before SETTLE; its stage-2 sample
                // interrupts either the first or second quiet decision.
                a.cycle(!ack ? 0 : 1, ack ? 0 : 1); a.cycles(4);
                a.expect(ack ? FOREIGN : GRANT, false, ack); a.release();
            });
    }
    return cases;
}

void write_coverage(const std::filesystem::path& path, const Coverage& c, bool success) {
    std::ofstream f(path);
    if (!f) throw std::runtime_error("cannot write coverage report: " + path.string());
    f << "{\n  \"board\": " << BOARD << ",\n  \"simulator\": \"" << Verilated::productVersion()
      << "\",\n  \"cases\": " << c.cases << ",\n  \"falling_edges\": " << c.falling_edges
      << ",\n  \"assertions\": " << c.assertions
      << ",\n  \"success\": " << (success ? "true" : "false")
      << ",\n  \"clock_period_ns\": 126,\n  \"release_bound_falling_edges\": 6,"
      << "\n  \"phase_offsets_ns\": [-2, -1, 0, 1, 2],\n  \"phase_edges\": [\"falling\", \"rising\"],"
      << "\n  \"states\": {";
    for (unsigned i = 0; i < 4; ++i) f << (i ? ", " : "") << '"' << names[i] << "\": " << c.states[i];
    f << "},\n  \"transitions\": {\n"; bool first = true;
    for (unsigned i = 0; i < 4; ++i) for (unsigned j = 0; j < 4; ++j) if (reachable[i][j]) {
        f << (first ? "" : ",\n") << "    \"" << names[i] << "->" << names[j] << "\": " << c.transitions[i][j]; first = false;
    }
    f << "\n  },\n  \"resets_by_state\": [";
    for (unsigned i = 0; i < 4; ++i) f << (i ? ", " : "") << c.resets[i];
    f << "],\n  \"settle_request_interruptions\": [" << c.interrupted_br[0] << ", " << c.interrupted_br[1]
      << "],\n  \"settle_ack_interruptions\": [" << c.interrupted_ack[0] << ", " << c.interrupted_ack[1]
      << "],\n  \"safe_returns\": " << c.returns << ",\n  \"deferred_grants\": " << c.deferred_grants
      << ",\n  \"held_grants\": " << c.held_grants
      << ",\n  \"sampled_pulses\": " << c.sampled_pulses << ",\n  \"missed_pulses\": " << c.missed_pulses << ",\n  \"completed_cases\": [";
    for (unsigned i = 0; i < c.completed_cases.size(); ++i)
        f << (i ? ", " : "") << '\"' << c.completed_cases[i] << '\"';
    std::ifstream manifest(MANIFEST);
    if (!manifest) throw std::runtime_error("cannot read fixture manifest");
    f << "],\n  \"source_manifest\": " << manifest.rdbuf() << "}\n";
}
std::string quote(const std::string& s) {
    std::string out = "'"; for (char c : s) out += c == '\'' ? "'\\''" : std::string(1, c); return out + "'";
}
} // namespace

int main(int argc, char** argv) {
    std::filesystem::path output = "arbiter-results";
    std::string selected = "all", active_case = "setup";
    Coverage coverage;
    try {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (++i >= argc) throw std::runtime_error("missing option value");
            if (arg == "--output") output = argv[i];
            else if (arg == "--case") selected = argv[i];
            else throw std::runtime_error("unknown option: " + arg);
        }
        std::filesystem::create_directories(output);
        std::ifstream manifest(MANIFEST);
        if (!manifest) throw std::runtime_error("cannot read fixture manifest");
        std::cout << "BusArbSync EPM" << BOARD << " simulator=" << Verilated::productVersion() << '\n' << manifest.rdbuf();
        auto cases = directed();
        for (bool rising : {false, true}) for (int offset : {-2, -1, 0, 1, 2}) {
            std::string suffix = std::string(rising ? "rise" : "fall") + std::to_string(offset);
            cases.emplace_back("sweep-idle-" + suffix, [=](Arbiter& a) {
                a.phase(rising, offset, 0, 1); a.cycles(5); a.expect(GRANT, false, false);
                a.phase(rising, offset, 0, 0); a.cycles(5); a.expect(FOREIGN, false, true);
                a.phase(rising, offset, 1, 0); a.cycles(6); a.expect(FOREIGN, false, true);
                a.phase(rising, offset, 1, 1); a.cycles(6); a.expect(OWNED, true, true);
            });
            cases.emplace_back("sweep-active-" + suffix, [=](Arbiter& a) {
                a.phase(rising, offset, 0, 1, 1); a.cycles(6); a.expect(GRANT, false, true);
                a.phase(rising, offset, 0, 1, 0); a.cycles(2); a.expect(GRANT, false, false); a.release();
            });
            cases.emplace_back("sweep-unexpected-ack-" + suffix, [=](Arbiter& a) {
                a.phase(rising, offset, 1, 0); a.cycles(6); a.expect(FOREIGN, false, true); a.release();
            });
            cases.emplace_back("sweep-pulse-" + suffix, [=](Arbiter& a) { a.pulse(rising, offset); });
        }
        bool found = false;
        for (const auto& [name, run] : cases) if (selected == "all" || selected == name) {
            found = true; active_case = name;
            Arbiter a(coverage, name, output / (name + ".vcd")); run(a);
            coverage.completed_cases.push_back(name);
            std::cout << "PASS " << name << '\n';
        }
        if (!found) throw std::runtime_error("unknown case: " + selected);
        active_case = "coverage";
        if (selected == "all") {
            for (unsigned i = 0; i < 4; ++i) {
                if (!coverage.states[i] || !coverage.resets[i]) throw std::runtime_error("missing state/reset coverage");
                for (unsigned j = 0; j < 4; ++j) if (reachable[i][j] && !coverage.transitions[i][j])
                    throw std::runtime_error(std::string("missing transition ") + names[i] + "->" + names[j]);
            }
            for (unsigned quiet = 0; quiet < 2; ++quiet)
                if (!coverage.interrupted_br[quiet] || !coverage.interrupted_ack[quiet]) throw std::runtime_error("missing SETTLE interruption branch");
            if (!coverage.sampled_pulses || !coverage.missed_pulses || !coverage.deferred_grants || !coverage.held_grants || !coverage.returns)
                throw std::runtime_error("missing pulse/grant/progress coverage");
        }
        write_coverage(output / "coverage.json", coverage, true);
        std::cout << "PASS " << coverage.cases << " cases; 4 states / 12 transitions required; "
                  << coverage.assertions << " assertions. Coverage: " << output / "coverage.json" << '\n';
    } catch (const std::exception& e) {
        try { write_coverage(output / "coverage.json", coverage, false); } catch (...) {}
        std::cerr << "FAIL " << e.what() << "\nWaveforms: " << output << "\nReproduce: "
                  << quote(std::filesystem::absolute(argv[0]).string()) << " --output " << quote(output.string())
                  << " --case " << quote(active_case == "coverage" || active_case == "setup" ? selected : active_case) << '\n';
        return 1;
    }
}
