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

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../protocol)

# linsightd: epoll daemon, .lsc writer thread, Unix-socket control
add_executable(linsightd
    src/main.c
    src/canlog.c
    src/session.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/frame.c)
target_link_libraries(linsightd pthread rt)

# linsight-cli: stats / live / export via control socket and .lsc files
add_executable(linsight-cli
    cli/cli.c
    src/canlog.c
    src/compare.c
    src/dbc.c
    src/replay.c
    src/session.c
    src/import.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/frame.c)
target_include_directories(linsight-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(linsight-cli rt)

enable_testing()
add_executable(test_record tests/test_record.c ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/frame.c)
add_executable(test_parser tests/test_parser.c ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/frame.c)
add_executable(test_fuzz tests/test_fuzz.c ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/frame.c)
# Firmware ring compiled against tiny ESP-IDF shims: verifies drop-oldest order.
add_executable(test_ring tests/test_ring.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/can_ring.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/frame.c)
target_include_directories(test_ring PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/shims
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware)
add_executable(test_canlog tests/test_canlog.c src/canlog.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/frame.c)
target_include_directories(test_canlog PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_executable(test_compare tests/test_compare.c src/canlog.c src/compare.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/frame.c)
target_include_directories(test_compare PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_executable(test_dbc tests/test_dbc.c src/dbc.c)
target_include_directories(test_dbc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_executable(test_session tests/test_session.c src/session.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/frame.c)
target_include_directories(test_session PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_executable(test_export tests/test_export.c src/canlog.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/frame.c)
target_include_directories(test_export PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_executable(test_import tests/test_import.c src/import.c src/session.c src/canlog.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/frame.c)
target_include_directories(test_import PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
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
