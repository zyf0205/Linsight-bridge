cmake_minimum_required(VERSION 3.16)
project(linsight-host C)

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()
add_compile_options(-Wall -Wextra)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../protocol)

# linsightd: epoll daemon, .lsc writer thread, Unix-socket control
add_executable(linsightd
    src/main.c
    src/canlog.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/frame.c)
target_link_libraries(linsightd pthread rt)

# linsight-cli: stats / live / export via control socket and .lsc files
add_executable(linsight-cli
    cli/cli.c
    src/canlog.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/frame.c)
target_include_directories(linsight-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

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
add_test(NAME record COMMAND test_record)
add_test(NAME parser COMMAND test_parser)
add_test(NAME fuzz COMMAND test_fuzz)
add_test(NAME ring COMMAND test_ring)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME integration
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py
                     $<TARGET_FILE:linsightd> $<TARGET_FILE:linsight-cli>)
endif()
