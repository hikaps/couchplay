// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 hikaps

#include "HostProcess.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QFile>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>

namespace
{
const QString kFlatpakSpawn = QStringLiteral("/usr/bin/flatpak-spawn");

QMutex &cacheMutex()
{
    static QMutex mutex;
    return mutex;
}

QHash<QString, QString> &libexecCache()
{
    static QHash<QString, QString> cache;
    return cache;
}

QString flatpakAppPath()
{
    QFile info(QStringLiteral("/.flatpak-info"));
    if (!info.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    bool inInstance = false;
    while (!info.atEnd()) {
        const QString line = QString::fromUtf8(info.readLine()).trimmed();
        if (line.startsWith(QLatin1Char('['))) {
            inInstance = line == QStringLiteral("[Instance]");
        } else if (inInstance && line.startsWith(QStringLiteral("app-path="))) {
            return line.mid(QStringLiteral("app-path=").size());
        }
    }
    return {};
}
}

bool HostProcess::runningInFlatpak()
{
    return qEnvironmentVariableIsSet("FLATPAK_ID");
}

void HostProcess::configure(QProcess &process,
                            const QString &program,
                            const QStringList &arguments,
                            const QStringList &extraEnv)
{
    if (runningInFlatpak()) {
        QStringList spawnArgs{QStringLiteral("--host"), QStringLiteral("--watch-bus")};
        for (const QString &entry : extraEnv) {
            spawnArgs << QStringLiteral("--env=%1").arg(entry);
        }
        spawnArgs << program << arguments;
        process.setProgram(kFlatpakSpawn);
        process.setArguments(spawnArgs);
        return;
    }

    process.setProgram(program);
    process.setArguments(arguments);
    if (!extraEnv.isEmpty()) {
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        for (const QString &entry : extraEnv) {
            const qsizetype equals = entry.indexOf(QLatin1Char('='));
            if (equals > 0) {
                environment.insert(entry.left(equals), entry.mid(equals + 1));
            }
        }
        process.setProcessEnvironment(environment);
    }
}

bool HostProcess::hostExecutableExists(const QString &path)
{
    if (path.isEmpty()) {
        return false;
    }
    if (!runningInFlatpak()) {
        const QFileInfo info(path);
        return info.isFile() && info.isExecutable();
    }

    QProcess probe;
    configure(probe, QStringLiteral("test"), {QStringLiteral("-x"), path});
    probe.start();
    if (!probe.waitForFinished(3000)) {
        probe.kill();
        probe.waitForFinished(500);
        return false;
    }
    return probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0;
}

QString HostProcess::hostFindExecutable(const QString &name)
{
    QProcess probe;
    configure(probe, QStringLiteral("sh"), {QStringLiteral("-c"), QStringLiteral("command -v \"$1\""), QStringLiteral("sh"), name});
    probe.setProcessChannelMode(QProcess::SeparateChannels);
    probe.start();
    if (!probe.waitForFinished(5000)) {
        probe.kill();
        probe.waitForFinished(500);
        return {};
    }
    if (probe.exitStatus() != QProcess::NormalExit || probe.exitCode() != 0) {
        return {};
    }
    return QString::fromLocal8Bit(probe.readAllStandardOutput()).trimmed();
}

QString HostProcess::hostLibexecPath(const QString &name)
{
    {
        QMutexLocker locker(&cacheMutex());
        const auto cached = libexecCache().constFind(name);
        if (cached != libexecCache().cend()) {
            return cached.value();
        }
    }

    QString found;
    if (runningInFlatpak()) {
        // The Flatpak's own files are visible on the host, so its tools run from there; nothing is installed.
        const QString appPath = flatpakAppPath();
        const QString candidate = appPath + QStringLiteral("/libexec/") + name;
        if (!appPath.isEmpty() && hostExecutableExists(candidate)) {
            found = candidate;
        }
    } else {
        const QString appDir = QCoreApplication::applicationDirPath();
        found = QStandardPaths::findExecutable(name,
                                               {appDir,
                                                appDir + QStringLiteral("/../libexec"),
                                                QStringLiteral("/usr/local/libexec"),
                                                QStringLiteral("/usr/libexec")});
    }

    // Only cache hits: a missing binary may be installed while the app runs.
    if (!found.isEmpty()) {
        QMutexLocker locker(&cacheMutex());
        libexecCache().insert(name, found);
    }
    return found;
}

void HostProcess::clearCache()
{
    QMutexLocker locker(&cacheMutex());
    libexecCache().clear();
}
