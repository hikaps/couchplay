// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "CouchPlayHelper.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDebug>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("couchplay-helper"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));
    app.setOrganizationDomain(QStringLiteral("io.github.hikaps"));

    // Create the helper service
    CouchPlayHelper helper;

    // Register on the system bus
    QDBusConnection systemBus = QDBusConnection::systemBus();
    
    if (!systemBus.isConnected()) {
        qCritical() << "Cannot connect to the D-Bus system bus";
        return 1;
    }

    // Register service name
    const QString serviceName = QStringLiteral("io.github.hikaps.CouchPlayHelper");
    if (!systemBus.registerService(serviceName)) {
        qCritical() << "Cannot register D-Bus service:" << systemBus.lastError().message();
        return 1;
    }

    // Register object
    const QString objectPath = QStringLiteral("/io/github/hikaps/CouchPlayHelper");
    if (!systemBus.registerObject(objectPath, &helper, 
            QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals)) {
        qCritical() << "Cannot register D-Bus object:" << systemBus.lastError().message();
        return 1;
    }

    qInfo() << "CouchPlay helper daemon started";
    qInfo() << "  Service:" << serviceName;
    qInfo() << "  Object:" << objectPath;

    return app.exec();
}
