# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2024 hikaps

# CouchPlay privileged helper daemon

add_executable(couchplay-helper
    main.cpp
    CouchPlayHelper.cpp
    CouchPlayHelper.h
    SystemOps.cpp
    SystemOps.h
    PolkitActions.h
    )

target_link_libraries(couchplay-helper PRIVATE
    Qt6::Core
    Qt6::DBus
)

if(PolkitQt6-1_FOUND)
    target_link_libraries(couchplay-helper PRIVATE PolkitQt6-1::Core)
    target_compile_definitions(couchplay-helper PRIVATE HAVE_POLKITQT)
endif()

target_include_directories(couchplay-helper PRIVATE
    ${PROJECT_BINARY_DIR}
)

# Install to libexec
install(TARGETS couchplay-helper
    DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}
)
