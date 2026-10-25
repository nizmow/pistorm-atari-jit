# Standard Atari ST ~8 MHz bus clock with a small frequency margin
# (124 ns = 8.0645 MHz). Adjust for faster physical bus clocks.
# Asynchronous interface timing is not constrained here.
create_clock -name M68K_CLK -period 124.000 [get_ports {M68K_CLK}]
