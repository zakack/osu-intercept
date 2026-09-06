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
pkg_check_modules(SAMPLERATE REQUIRED samplerate)
pkg_check_modules(SNDFILE    REQUIRED sndfile)

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
    ${SAMPLERATE_CFLAGS_OTHER}
    ${SNDFILE_CFLAGS_OTHER}
)

target_include_directories(doubletapd PRIVATE
    ${PIPEWIRE_INCLUDE_DIRS}
    ${LIBEVDEV_INCLUDE_DIRS}
    ${LIBYAML_INCLUDE_DIRS}
    ${SAMPLERATE_INCLUDE_DIRS}
    ${SNDFILE_INCLUDE_DIRS}
)

target_link_directories(doubletapd PRIVATE
    ${PIPEWIRE_LIBRARY_DIRS}
    ${LIBEVDEV_LIBRARY_DIRS}
    ${LIBYAML_LIBRARY_DIRS}
    ${SAMPLERATE_LIBRARY_DIRS}
    ${SNDFILE_LIBRARY_DIRS}
)

target_link_libraries(doubletapd PRIVATE
    ${PIPEWIRE_LIBRARIES}
    ${LIBEVDEV_LIBRARIES}
    ${LIBYAML_LIBRARIES}
    ${SAMPLERATE_LIBRARIES}
    ${SNDFILE_LIBRARIES}
)

# Developer tools. Not installed, but deliberately part of the default build:
# each #includes doubletapd.c so it exercises the daemon's own state machine
# rather than a copy, and that guarantee is worthless if the binary can go
# stale. EXCLUDE_FROM_ALL let `cmake --build build` skip them silently, which
# produced a `replay` validating configs against rules the daemon no longer
# had. A few seconds of build time is the cheaper mistake.
#   ./build/replay TRACE.csv   replay a `doubletapd -T` recording
#   ./build/regimetest         assertions over the regime
#   ./build/mixtest            assertions over the audio voice pool
foreach(tool replay regimetest mixtest)
    add_executable(${tool} tools/${tool}.c)

    target_compile_definitions(${tool} PRIVATE
        DEF_WAV="${CMAKE_INSTALL_FULL_DATADIR}/doubletap/click.wav"
        DEF_CONFIG="${CMAKE_INSTALL_FULL_DATADIR}/doubletap/config.yaml"
    )
    target_compile_options(${tool} PRIVATE
        -Wall -Wextra
        ${PIPEWIRE_CFLAGS_OTHER} ${LIBEVDEV_CFLAGS_OTHER} ${LIBYAML_CFLAGS_OTHER}
        ${SAMPLERATE_CFLAGS_OTHER} ${SNDFILE_CFLAGS_OTHER}
    )
    target_include_directories(${tool} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PIPEWIRE_INCLUDE_DIRS} ${LIBEVDEV_INCLUDE_DIRS} ${LIBYAML_INCLUDE_DIRS}
        ${SAMPLERATE_INCLUDE_DIRS} ${SNDFILE_INCLUDE_DIRS}
    )
    target_link_directories(${tool} PRIVATE
        ${PIPEWIRE_LIBRARY_DIRS} ${LIBEVDEV_LIBRARY_DIRS} ${LIBYAML_LIBRARY_DIRS}
        ${SAMPLERATE_LIBRARY_DIRS} ${SNDFILE_LIBRARY_DIRS}
    )
    target_link_libraries(${tool} PRIVATE
        ${PIPEWIRE_LIBRARIES} ${LIBEVDEV_LIBRARIES} ${LIBYAML_LIBRARIES}
        ${SAMPLERATE_LIBRARIES} ${SNDFILE_LIBRARIES}
    )
endforeach()

configure_file(doubletap.service.in doubletap.service @ONLY)

install(TARGETS doubletapd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES   config.yaml             DESTINATION ${CMAKE_INSTALL_DATADIR}/doubletap)
install(FILES   click.wav               DESTINATION ${CMAKE_INSTALL_DATADIR}/doubletap)
install(FILES   ${CMAKE_CURRENT_BINARY_DIR}/doubletap.service
                                        DESTINATION ${SYSTEMD_USER_UNIT_DIR})
install(FILES   70-doubletap-uinput.rules
                                        DESTINATION ${UDEV_RULES_DIR})
