// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QIcon>
#include <QtQml>

#include <KLocalizedContext>
#include <KLocalizedString>
#include <KIconTheme>

#include "core/DeviceManager.h"
#include "core/SessionManager.h"
#include "core/SessionRunner.h"
#include "core/GamescopeInstance.h"
#include "core/UserManager.h"
#include "core/MonitorManager.h"
#include "core/GameLibrary.h"
#include "core/AudioManager.h"
#include "dbus/CouchPlayHelperClient.h"

int main(int argc, char *argv[])
{
    // Initialize KDE icon theme before QApplication
    KIconTheme::initTheme();

    QApplication app(argc, argv);

    // Set application metadata
    KLocalizedString::setApplicationDomain("couchplay");
    QApplication::setOrganizationName(QStringLiteral("hikaps"));
    QApplication::setOrganizationDomain(QStringLiteral("github.com/hikaps"));
    QApplication::setApplicationName(QStringLiteral("CouchPlay"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QApplication::setDesktopFileName(QStringLiteral("io.github.hikaps.couchplay"));
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("io.github.hikaps.couchplay")));

    // Set Qt Quick style
    QApplication::setStyle(QStringLiteral("breeze"));
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) {
        QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));
    }

    // Register QML types
    qmlRegisterType<DeviceManager>("io.github.hikaps.couchplay", 1, 0, "DeviceManager");
    qmlRegisterType<SessionManager>("io.github.hikaps.couchplay", 1, 0, "SessionManager");
    qmlRegisterType<SessionRunner>("io.github.hikaps.couchplay", 1, 0, "SessionRunner");
    qmlRegisterType<GamescopeInstance>("io.github.hikaps.couchplay", 1, 0, "GamescopeInstance");
    qmlRegisterType<UserManager>("io.github.hikaps.couchplay", 1, 0, "UserManager");
    qmlRegisterType<MonitorManager>("io.github.hikaps.couchplay", 1, 0, "MonitorManager");
    qmlRegisterType<GameLibrary>("io.github.hikaps.couchplay", 1, 0, "GameLibrary");
    qmlRegisterType<AudioManager>("io.github.hikaps.couchplay", 1, 0, "AudioManager");
    qmlRegisterType<CouchPlayHelperClient>("io.github.hikaps.couchplay", 1, 0, "HelperClient");

    // Create QML engine
    QQmlApplicationEngine engine;

    // Add i18n context
    engine.rootContext()->setContextObject(new KLocalizedContext(&engine));

    // Load main QML file
    engine.loadFromModule("io.github.hikaps.couchplay", "Main");

    if (engine.rootObjects().isEmpty()) {
        return -1;
    }

    return app.exec();
}
