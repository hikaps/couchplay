// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "SystemOps.h"
#include "PolkitActions.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#ifdef HAVE_POLKITQT
#include <PolkitQt1/Authority>
#include <PolkitQt1/Subject>
#endif

#include <cerrno>
#include <cstring>
#include <unistd.h>

RealSystemOps::RealSystemOps(QObject *parent)
    : QObject(parent)
{
}

struct passwd *RealSystemOps::getpwnam(const char *name)
{
    return ::getpwnam(name);
}

struct passwd *RealSystemOps::getpwuid(uid_t uid)
{
    return ::getpwuid(uid);
}

struct group *RealSystemOps::getgrnam(const char *name)
{
    return ::getgrnam(name);
}

bool RealSystemOps::fileExists(const QString &path)
{
    return QFile::exists(path);
}

bool RealSystemOps::isDirectory(const QString &path)
{
    return QFileInfo(path).isDir();
}

bool RealSystemOps::mkpath(const QString &path)
{
    return QDir().mkpath(path);
}

bool RealSystemOps::removeFile(const QString &path)
{
    return QFile::remove(path);
}

bool RealSystemOps::copyFile(const QString &source, const QString &dest)
{
    return QFile::copy(source, dest);
}

bool RealSystemOps::writeFile(const QString &path, const QByteArray &content)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    qint64 written = file.write(content);
    file.close();
    return written == content.size();
}

bool RealSystemOps::statPath(const QString &path, struct stat *buf)
{
    return stat(path.toLocal8Bit().constData(), buf) == 0;
}

bool RealSystemOps::isCharDevice(mode_t mode)
{
    return S_ISCHR(mode);
}

int RealSystemOps::chown(const QString &path, uid_t owner, gid_t group)
{
    return ::chown(path.toLocal8Bit().constData(), owner, group);
}

int RealSystemOps::chmod(const QString &path, mode_t mode)
{
    return ::chmod(path.toLocal8Bit().constData(), mode);
}

QProcess *RealSystemOps::createProcess(QObject *parent)
{
    return new QProcess(parent);
}

void RealSystemOps::startProcess(QProcess *process, const QString &program, const QStringList &arguments)
{
    process->start(program, arguments);
}

bool RealSystemOps::waitForFinished(QProcess *process, int msecs)
{
    return process->waitForFinished(msecs);
}

int RealSystemOps::processExitCode(QProcess *process)
{
    return process->exitCode();
}

QByteArray RealSystemOps::readStandardError(QProcess *process)
{
    return process->readAllStandardError();
}

QByteArray RealSystemOps::readAllStandardOutput(QProcess *process)
{
    return process->readAllStandardOutput();
}

QStringList RealSystemOps::entryList(const QString &path, const QStringList &nameFilters, QDir::Filters filters)
{
    QDir dir(path);
    return dir.entryList(nameFilters, filters);
}

bool RealSystemOps::killProcess(pid_t pid, int signal)
{
    return ::kill(pid, signal) == 0;
}

bool RealSystemOps::checkAuthorization(const QString &action, const QString &callerBusName)
{
    // Only user account lifecycle actions require Polkit authentication.
    // Other privileged operations (device ownership, instance launch, mount
    // management) rely on D-Bus system bus ACL restricted to wheel/games groups.
    // Rationale: user accounts persist beyond the session; other operations are
    // transient and scoped to the local machine where physical access is assumed.
    if (action != PolkitActions::ACTION_CREATE_USER
        && action != PolkitActions::ACTION_DELETE_USER) {
        return true;
    }

    if (callerBusName.isEmpty()) {
        qWarning() << "checkAuthorization: caller bus name is empty";
        return false;
    }

#ifdef HAVE_POLKITQT
    PolkitQt1::Authority *authority = PolkitQt1::Authority::instance();
    if (authority->hasError()) {
        qWarning() << "Polkit authority error:" << authority->lastError()
                   << "- denying action:" << action;
        return false;
    }

    PolkitQt1::Authority::Result result =
        authority->checkAuthorizationSync(
            action,
            PolkitQt1::SystemBusNameSubject(callerBusName),
            PolkitQt1::Authority::AllowUserInteraction);

    if (result == PolkitQt1::Authority::Unknown) {
        qWarning() << "Polkit returned Unknown (daemon unavailable?) for action:" << action
                   << "caller:" << callerBusName;
        return false;
    }
    if (result != PolkitQt1::Authority::Yes) {
        qWarning() << "Polkit authorization denied for action:" << action
                   << "caller:" << callerBusName;
        return false;
    }
    return true;
#else
    qWarning() << "Polkit not available, denying privileged action:" << action;
    return false;
#endif
}
