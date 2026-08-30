// SPDX-License-Identifier: MIT
/*
 * version_reg.v — CPLD firmware revision register (11 bits).
 *
 * Included by pistormsxb_dev_ng.v. The .qsf normally regenerates this
 * via update_version.tcl at build time (see pistormsxb_dev_ng.qsf);
 * this static default is what you get if that hook is absent.
 *
 * Encoding (read back by ps_get_firmware_revision()):
 *   bits 10:9  release type: 0,1 = alpha  1,0 = beta  0,0 = release
 *   bit  8     CPLD type:    1 = EPM570, 0 = EPM240
 *   bits  7:0  version number (alpha: 0-255; beta/release: 3-bit major,
 *              5-bit minor -> max 7.31)
 */
reg [10:0] fwrev = 11'b00_0_00000000;  // 0.0 release, EPM240
