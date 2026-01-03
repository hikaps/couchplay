# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2024 hikaps

# CouchPlay privileged helper daemon

add_executable(couchplay-helper
    main.cpp
    CouchPlayHelper.cpp
    CouchPlayHelper.h
)

target_link_libraries(couchplay-helper PRIVATE
    Qt6::Core
    Qt6::DBus
)

# Install to libexec
install(TARGETS couchplay-helper
    DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}
)
