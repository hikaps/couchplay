// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

#include <QApplication>
#include <QDBusConnectionInterface>
#include <QDBusConnection>
#include <QCommandLineParser>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QtQml>

#include <KIconTheme>
#include <KLocalizedContext>
#include <KDBusService>
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
#include "core/CommandLineBridge.h"
#include "core/SessionLaunchClient.h"
#include "core/CommandLineOptions.h"

#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QStandardPaths>
#include <QWindow>

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
    s_originalHandler = qInstallMessageHandler(couchplayMessageHandler);
    KIconTheme::initTheme();

    QApplication app(argc, argv);
    KLocalizedString::setApplicationDomain("couchplay");
    QApplication::setOrganizationName(QStringLiteral("hikaps"));
    QApplication::setOrganizationDomain(QStringLiteral("github.com"));
    QApplication::setApplicationName(QStringLiteral("CouchPlay"));
    QApplication::setApplicationVersion(QStringLiteral(COUCHPLAY_VERSION_STRING));
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
    parser.process(app);

    QString parseError;
    const CommandLineRequest initialRequest = CommandLineOptions::parse(app.arguments(), &parseError);
    if (!parseError.isEmpty()) {
        qCritical().noquote() << parseError;
        return 2;
    }
    const bool waitingLaunch = initialRequest.start && initialRequest.exitAfterSession;
    CommandLineBridge commandLineBridge;
    if (!QDBusConnection::sessionBus().registerObject(QStringLiteral("/SessionLauncher"),
                                                       &commandLineBridge,
                                                       QDBusConnection::ExportAdaptors)) {
        qWarning() << "Failed to register session launch bridge:"
                   << QDBusConnection::sessionBus().lastError().message();
        return 1;
    }
    KDBusService service(KDBusService::Unique | KDBusService::NoExitOnFailure);
    if (!service.isRegistered()) {
        if (waitingLaunch) {
            return SessionLaunchClient::run(app, initialRequest, QStringLiteral("com.github.CouchPlay"));
        }
        return 0;
    }

    QApplication::setStyle(QStringLiteral("breeze"));
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) {
        QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));
    }

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("commandLineBridge"), &commandLineBridge);
    engine.setInitialProperties({
        {QStringLiteral("startupProfileName"), initialRequest.profileName},
        {QStringLiteral("startupStart"), initialRequest.start},
        {QStringLiteral("startupExitAfterSession"), initialRequest.exitAfterSession},
    });
    engine.rootContext()->setContextObject(new KLocalizedContext(&engine));
    engine.loadFromModule("io.github.hikaps.couchplay", "Main");
    if (engine.rootObjects().isEmpty()) {
        qCritical() << "CouchPlay QML root failed to load";
        return -1;
    }
    auto *terminationNotifier = SessionLaunchClient::watchTermination(&app, [&commandLineBridge] {
        commandLineBridge.requestStop();
    });
    Q_UNUSED(terminationNotifier);

    QObject *root = engine.rootObjects().constFirst();
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
                         if (request.start && request.exitAfterSession) {
                             return;
                         }
                         commandLineBridge.setRequestAccepted(false);
                         Q_EMIT commandLineBridge.launchRequested(request.profileName, request.start, request.exitAfterSession);
                         service.setExitValue(commandLineBridge.requestAccepted() ? 0 : 2);
                     },
                     Qt::DirectConnection);

    commandLineBridge.setReady(true);
    return app.exec();
}
