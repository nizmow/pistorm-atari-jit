# Review target: standard ST/H5 ~8 MHz CPU clock, with a small frequency margin
# (124 ns = 8.0645 MHz). The inherited 141 ns constraint was too slow for ST.
# This does not cover a faster H5 clock or constrain the asynchronous interface.
create_clock -name M68K_CLK -period 124.000 [get_ports {M68K_CLK}]
