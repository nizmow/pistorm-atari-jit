// SPDX-License-Identifier: MIT
// Observation only: instantiate the checked-in arbiter without changing logic.
module ArbiterProbe (
    input wire clk, reset_n, br_n, bgack_n, as_busy,
    output wire br_clear, bg_n, bus_mine,
    output wire [1:0] sim_state, sim_br_s, sim_bgk_s,
    output wire sim_quiet
);
    BusArbSync dut (.*);
    assign sim_state = dut.st;
    assign sim_quiet = dut.quiet;
    assign sim_br_s = dut.br_s;
    assign sim_bgk_s = dut.bgk_s;
endmodule
