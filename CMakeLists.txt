cmake_minimum_required(VERSION 3.16)
project(mcl_ap LANGUAGES C)

option(MCL_AP_BUILD_TESTS "Build host-side reference tests" ON)
option(MCL_AP_BUILD_EXPERIMENTS "Build experimental physical-layer testbeds" ON)

set(MCL_WIRE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../mcl-wire" CACHE PATH "Path to mcl-wire repository")

add_library(mcl_ap_channel STATIC
    src/ap_channel.c)
target_include_directories(mcl_ap_channel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(NOT WIN32)
    target_link_libraries(mcl_ap_channel PUBLIC m)
endif()
set_target_properties(mcl_ap_channel PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)

if(MCL_AP_BUILD_TESTS)
    enable_testing()

    add_executable(mcl_ap_test_channel tests/test_ap_channel.c)
    target_link_libraries(mcl_ap_test_channel PRIVATE mcl_ap_channel)
    set_target_properties(mcl_ap_test_channel PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)
    add_test(NAME mcl_ap_test COMMAND mcl_ap_test_channel)

    add_executable(mcl_ap_test_standalone_header tests/test_standalone_header.c)
    target_link_libraries(mcl_ap_test_standalone_header PRIVATE mcl_ap_channel)
    set_target_properties(mcl_ap_test_standalone_header PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)
    add_test(NAME mcl_ap_standalone_header COMMAND mcl_ap_test_standalone_header)
endif()

if(MCL_AP_BUILD_EXPERIMENTS AND EXISTS "${MCL_WIRE_DIR}/include/mcl/wire.h")
    # Sibling wire dependency for experimental waveform harness
    add_library(mcl_ap_exp_wire_dep STATIC
        ${MCL_WIRE_DIR}/src/wire.c
        ${MCL_WIRE_DIR}/src/extension.c)
    target_include_directories(mcl_ap_exp_wire_dep PUBLIC ${MCL_WIRE_DIR}/include)
    set_target_properties(mcl_ap_exp_wire_dep PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)

    add_library(mcl_ap_exp001 STATIC
        experiments/001-known-waveform/exp001.c)
    target_include_directories(mcl_ap_exp001 PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/experiments/001-known-waveform
        ${MCL_WIRE_DIR}/include)
    target_link_libraries(mcl_ap_exp001 PUBLIC mcl_ap_exp_wire_dep)
    if(NOT WIN32)
        target_link_libraries(mcl_ap_exp001 PUBLIC m)
    endif()
    set_target_properties(mcl_ap_exp001 PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)

    add_executable(mcl_ap_exp001_test
        experiments/001-known-waveform/test_exp001.c)
    target_link_libraries(mcl_ap_exp001_test PRIVATE mcl_ap_exp001)
    set_target_properties(mcl_ap_exp001_test PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)

    add_executable(mcl_ap_exp001_bakeoff
        experiments/001-known-waveform/bakeoff.c)
    target_link_libraries(mcl_ap_exp001_bakeoff PRIVATE mcl_ap_exp001)
    set_target_properties(mcl_ap_exp001_bakeoff PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)

    add_executable(mcl_ap_exp001_decode_wav
        experiments/001-known-waveform/decode_wav.c)
    target_link_libraries(mcl_ap_exp001_decode_wav PRIVATE mcl_ap_exp001)
    set_target_properties(mcl_ap_exp001_decode_wav PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)

    # Experiment 002 reuses the Experiment 001 WAV I/O and shares its
    # LAB/EXPERIMENTAL status. It is a measurement instrument, not a modem.
    add_executable(mcl_ap_exp002_sweep
        experiments/002-path-characterization/exp002_sweep.c)
    target_link_libraries(mcl_ap_exp002_sweep PRIVATE mcl_ap_exp001)
    set_target_properties(mcl_ap_exp002_sweep PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)

    # Experiment 003 evaluates an AP-B0 candidate whose tone pair comes from
    # the Experiment 002 measurement. Same frame, preamble and receiver as
    # Experiment 001; only the band differs.
    add_executable(mcl_ap_exp003
        experiments/003-band-informed-candidate/exp003.c)
    target_link_libraries(mcl_ap_exp003 PRIVATE mcl_ap_exp001)
    set_target_properties(mcl_ap_exp003 PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)

    if(MCL_AP_BUILD_TESTS)
        add_test(NAME mcl_ap_exp001_unit COMMAND mcl_ap_exp001_test)
        add_test(
            NAME mcl_ap_exp001_source_decode
            COMMAND mcl_ap_exp001_decode_wav
                    "${CMAKE_CURRENT_SOURCE_DIR}/experiments/001-known-waveform/exp001_e3_source.wav")
    endif()
endif()
