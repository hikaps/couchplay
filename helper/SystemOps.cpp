// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "SystemOps.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cerrno>
#include <cstring>
#include <unistd.h>

RealSystemOps::RealSystemOps(QObject *parent)
    : QObject(parent)
{
}

// User/group lookup operations
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

// Filesystem operations
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

// Device path validation
bool RealSystemOps::statPath(const QString &path, struct stat *buf)
{
    return stat(path.toLocal8Bit().constData(), buf) == 0;
}

bool RealSystemOps::isCharDevice(mode_t mode)
{
    return S_ISCHR(mode);
}

// Ownership and permissions
int RealSystemOps::chown(const QString &path, uid_t owner, gid_t group)
{
    return ::chown(path.toLocal8Bit().constData(), owner, group);
}

int RealSystemOps::chmod(const QString &path, mode_t mode)
{
    return ::chmod(path.toLocal8Bit().constData(), mode);
}

// Process operations
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

// Directory listing
QStringList RealSystemOps::entryList(const QString &path, const QStringList &nameFilters, QDir::Filters filters)
{
    QDir dir(path);
    return dir.entryList(nameFilters, filters);
}

// Process signaling
bool RealSystemOps::killProcess(pid_t pid, int signal)
{
    return ::kill(pid, signal) == 0;
}

// Authorization check
bool RealSystemOps::checkAuthorization(const QString &action)
{
    // In a full implementation, this would check PolicyKit
    // For now, we trust the D-Bus system bus ACL
    // TODO: Implement proper PolicyKit authorization check

    Q_UNUSED(action)

    // Check if caller is root or in appropriate group
    // The system D-Bus policy should restrict who can call us
    return true;
}
