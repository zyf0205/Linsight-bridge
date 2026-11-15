cmake_minimum_required(VERSION 3.16)
project(linsight-host C)

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()
add_compile_options(-Wall -Wextra)

option(LINSIGHT_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(LINSIGHT_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# Shared host code: session dedupe and the v2.1 parser (no .lsc I/O —
# tools/lsc.py owns file formats; the bridge never records).
add_library(linsight_core STATIC
    src/session.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/frame.c)
target_include_directories(linsight_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol)

# linsightd: single-threaded epoll bridge (paced vcan forward + --dump)
add_executable(linsightd src/main.c src/pace.c)
target_link_libraries(linsightd PRIVATE linsight_core rt)

enable_testing()
add_executable(test_record tests/test_record.c)
target_link_libraries(test_record PRIVATE linsight_core)
add_executable(test_parser tests/test_parser.c)
target_link_libraries(test_parser PRIVATE linsight_core)
add_executable(test_fuzz tests/test_fuzz.c)
target_link_libraries(test_fuzz PRIVATE linsight_core)
# Firmware ring compiled against tiny ESP-IDF shims: verifies drop-oldest order.
add_executable(test_ring tests/test_ring.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/can_ring.c)
target_link_libraries(test_ring PRIVATE linsight_core)
target_include_directories(test_ring PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/shims
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware)
add_executable(test_session tests/test_session.c)
target_link_libraries(test_session PRIVATE linsight_core)
add_executable(test_pace tests/test_pace.c src/pace.c)
target_link_libraries(test_pace PRIVATE linsight_core)

add_test(NAME record COMMAND test_record)
add_test(NAME parser COMMAND test_parser)
add_test(NAME fuzz COMMAND test_fuzz)
add_test(NAME ring COMMAND test_ring)
add_test(NAME session COMMAND test_session)
add_test(NAME pace COMMAND test_pace)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME integration
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py
                     $<TARGET_FILE:linsightd>)
    add_test(NAME tools
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tools.py)
    add_test(NAME lsc
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_lsc.py)
endif()

# Install: bridge binary + systemd unit + /etc/default args.
include(GNUInstallDirs)
install(TARGETS linsightd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
configure_file(systemd/linsightd.service.in
               ${CMAKE_CURRENT_BINARY_DIR}/linsightd.service @ONLY)
configure_file(systemd/linsightd.default.in
               ${CMAKE_CURRENT_BINARY_DIR}/linsightd.default @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/linsightd.service
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/linsightd.default
        DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/default RENAME linsightd)
