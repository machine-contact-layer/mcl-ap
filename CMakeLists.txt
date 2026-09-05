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

# The portable candidate modem. Unlike the experiment code it is built for
# both the host and an embedded target, so it takes no filesystem, allocates
# nothing, and keeps its receive working set in a caller-owned struct.
#
# EXPERIMENTAL: this is the Experiment 003 candidate waveform, not AP-B0.
set(MCL_LINK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../mcl-link" CACHE PATH "Path to mcl-link repository")

add_library(mcl_ap_modem STATIC
    src/ap_modem.c)
target_include_directories(mcl_ap_modem PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(NOT WIN32)
    target_link_libraries(mcl_ap_modem PUBLIC m)
endif()
set_target_properties(mcl_ap_modem PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)

# The continuous listener: the same modem receiver driven by an audio stream
# that nobody scheduled, so a machine busy doing its job can still be called.
# Header-and-source only, no allocation, no dependency beyond the modem.
add_library(mcl_ap_listen STATIC
    src/ap_listen.c)
target_include_directories(mcl_ap_listen PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(mcl_ap_listen PUBLIC mcl_ap_modem)
set_target_properties(mcl_ap_listen PROPERTIES
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

    # The modem test decodes the ARCHIVED E4 captures and requires the same
    # result the frozen Experiment 003 instrument recorded on those files.
    # Needs mcl-wire and mcl-link because the payload it sends is a real
    # Tier-0 object inside a real Link frame, built on the spot.
    if(EXISTS "${MCL_WIRE_DIR}/include/mcl/wire.h" AND
       EXISTS "${MCL_LINK_DIR}/include/mcl/link.h")
        add_executable(mcl_ap_test_modem
            tests/test_ap_modem.c
            ${MCL_WIRE_DIR}/src/wire.c
            ${MCL_LINK_DIR}/src/link.c)
        target_include_directories(mcl_ap_test_modem PRIVATE
            ${MCL_WIRE_DIR}/include
            ${MCL_LINK_DIR}/include)
        target_link_libraries(mcl_ap_test_modem PRIVATE mcl_ap_modem)
        set_target_properties(mcl_ap_test_modem PROPERTIES
            C_STANDARD 99
            C_STANDARD_REQUIRED YES
            C_EXTENSIONS NO)
        add_test(NAME mcl_ap_modem_test
                 COMMAND mcl_ap_test_modem "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()

    # The host half of Experiment 004: the same three sources the DFR1154
    # firmware compiles, without the I2S peripherals. It is what the laptop
    # runs when the board is the other peer.
    if(EXISTS "${MCL_WIRE_DIR}/include/mcl/wire.h" AND
       EXISTS "${MCL_LINK_DIR}/include/mcl/link.h")
        add_executable(mcl_ap_node
            tools/mcl_ap_node.c
            ${MCL_WIRE_DIR}/src/wire.c
            ${MCL_LINK_DIR}/src/link.c)
        target_include_directories(mcl_ap_node PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tools
            ${MCL_WIRE_DIR}/include
            ${MCL_LINK_DIR}/include)
        target_link_libraries(mcl_ap_node PRIVATE mcl_ap_listen)
        set_target_properties(mcl_ap_node PROPERTIES
            C_STANDARD 99
            C_STANDARD_REQUIRED YES
            C_EXTENSIONS NO)
        add_test(NAME mcl_ap_node_selftest COMMAND mcl_ap_node selftest)
    endif()

    # The listener is driven by the ARCHIVED captures as well as by synthetic
    # streams: it must reach the same verdict, byte for byte, that the one-shot
    # decoder reaches on the same files.
    add_executable(mcl_ap_test_listen tests/test_ap_listen.c)
    target_include_directories(mcl_ap_test_listen PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tools)
    target_link_libraries(mcl_ap_test_listen PRIVATE mcl_ap_listen)
    set_target_properties(mcl_ap_test_listen PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)
    add_test(NAME mcl_ap_listen_test
             COMMAND mcl_ap_test_listen "${CMAKE_CURRENT_SOURCE_DIR}")

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

    # Experiment 010 measures the bit-error structure of the retained corpus so
    # that a coding scheme for AP-BOOTSTRAP-1 is chosen from evidence rather
    # than from assumption.
    #
    # It links NO mcl_ap target on purpose. The per-bit soft value it needs is
    # internal to the decoder, so it includes src/ap_modem.c directly; linking
    # the library as well would define every one of those symbols twice. The
    # alternative -- reimplementing the acquisition, timing search and Goertzel
    # demodulation here -- would measure a second modem resembling the first,
    # and any difference between them would be indistinguishable from a result.
    add_executable(mcl_ap_exp010_error_structure
        experiments/010-bootstrap-bakeoff/error_structure.c)
    target_include_directories(mcl_ap_exp010_error_structure PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(NOT WIN32)
        target_link_libraries(mcl_ap_exp010_error_structure PRIVATE m)
    endif()
    set_target_properties(mcl_ap_exp010_error_structure PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)

    # Experiment 010b derives the timing tolerance of a frame as a function of
    # its length, using the real major-1 objects, so it links mcl-wire. Same
    # rule as 010: it includes src/ap_modem.c and must not also link mcl_ap.
    add_executable(mcl_ap_exp010_timing_tolerance
        experiments/010-bootstrap-bakeoff/timing_tolerance.c)
    target_include_directories(mcl_ap_exp010_timing_tolerance PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(mcl_ap_exp010_timing_tolerance PRIVATE mcl_ap_exp_wire_dep)
    if(NOT WIN32)
        target_link_libraries(mcl_ap_exp010_timing_tolerance PRIVATE m)
    endif()
    set_target_properties(mcl_ap_exp010_timing_tolerance PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)

    # Experiment 010c tests the timing remedy 010b indicated, with a blind
    # estimator a receiver could run. Same rule: includes src/ap_modem.c, so it
    # must not also link mcl_ap.
    add_executable(mcl_ap_exp010_timing_remedy
        experiments/010-bootstrap-bakeoff/timing_remedy.c)
    target_include_directories(mcl_ap_exp010_timing_remedy PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(NOT WIN32)
        target_link_libraries(mcl_ap_exp010_timing_remedy PRIVATE m)
    endif()
    set_target_properties(mcl_ap_exp010_timing_remedy PROPERTIES
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
