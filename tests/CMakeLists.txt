# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors

# Find Qt Test and Qml (needed for QML integration headers)
find_package(Qt6 REQUIRED COMPONENTS Test Qml DBus)

# Enable testing
enable_testing()

# DeviceManager tests
add_executable(test_devicemanager
    test_devicemanager.cpp
    ${CMAKE_SOURCE_DIR}/src/core/DeviceManager.cpp
    ${CMAKE_SOURCE_DIR}/src/core/DeviceManager.h
)

target_include_directories(test_devicemanager PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_BINARY_DIR}/src
)

target_link_libraries(test_devicemanager PRIVATE
    Qt6::Core
    Qt6::Test
    Qt6::Qml
)

add_test(NAME DeviceManagerTest COMMAND test_devicemanager)

# SessionManager tests
add_executable(test_sessionmanager
    test_sessionmanager.cpp
    ${CMAKE_SOURCE_DIR}/src/core/SessionManager.cpp
    ${CMAKE_SOURCE_DIR}/src/core/SessionManager.h
)

target_include_directories(test_sessionmanager PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_BINARY_DIR}/src
)

target_link_libraries(test_sessionmanager PRIVATE
    Qt6::Core
    Qt6::Test
    Qt6::Qml
    KF6::ConfigCore
    KF6::ConfigGui
)

add_test(NAME SessionManagerTest COMMAND test_sessionmanager)

# GameLibrary tests
add_executable(test_gamelibrary
    test_gamelibrary.cpp
    ${CMAKE_SOURCE_DIR}/src/core/GameLibrary.cpp
    ${CMAKE_SOURCE_DIR}/src/core/GameLibrary.h
)

target_include_directories(test_gamelibrary PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_BINARY_DIR}/src
)

target_link_libraries(test_gamelibrary PRIVATE
    Qt6::Core
    Qt6::Test
    Qt6::Qml
)

add_test(NAME GameLibraryTest COMMAND test_gamelibrary)

# UserManager tests
add_executable(test_usermanager
    test_usermanager.cpp
    ${CMAKE_SOURCE_DIR}/src/core/UserManager.cpp
    ${CMAKE_SOURCE_DIR}/src/core/UserManager.h
)

target_include_directories(test_usermanager PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_BINARY_DIR}/src
)

target_link_libraries(test_usermanager PRIVATE
    Qt6::Core
    Qt6::Test
    Qt6::Qml
)

add_test(NAME UserManagerTest COMMAND test_usermanager)

# GamescopeInstance tests
add_executable(test_gamescopeinstance
    test_gamescopeinstance.cpp
    ${CMAKE_SOURCE_DIR}/src/core/GamescopeInstance.cpp
    ${CMAKE_SOURCE_DIR}/src/core/GamescopeInstance.h
)

target_include_directories(test_gamescopeinstance PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_BINARY_DIR}/src
)

target_link_libraries(test_gamescopeinstance PRIVATE
    Qt6::Core
    Qt6::Test
    Qt6::Qml
    Qt6::DBus
)

add_test(NAME GamescopeInstanceTest COMMAND test_gamescopeinstance)

# MonitorManager tests
add_executable(test_monitormanager
    test_monitormanager.cpp
    ${CMAKE_SOURCE_DIR}/src/core/MonitorManager.cpp
    ${CMAKE_SOURCE_DIR}/src/core/MonitorManager.h
)

target_include_directories(test_monitormanager PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_BINARY_DIR}/src
)

target_link_libraries(test_monitormanager PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    Qt6::Qml
)

add_test(NAME MonitorManagerTest COMMAND test_monitormanager)

# PresetManager tests
add_executable(test_presetmanager
    test_presetmanager.cpp
    ${CMAKE_SOURCE_DIR}/src/core/PresetManager.cpp
    ${CMAKE_SOURCE_DIR}/src/core/PresetManager.h
)

target_include_directories(test_presetmanager PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_BINARY_DIR}/src
)

target_link_libraries(test_presetmanager PRIVATE
    Qt6::Core
    Qt6::Test
    Qt6::Qml
)

add_test(NAME PresetManagerTest COMMAND test_presetmanager)
