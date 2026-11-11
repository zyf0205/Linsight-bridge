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

# Shared host code: .lsc I/O, session dedupe and the v2.1 parser.
add_library(linsight_core STATIC
    src/canlog.c
    src/session.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/frame.c)
target_include_directories(linsight_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol)

# Offline file tools used only by linsight-cli.
add_library(linsight_offline STATIC
    src/compare.c
    src/dbc.c
    src/replay.c
    src/import.c)
target_link_libraries(linsight_offline PUBLIC linsight_core)

# linsightd: epoll daemon, .lsc writer thread, Unix-socket control
add_executable(linsightd src/main.c)
target_link_libraries(linsightd PRIVATE linsight_core pthread rt)

# linsight-cli: stats / live / export via control socket and .lsc files
add_executable(linsight-cli cli/cli.c)
target_link_libraries(linsight-cli PRIVATE linsight_offline rt)

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
add_executable(test_canlog tests/test_canlog.c)
target_link_libraries(test_canlog PRIVATE linsight_core)
add_executable(test_compare tests/test_compare.c)
target_link_libraries(test_compare PRIVATE linsight_offline)
add_executable(test_dbc tests/test_dbc.c)
target_link_libraries(test_dbc PRIVATE linsight_offline)
add_executable(test_session tests/test_session.c)
target_link_libraries(test_session PRIVATE linsight_core)
add_executable(test_export tests/test_export.c)
target_link_libraries(test_export PRIVATE linsight_core)
add_executable(test_import tests/test_import.c)
target_link_libraries(test_import PRIVATE linsight_offline)

add_test(NAME record COMMAND test_record)
add_test(NAME parser COMMAND test_parser)
add_test(NAME fuzz COMMAND test_fuzz)
add_test(NAME ring COMMAND test_ring)
add_test(NAME canlog COMMAND test_canlog)
add_test(NAME compare COMMAND test_compare)
add_test(NAME dbc COMMAND test_dbc)
add_test(NAME session COMMAND test_session)
add_test(NAME export COMMAND test_export)
add_test(NAME import COMMAND test_import)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME integration
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py
                     $<TARGET_FILE:linsightd> $<TARGET_FILE:linsight-cli>)
    add_test(NAME rotation
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/rotation.py
                     $<TARGET_FILE:linsightd>)
    add_test(NAME cli
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.py
                     $<TARGET_FILE:linsight-cli>)
    add_test(NAME tools
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tools.py)
endif()

# Install: binaries, systemd unit + /etc/default args.
include(GNUInstallDirs)
install(TARGETS linsightd linsight-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
configure_file(systemd/linsightd.service.in
               ${CMAKE_CURRENT_BINARY_DIR}/linsightd.service @ONLY)
configure_file(systemd/linsightd.default.in
               ${CMAKE_CURRENT_BINARY_DIR}/linsightd.default @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/linsightd.service
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/linsightd.default
        DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/default RENAME linsightd)
