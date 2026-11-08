# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 hikaps

# couchplay-vdisplay: holds one KWin virtual output open. Runs as the desktop
# user (not the root helper), so it links only libwayland-client.
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_get_variable(WAYLAND_SCANNER wayland-scanner wayland_scanner)
if(NOT WAYLAND_SCANNER)
    find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
endif()

set(VDISPLAY_PROTOCOL ${CMAKE_CURRENT_SOURCE_DIR}/zkde-screencast-unstable-v1.xml)
set(VDISPLAY_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR})

add_custom_command(
    OUTPUT ${VDISPLAY_GEN_DIR}/zkde-screencast-unstable-v1-client-protocol.h
    COMMAND ${WAYLAND_SCANNER} client-header ${VDISPLAY_PROTOCOL}
            ${VDISPLAY_GEN_DIR}/zkde-screencast-unstable-v1-client-protocol.h
    DEPENDS ${VDISPLAY_PROTOCOL}
    VERBATIM
)
add_custom_command(
    OUTPUT ${VDISPLAY_GEN_DIR}/zkde-screencast-unstable-v1-protocol.c
    COMMAND ${WAYLAND_SCANNER} private-code ${VDISPLAY_PROTOCOL}
            ${VDISPLAY_GEN_DIR}/zkde-screencast-unstable-v1-protocol.c
    DEPENDS ${VDISPLAY_PROTOCOL}
    VERBATIM
)

add_executable(couchplay-vdisplay
    couchplay-vdisplay.c
    ${VDISPLAY_GEN_DIR}/zkde-screencast-unstable-v1-protocol.c
    ${VDISPLAY_GEN_DIR}/zkde-screencast-unstable-v1-client-protocol.h
)
target_include_directories(couchplay-vdisplay PRIVATE ${VDISPLAY_GEN_DIR})
target_link_libraries(couchplay-vdisplay PRIVATE PkgConfig::WAYLAND_CLIENT)

install(TARGETS couchplay-vdisplay
    DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}
)
