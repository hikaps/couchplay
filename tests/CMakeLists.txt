# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2025 CouchPlay Contributors

# Enable testing
enable_testing()

# Find Qt6 Test
find_package(Qt6 6.5 REQUIRED COMPONENTS Test)

# Include directories for tests
set(TEST_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/dbus
    ${CMAKE_CURRENT_BINARY_DIR}
)

# Common manager sources (for tests)
set(TEST_MANAGER_SOURCES
    ../src/core/DeviceManager.cpp
    ../src/core/DeviceManager.h
    ../src/core/SessionManager.cpp
    ../src/core/SessionManager.h
    ../src/core/SessionRunner.cpp
    ../src/core/SessionRunner.h
    ../src/core/GamescopeInstance.cpp
    ../src/core/GamescopeInstance.h
    ../src/core/UserManager.cpp
    ../src/core/UserManager.h
    ../src/core/MonitorManager.cpp
    ../src/core/MonitorManager.h
    ../src/core/GameLibrary.cpp
    ../src/core/GameLibrary.h
    ../src/core/AudioManager.cpp
    ../src/core/AudioManager.h
    ../src/core/WindowManager.cpp
    ../src/core/WindowManager.h
    ../src/core/PresetManager.cpp
    ../src/core/PresetManager.h
     ../src/core/HeroicConfigManager.cpp
     ../src/core/HeroicConfigManager.h
     ../src/core/SteamConfigManager.cpp
     ../src/core/SteamConfigManager.h
    ../src/core/SettingsManager.cpp
    ../src/core/SettingsManager.h
    ../src/core/Logging.cpp
    ../src/core/Logging.h
    ../src/core/CommandVerifier.cpp
    ../src/core/CommandVerifier.h
    ../src/dbus/CouchPlayHelperClient.cpp
    ../src/dbus/CouchPlayHelperClient.h
)

# CouchPlayHelper sources for helper tests
set(HELPER_TEST_SOURCES
    ../helper/CouchPlayHelper.cpp
    ../helper/CouchPlayHelper.h
    ../helper/SystemOps.cpp
    ../helper/SystemOps.h
)

# Function to create a test executable
function(add_couchplay_test TEST_NAME)
    add_executable(${TEST_NAME}
        ${TEST_NAME}.cpp
        ${TEST_MANAGER_SOURCES}
    )

    target_link_libraries(${TEST_NAME} PRIVATE
        Qt6::Core
        Qt6::Quick
        Qt6::Qml
        Qt6::Gui
        Qt6::QuickControls2
        Qt6::Widgets
        Qt6::DBus
        Qt6::Test
        KF6::I18n
        KF6::CoreAddons
        KF6::ConfigCore
        KF6::ConfigGui
        KF6::IconThemes
        KF6::GlobalAccel
    )

    target_include_directories(${TEST_NAME} PRIVATE ${TEST_INCLUDE_DIRS})

    # Enable automoc for QtTest
    set_target_properties(${TEST_NAME} PROPERTIES
        AUTOMOC ON
        AUTORCC ON
    )

    # Register with CTest
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endfunction()

# Function to create helper test executable (needs helper sources)
function(add_helper_test TEST_NAME)
    add_executable(${TEST_NAME}
        ${TEST_NAME}.cpp
        ${HELPER_TEST_SOURCES}
    )

    target_link_libraries(${TEST_NAME} PRIVATE
        Qt6::Core
        Qt6::DBus
        Qt6::Test
    )

    target_include_directories(${TEST_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../helper
    )

    # Enable automoc for QtTest
    set_target_properties(${TEST_NAME} PROPERTIES
        AUTOMOC ON
        AUTORCC ON
    )

    # Register with CTest
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endfunction()

# Add test executables
add_couchplay_test(test_commandverifier)
add_couchplay_test(test_devicemanager)
add_couchplay_test(test_gamelibrary)
add_couchplay_test(test_gamescopeinstance)
add_couchplay_test(test_heroicconfigmanager)
add_couchplay_test(test_monitormanager)
add_couchplay_test(test_presetmanager)
add_couchplay_test(test_presetmanager_integration)
add_couchplay_test(test_scanapplications)
add_couchplay_test(test_sessionmanager)
add_couchplay_test(test_sessionrunner)
add_couchplay_test(test_usermanager)

# Add helper tests
add_helper_test(test_couchplayhelper)
