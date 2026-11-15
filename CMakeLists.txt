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
# the bridge never records).
add_library(linsight_core STATIC
    src/session.c
    ${CMAKE_CURRENT_SOURCE_DIR}/protocol/frame.c)
target_include_directories(linsight_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/protocol)

# linsightd: single-threaded epoll bridge (paced vcan forward + --dump)
add_executable(linsightd src/main.c src/pace.c)
target_link_libraries(linsightd PRIVATE linsight_core rt)

# Install: bridge binary + systemd units + prefix etc/default args. The vcan0
# oneshot unit creates the virtual CAN interface before linsightd starts.
include(GNUInstallDirs)
install(TARGETS linsightd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
configure_file(systemd/linsightd.service.in
               ${CMAKE_CURRENT_BINARY_DIR}/linsightd.service @ONLY)
configure_file(systemd/linsightd.default.in
               ${CMAKE_CURRENT_BINARY_DIR}/linsightd.default @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/linsightd.service
        systemd/vcan0.service
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/linsightd.default
        DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/default RENAME linsightd)
