// SPDX-License-Identifier: MIT
#include "verilated.h"
#include <stdexcept>
#include <string>

// Verilator normally exits immediately. Unwind through the runner so traces
// close and the normal failure/reproduction report is emitted instead.
void vl_fatal(const char* filename, int line, const char* hierarchy, const char* message) {
    throw std::runtime_error(std::string("simulator fatal: ") + filename + ":" +
        std::to_string(line) + " " + hierarchy + ": " + message);
}
