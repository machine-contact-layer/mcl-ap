cmake_minimum_required(VERSION 3.16)
project(mcl_ap LANGUAGES C)

option(MCL_AP_BUILD_TESTS "Build host-side reference tests" ON)

add_library(mcl_ap_channel STATIC
    src/ap_channel.c)
target_include_directories(mcl_ap_channel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
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
