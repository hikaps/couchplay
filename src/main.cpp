// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QtQml>

#include <KIconTheme>
#include <KLocalizedContext>
#include <KLocalizedString>

#include <memory>

#include "couchplay-version.h"

#include "core/AudioManager.h"
#include "core/DeviceManager.h"
#include "core/GamescopeInstance.h"
#include "core/Logging.h"
#include "core/MonitorManager.h"
#include "core/PresetManager.h"
#include "core/SessionManager.h"
#include "core/SessionRunner.h"
#include "core/UserManager.h"
#include "dbus/CouchPlayHelperClient.h"

// Custom message handler to filter noisy Qt warnings
static QtMessageHandler s_originalHandler = nullptr;
static std::unique_ptr<RotatingFileLogger> s_fileLogger;

void couchplayMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    // Suppress QStandardPaths permission warnings (common on Bazzite/immutable distros with 0710 permissions)
    if (type == QtWarningMsg
        && msg.contains(QStringLiteral("QStandardPaths: wrong permissions on runtime directory"))) {
        return;
    }

    // Beta channel: mirror every message to the rotating file sink (no-op when null).
    if (s_fileLogger) {
        s_fileLogger->write(type, context, msg);
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

#ifdef COUCHPLAY_BETA
    // Beta channel: enable all couchplay.* debug categories and mirror to a rotating file.
    // QT_LOGGING_RULES still overrides this (env var > setFilterRules).
    QLoggingCategory::setFilterRules(QStringLiteral("couchplay.*=true"));
    const QString logDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/logs");
    QDir().mkpath(logDir);
    s_fileLogger = std::make_unique<RotatingFileLogger>(logDir + QStringLiteral("/couchplay.log"));
    if (!s_fileLogger->open()) {
        s_fileLogger.reset(); // file logging unavailable; console (verbose) still works
    }
#else
    // Prod channel: keep category defaults (warnings only). QT_LOGGING_RULES still works.
#endif

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
