// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami
import io.github.hikaps.couchplay 1.0

import "pages"

Kirigami.ApplicationWindow {
    id: root

    title: i18nc("@title:window", "CouchPlay")
    minimumWidth: Kirigami.Units.gridUnit * 40
    minimumHeight: Kirigami.Units.gridUnit * 30

    property string startupProfileName: ""
    property bool startupStart: false
    property bool startupExitAfterSession: false
    property bool initialRequestPending: false
    property bool exitOnStartupFailure: false
    property bool exitAfterActiveSession: false
    signal startupFailed(int exitCode)

    function handleLaunchRequest(profileName, start, exitAfterSession) {
        if (!profileName || profileName === "" || sessionRunner.active) {
            return false
        }
        if (!sessionManager.loadProfile(profileName)) {
            if (initialRequestPending) startupFailed(1)
            return false
        }
        if (!start) {
            pushSessionSetupPage()
            return true
        }
        exitOnStartupFailure = initialRequestPending
        exitAfterActiveSession = exitAfterSession
        if (!sessionRunner.start()) {
            exitOnStartupFailure = false
            exitAfterActiveSession = false
            if (initialRequestPending) startupFailed(1)
            return false
        }
        return true
    }

    Component.onCompleted: {
        if (startupProfileName !== "") {
            initialRequestPending = true
            Qt.callLater(function() {
                handleLaunchRequest(startupProfileName, startupStart, startupExitAfterSession)
                initialRequestPending = false
            })
        }
    }

    SettingsManager {
        id: settingsManager
    }
    Connections {
        target: commandLineBridge
        function onLaunchRequested(profileName, start, exitAfterSession) {
            commandLineBridge.setRequestAccepted(root.handleLaunchRequest(profileName, start, exitAfterSession))
        }
    }


    DeviceManager {
        id: deviceManager
        settingsManager: settingsManager
        
        onDeviceAssigned: function(eventNumber, instanceIndex, previousInstanceIndex) {
            if (sessionManager) {
                if (instanceIndex >= 0) {
                    let stableIds = deviceManager.getStableIdsForInstance(instanceIndex)
                    let names = deviceManager.getDeviceNamesForInstance(instanceIndex)
                    sessionManager.setInstanceDeviceStableIds(instanceIndex, stableIds, names)
                }
                
                if (previousInstanceIndex >= 0 && previousInstanceIndex !== instanceIndex) {
                    let stableIds = deviceManager.getStableIdsForInstance(previousInstanceIndex)
                    let names = deviceManager.getDeviceNamesForInstance(previousInstanceIndex)
                    sessionManager.setInstanceDeviceStableIds(previousInstanceIndex, stableIds, names)
                }
            }
        }
        
        onDeviceAutoRestored: function(name, instanceIndex) {
            applicationWindow().showPassiveNotification(
                i18nc("@info", "%1 reconnected to Player %2", name, instanceIndex + 1))
        }
    }

    SessionManager {
        id: sessionManager
        presetManager: presetManager
        helperClient: helperClient

        onProfileLoaded: function(deviceInfoByInstance) {
            if (deviceManager) {
                deviceManager.unassignAll()
                
                for (let instanceStr in deviceInfoByInstance) {
                    let instanceIndex = parseInt(instanceStr)
                    let info = deviceInfoByInstance[instanceStr]
                    let stableIds = info.stableIds || []
                    let names = info.names || []
                    if (stableIds.length > 0) {
                        deviceManager.restoreAssignmentsFromStableIds(instanceIndex, stableIds, names)
                    }
                }
            }
        }
        onErrorOccurred: function(message) {
            applicationWindow().showPassiveNotification(message, "long")
        }
    }

    SessionRunner {
        id: sessionRunner
        sessionManager: sessionManager
        deviceManager: deviceManager
        helperClient: helperClient
        presetManager: presetManager
        steamConfigManager: steamConfigManager
        heroicConfigManager: heroicConfigManager
        settingsManager: settingsManager

        onErrorOccurred: (message) => {
            applicationWindow().showPassiveNotification(message, "long")
        }
        onSessionStarted: {
            applicationWindow().showPassiveNotification(
                i18nc("@info", "Session started with %1 instances", runningInstanceCount))
        }
        onSessionStopped: {
            applicationWindow().showPassiveNotification(
                i18nc("@info", "Session stopped"))
            if (root.exitAfterActiveSession) {
                root.exitAfterActiveSession = false
                Qt.callLater(function() { Qt.quit() })
            }
        }
        onSessionStartFailed: function(message) {
            if (root.exitOnStartupFailure) {
                root.exitOnStartupFailure = false
                root.startupFailed(1)
            }
        }
    }

    UserManager {
        id: userManager
        Component.onCompleted: setHelper(helperClient)
    }

    CouchPlayHelperClient {
        id: helperClient
    }

    MonitorManager {
        id: monitorManager
    }

    PresetManager {
        id: presetManager
        
        Component.onCompleted: {
            setHeroicConfigManager(heroicConfigManager)
            setSteamConfigManager(steamConfigManager)
        }
    }

    HeroicConfigManager {
        Component.onCompleted: {
            detectHeroicPaths()
            loadGames()
        }
        id: heroicConfigManager
        helperClient: helperClient
    }

    SteamConfigManager {
        id: steamConfigManager
        helperClient: helperClient
        
        Component.onCompleted: {
            detectSteamPaths()
            loadGames()
        }
    }

    AudioManager {
        id: audioManager
    }

    globalDrawer: Kirigami.GlobalDrawer {
        id: drawer
        title: i18nc("@title", "CouchPlay")
        titleIcon: "io.github.hikaps.couchplay"
        isMenu: false
        modal: !root.wideScreen

        actions: [
            Kirigami.Action {
                objectName: "actionHome"
                Accessible.role: Accessible.Button
                Accessible.name: i18nc("@action:button", "Home")
                Accessible.onPressAction: triggered()
                icon.name: "go-home"
                text: i18nc("@action:button", "Home")
                onTriggered: pushHomePage()
            },
            Kirigami.Action {
                objectName: "actionNewSession"
                Accessible.role: Accessible.Button
                Accessible.name: i18nc("@action:button", "New Session")
                Accessible.onPressAction: triggered()
                icon.name: "list-add"
                text: i18nc("@action:button", "New Session")
                onTriggered: pushSessionSetupPage()
            },
            Kirigami.Action {
                objectName: "actionProfiles"
                Accessible.role: Accessible.Button
                Accessible.name: i18nc("@action:button", "Profiles")
                Accessible.onPressAction: triggered()
                icon.name: "bookmark"
                text: i18nc("@action:button", "Profiles")
                onTriggered: pushProfilesPage()
            },
            Kirigami.Action {
                objectName: "actionUsers"
                Accessible.role: Accessible.Button
                Accessible.name: i18nc("@action:button", "Users")
                Accessible.onPressAction: triggered()
                icon.name: "system-users"
                text: i18nc("@action:button", "Users")
                onTriggered: pushUsersPage()
            },
            Kirigami.Action {
                objectName: "actionSettings"
                Accessible.role: Accessible.Button
                Accessible.name: i18nc("@action:button", "Settings")
                Accessible.onPressAction: triggered()
                icon.name: "configure"
                text: i18nc("@action:button", "Settings")
                onTriggered: pushSettingsPage()
            }
        ]
    }

    pageStack.initialPage: HomePage {
        sessionManager: sessionManager
        sessionRunner: sessionRunner
        deviceManager: deviceManager
        helperClient: helperClient
    }

    Component {
        id: homePage
        HomePage {}
    }

    Component {
        id: sessionSetupPage
        SessionSetupPage {}
    }

    Component {
        id: deviceAssignmentPage
        DeviceAssignmentPage {}
    }

    Component {
        id: profilesPage
        ProfilesPage {}
    }

    Component {
        id: usersPage
        UsersPage {}
    }

    Component {
        id: settingsPage
        SettingsPage {}
    }

    function pushHomePage() {
        pageStack.clear()
        pageStack.push(homePage, {
            sessionManager: sessionManager,
            sessionRunner: sessionRunner,
            deviceManager: deviceManager,
            helperClient: helperClient
        })
    }

    function pushSessionSetupPage() {
        pageStack.clear()
        pageStack.push(sessionSetupPage, {
            sessionManager: sessionManager,
            sessionRunner: sessionRunner,
            deviceManager: deviceManager,
            monitorManager: monitorManager,
            userManager: userManager,
            steamConfigManager: steamConfigManager,
            heroicConfigManager: heroicConfigManager,
            presetManager: presetManager
        })
    }

    function pushDeviceAssignmentPage() {
        pageStack.clear()
        pageStack.push(deviceAssignmentPage, {
            deviceManager: deviceManager,
            instanceCount: sessionManager.instanceCount
        })
    }

    function pushProfilesPage() {
        pageStack.clear()
        pageStack.push(profilesPage, {
            sessionManager: sessionManager,
            sessionRunner: sessionRunner
        })
    }

    function pushUsersPage() {
        pageStack.clear()
        pageStack.push(usersPage, {
            userManager: userManager,
            helperClient: helperClient
        })
    }

    function pushSettingsPage() {
        pageStack.clear()
        pageStack.push(settingsPage, {
            sessionRunner: sessionRunner,
            helperClient: helperClient,
            presetManager: presetManager,
            steamConfigManager: steamConfigManager,
            settingsManager: settingsManager,
            heroicConfigManager: heroicConfigManager,
            audioManager: audioManager
        })
    }
}
