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
    ${CMAKE_CURRENT_BINARY_DIR}
    ${PROJECT_BINARY_DIR}
)

# Common manager sources (for tests)
set(TEST_MANAGER_SOURCES
    ../src/core/DeviceManager.cpp
    ../src/core/DeviceManager.h
    ../src/core/SessionManager.cpp
    ../src/core/SessionManager.h
    ../src/core/SessionRunner.cpp
    ../src/core/SessionRunner.h
    ../src/core/LaunchTypes.h
    ../src/core/CommandLineOptions.cpp
    ../src/core/CommandLineOptions.h
    ../src/core/SignalBridge.cpp
    ../src/core/SignalBridge.h
    ../src/core/GamescopeInstance.cpp
    ../src/core/GamescopeInstance.h
    ../src/core/MonitorManager.cpp
    ../src/core/MonitorManager.h
    ../src/core/AudioManager.cpp
    ../src/core/AudioManager.h
    ../src/core/WindowManager.cpp
    ../src/core/WindowManager.h
    ../src/core/PresetManager.cpp
    ../src/core/PresetManager.h
    ../src/core/SteamCompatToolMapping.cpp
    ../src/core/SteamCompatToolMapping.h
    ../src/core/HeroicConfigManager.cpp
    ../src/core/HeroicConfigManager.h
    ../src/core/SteamConfigManager.cpp
    ../src/core/SteamConfigManager.h
    ../src/core/SteamShortcutManager.cpp
    ../src/core/SteamShortcutManager.h
    ../src/core/SteamAccountDiscovery.cpp
    ../src/core/SteamAccountDiscovery.h
    ../src/core/SteamShortcutsVdf.cpp
    ../src/core/SteamShortcutsVdf.h
    ../src/core/SettingsManager.cpp
    ../src/core/SettingsManager.h
    ../src/core/Logging.cpp
    ../src/core/Logging.h
    ../src/core/CommandVerifier.cpp
    ../src/core/CommandVerifier.h

    ../src/core/SunshineConfig.cpp
    ../src/core/SunshineConfig.h
    ../src/core/StreamManager.cpp
    ../src/core/StreamManager.h
    ../src/core/SunshineApiClient.cpp
    ../src/core/SunshineApiClient.h
    ../src/core/HostProcess.cpp
    ../src/core/HostProcess.h
    ../src/core/VirtualDisplayManager.cpp
    ../src/core/VirtualDisplayManager.h
    ../src/core/PairingSession.cpp
    ../src/core/PairingSession.h
    ../src/core/StreamAudio.cpp
    ../src/core/StreamAudio.h
    ../src/core/UpdateChecker.cpp
    ../src/core/UpdateChecker.h
    ../src/core/PlayerStore.cpp
    ../src/core/PlayerStore.h
    ../src/core/HostStatus.cpp
    ../src/core/HostStatus.h
    ../src/core/PlayerFiles.cpp
    ../src/core/PlayerFiles.h
    ../src/core/SandboxCommand.cpp
    ../src/core/SandboxCommand.h
    ../src/core/PadForwarder.cpp
    ../src/core/PadForwarder.h
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
        Qt6::Network
        Qt6::Test
        KF6::I18n
        KF6::CoreAddons
        KF6::ConfigCore
        KF6::ConfigGui
        KF6::DBusAddons
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



add_couchplay_test(test_commandlineoptions)
add_couchplay_test(test_signalbridge)
add_couchplay_test(test_logredactor)
# Add test executables
add_couchplay_test(test_steamshortcutmanager)
add_couchplay_test(test_steamregistration)
add_couchplay_test(test_steamconfigmanager)
add_couchplay_test(test_audiomanager)
add_couchplay_test(test_commandverifier)
add_couchplay_test(test_devicemanager)
add_couchplay_test(test_gamescopeinstance)
add_couchplay_test(test_heroicconfigmanager)
add_couchplay_test(test_monitormanager)
# A fractional scale makes QScreen::size() (logical) differ from the physical size, which the test checks.
set_tests_properties(test_monitormanager PROPERTIES ENVIRONMENT "QT_SCALE_FACTOR=2.5")
add_couchplay_test(test_presetmanager)
add_couchplay_test(test_windowmanager)
add_couchplay_test(test_presetmanager_integration)
add_couchplay_test(test_scanapplications)
add_couchplay_test(test_sessionmanager)
add_couchplay_test(test_sessionrunner)
add_couchplay_test(test_streamaudio)
add_couchplay_test(test_streaming_config)
add_couchplay_test(test_streaming_session)
add_couchplay_test(test_pairingsession)
add_couchplay_test(test_streammanager)
add_couchplay_test(test_sunshine_api)
target_compile_definitions(test_sunshine_api PRIVATE TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_couchplay_test(test_sunshine_config)
add_couchplay_test(test_steamcompattoolmapping)
add_couchplay_test(test_updatechecker)
add_couchplay_test(test_virtualdisplaymanager)
add_couchplay_test(test_sandboxcommand)
add_couchplay_test(test_playerstore)
add_couchplay_test(test_playerfiles)
add_couchplay_test(test_hoststatus)
add_couchplay_test(test_logging)

# couchplay-padfwd's pure translation/chord logic (C), tested without uinput
add_executable(test_padmap
    test_padmap.cpp
    ../tools/padfwd/padmap.c
)
target_link_libraries(test_padmap PRIVATE Qt6::Core Qt6::Test)
target_include_directories(test_padmap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../tools/padfwd)
set_target_properties(test_padmap PROPERTIES AUTOMOC ON)
add_test(NAME test_padmap COMMAND test_padmap)
