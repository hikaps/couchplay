# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2024 hikaps

# Create a static library for non-QML sources (used by tests)
add_library(couchplay_core STATIC
    core/CommandVerifier.cpp
    core/CommandVerifier.h
    core/Logging.cpp
    core/Logging.h
)

# Link libraries for core library
target_link_libraries(couchplay_core PUBLIC
    Qt6::Core
    Qt6::Quick
    Qt6::Qml
    Qt6::Gui
    Qt6::QuickControls2
    Qt6::Widgets
    Qt6::DBus
    KF6::I18n
    KF6::CoreAddons
    KF6::ConfigCore
    KF6::ConfigGui
    KF6::IconThemes
    KF6::GlobalAccel
)

# Include directories for core library
target_include_directories(couchplay_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/dbus
    ${CMAKE_CURRENT_BINARY_DIR}
)

# Enable AUTOMOC for core library
set_target_properties(couchplay_core PROPERTIES
    AUTOMOC ON
    AUTORCC ON
)

# Create the executable
add_executable(couchplay)

# Create QML module
ecm_add_qml_module(couchplay
    URI io.github.hikaps.couchplay
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/pages/HomePage.qml
        qml/pages/SessionSetupPage.qml
        qml/pages/DeviceAssignmentPage.qml
        qml/pages/LayoutPage.qml
        qml/pages/ProfilesPage.qml
        qml/pages/GamesPage.qml
        qml/pages/UsersPage.qml
        qml/pages/SettingsPage.qml
        qml/components/PresetSelector.qml
        qml/components/SelectableCard.qml
        qml/components/InfoCard.qml
        qml/components/ActionCard.qml
        qml/components/dialogs/ResetSettingsDialog.qml
        qml/components/dialogs/InstallHelperDialog.qml
        qml/components/dialogs/EditPresetDialog.qml
        qml/components/dialogs/DeletePresetDialog.qml
        qml/components/dialogs/AddPresetDialog.qml
)

# C++ sources for main executable (includes QML_ELEMENT classes)
target_sources(couchplay PRIVATE
    main.cpp
    core/DeviceManager.cpp
    core/DeviceManager.h
    core/SessionManager.cpp
    core/SessionManager.h
    core/SessionRunner.cpp
    core/SessionRunner.h
    core/GamescopeInstance.cpp
    core/GamescopeInstance.h
    core/UserManager.cpp
    core/UserManager.h
    core/MonitorManager.cpp
    core/MonitorManager.h
    core/GameLibrary.cpp
    core/GameLibrary.h
    core/AudioManager.cpp
    core/AudioManager.h
    core/WindowManager.cpp
    core/WindowManager.h
     core/PresetManager.cpp
     core/PresetManager.h
     core/HeroicConfigManager.cpp
     core/HeroicConfigManager.h
     core/SteamConfigManager.cpp
     core/SteamConfigManager.h
    core/SettingsManager.cpp
    core/SettingsManager.h
    core/AppImageHelper.cpp
    core/AppImageHelper.h
    dbus/CouchPlayHelperClient.cpp
    dbus/CouchPlayHelperClient.h
    ../resources/appimage.qrc
)

# Link against core library
target_link_libraries(couchplay PRIVATE
    couchplay_core
)
