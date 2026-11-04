// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "CouchPlayHelper.h"
#include "DirectoryPlan.h"
#include "InstanceBootstrap.h"
#include "couchplay-version.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDebug>
#include <QString>

#include <cerrno>
#include <cstring>
#include <limits.h>
#include <unistd.h>

namespace
{
QString runningExecutablePath()
{
    char path[PATH_MAX + 1];
    const ssize_t size = ::readlink("/proc/self/exe", path, PATH_MAX);
    if (size <= 0 || size > PATH_MAX) return {};
    path[size] = '\0';
    return QString::fromLocal8Bit(path, static_cast<qsizetype>(size));
}
}

int main(int argc, char *argv[])
{
    if (argc > 1 && std::strcmp(argv[1], "--run-instance") == 0) {
        if (argc != 3) {
            qCritical() << "Usage: couchplay-helper --run-instance <uuid>";
            return 2;
        }
        return InstanceBootstrap::run(QString::fromLocal8Bit(argv[2]), runningExecutablePath());
    }

    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("couchplay-helper"));
    app.setApplicationVersion(QStringLiteral(COUCHPLAY_VERSION_STRING));
    app.setOrganizationDomain(QStringLiteral("io.github.hikaps"));
    DirectoryPlan::registerSystemdTypes();

    CouchPlayHelper helper;
    QDBusConnection systemBus = QDBusConnection::systemBus();
    if (!systemBus.isConnected()) {
        qCritical() << "Cannot connect to the D-Bus system bus";
        return 1;
    }

    const QString serviceName = QStringLiteral("io.github.hikaps.CouchPlayHelper");
    if (!systemBus.registerService(serviceName)) {
        qCritical() << "Cannot register service:" << systemBus.lastError().message();
        return 1;
    }
    const QString objectPath = QStringLiteral("/io/github/hikaps/CouchPlayHelper");
    if (!systemBus.registerObject(objectPath,
                                  &helper,
                                  QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals)) {
        qCritical() << "Cannot register object:" << systemBus.lastError().message();
        return 1;
    }
    qInfo() << "CouchPlay helper daemon started";
    qInfo() << "  Service:" << serviceName;
    qInfo() << "  Object:" << objectPath;
    return app.exec();
}
