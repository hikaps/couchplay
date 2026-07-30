// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

#include <QApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QtQml>

#include <KIconTheme>
#include <KLocalizedContext>
#include <KLocalizedString>

#include "couchplay-version.h"

#include "core/AudioManager.h"
#include "core/DeviceManager.h"
#include "core/GamescopeInstance.h"
#include "core/MonitorManager.h"
#include "core/PresetManager.h"
#include "core/SessionManager.h"
#include "core/SessionRunner.h"
#include "core/UserManager.h"
#include "dbus/CouchPlayHelperClient.h"

#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QStandardPaths>

// Custom message handler to filter noisy Qt warnings
static QtMessageHandler s_originalHandler = nullptr;

void couchplayMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    // Suppress QStandardPaths permission warnings (common on Bazzite/immutable distros with 0710 permissions)
    if (type == QtWarningMsg
        && msg.contains(QStringLiteral("QStandardPaths: wrong permissions on runtime directory"))) {
        return;
    }

    // Optional file logging, opt-in via COUCHPLAY_LOG (debug aid; off by default).
    static const bool s_logToFile = !qgetenv("COUCHPLAY_LOG").isEmpty();
    if (s_logToFile) {
        QFile logFile(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                      + QStringLiteral("/couchplay/couchplay.log"));
        if (logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            QTextStream stream(&logFile);
            QString typeStr = QStringLiteral("DEBUG");
            switch (type) {
            case QtDebugMsg: typeStr = QStringLiteral("DEBUG"); break;
            case QtInfoMsg: typeStr = QStringLiteral("INFO"); break;
            case QtWarningMsg: typeStr = QStringLiteral("WARN"); break;
            case QtCriticalMsg: typeStr = QStringLiteral("CRIT"); break;
            case QtFatalMsg: typeStr = QStringLiteral("FATAL"); break;
            }
            stream << "[" << QDateTime::currentDateTime().toString(Qt::ISODate) << "] [" << typeStr << "] " << msg << "\n";
        }
    }

    if (s_originalHandler) {
        s_originalHandler(type, context, msg);
    }
}

int main(int argc, char *argv[])
{
    // Install custom message handler before QApplication
    s_originalHandler = qInstallMessageHandler(couchplayMessageHandler);

    // Initialize KDE icon theme before QApplication
    KIconTheme::initTheme();

    QApplication app(argc, argv);

    // Set application metadata
    KLocalizedString::setApplicationDomain("couchplay");
    QApplication::setOrganizationName(QStringLiteral("hikaps"));
    QApplication::setOrganizationDomain(QStringLiteral("github.com/hikaps"));
    QApplication::setApplicationName(QStringLiteral("CouchPlay"));
    QApplication::setApplicationVersion(QStringLiteral(COUCHPLAY_VERSION_STRING));
    QApplication::setDesktopFileName(QStringLiteral("io.github.hikaps.couchplay"));
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("io.github.hikaps.couchplay")));

    // Set Qt Quick style
    QApplication::setStyle(QStringLiteral("breeze"));
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) {
        QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));
    }

    // QML types are registered via QML_ELEMENT macro in headers

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
