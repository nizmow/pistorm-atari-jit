# pistormsxb_dev_ng.sdc — timing constraints for the next-gen SXB CPLD
#
# The design is fully asynchronous by construction: the Pi protocol
# handshakes (PI_WR/PI_RD/PI_A/PI_D/TXN) are level/edge based on no
# common clock with the 200 MHz-free board, and the 68000 bus side
# (DTACK/VPA/IPL/BERR/BR/BGACK) is synchronized into the M68K_CLK domain
# by the c8m_sync/aCLK/BGACK_DELAY shift registers. So the only real
# clock is the Atari's M68K_CLK; everything async is declared false path.

create_clock -name {M68K_CLK} -period 125.000 [get_ports {M68K_CLK}]

# Async protocol inputs from the Pi (no common clock)
set_false_path -from [get_ports {PI_A[*] PI_D[*] PI_RD PI_WR}]

# 68000 bus inputs, synchronized before use
set_false_path -from [get_ports {M68K_DTACK_n M68K_VPA_n M68K_IPL_n[*]
                                 M68K_BERR_n M68K_BR_n M68K_BGACK_n}]

# Latches/control outputs are async strobes; 68000 bus outputs are
# generated from the clocked state machine but cross no clock domains
set_false_path -to [get_ports {LTCH_A_0 LTCH_A_8 LTCH_A_16 LTCH_A_24
                               LTCH_A_OE_n LTCH_D_RD_U LTCH_D_RD_L
                               LTCH_D_RD_OE_n LTCH_D_WR_U LTCH_D_WR_L
                               LTCH_D_WR_OE_n}]
set_false_path -to [get_ports {M68K_AS_n M68K_UDS_n M68K_LDS_n M68K_RW
                               M68K_FC[*] M68K_E M68K_VMA_n M68K_BG_n
                               M68K_RESET_n M68K_HALT_n}]
set_false_path -to [get_ports {PI_TXN_IN_PROGRESS PI_IPL1 PI_IPL2
                               PI_BERR PI_D[*]}]
