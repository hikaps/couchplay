// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#pragma once

#include <QDBusArgument>
#include <QDBusVariant>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <sys/types.h>

struct DirectoryPreparation
{
    QString source;
    QString mode;
    QString targetAlias;
    QString stagingPath;

    QVariantMap toVariant() const;
};

using DirectoryRecord = DirectoryPreparation;

struct LaunchManifest
{
    int schemaVersion = 1;
    QString unitName;
    QString unitRole = QStringLiteral("main");
    uid_t callerUid = 0;
    gid_t callerGid = 0;
    QString targetUsername;
    uid_t targetUid = 0;
    gid_t targetGid = 0;
    QString declaredHome;
    QString canonicalHome;
    QString targetDeclaredHome;
    QString targetCanonicalHome;
    QList<DirectoryRecord> directories;
    QStringList gamescopeArgv;
    QString workingDirectory;
    QMap<QString, QString> environment;
};

struct SystemdBindPath
{
    QString source;
    QString target;
    bool ignoreMissing = false;
    quint64 flags = 0;
};

QDBusArgument &operator<<(QDBusArgument &argument, const SystemdBindPath &path);
const QDBusArgument &operator>>(const QDBusArgument &argument, SystemdBindPath &path);
struct SystemdProperty
{
    QString name;
    QDBusVariant value;
};

struct SystemdExecCommand
{
    QString path;
    QStringList argv;
    QStringList flags;
};

struct SystemdAuxiliaryUnit
{
    QString name;
    QList<SystemdProperty> properties;
};

QDBusArgument &operator<<(QDBusArgument &argument, const SystemdProperty &property);
const QDBusArgument &operator>>(const QDBusArgument &argument, SystemdProperty &property);
QDBusArgument &operator<<(QDBusArgument &argument, const SystemdExecCommand &command);
const QDBusArgument &operator>>(const QDBusArgument &argument, SystemdExecCommand &command);
QDBusArgument &operator<<(QDBusArgument &argument, const SystemdAuxiliaryUnit &unit);
const QDBusArgument &operator>>(const QDBusArgument &argument, SystemdAuxiliaryUnit &unit);

namespace DirectoryPlan
{
bool directoryPathHasTraversal(const QString &path);
bool isSafeRelativeDirectoryPath(const QString &path);
bool parseDirectoryPreparations(const QVariantList &input,
                                QList<DirectoryPreparation> *output,
                                QString *error = nullptr);
bool validateDirectoryPreparations(const QList<DirectoryPreparation> &input, QString *error = nullptr);
QString dataDirectoryStagingSlug(const QString &dirPath, const QString &compositorHome);
QString copyScratchName(const QString &leafName, const QString &launchToken);

bool writeLaunchManifest(const LaunchManifest &manifest, QString *token, QString *error = nullptr);
bool readLaunchManifest(const QString &token, LaunchManifest *manifest, QString *error = nullptr);
bool unlinkLaunchManifest(const QString &token, QString *error = nullptr);
bool isLaunchToken(const QString &token);
void registerSystemdTypes();
}

Q_DECLARE_METATYPE(SystemdProperty)
Q_DECLARE_METATYPE(SystemdExecCommand)
Q_DECLARE_METATYPE(QList<SystemdExecCommand>)
Q_DECLARE_METATYPE(SystemdAuxiliaryUnit)
Q_DECLARE_METATYPE(QList<SystemdProperty>)
Q_DECLARE_METATYPE(QList<SystemdAuxiliaryUnit>)
Q_DECLARE_METATYPE(SystemdBindPath)
Q_DECLARE_METATYPE(QList<SystemdBindPath>)
