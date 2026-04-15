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

    // Backend managers
    SettingsManager {
        id: settingsManager
    }

    DeviceManager {
        id: deviceManager
        settingsManager: settingsManager
        
        // When a device is assigned or unassigned, update the stableIds and names in SessionManager
        onDeviceAssigned: function(eventNumber, instanceIndex, previousInstanceIndex) {
            if (sessionManager) {
                // Update the new instance (if assigning to an instance)
                if (instanceIndex >= 0) {
                    let stableIds = deviceManager.getStableIdsForInstance(instanceIndex)
                    let names = deviceManager.getDeviceNamesForInstance(instanceIndex)
                    sessionManager.setInstanceDeviceStableIds(instanceIndex, stableIds, names)
                }
                
                // Update the previous instance (if it was assigned somewhere before)
                if (previousInstanceIndex >= 0 && previousInstanceIndex !== instanceIndex) {
                    let stableIds = deviceManager.getStableIdsForInstance(previousInstanceIndex)
                    let names = deviceManager.getDeviceNamesForInstance(previousInstanceIndex)
                    sessionManager.setInstanceDeviceStableIds(previousInstanceIndex, stableIds, names)
                }
            }
        }
        
        // When a pending device reconnects and is auto-restored
        onDeviceAutoRestored: function(name, instanceIndex) {
            applicationWindow().showPassiveNotification(
                i18nc("@info", "%1 reconnected to Player %2", name, instanceIndex + 1))
        }
    }

    SessionManager {
        id: sessionManager
        
        // When a profile is loaded, restore device assignments from stableIds
        onProfileLoaded: function(deviceInfoByInstance) {
            if (deviceManager) {
                // Clear ALL existing assignments before restoring (prevents stale assignments from previous session/profile)
                deviceManager.unassignAll()
                
                // Restore assignments for each instance that has saved stableIds
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
    }

    SessionRunner {
        id: sessionRunner
        sessionManager: sessionManager
        deviceManager: deviceManager
        helperClient: helperClient
        presetManager: presetManager
        steamConfigManager: steamConfigManager
        heroicConfigManager: heroicConfigManager
        borderlessWindows: settingsManager.borderlessWindows

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
        id: heroicConfigManager
        helperClient: helperClient
    }

    SteamConfigManager {
        id: steamConfigManager
        helperClient: helperClient
        
        Component.onCompleted: {
            detectSteamPaths()
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
                icon.name: "go-home"
                text: i18nc("@action:button", "Home")
                onTriggered: {
                    pageStack.clear()
                    pageStack.push(homePage, {
                        sessionManager: sessionManager,
                        sessionRunner: sessionRunner,
                        deviceManager: deviceManager,
                        helperClient: helperClient
                    })
                }
            },
            Kirigami.Action {
                icon.name: "list-add"
                text: i18nc("@action:button", "New Session")
                onTriggered: {
                    pageStack.clear()
                    pageStack.push(sessionSetupPage, {
                        sessionManager: sessionManager,
                        sessionRunner: sessionRunner,
                        deviceManager: deviceManager,
                        monitorManager: monitorManager,
                        userManager: userManager,
                        presetManager: presetManager
                    })
                }
            },
            Kirigami.Action {
                icon.name: "bookmark"
                text: i18nc("@action:button", "Profiles")
                onTriggered: {
                    pageStack.clear()
                    pageStack.push(profilesPage, {
                        sessionManager: sessionManager,
                        sessionRunner: sessionRunner
                    })
                }
            },
            Kirigami.Action {
                icon.name: "system-users"
                text: i18nc("@action:button", "Users")
                onTriggered: {
                    pageStack.clear()
                    pageStack.push(usersPage, {
                        userManager: userManager,
                        helperClient: helperClient
                    })
                }
            },
            Kirigami.Action {
                icon.name: "configure"
                text: i18nc("@action:button", "Settings")
                onTriggered: {
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
        ]
    }

    pageStack.initialPage: HomePage {
        sessionManager: sessionManager
        sessionRunner: sessionRunner
        deviceManager: deviceManager
        helperClient: helperClient
    }

    // Page components - properties must be passed via pageStack.push()
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

    // Helper functions to push pages with required properties
    // All use clear() + push() to prevent split-view stacking
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
