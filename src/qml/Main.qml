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
    DeviceManager {
        id: deviceManager
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
                    pageStack.push(homePage)
                }
            },
            Kirigami.Action {
                icon.name: "list-add"
                text: i18nc("@action:button", "New Session")
                onTriggered: {
                    pageStack.clear()
                    pageStack.push(sessionSetupPage)
                }
            },
            Kirigami.Action {
                icon.name: "bookmark"
                text: i18nc("@action:button", "Profiles")
                onTriggered: {
                    pageStack.clear()
                    pageStack.push(profilesPage)
                }
            },
            Kirigami.Action {
                icon.name: "applications-games"
                text: i18nc("@action:button", "Games")
                onTriggered: {
                    pageStack.clear()
                    pageStack.push(gamesPage)
                }
            },
            Kirigami.Action {
                icon.name: "system-users"
                text: i18nc("@action:button", "Users")
                onTriggered: {
                    pageStack.clear()
                    pageStack.push(usersPage)
                }
            },
            Kirigami.Action {
                icon.name: "configure"
                text: i18nc("@action:button", "Settings")
                onTriggered: {
                    pageStack.clear()
                    pageStack.push(settingsPage)
                }
            }
        ]
    }

    pageStack.initialPage: homePage

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
        DeviceAssignmentPage {
            deviceManager: deviceManager
        }
    }

    Component {
        id: layoutPage
        LayoutPage {}
    }

    Component {
        id: profilesPage
        ProfilesPage {}
    }

    Component {
        id: gamesPage
        GamesPage {}
    }

    Component {
        id: usersPage
        UsersPage {}
    }

    Component {
        id: settingsPage
        SettingsPage {}
    }
}
