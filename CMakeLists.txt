# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2024 hikaps

cmake_minimum_required(VERSION 3.20)

project(couchplay VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find ECM (Extra CMake Modules)
find_package(ECM 6.0.0 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

# Include KDE CMake settings
include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(ECMFindQmlModule)
include(ECMQmlModule)
include(ECMSetupVersion)
include(FeatureSummary)

# Setup version
ecm_setup_version(${PROJECT_VERSION}
    VARIABLE_PREFIX COUCHPLAY
    VERSION_HEADER ${CMAKE_CURRENT_BINARY_DIR}/couchplay-version.h
)

# Find Qt6
find_package(Qt6 6.5 REQUIRED COMPONENTS
    Core
    Quick
    Qml
    Gui
    QuickControls2
    Widgets
    DBus
)

# Find KDE Frameworks 6
find_package(KF6 6.0 REQUIRED COMPONENTS
    Kirigami
    I18n
    CoreAddons
    Config
    IconThemes
    QQC2DesktopStyle
)

# Find QML modules at runtime
ecm_find_qmlmodule(org.kde.kirigami REQUIRED)

# Add subdirectories
add_subdirectory(src)
add_subdirectory(helper)

# Enable testing
option(BUILD_TESTING "Build the testing tree." ON)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# Install desktop file
install(PROGRAMS io.github.hikaps.couchplay.desktop
    DESTINATION ${KDE_INSTALL_APPDIR}
)

# Install metainfo
install(FILES io.github.hikaps.couchplay.metainfo.xml
    DESTINATION ${KDE_INSTALL_METAINFODIR}
)

# Install icons
install(FILES data/icons/couchplay.svg
    DESTINATION ${KDE_INSTALL_ICONDIR}/hicolor/scalable/apps
    RENAME io.github.hikaps.couchplay.svg
)

# Install polkit policy
install(FILES data/polkit/io.github.hikaps.couchplay.policy
    DESTINATION ${CMAKE_INSTALL_PREFIX}/share/polkit-1/actions
)

# Install D-Bus service files
install(FILES data/dbus/io.github.hikaps.CouchPlayHelper.service
    DESTINATION ${CMAKE_INSTALL_PREFIX}/share/dbus-1/system-services
)

install(FILES data/dbus/io.github.hikaps.CouchPlayHelper.conf
    DESTINATION ${CMAKE_INSTALL_PREFIX}/share/dbus-1/system.d
)

# Summary
feature_summary(WHAT ALL INCLUDE_QUIET_PACKAGES FATAL_ON_MISSING_REQUIRED_PACKAGES)
