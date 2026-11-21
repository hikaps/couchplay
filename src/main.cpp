// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QDir>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QtQml>

#include <KIconTheme>
#include <KLocalizedContext>
#include <KDBusService>
#include <KLocalizedString>

#include <memory>

#include "couchplay-version.h"
#include "couchplay-buildinfo.h"

#include "core/AudioManager.h"
#include "core/DeviceManager.h"
#include "core/GamescopeInstance.h"
#include "core/Logging.h"
#include "core/MonitorManager.h"
#include "core/PresetManager.h"
#include "core/SessionManager.h"
#include "core/SessionRunner.h"
#include "core/CommandLineBridge.h"
#include "core/CommandLineOptions.h"
#include "core/SignalBridge.h"

#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QStandardPaths>
#include <QWindow>

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
    s_originalHandler = qInstallMessageHandler(couchplayMessageHandler);
    KIconTheme::initTheme();

    QApplication app(argc, argv);
    KLocalizedString::setApplicationDomain("couchplay");
    QApplication::setOrganizationName(QStringLiteral("hikaps"));
    QApplication::setOrganizationDomain(QStringLiteral("github.com"));
    QApplication::setApplicationName(QStringLiteral("CouchPlay"));
    QApplication::setApplicationVersion(QStringLiteral(COUCHPLAY_DISPLAY_VERSION));
    QApplication::setDesktopFileName(QStringLiteral("io.github.hikaps.couchplay"));
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("io.github.hikaps.couchplay")));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Run split-screen gaming sessions"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({{QStringLiteral("p"), QStringLiteral("profile")},
                      QStringLiteral("Load a saved profile"),
                      QStringLiteral("name")});
    parser.addOption({QStringLiteral("start"), QStringLiteral("Start the loaded profile session")});
    parser.addOption({QStringLiteral("exit-after-session"), QStringLiteral("Exit after the session and post hook finish")});
    parser.addOption({QStringLiteral("skip-pairing-check"),
                      QStringLiteral("Start streaming players even without a paired Moonlight client")});
    parser.process(app);

    QString parseError;
    const CommandLineRequest initialRequest = CommandLineOptions::parse(app.arguments(), &parseError);
    if (!parseError.isEmpty()) {
        qCritical().noquote() << parseError;
        return 2;
    }
    KDBusService service(KDBusService::Unique);
    if (!service.isRegistered()) {
        qWarning() << "CouchPlay singleton unavailable:" << service.errorMessage();
    }

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

    QQmlApplicationEngine engine;
    CommandLineBridge commandLineBridge;
    engine.rootContext()->setContextProperty(QStringLiteral("commandLineBridge"), &commandLineBridge);
    engine.setInitialProperties({
        {QStringLiteral("startupProfileName"), initialRequest.profileName},
        {QStringLiteral("startupStart"), initialRequest.start},
        {QStringLiteral("startupExitAfterSession"), initialRequest.exitAfterSession},
        {QStringLiteral("startupSkipPairingCheck"), initialRequest.skipPairingCheck},
    });
    engine.rootContext()->setContextObject(new KLocalizedContext(&engine));
    engine.loadFromModule("io.github.hikaps.couchplay", "Main");
    if (engine.rootObjects().isEmpty()) {
        qCritical() << "CouchPlay QML root failed to load";
        return -1;
    }

    QObject *root = engine.rootObjects().constFirst();
    SignalBridge signalBridge;
    QObject::connect(&signalBridge, &SignalBridge::terminationRequested, root, [root](int signalNumber) {
        qInfo() << "Received signal" << signalNumber << "- stopping session and quitting";
        QMetaObject::invokeMethod(root, "requestShutdown");
    });
    QObject::connect(root, SIGNAL(startupFailed(int)), &app, SLOT(exit(int)));
    QObject::connect(&service,
                     &KDBusService::activateRequested,
                     root,
                     [root, &service, &commandLineBridge](const QStringList &arguments, const QString &workingDirectory) {
                         Q_UNUSED(workingDirectory)
                         if (auto *window = qobject_cast<QWindow *>(root)) {
                             window->show();
                             window->raise();
                             window->requestActivate();
                         }
                         if (arguments.isEmpty()) {
                             return;
                         }
                         QString forwardedError;
                         const CommandLineRequest request = CommandLineOptions::parse(arguments, &forwardedError);
                         if (!forwardedError.isEmpty()) {
                             qWarning().noquote() << forwardedError;
                             service.setExitValue(2);
                             return;
                         }
                         if (!request.requested()) {
                             return;
                         }
                         commandLineBridge.setRequestAccepted(false);
                         Q_EMIT commandLineBridge.launchRequested(request.profileName,
                                                                  request.start,
                                                                  request.exitAfterSession,
                                                                  request.skipPairingCheck);
                         service.setExitValue(commandLineBridge.requestAccepted() ? 0 : 2);
                     },
                     Qt::DirectConnection);

    return app.exec();
}
