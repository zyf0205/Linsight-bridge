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

# linsightd: single-threaded epoll SocketCAN bridge + shared v2.1 parser
add_executable(linsightd
    src/main.c
    protocol/frame.c)
target_include_directories(linsightd PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/protocol)

# Install: bridge binary + systemd units + prefix etc/default args.
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
