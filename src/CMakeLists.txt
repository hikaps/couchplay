# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2024 hikaps

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
        qml/components/DeviceCard.qml
        qml/components/InstanceCard.qml
        qml/components/LayoutPreview.qml
        qml/components/MonitorSelector.qml
        qml/components/GameCard.qml
        qml/components/PresetSelector.qml
)

# C++ sources
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
    dbus/CouchPlayHelperClient.cpp
    dbus/CouchPlayHelperClient.h
)

# Link libraries
target_link_libraries(couchplay PRIVATE
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

# Include directories
target_include_directories(couchplay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/dbus
    ${CMAKE_CURRENT_BINARY_DIR}
)

# Install
install(TARGETS couchplay ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
