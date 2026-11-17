# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 hikaps

# couchplay-padfwd: host-side virtual Xbox 360 pad per player. Runs as the
# desktop user, libc only. Linked statically when possible so it runs on any
# host regardless of its glibc version.
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_LINK_OPTIONS -static)
check_c_source_compiles("int main(void) { return 0; }" COUCHPLAY_PADFWD_CAN_LINK_STATIC)
unset(CMAKE_REQUIRED_LINK_OPTIONS)

add_executable(couchplay-padfwd couchplay-padfwd.c padmap.c)
target_compile_options(couchplay-padfwd PRIVATE -Wall -Wextra)
# Pure C: keep Qt's global AUTOMOC off it. It adds a C++ file that pulls -lstdc++/-lm into the link, which breaks
# -static where libm.a is missing (Flatpak SDK).
set_target_properties(couchplay-padfwd PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF LINKER_LANGUAGE C)
if(COUCHPLAY_PADFWD_CAN_LINK_STATIC)
    target_link_options(couchplay-padfwd PRIVATE -static)
endif()

install(TARGETS couchplay-padfwd
    DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}
)
