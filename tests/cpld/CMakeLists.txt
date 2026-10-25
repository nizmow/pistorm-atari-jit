cmake_minimum_required(VERSION 3.20...4.1)
project(AtariRtlTests LANGUAGES C CXX)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(verilator 5.052 EXACT REQUIRED HINTS $ENV{VERILATOR_ROOT})
enable_testing()
set(ATARI_RTL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../rtl" CACHE PATH "Actual Atari RTL checkout")
add_library(atari_bus STATIC driver/ps_atari_bus.c)
target_compile_features(atari_bus PUBLIC c_std_11)
target_include_directories(atari_bus PUBLIC driver)
target_compile_options(atari_bus PRIVATE -Wall -Wextra -Werror)
foreach(board 240 570)
    set(rtl "${ATARI_RTL_DIR}/pistormSXB_devEPM${board}_1.1b.v")
    set(fixture "${CMAKE_CURRENT_BINARY_DIR}/epm${board}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${rtl}" prepare_rtl.py arbiter.cpp arbiter_probe.sv harness.cpp
        driver/ps_atari_bus.c "${ATARI_RTL_DIR}/version_reg.v" "${ATARI_RTL_DIR}/version_reg_570.v")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/prepare_rtl.py"
        "${rtl}" "${fixture}" "${board}" COMMAND_ERROR_IS_FATAL ANY)
    add_executable(rtl_${board} harness.cpp sim_fatal.cpp)
    target_link_libraries(rtl_${board} PRIVATE atari_bus)
    target_compile_features(rtl_${board} PRIVATE cxx_std_17)
    target_compile_options(rtl_${board} PRIVATE -Wall -Wextra -Wno-sign-compare)
    target_compile_definitions(rtl_${board} PRIVATE VL_USER_FATAL BOARD=${board} MANIFEST="${fixture}/manifest.json")
    verilate(rtl_${board} SOURCES "${fixture}/cpld.v" TOP_MODULE pistormsxb_devEPM${board}
        PREFIX Vcpld TRACE_VCD INCLUDE_DIRS "${ATARI_RTL_DIR}"
        VERILATOR_ARGS -Wno-LATCH -Wno-COMBDLY -Wno-CASEINCOMPLETE --timescale 1ns/1ns)
    foreach(scenario ram lanes slow berr dma controls arbitration-pending arbitration-active request-strobe)
        add_test(NAME rtl_${board}_${scenario} COMMAND rtl_${board} --scenario ${scenario}
            --trace "${fixture}/${scenario}.vcd")
    endforeach()
    # Exercise different GPIO spacings and ST clock phases deterministically.
    add_test(NAME rtl_${board}_phase COMMAND rtl_${board} --scenario dma --seed 19
        --gpio-ns 17 --phase-ns 43 --trace "${fixture}/phase.vcd")
    add_executable(arbiter_${board} arbiter.cpp sim_fatal.cpp)
    target_compile_definitions(arbiter_${board} PRIVATE VL_USER_FATAL BOARD=${board} MANIFEST="${fixture}/manifest.json")
    target_compile_features(arbiter_${board} PRIVATE cxx_std_17)
    verilate(arbiter_${board} SOURCES arbiter_probe.sv "${rtl}" TOP_MODULE ArbiterProbe
        PREFIX Varb TRACE_VCD INCLUDE_DIRS "${ATARI_RTL_DIR}" VERILATOR_ARGS -Wno-LATCH -Wno-COMBDLY -Wno-CASEINCOMPLETE)
    add_test(NAME arbiter_${board} COMMAND arbiter_${board} --output "${fixture}/arbiter")
endforeach()

# A deliberately conflicting equal-value driver must fail and leave a readable
# trace and reproduction command. This checks the monitor/diagnostic path.
foreach(board 240 570)
    add_test(NAME rtl_${board}_diagnostics COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/check_failure.py" "$<TARGET_FILE:rtl_${board}>"
        "${CMAKE_CURRENT_BINARY_DIR}/epm${board}/injected.vcd")
endforeach()
foreach(board 240 570)
    add_test(NAME fault_campaign_${board} COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/fault_campaign.py" --runner "$<TARGET_FILE:rtl_${board}>"
        --output "${CMAKE_CURRENT_BINARY_DIR}/epm${board}/faults")
endforeach()
get_property(rtl_tests DIRECTORY PROPERTY TESTS)
set_tests_properties(${rtl_tests} PROPERTIES TIMEOUT 30)

set_tests_properties(fault_campaign_240 fault_campaign_570 PROPERTIES TIMEOUT 240 LABELS faults)

# Also cover transparent LVC16373A external latches.
# Keep the RTL's LS374 compatibility timing, but model the physical latch part.
foreach(scenario ram lanes slow berr dma controls arbitration-pending arbitration-active request-strobe)
    add_test(NAME rtl_240_16373_${scenario} COMMAND rtl_240 --scenario ${scenario}
        --latch-model 16373 --trace "${CMAKE_CURRENT_BINARY_DIR}/epm240/16373/${scenario}.vcd")
    set_tests_properties(rtl_240_16373_${scenario} PROPERTIES TIMEOUT 30)
endforeach()
add_test(NAME fault_campaign_240_16373 COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/fault_campaign.py" --runner "$<TARGET_FILE:rtl_240>"
    --latch-model 16373 --output "${CMAKE_CURRENT_BINARY_DIR}/epm240/16373/faults")
set_tests_properties(fault_campaign_240_16373 PROPERTIES TIMEOUT 240 LABELS faults)
