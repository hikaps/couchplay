// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors
//
// Stub "gamescope" window for e2e session-lifecycle tests. Compiled to a binary
// NAMED "gamescope" so Qt6 Wayland reports app_id/resourceClass "gamescope",
// which the app's WindowManager (findAllGamescopeWindows) matches. A Python/
// PySide6 stub does NOT work here -- Qt6 Wayland takes the app_id from the
// executable name, which would be "python3".
//
// Built by appiumtests/Dockerfile to /opt/couchplay-e2e/gamescope and launched
// by mock_helper.LaunchInstance (COUCHPLAY_STUB_GAMESCOPE).
#include <QApplication>
#include <QWidget>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName("gamescope");

    QWidget window;
    window.setWindowTitle("gamescope");
    window.resize(512, 768);
    window.show();

    return app.exec();
}
