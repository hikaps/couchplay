// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "SystemOps.h"
#include "PolkitActions.h"
#include "SecureFs.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>

#ifdef HAVE_POLKITQT
#include <PolkitQt1/Authority>
#include <PolkitQt1/Subject>
#endif
#include <fcntl.h>
static constexpr off_t MAX_SECURE_COPY_BYTES = 64 * 1024 * 1024;

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
uid_t RealSystemOps::connectionUnixUser(const QString &busName)
{
    QDBusInterface bus(QStringLiteral("org.freedesktop.DBus"),
                       QStringLiteral("/org/freedesktop/DBus"),
                       QStringLiteral("org.freedesktop.DBus"),
                       QDBusConnection::systemBus());
    const QDBusReply<uint> reply = bus.call(QStringLiteral("GetConnectionUnixUser"), busName);
    return reply.isValid() ? static_cast<uid_t>(reply.value()) : static_cast<uid_t>(-1);
}

bool RealSystemOps::fileExists(const QString &path)
{
    return QFile::exists(path);
}

bool RealSystemOps::isDirectory(const QString &path)
{
    return QFileInfo(path).isDir();
}

bool RealSystemOps::isSymLink(const QString &path)
{
    struct stat st;
    return lstat(path.toLocal8Bit().constData(), &st) == 0 && S_ISLNK(st.st_mode);
}

QString RealSystemOps::canonicalFilePath(const QString &path)
{
    return QFileInfo(path).canonicalFilePath();
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
bool RealSystemOps::copyFileSecure(const QString &source,
                                   const QString &dest,
                                   uid_t sourceOwner,
                                   uid_t destOwner,
                                   gid_t destGroup)
{
    const QFileInfo sourceInfo(source);
    const int sourceParentFd = SecureFs::openExistingDirNoFollow(sourceInfo.absolutePath());
    if (sourceParentFd < 0) {
        return false;
    }
    const QByteArray sourceName = sourceInfo.fileName().toLocal8Bit();
    const int sourceFd = ::openat(sourceParentFd, sourceName.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    ::close(sourceParentFd);
    if (sourceFd < 0) {
        return false;
    }
    struct stat sourceStat {};
    if (::fstat(sourceFd, &sourceStat) != 0 || !S_ISREG(sourceStat.st_mode) || sourceStat.st_uid != sourceOwner
        || !(sourceStat.st_mode & S_IRUSR)) {
        ::close(sourceFd);
        return false;
    }
    if (sourceStat.st_size > MAX_SECURE_COPY_BYTES) {
        ::close(sourceFd);
        return false;
    }
    off_t copiedBytes = 0;
    QByteArray content;
    char buffer[8192];
    ssize_t readCount = 0;
    while ((readCount = ::read(sourceFd, buffer, sizeof(buffer))) > 0) {
        if (copiedBytes > MAX_SECURE_COPY_BYTES - readCount) {
            ::close(sourceFd);
            return false;
        }
        content.append(buffer, static_cast<qsizetype>(readCount));
        copiedBytes += readCount;
    }
    ::close(sourceFd);
    if (readCount < 0) {
        return false;
    }

    const QFileInfo targetInfo(dest);
    const int targetParentFd = SecureFs::openExistingDirNoFollow(targetInfo.absolutePath());
    if (targetParentFd < 0) {
        return false;
    }
    const int result = SecureFs::writeFileAt(targetParentFd, targetInfo.fileName(), content, destOwner, destGroup, 0644);
    ::close(targetParentFd);
    return result == 0;
}
bool RealSystemOps::createDirectorySecure(const QString &path, uid_t owner, gid_t group)
{
    const int rootFd = SecureFs::openBaseDir(QStringLiteral("/"));
    if (rootFd < 0) {
        return false;
    }
    const QStringList parts = QDir::cleanPath(path).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    const int directoryFd = SecureFs::openDirBelow(rootFd, parts, true, owner, group);
    ::close(rootFd);
    if (directoryFd < 0) {
        return false;
    }
    ::close(directoryFd);
    return true;
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









    if (callerBusName.isEmpty()) {
        qWarning() << "checkAuthorization: caller bus name is empty";
        return false;
    }

#ifdef HAVE_POLKITQT
    PolkitQt1::Authority *authority = PolkitQt1::Authority::instance();
    if (authority->hasError()) {
        qWarning() << "Polkit authority error:" << authority->lastError() << "- denying action:" << action;
        return false;
    }

    PolkitQt1::Authority::Result result =
        authority->checkAuthorizationSync(action,
                                          PolkitQt1::SystemBusNameSubject(callerBusName),
                                          PolkitQt1::Authority::AllowUserInteraction);

    if (result == PolkitQt1::Authority::Unknown) {
        qWarning() << "Polkit returned Unknown (daemon unavailable?) for action:" << action
                   << "caller:" << callerBusName;
        return false;
    }
    if (result != PolkitQt1::Authority::Yes) {
        qWarning() << "Polkit authorization denied for action:" << action << "caller:" << callerBusName;
        return false;
    }
    return true;
#else
    qWarning() << "Polkit not available, denying privileged action:" << action;
    return false;
#endif
}
