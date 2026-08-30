cmake_minimum_required(VERSION 3.10)

project(doubletap VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
pkg_check_modules(LIBEVDEV REQUIRED libevdev)
pkg_check_modules(LIBYAML  REQUIRED yaml-0.1)

# Installation directories for the systemd user unit and udev rule. Default
# to what the host's systemd/udev report; override with -D for packaging.
if(NOT SYSTEMD_USER_UNIT_DIR)
    pkg_get_variable(SYSTEMD_USER_UNIT_DIR systemd systemduserunitdir)
    if(NOT SYSTEMD_USER_UNIT_DIR)
        set(SYSTEMD_USER_UNIT_DIR "${CMAKE_INSTALL_LIBDIR}/systemd/user")
    endif()
endif()
if(NOT UDEV_RULES_DIR)
    pkg_get_variable(UDEV_DIR udev udevdir)
    if(UDEV_DIR)
        set(UDEV_RULES_DIR "${UDEV_DIR}/rules.d")
    else()
        set(UDEV_RULES_DIR "${CMAKE_INSTALL_LIBDIR}/udev/rules.d")
    endif()
endif()

add_executable(doubletapd doubletapd.c)

target_compile_definitions(doubletapd PRIVATE
    DEF_WAV="${CMAKE_INSTALL_FULL_DATADIR}/doubletap/click.wav"
    DEF_CONFIG="${CMAKE_INSTALL_FULL_DATADIR}/doubletap/config.yaml"
)

target_compile_options(doubletapd PRIVATE
    -Wall -Wextra
    ${PIPEWIRE_CFLAGS_OTHER}
    ${LIBEVDEV_CFLAGS_OTHER}
    ${LIBYAML_CFLAGS_OTHER}
)

target_include_directories(doubletapd PRIVATE
    ${PIPEWIRE_INCLUDE_DIRS}
    ${LIBEVDEV_INCLUDE_DIRS}
    ${LIBYAML_INCLUDE_DIRS}
)

target_link_directories(doubletapd PRIVATE
    ${PIPEWIRE_LIBRARY_DIRS}
    ${LIBEVDEV_LIBRARY_DIRS}
    ${LIBYAML_LIBRARY_DIRS}
)

target_link_libraries(doubletapd PRIVATE
    ${PIPEWIRE_LIBRARIES}
    ${LIBEVDEV_LIBRARIES}
    ${LIBYAML_LIBRARIES}
)

# Developer tool, not installed: replays a `doubletapd -T` trace through the
# real analog state machine offline. It #includes doubletapd.c and stubs the
# uinput writes, so it exercises the daemon's own code rather than a copy.
# EXCLUDE_FROM_ALL - build it on demand with `--target replay`.
add_executable(replay EXCLUDE_FROM_ALL tools/replay.c)

target_compile_definitions(replay PRIVATE
    DEF_WAV="${CMAKE_INSTALL_FULL_DATADIR}/doubletap/click.wav"
    DEF_CONFIG="${CMAKE_INSTALL_FULL_DATADIR}/doubletap/config.yaml"
)

target_compile_options(replay PRIVATE
    -Wall -Wextra
    ${PIPEWIRE_CFLAGS_OTHER}
    ${LIBEVDEV_CFLAGS_OTHER}
    ${LIBYAML_CFLAGS_OTHER}
)

target_include_directories(replay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PIPEWIRE_INCLUDE_DIRS}
    ${LIBEVDEV_INCLUDE_DIRS}
    ${LIBYAML_INCLUDE_DIRS}
)

target_link_directories(replay PRIVATE
    ${PIPEWIRE_LIBRARY_DIRS}
    ${LIBEVDEV_LIBRARY_DIRS}
    ${LIBYAML_LIBRARY_DIRS}
)

target_link_libraries(replay PRIVATE
    ${PIPEWIRE_LIBRARIES}
    ${LIBEVDEV_LIBRARIES}
    ${LIBYAML_LIBRARIES}
)

configure_file(doubletap.service.in doubletap.service @ONLY)

install(TARGETS doubletapd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES   config.yaml             DESTINATION ${CMAKE_INSTALL_DATADIR}/doubletap)
install(FILES   click.wav               DESTINATION ${CMAKE_INSTALL_DATADIR}/doubletap)
install(FILES   ${CMAKE_CURRENT_BINARY_DIR}/doubletap.service
                                        DESTINATION ${SYSTEMD_USER_UNIT_DIR})
install(FILES   70-doubletap-uinput.rules
                                        DESTINATION ${UDEV_RULES_DIR})
