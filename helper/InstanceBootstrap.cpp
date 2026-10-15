// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include "InstanceBootstrap.h"

#include "DirectoryPlan.h"
#include "HomePathMapping.h"
#include "SecureFs.h"

#include <QCryptographicHash>
#include <QDir>
#include <QMap>
#include <QSet>

#include <dirent.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <grp.h>
#include <linux/capability.h>
#include <linux/mount.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <cstddef>
#include <limits.h>
#include <sys/uio.h>
#include <unistd.h>
#include <vector>
#include <utility>

namespace
{
class UniqueFd
{
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : m_fd(fd) {}
    ~UniqueFd() { reset(); }
    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;
    UniqueFd(UniqueFd &&other) noexcept : m_fd(other.release()) {}
    UniqueFd &operator=(UniqueFd &&other) noexcept
    {
        if (this != &other) reset(other.release());
        return *this;
    }

    int get() const { return m_fd; }
    explicit operator bool() const { return m_fd >= 0; }
    int release()
    {
        const int fd = m_fd;
        m_fd = -1;
        return fd;
    }
    void reset(int fd = -1)
    {
        if (m_fd >= 0) ::close(m_fd);
        m_fd = fd;
    }

private:
    int m_fd = -1;
};

struct Account
{
    QString username;
    QString declaredHome;
    QString canonicalHome;
    QString shell;
    uid_t uid = 0;
    gid_t gid = 0;
};

struct PreparedDirectory
{
    DirectoryRecord record;
    QString canonicalSource;
    QString targetAlias;
    struct stat sourceStat{};
    UniqueFd sourceFd;
    UniqueFd detachedMountFd;
    UniqueFd stagingFd;
};

bool setError(QString *error, int *errorNumber, int value, const QString &message)
{
    if (error) *error = message;
    if (errorNumber) *errorNumber = value > 0 ? value : EIO;
    return false;
}

QString systemError(int value)
{
    return QString::fromLocal8Bit(std::strerror(value > 0 ? value : EIO));
}

QString canonicalPath(const QString &path, int *errorNumber)
{
    if (!QDir::isAbsolutePath(path) || DirectoryPlan::directoryPathHasTraversal(path)
        || path.contains(QChar::Null)) {
        if (errorNumber) *errorNumber = EINVAL;
        return {};
    }
    const QByteArray encoded = path.toLocal8Bit();
    char resolved[PATH_MAX];
    if (!::realpath(encoded.constData(), resolved)) {
        if (errorNumber) *errorNumber = errno;
        return {};
    }
    if (errorNumber) *errorNumber = 0;
    return QString::fromLocal8Bit(resolved);
}

bool pathWithin(const QString &path, const QString &root)
{
    return path == root || path.startsWith(root == QStringLiteral("/") ? root : root + QLatin1Char('/'));
}

bool permittedExternalPath(const QString &path)
{
    static const QStringList prefixes = {
        QStringLiteral("/opt/"), QStringLiteral("/srv/"), QStringLiteral("/run/media/"),
        QStringLiteral("/media/"), QStringLiteral("/mnt/"), QStringLiteral("/tmp/")};
    for (const QString &prefix : prefixes) {
        if (path.startsWith(prefix)) return true;
    }
    struct CanonicalPrefix { const char *alias; const char *canonical; };
    static constexpr CanonicalPrefix canonicalPrefixes[] = {
        {"/mnt", "/var/mnt"}, {"/opt", "/var/opt"}, {"/srv", "/var/srv"}};
    for (const CanonicalPrefix &prefix : canonicalPrefixes) {
        const QString canonicalAlias = canonicalPath(QString::fromLatin1(prefix.alias), nullptr);
        const QString expected = QString::fromLatin1(prefix.canonical);
        if (canonicalAlias == expected && path.startsWith(expected + QLatin1Char('/'))) return true;
    }
    return false;
}

bool isCallerHomeRoot(const QString &path, const LaunchManifest &manifest)
{
    const QString cleanPath = QDir::cleanPath(path);
    return cleanPath == QDir::cleanPath(manifest.declaredHome)
        || cleanPath == QDir::cleanPath(manifest.canonicalHome);
}

bool isCallerHomeDescendant(const QString &path, const LaunchManifest &manifest)
{
    const HomePathMapping home = mapHomeRelativePath(path, manifest.declaredHome, manifest.canonicalHome);
    return home.kind == HomePathKind::Relative && !home.relativePath.isEmpty();
}

bool permittedSourcePath(const QString &path, const LaunchManifest &manifest, bool allowHomeRoot)
{
    const HomePathMapping home = mapHomeRelativePath(path, manifest.declaredHome, manifest.canonicalHome);
    if (home.kind == HomePathKind::Relative) return !home.relativePath.isEmpty();
    if (home.kind == HomePathKind::Invalid) return allowHomeRoot && isCallerHomeRoot(path, manifest);
    return permittedExternalPath(path);
}
bool relativeBelow(const QString &path, const QString &root, QString *relative)
{
    if (path.size() <= root.size() || !path.startsWith(root)
        || path.at(root.size()) != QLatin1Char('/')) {
        return false;
    }
    *relative = path.mid(root.size() + 1);
    return !relative->isEmpty();
}

// Resolve only an allowlisted external prefix; descendant components are opened nofollow below it.
bool externalSourceRoot(const QString &requestedPath, QString *root, QString *relative)
{
    if (!QDir::isAbsolutePath(requestedPath) || pathHasDotDotComponent(requestedPath)
        || requestedPath.contains(QChar::Null) || !permittedExternalPath(requestedPath)) {
        return false;
    }
    const QString path = QDir::cleanPath(requestedPath);
    struct CanonicalPrefix { const char *alias; const char *canonical; };
    static constexpr CanonicalPrefix canonicalPrefixes[] = {
        {"/mnt", "/var/mnt"}, {"/opt", "/var/opt"}, {"/srv", "/var/srv"}};
    for (const CanonicalPrefix &prefix : canonicalPrefixes) {
        const QString alias = QString::fromLatin1(prefix.alias);
        const QString expected = QString::fromLatin1(prefix.canonical);
        QString suffix;
        if (relativeBelow(path, alias, &suffix)) {
            const QString canonicalAlias = canonicalPath(alias, nullptr);
            if (canonicalAlias.isEmpty()) return false;
            const QString resolved = QDir(canonicalAlias).filePath(suffix);
            if (!permittedExternalPath(resolved)) return false;
            *root = canonicalAlias;
            *relative = suffix;
            return true;
        }
        if (relativeBelow(path, expected, &suffix)) {
            const QString canonicalAlias = canonicalPath(alias, nullptr);
            if (canonicalAlias == expected) {
                *root = expected;
                *relative = suffix;
                return true;
            }
        }
    }
    static const QStringList externalPrefixes = {
        QStringLiteral("/run/media"), QStringLiteral("/media"), QStringLiteral("/tmp")};
    for (const QString &prefix : externalPrefixes) {
        QString suffix;
        if (!relativeBelow(path, prefix, &suffix)) continue;
        const QString canonicalPrefix = canonicalPath(prefix, nullptr);
        if (canonicalPrefix.isEmpty()) return false;
        const QString resolved = QDir(canonicalPrefix).filePath(suffix);
        if (!permittedExternalPath(resolved)) return false;
        *root = canonicalPrefix;
        *relative = suffix;
        return true;
    }
    return false;
}

bool permittedStagingPath(const QString &path, const LaunchManifest &manifest)
{
    return isCallerHomeDescendant(path, manifest);
}

bool sameInode(const struct stat &left, const struct stat &right)
{
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

bool pinValidatedDirectory(const QString &requestedPath,
                       const LaunchManifest &manifest,
                       bool staging,
                       bool allowHomeRoot,
                       UniqueFd *fd,
                       QString *canonical,
                       struct stat *pinnedStat,
                       QString *error,
                       int *errorNumber)
{
    if (!QDir::isAbsolutePath(requestedPath) || pathHasDotDotComponent(requestedPath)
        || requestedPath.contains(QChar::Null)) {
        return setError(error, errorNumber, EINVAL,
                        QStringLiteral("Invalid directory path: %1").arg(requestedPath));
    }
    const bool allowed = staging ? permittedStagingPath(requestedPath, manifest)
                                 : permittedSourcePath(requestedPath, manifest, allowHomeRoot);
    if (!allowed) {
        return setError(error, errorNumber, EACCES,
                        QStringLiteral("Directory is outside the caller's permitted roots: %1").arg(requestedPath));
    }

    const HomePathMapping home = mapHomeRelativePath(requestedPath, manifest.declaredHome,
                                                     manifest.canonicalHome);
    QString rootPath;
    QString relativePath;
    if (home.kind == HomePathKind::Relative) {
        rootPath = manifest.canonicalHome;
        relativePath = home.relativePath;
    } else if (!staging && allowHomeRoot && home.kind == HomePathKind::Invalid
               && isCallerHomeRoot(requestedPath, manifest)) {
        rootPath = manifest.canonicalHome;
    } else if (!staging && home.kind == HomePathKind::External) {
        if (!externalSourceRoot(requestedPath, &rootPath, &relativePath)) {
            return setError(error, errorNumber, EACCES,
                            QStringLiteral("Directory is outside the caller's permitted roots: %1")
                                .arg(requestedPath));
        }
    } else {
        return setError(error, errorNumber, EACCES,
                        QStringLiteral("Directory is outside the caller's permitted roots: %1").arg(requestedPath));
    }

    const QString resolved = relativePath.isEmpty()
        ? QDir::cleanPath(rootPath) : QDir(rootPath).filePath(relativePath);
    if (!(staging ? permittedStagingPath(resolved, manifest)
                   : permittedSourcePath(resolved, manifest, allowHomeRoot))) {
        return setError(error, errorNumber, EACCES,
                        QStringLiteral("Directory resolves outside the caller's permitted roots: %1")
                            .arg(requestedPath));
    }
    const int base = SecureFs::openExistingDirNoFollow(rootPath);
    if (base < 0) {
        return setError(error, errorNumber, -base,
                        QStringLiteral("Could not securely open directory root %1: %2")
                            .arg(rootPath, systemError(-base)));
    }
    UniqueFd baseFd(base);
    int opened = -1;
    if (relativePath.isEmpty()) {
        opened = ::fcntl(base, F_DUPFD_CLOEXEC, 3);
        if (opened < 0) opened = -errno;
    } else {
        const QStringList components = relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        opened = SecureFs::openDirBelow(base, components, false, 0, 0);
    }
    if (opened < 0) {
        return setError(error, errorNumber, -opened,
                        QStringLiteral("Could not securely open directory %1: %2")
                            .arg(requestedPath, systemError(-opened)));
    }
    UniqueFd result(opened);
    struct stat fdStat{};
    struct stat pathStat{};
    const QByteArray encoded = requestedPath.toLocal8Bit();
    if (::fstat(result.get(), &fdStat) != 0 || ::stat(encoded.constData(), &pathStat) != 0) {
        const int value = errno;
        return setError(error, errorNumber, value,
                        QStringLiteral("Could not verify directory identity for %1: %2")
                            .arg(requestedPath, systemError(value)));
    }
    const bool callerOwned = staging || isCallerHomeDescendant(requestedPath, manifest)
        || isCallerHomeDescendant(resolved, manifest)
        || (allowHomeRoot && (isCallerHomeRoot(requestedPath, manifest) || isCallerHomeRoot(resolved, manifest)));
    if (!S_ISDIR(fdStat.st_mode) || !sameInode(fdStat, pathStat)
        || (callerOwned && fdStat.st_uid != manifest.callerUid)) {
        return setError(error, errorNumber, EACCES,
                        QStringLiteral("Directory is not a stable directory with permitted ownership: %1")
                            .arg(requestedPath));
    }
    if (staging && ((fdStat.st_mode & S_IRUSR) == 0 || (fdStat.st_mode & S_IXUSR) == 0)) {
        return setError(error, errorNumber, EACCES,
                        QStringLiteral("Staging directory is not readable by its owner: %1").arg(requestedPath));
    }
    if (canonical) *canonical = resolved;
    if (pinnedStat) *pinnedStat = fdStat;
    fd->reset(result.release());
    return true;
}
int readableTree(int directoryFd)
{
    const int readableDirectory = ::openat(directoryFd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (readableDirectory < 0) return errno;
    DIR *directory = ::fdopendir(readableDirectory);
    if (!directory) {
        const int value = errno;
        ::close(readableDirectory);
        return value;
    }
    int result = 0;
    for (;;) {
        errno = 0;
        dirent *entry = ::readdir(directory);
        if (!entry) {
            if (errno != 0) result = errno;
            break;
        }
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
        struct stat before{};
        if (::fstatat(::dirfd(directory), entry->d_name, &before, AT_SYMLINK_NOFOLLOW) != 0) {
            result = errno;
            break;
        }
        if (S_ISDIR(before.st_mode)) {
            const int child = ::openat(::dirfd(directory), entry->d_name,
                                       O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0) {
                result = errno;
                break;
            }
            struct stat after{};
            if (::fstat(child, &after) != 0) {
                result = errno;
                ::close(child);
                break;
            }
            if (!sameInode(before, after)) {
                result = ESTALE;
                ::close(child);
                break;
            }
            result = readableTree(child);
            ::close(child);
            if (result != 0) break;
        } else if (S_ISREG(before.st_mode)) {
            const int child = ::openat(::dirfd(directory), entry->d_name,
                                       O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0) {
                result = errno;
                break;
            }
            struct stat after{};
            if (::fstat(child, &after) != 0) {
                result = errno;
                ::close(child);
                break;
            }
            if (!S_ISREG(after.st_mode) || !sameInode(before, after)) {
                result = ESTALE;
                ::close(child);
                break;
            }
            ::close(child);
        } else if (S_ISLNK(before.st_mode)) {
            std::array<char, PATH_MAX + 1> target{};
            const ssize_t length = ::readlinkat(::dirfd(directory), entry->d_name, target.data(), PATH_MAX);
            if (length < 0 || length == PATH_MAX) {
                result = length < 0 ? errno : ENAMETOOLONG;
                break;
            }
        } else {
            result = EOPNOTSUPP;
            break;
        }
    }
    ::closedir(directory);
    return result;
}

bool reapChild(pid_t child, int *status);
int checkTreeReadableAsCaller(int sourceFd, const QString &requestedPath,
                              const QString &canonicalPath, const Account &caller,
                              bool recursive)
{
    int sockets[2];
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0) return errno;
    UniqueFd parentSocket(sockets[0]);
    UniqueFd childSocket(sockets[1]);
    const pid_t parentPid = ::getpid();
    const pid_t child = ::fork();
    if (child < 0) return errno;
    if (child == 0) {
        parentSocket.reset();
        int result = 0;
        if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) {
            result = errno;
        } else if (::getppid() != parentPid) {
            result = EPIPE;
        } else if (::initgroups(caller.username.toLocal8Bit().constData(), caller.gid) != 0
                   || ::setresgid(caller.gid, caller.gid, caller.gid) != 0
                   || ::setresuid(caller.uid, caller.uid, caller.uid) != 0) {
            result = errno;
        } else {
            struct __user_cap_header_struct capabilityHeader{};
            capabilityHeader.version = _LINUX_CAPABILITY_VERSION_3;
            struct __user_cap_data_struct emptyCapabilities[2]{};
            if (::syscall(SYS_capset, &capabilityHeader, emptyCapabilities) != 0) {
                result = errno;
            } else if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
                result = errno;
            } else {
                uid_t realUid, effectiveUid, savedUid;
                gid_t realGid, effectiveGid, savedGid;
                if (::getresuid(&realUid, &effectiveUid, &savedUid) != 0
                    || ::getresgid(&realGid, &effectiveGid, &savedGid) != 0
                    || realUid != caller.uid || effectiveUid != caller.uid || savedUid != caller.uid
                    || realGid != caller.gid || effectiveGid != caller.gid || savedGid != caller.gid) {
                    result = EPERM;
                } else {
                    auto openAsCaller = [&](const QString &path, UniqueFd *opened) {
                        const QByteArray encodedPath = path.toLocal8Bit();
                        const int fd = ::open(encodedPath.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                        if (fd < 0) return errno;
                        opened->reset(fd);
                        struct stat callerStat{};
                        struct stat pinnedStat{};
                        if (::fstat(fd, &callerStat) != 0 || ::fstat(sourceFd, &pinnedStat) != 0) {
                            return errno;
                        }
                        return sameInode(callerStat, pinnedStat) ? 0 : ESTALE;
                    };
                    UniqueFd requestedSource;
                    result = openAsCaller(requestedPath, &requestedSource);
                    UniqueFd canonicalSource;
                    if (result == 0 && canonicalPath != requestedPath) {
                        result = openAsCaller(canonicalPath, &canonicalSource);
                    }
                    if (result == 0 && recursive) {
                        result = readableTree(requestedSource.get());
                    } else if (result == 0
                               && ::syscall(SYS_faccessat2, requestedSource.get(), "", R_OK | X_OK,
                                            AT_EMPTY_PATH | AT_EACCESS) != 0) {
                        result = errno;
                    }
                }
            }
        }
        (void)::send(childSocket.get(), &result, sizeof(result), MSG_NOSIGNAL);
        _exit(result == 0 ? 0 : 1);
    }
    childSocket.reset();
    int result = EIO;
    ssize_t received;
    do {
        received = ::recv(parentSocket.get(), &result, sizeof(result), 0);
    } while (received < 0 && errno == EINTR);
    int status = -1;
    if (!reapChild(child, &status)) return errno ? errno : ECHILD;
    if (received != sizeof(result) || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return received == sizeof(result) && result != 0 ? result : EIO;
    }
    return result;
}

bool loadAccount(uid_t uid, const QString &name, Account *account, int *errorNumber)
{
    struct passwd *pw = name.isEmpty() ? ::getpwuid(uid) : ::getpwnam(name.toLocal8Bit().constData());
    if (!pw) {
        if (errorNumber) *errorNumber = errno ? errno : ENOENT;
        return false;
    }
    account->username = QString::fromLocal8Bit(pw->pw_name);
    account->declaredHome = QString::fromLocal8Bit(pw->pw_dir);
    account->shell = QString::fromLocal8Bit(pw->pw_shell ? pw->pw_shell : "");
    account->uid = pw->pw_uid;
    account->gid = pw->pw_gid;
    int canonicalError = 0;
    account->canonicalHome = canonicalPath(account->declaredHome, &canonicalError);
    if (account->canonicalHome.isEmpty()) {
        if (errorNumber) *errorNumber = canonicalError ? canonicalError : ENOENT;
        return false;
    }
    if (errorNumber) *errorNumber = 0;
    return true;
}

bool verifyManifestIdentity(const LaunchManifest &manifest, Account *caller, Account *target,
                            QString *error, int *errorNumber)
{
    int lookupError = 0;
    if (!loadAccount(manifest.callerUid, {}, caller, &lookupError)
        || caller->uid != manifest.callerUid || caller->gid != manifest.callerGid
        || caller->declaredHome != manifest.declaredHome || caller->canonicalHome != manifest.canonicalHome) {
        return setError(error, errorNumber, lookupError ? lookupError : ESTALE,
                        QStringLiteral("Authenticated caller account changed since launch request"));
    }
    if (!loadAccount(0, manifest.targetUsername, target, &lookupError)
        || target->uid == 0 || target->uid != manifest.targetUid || target->gid != manifest.targetGid
        || target->declaredHome != manifest.targetDeclaredHome
        || target->canonicalHome != manifest.targetCanonicalHome) {
        return setError(error, errorNumber, lookupError ? lookupError : ESTALE,
                        QStringLiteral("Target account changed since launch request"));
    }
    return true;
}

QString defaultTargetAlias(const DirectoryRecord &record, const LaunchManifest &manifest)
{
    if (!record.targetAlias.isEmpty()) return record.targetAlias;
    const HomePathMapping source = mapHomeRelativePath(record.source, manifest.declaredHome, manifest.canonicalHome);
    if (source.kind == HomePathKind::Relative) return source.relativePath;
    if (source.kind != HomePathKind::External) return {};
    if (record.mode == QStringLiteral("copy")) {
        return QStringLiteral(".couchplay/copies/")
            + DirectoryPlan::dataDirectoryStagingSlug(record.source, manifest.declaredHome);
    }
    return QStringLiteral(".couchplay/mounts/") + record.source.mid(1);
}

bool targetAbsolutePath(const QString &alias, const LaunchManifest &manifest, QString *path)
{
    if (!DirectoryPlan::isSafeRelativeDirectoryPath(alias)) return false;
    const QString declaredPath = QDir(manifest.targetDeclaredHome).filePath(alias);
    const HomePathMapping mapped = mapHomeRelativePath(declaredPath,
                                                        manifest.targetDeclaredHome,
                                                        manifest.targetCanonicalHome);
    if (mapped.kind != HomePathKind::Relative || mapped.relativePath != alias) return false;
    *path = QDir(manifest.targetCanonicalHome).filePath(mapped.relativePath);
    return true;
}

bool prepareHostDirectories(const LaunchManifest &manifest,
                            const Account &caller,
                            int userNamespaceFd,
                            std::vector<PreparedDirectory> *prepared,
                            QString *error,
                            int *errorNumber)
{
    prepared->reserve(static_cast<size_t>(manifest.directories.size()));
    for (const DirectoryRecord &record : manifest.directories) {
        PreparedDirectory item;
        item.record = record;
        item.targetAlias = defaultTargetAlias(record, manifest);
        if (record.mode != QStringLiteral("acl")) {
            QString ignored;
            if (!targetAbsolutePath(item.targetAlias, manifest, &ignored)) {
                return setError(error, errorNumber, EINVAL,
                                QStringLiteral("Invalid target mapping for source %1").arg(record.source));
            }
        }
        if (!pinValidatedDirectory(record.source, manifest, false, !record.targetAlias.isEmpty(),
                                   &item.sourceFd, &item.canonicalSource, &item.sourceStat, error, errorNumber)) {
            return false;
        }
        if (record.mode == QStringLiteral("copy") || record.mode == QStringLiteral("bind")
            || record.mode == QStringLiteral("overlay")) {
            const bool recursive = record.mode == QStringLiteral("copy");
            const int unreadable = checkTreeReadableAsCaller(item.sourceFd.get(), record.source,
                                                              item.canonicalSource, caller, recursive);
            if (unreadable != 0) {
                const QString reason = recursive
                    ? QStringLiteral("Source is not fully readable by the authenticated caller: %1 (%2)")
                    : QStringLiteral("Source directory is not readable and traversable by the authenticated caller: %1 (%2)");
                return setError(error, errorNumber, unreadable,
                                reason.arg(record.source, systemError(unreadable)));
            }
        }
        if (record.mode == QStringLiteral("bind")) {
            const int mountFd = SecureFs::cloneMountFd(item.sourceFd.get());
            if (mountFd < 0) {
                return setError(error, errorNumber, -mountFd,
                                QStringLiteral("Could not clone source mount for %1: %2")
                                    .arg(record.source, systemError(-mountFd)));
            }
            item.detachedMountFd.reset(mountFd);
        } else if (record.mode == QStringLiteral("overlay")) {
            const int mountFd = SecureFs::cloneIdmappedLowerFd(item.sourceFd.get(), userNamespaceFd);
            if (mountFd < 0) {
                return setError(error, errorNumber, -mountFd,
                                QStringLiteral("Overlay ownership mapping is not supported for %1; use Copy files")
                                    .arg(record.source));
            }
            item.detachedMountFd.reset(mountFd);
        }
        QString stagingCanonical;
        if (!record.stagingPath.isEmpty()) {
            struct stat stagingStat{};
            if (!pinValidatedDirectory(record.stagingPath, manifest, true, false, &item.stagingFd,
                                       &stagingCanonical, &stagingStat, error, errorNumber)) {
                return false;
            }
        }
        if (item.stagingFd) {
            const int unreadable = checkTreeReadableAsCaller(item.stagingFd.get(), record.stagingPath,
                                                              stagingCanonical, caller, true);
            if (unreadable != 0) {
                return setError(error, errorNumber, unreadable,
                                QStringLiteral("Staging tree is not readable by the authenticated caller: %1 (%2)")
                                    .arg(record.stagingPath, systemError(unreadable)));
            }
        }
        prepared->push_back(std::move(item));
    }
    return true;
}

bool writeProcFile(const QString &path, const QByteArray &content, int *errorNumber)
{
    const QByteArray encoded = path.toLocal8Bit();
    const int fd = ::open(encoded.constData(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errorNumber) *errorNumber = errno;
        return false;
    }
    qsizetype offset = 0;
    while (offset < content.size()) {
        const ssize_t count = ::write(fd, content.constData() + offset,
                                      static_cast<size_t>(content.size() - offset));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            const int value = count < 0 ? errno : EIO;
            ::close(fd);
            if (errorNumber) *errorNumber = value;
            return false;
        }
        offset += count;
    }
    if (::close(fd) != 0) {
        if (errorNumber) *errorNumber = errno;
        return false;
    }
    if (errorNumber) *errorNumber = 0;
    return true;
}

QByteArray idMap(uid_t callerId, uid_t targetId)
{
    std::vector<quint32> special{static_cast<quint32>(callerId)};
    if (callerId != targetId) special.push_back(static_cast<quint32>(targetId));
    std::sort(special.begin(), special.end());
    QByteArray contents;
    quint64 next = 0;
    const quint64 limit = UINT32_MAX;
    for (const quint32 id : special) {
        if (next < id) {
            contents += QByteArray::number(next) + ' ' + QByteArray::number(next) + ' '
                + QByteArray::number(static_cast<quint64>(id) - next);
            contents.append(char(10));
        }
        const quint32 hostId = id == callerId ? static_cast<quint32>(targetId)
                                               : static_cast<quint32>(callerId);
        contents += QByteArray::number(id) + ' ' + QByteArray::number(hostId) + QByteArrayLiteral(" 1");
        contents.append(char(10));
        next = static_cast<quint64>(id) + 1;
    }
    if (next < limit) {
        contents += QByteArray::number(next) + ' ' + QByteArray::number(next) + ' '
            + QByteArray::number(limit - next);
        contents.append(char(10));
    }
    return contents;
}

bool sendFd(int socketFd, int fd)
{
    char byte = 'F';
    struct iovec vector { &byte, sizeof(byte) };
    char control[CMSG_SPACE(sizeof(int))]{};
    struct msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(header), &fd, sizeof(fd));
    return ::sendmsg(socketFd, &message, 0) == sizeof(byte);
}

int receiveFd(int socketFd)
{
    char byte = 0;
    struct iovec vector { &byte, sizeof(byte) };
    char control[CMSG_SPACE(sizeof(int))]{};
    struct msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    ssize_t received;
    do {
        received = ::recvmsg(socketFd, &message, MSG_CMSG_CLOEXEC);
    } while (received < 0 && errno == EINTR);

    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    const bool valid = received == sizeof(byte) && byte == 'F'
        && !(message.msg_flags & (MSG_CTRUNC | MSG_TRUNC))
        && header && header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS
        && header->cmsg_len == CMSG_LEN(sizeof(int)) && !CMSG_NXTHDR(&message, header);
    if (!valid) {
        for (struct cmsghdr *item = header; item; item = CMSG_NXTHDR(&message, item)) {
            if (item->cmsg_level == SOL_SOCKET && item->cmsg_type == SCM_RIGHTS
                && item->cmsg_len >= CMSG_LEN(0)) {
                const size_t count = (item->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                const int *receivedFds = reinterpret_cast<const int *>(CMSG_DATA(item));
                for (size_t i = 0; i < count; ++i) ::close(receivedFds[i]);
            }
        }
        return -1;
    }
    int fd = -1;
    std::memcpy(&fd, CMSG_DATA(header), sizeof(fd));
    return fd;
}

bool reapChild(pid_t child, int *status)
{
    while (::waitpid(child, status, 0) < 0) {
        if (errno == EINTR) continue;
        return false;
    }
    return true;
}

UniqueFd createMappedUserNamespace(uid_t callerUid, uid_t targetUid, gid_t callerGid, gid_t targetGid,
                                   QString *error, int *errorNumber)
{
    if (callerUid == static_cast<uid_t>(UINT32_MAX) || targetUid == static_cast<uid_t>(UINT32_MAX)
        || callerGid == static_cast<gid_t>(UINT32_MAX) || targetGid == static_cast<gid_t>(UINT32_MAX)) {
        setError(error, errorNumber, EINVAL, QStringLiteral("Invalid identity for id-mapping user namespace"));
        return UniqueFd();
    }
    int sockets[2];
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0) {
        setError(error, errorNumber, errno, QStringLiteral("Could not create id-mapping control socket"));
        return {};
    }
    UniqueFd parentSocket(sockets[0]);
    UniqueFd childSocket(sockets[1]);
    const pid_t parentPid = ::getpid();
    const pid_t child = ::fork();
    if (child < 0) {
        setError(error, errorNumber, errno, QStringLiteral("Could not create id-mapping namespace child"));
        return {};
    }
    if (child == 0) {
        parentSocket.reset();
        if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) {
            const int value = errno;
            (void)::send(childSocket.get(), &value, sizeof(value), 0);
            _exit(1);
        }
        if (::getppid() != parentPid) {
            const int value = EPIPE;
            (void)::send(childSocket.get(), &value, sizeof(value), 0);
            _exit(1);
        }
        if (::unshare(CLONE_NEWUSER) != 0) {
            const int value = errno;
            (void)::send(childSocket.get(), &value, sizeof(value), 0);
            _exit(1);
        }
        const int ready = 0;
        if (::send(childSocket.get(), &ready, sizeof(ready), 0) != sizeof(ready)) _exit(1);
        char command = 0;
        if (::recv(childSocket.get(), &command, sizeof(command), 0) != sizeof(command) || command != 'G') _exit(1);
        const int namespaceFd = ::open("/proc/self/ns/user", O_RDONLY | O_CLOEXEC);
        if (namespaceFd < 0) _exit(1);
        const bool sent = sendFd(childSocket.get(), namespaceFd);
        ::close(namespaceFd);
        _exit(sent ? 0 : 1);
    }
    childSocket.reset();
    int childStatus = -1;
    int childError = EIO;
    ssize_t started;
    do {
        started = ::recv(parentSocket.get(), &childError, sizeof(childError), 0);
    } while (started < 0 && errno == EINTR);
    if (started != sizeof(childError)) {
        childError = started < 0 ? errno : EIO;
        ::kill(child, SIGKILL);
        reapChild(child, &childStatus);
        setError(error, errorNumber, childError, QStringLiteral("Could not create id-mapping user namespace"));
        return {};
    }
    if (childError != 0) {
        reapChild(child, &childStatus);
        setError(error, errorNumber, childError,
                 QStringLiteral("Overlay ownership mapping is not supported by this kernel"));
        return {};
    }
    int mappingError = 0;
    const QString proc = QStringLiteral("/proc/%1/").arg(child);
    QByteArray denySetgroups = QByteArrayLiteral("deny");
    denySetgroups.append(char(10));
    if (!writeProcFile(proc + QStringLiteral("setgroups"), denySetgroups, &mappingError)
        || !writeProcFile(proc + QStringLiteral("uid_map"), idMap(callerUid, targetUid), &mappingError)
        || !writeProcFile(proc + QStringLiteral("gid_map"), idMap(callerGid, targetGid), &mappingError)) {
        const char abort = 'X';
        (void)::send(parentSocket.get(), &abort, sizeof(abort), 0);
        ::kill(child, SIGKILL);
        reapChild(child, &childStatus);
        setError(error, errorNumber, mappingError,
                 QStringLiteral("Overlay ownership mapping is not supported by this kernel"));
        return {};
    }
    const char go = 'G';
    if (::send(parentSocket.get(), &go, sizeof(go), 0) != sizeof(go)) {
        const int value = errno ? errno : EIO;
        ::kill(child, SIGKILL);
        reapChild(child, &childStatus);
        setError(error, errorNumber, value, QStringLiteral("Could not complete id-mapping namespace setup"));
        return {};
    }
    UniqueFd namespaceFd(receiveFd(parentSocket.get()));
    const bool childReaped = reapChild(child, &childStatus);
    if (!namespaceFd || !childReaped || !WIFEXITED(childStatus) || WEXITSTATUS(childStatus) != 0) {
        const int value = errno ? errno : EIO;
        setError(error, errorNumber, value, QStringLiteral("Could not retain id-mapping namespace"));
        return {};
    }
    return namespaceFd;
}

bool ensureMountpoint(int parentFd, const QString &leaf, uid_t uid, gid_t gid,
                      QString *error, int *errorNumber)
{
    const QByteArray name = leaf.toUtf8();
    struct stat st{};
    if (::fstatat(parentFd, name.constData(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISDIR(st.st_mode) || st.st_uid != uid) {
            return setError(error, errorNumber, EACCES,
                            QStringLiteral("Mount target is not a player-owned directory: %1").arg(leaf));
        }
        const int fd = ::openat(parentFd, name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) return setError(error, errorNumber, errno, QStringLiteral("Could not open mount target %1").arg(leaf));
        ::close(fd);
        return true;
    }
    if (errno != ENOENT) return setError(error, errorNumber, errno, QStringLiteral("Could not inspect mount target %1").arg(leaf));
    if (::mkdirat(parentFd, name.constData(), 0755) != 0) {
        return setError(error, errorNumber, errno, QStringLiteral("Could not create mount target %1").arg(leaf));
    }
    const int fd = ::openat(parentFd, name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return setError(error, errorNumber, errno, QStringLiteral("Could not open new mount target %1").arg(leaf));
    UniqueFd target(fd);
    if (::fchown(fd, uid, gid) != 0 || ::fchmod(fd, 0755) != 0) {
        const int value = errno;
        return setError(error, errorNumber, value, QStringLiteral("Could not assign mount target ownership %1").arg(leaf));
    }
    return true;
}

int openTargetParent(int homeFd, const QString &alias, const LaunchManifest &manifest,
                     const QString &targetPath, QString *leaf, QString *error, int *errorNumber)
{
    const QStringList parts = alias.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        setError(error, errorNumber, EINVAL, QStringLiteral("Directory projection cannot replace player HOME"));
        return -EINVAL;
    }
    *leaf = parts.constLast();
    QStringList parentParts = parts;
    parentParts.removeLast();
    const int parent = SecureFs::openDirBelow(homeFd, parentParts, true, manifest.targetUid, manifest.targetGid);
    if (parent < 0) {
        setError(error, errorNumber, -parent,
                 QStringLiteral("Could not securely prepare target parent %1: %2")
                     .arg(targetPath, systemError(-parent)));
        return parent;
    }
    return parent;
}

bool ensureOverlayStorage(const QString &username, const QString &sourcePath,
                          const LaunchManifest &manifest, QString *upperPath, QString *workPath,
                          QString *error, int *errorNumber)
{
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(sourcePath.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
    const QString basePath = QStringLiteral("/var/lib/couchplay/overlays/%1/%2").arg(username, hash);
    const int varLib = SecureFs::openExistingDirNoFollow(QStringLiteral("/var/lib"));
    if (varLib < 0) {
        return setError(error, errorNumber, -varLib, QStringLiteral("Could not open protected overlay storage: %1")
                                                       .arg(systemError(-varLib)));
    }
    UniqueFd protectedParent(varLib);
    struct stat parentStat{};
    if (::fstat(varLib, &parentStat) != 0 || !S_ISDIR(parentStat.st_mode)
        || parentStat.st_uid != 0 || parentStat.st_gid != 0 || (parentStat.st_mode & 0022) != 0) {
        return setError(error, errorNumber, EPERM,
                        QStringLiteral("Overlay storage parent is not protected"));
    }
    const QStringList protectedComponents{QStringLiteral("couchplay"), QStringLiteral("overlays"), username, hash};
    for (const QString &component : protectedComponents) {
        const int next = SecureFs::openDirBelow(protectedParent.get(), {component}, true, 0, 0);
        if (next < 0) {
            return setError(error, errorNumber, -next,
                            QStringLiteral("Could not create protected overlay storage: %1")
                                .arg(systemError(-next)));
        }
        UniqueFd nextFd(next);
        struct stat componentStat{};
        if (::fstat(next, &componentStat) != 0 || !S_ISDIR(componentStat.st_mode)
            || componentStat.st_uid != 0 || componentStat.st_gid != 0 || (componentStat.st_mode & 0022) != 0) {
            return setError(error, errorNumber, EPERM,
                            QStringLiteral("Overlay storage path contains an unprotected parent"));
        }
        protectedParent = std::move(nextFd);
    }
    const int base = protectedParent.get();
    const int upper = SecureFs::openDirBelow(base, {QStringLiteral("upper")}, true,
                                              manifest.targetUid, manifest.targetGid);
    if (upper < 0) {
        return setError(error, errorNumber, -upper,
                        QStringLiteral("Could not open overlay upper directory: %1").arg(systemError(-upper)));
    }
    UniqueFd upperFd(upper);
    const int work = SecureFs::openDirBelow(base, {QStringLiteral("work")}, true,
                                             manifest.targetUid, manifest.targetGid);
    if (work < 0) {
        return setError(error, errorNumber, -work,
                        QStringLiteral("Could not open overlay work directory: %1").arg(systemError(-work)));
    }
    UniqueFd workFd(work);
    struct stat upperStat{};
    struct stat workStat{};
    if (::fstat(upper, &upperStat) != 0 || ::fstat(work, &workStat) != 0
        || !S_ISDIR(upperStat.st_mode) || !S_ISDIR(workStat.st_mode)
        || upperStat.st_uid != manifest.targetUid || workStat.st_uid != manifest.targetUid
        || upperStat.st_gid != manifest.targetGid || workStat.st_gid != manifest.targetGid
        || upperStat.st_dev != workStat.st_dev) {
        return setError(error, errorNumber, EPERM,
                        QStringLiteral("Overlay upper/work ownership or filesystem is invalid"));
    }
    if (::fchmod(upper, 0700) != 0 || ::fchmod(work, 0700) != 0) {
        return setError(error, errorNumber, errno,
                        QStringLiteral("Could not protect overlay upper/work permissions"));
    }
    *upperPath = basePath + QStringLiteral("/upper");
    *workPath = basePath + QStringLiteral("/work");
    return true;
}

bool prepareUnprivilegedExecutable(const LaunchManifest &manifest, QString *error, int *errorNumber)
{
    int lookupError = 0;
    const QString path = canonicalPath(manifest.gamescopeArgv.constFirst(), &lookupError);
    const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
    if (path.isEmpty() || slash < 0) {
        return setError(error, errorNumber, lookupError ? lookupError : EINVAL,
                        QStringLiteral("Could not resolve Gamescope executable"));
    }
    const int parentFd = SecureFs::openExistingDirNoFollow(slash == 0 ? QStringLiteral("/") : path.left(slash));
    if (parentFd < 0) return setError(error, errorNumber, -parentFd, QStringLiteral("Could not open Gamescope parent"));
    UniqueFd parent(parentFd);
    UniqueFd executable(::openat(parent.get(), path.mid(slash + 1).toLocal8Bit().constData(),
                                 O_PATH | O_NOFOLLOW | O_CLOEXEC));
    struct stat status{};
    if (!executable || ::fstat(executable.get(), &status) != 0 || !S_ISREG(status.st_mode)) {
        return setError(error, errorNumber, errno ? errno : EINVAL, QStringLiteral("Invalid Gamescope executable"));
    }
    UniqueFd mountFd(static_cast<int>(::syscall(SYS_open_tree, executable.get(), "",
                                               AT_EMPTY_PATH | OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC)));
    struct mount_attr attributes{};
    // Ignore distro file capabilities only in this instance; keep all process capability sets empty.
    attributes.attr_set = MOUNT_ATTR_NOSUID | MOUNT_ATTR_RDONLY;
    if (!mountFd || ::syscall(SYS_mount_setattr, mountFd.get(), "", AT_EMPTY_PATH,
                              &attributes, sizeof(attributes)) != 0
        || ::syscall(SYS_move_mount, mountFd.get(), "", executable.get(), "",
                     MOVE_MOUNT_F_EMPTY_PATH | MOVE_MOUNT_T_EMPTY_PATH) != 0) {
        return setError(error, errorNumber, errno,
                        QStringLiteral("Could not prepare unprivileged Gamescope executable: %1").arg(systemError(errno)));
    }
    return true;
}

bool prepareMounts(const LaunchManifest &manifest, const QString &launchToken,
                   std::vector<PreparedDirectory> *prepared, UniqueFd *targetHomeFd,
                   QString *workingDirectory, QString *error, int *errorNumber)
{
    if (::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
        return setError(error, errorNumber, errno, QStringLiteral("Could not make instance mounts private: %1")
                                                       .arg(systemError(errno)));
    }
    if (!prepareUnprivilegedExecutable(manifest, error, errorNumber)) return false;
    const int home = SecureFs::openExistingDirNoFollow(manifest.targetCanonicalHome);
    if (home < 0) {
        return setError(error, errorNumber, -home,
                        QStringLiteral("Could not securely open player HOME: %1").arg(systemError(-home)));
    }
    targetHomeFd->reset(home);
    struct stat homeStat{};
    if (::fstat(home, &homeStat) != 0 || !S_ISDIR(homeStat.st_mode) || homeStat.st_uid != manifest.targetUid) {
        return setError(error, errorNumber, errno ? errno : EACCES,
                        QStringLiteral("Player HOME is not owned by the target account"));
    }
    for (const PreparedDirectory &item : *prepared) {
        if (item.record.mode == QStringLiteral("bind") && !SecureFs::mountApiAvailable()) {
            return setError(error, errorNumber, EOPNOTSUPP,
                            QStringLiteral("FD-relative mounts are not supported for %1").arg(item.record.source));
        }
        if (item.record.mode == QStringLiteral("overlay")
            && (!SecureFs::mountApiAvailable() || !SecureFs::overlayMountApiAvailable())) {
            return setError(error, errorNumber, EOPNOTSUPP,
                            QStringLiteral("Overlay ownership mapping is not supported for %1; use Copy files")
                                .arg(item.record.source));
        }
    }
    for (const PreparedDirectory &item : *prepared) {
        if ((item.record.mode == QStringLiteral("bind") || item.record.mode == QStringLiteral("overlay"))
            && !item.detachedMountFd) {
            return setError(error, errorNumber, EOPNOTSUPP,
                            QStringLiteral("Could not pin source mount for %1").arg(item.record.source));
        }
    }

    for (PreparedDirectory &item : *prepared) {
        const QString &mode = item.record.mode;
        if (mode == QStringLiteral("acl")) {
            // Original-path shares must name the same host inode in this instance.
            const int visible = SecureFs::openExistingDirNoFollow(item.canonicalSource);
            if (visible < 0) {
                return setError(error, errorNumber, -visible,
                                QStringLiteral("ACL source %1 is hidden from this instance namespace")
                                    .arg(item.record.source));
            }
            UniqueFd visibleFd(visible);
            struct stat current{};
            if (::fstat(visible, &current) != 0 || !sameInode(current, item.sourceStat)) {
                return setError(error, errorNumber, ESTALE,
                                QStringLiteral("ACL source changed before preparation: %1").arg(item.record.source));
            }
            const int result = SecureFs::applyReadExecuteAclTree(visible, manifest.targetUid);
            if (result != 0) {
                return setError(error, errorNumber, -result,
                                QStringLiteral("Could not grant shared-directory access to %1: %2")
                                    .arg(item.record.source, systemError(-result)));
            }
            continue;
        }
        QString targetPath;
        if (!targetAbsolutePath(item.targetAlias, manifest, &targetPath)) {
            return setError(error, errorNumber, EINVAL,
                            QStringLiteral("Invalid projected target for %1").arg(item.record.source));
        }
        QString leaf;
        const int parent = openTargetParent(home, item.targetAlias, manifest, targetPath, &leaf, error, errorNumber);
        if (parent < 0) return false;
        UniqueFd parentFd(parent);
        if (mode == QStringLiteral("copy")) {
            const QString scratchName = DirectoryPlan::copyScratchName(leaf, launchToken);
            const int result = SecureFs::replaceTreeContents(item.sourceFd.get(), -1, parent,
                                                              leaf, scratchName, manifest.targetUid, manifest.targetGid);
            if (result != 0) {
                const int value = -result;
                return setError(error, errorNumber, value,
                                QStringLiteral("Failed to copy directory %1 into player HOME: %2")
                                    .arg(item.record.source, systemError(value)));
            }
            if (item.stagingFd) {
                const QByteArray name = leaf.toUtf8();
                const int target = ::openat(parent, name.constData(),
                                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                if (target < 0) {
                    return setError(error, errorNumber, errno,
                                    QStringLiteral("Could not open copied directory for staging: %1")
                                        .arg(item.record.source));
                }
                UniqueFd targetFd(target);
                const int staged = SecureFs::copyTreeContents(item.stagingFd.get(), target,
                                                               manifest.targetUid, manifest.targetGid);
                if (staged != 0) {
                    return setError(error, errorNumber, -staged,
                                    QStringLiteral("Could not apply staged files to %1: %2")
                                        .arg(item.record.source, systemError(-staged)));
                }
            }
            continue;
        }
        if (!ensureMountpoint(parent, leaf, manifest.targetUid, manifest.targetGid, error, errorNumber)) return false;
        if (mode == QStringLiteral("bind")) {
            const int result = SecureFs::bindMountFd(item.detachedMountFd.get(), parent, leaf);
            if (result != 0) {
                return setError(error, errorNumber, -result,
                                QStringLiteral("Could not bind source %1: %2")
                                    .arg(item.record.source, systemError(-result)));
            }
            item.detachedMountFd.reset();
            continue;
        }
        QString upperPath;
        QString workPath;
        if (!ensureOverlayStorage(manifest.targetUsername, item.record.source, manifest,
                                  &upperPath, &workPath, error, errorNumber)) return false;
        const int mounted = SecureFs::overlayMountFd(item.detachedMountFd.get(),
                                                       upperPath, workPath, parent, leaf);
        if (mounted != 0) {
            if (mounted == -EOPNOTSUPP || mounted == -ENOSYS || mounted == -EINVAL
                || mounted == -ENODEV || mounted == -EPERM) {
                return setError(error, errorNumber, -mounted,
                                QStringLiteral("Overlay ownership mapping is not supported for %1; use Copy files")
                                    .arg(item.record.source));
            }
            return setError(error, errorNumber, -mounted,
                            QStringLiteral("Could not attach private overlay for %1: %2")
                                .arg(item.record.source, systemError(-mounted)));
        }
        item.detachedMountFd.reset();
        if (item.stagingFd) {
            const int target = ::openat(parent, leaf.toUtf8().constData(),
                                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (target < 0) {
                return setError(error, errorNumber, errno,
                                QStringLiteral("Could not open projected overlay for staging: %1")
                                    .arg(item.record.source));
            }
            UniqueFd targetFd(target);
            const int copied = SecureFs::copyTreeContents(item.stagingFd.get(), target,
                                                           manifest.targetUid, manifest.targetGid);
            if (copied != 0) {
                return setError(error, errorNumber, -copied,
                                QStringLiteral("Could not apply staged files to %1: %2")
                                    .arg(item.record.source, systemError(-copied)));
            }
        }
    }

    if (manifest.workingDirectory.isEmpty()
        || manifest.workingDirectory == manifest.targetDeclaredHome
        || manifest.workingDirectory == manifest.targetCanonicalHome) {
        *workingDirectory = manifest.targetCanonicalHome;
    } else {
        const HomePathMapping inHome = mapHomeRelativePath(manifest.workingDirectory,
                                                           manifest.targetDeclaredHome,
                                                           manifest.targetCanonicalHome);
        if (inHome.kind == HomePathKind::Relative && !inHome.relativePath.isEmpty()) {
            *workingDirectory = QDir(manifest.targetCanonicalHome).filePath(inHome.relativePath);
        } else {
            bool mapped = false;
            for (const PreparedDirectory &item : *prepared) {
                const QStringList roots{item.record.source, item.canonicalSource};
                for (const QString &root : roots) {
                    if (!pathWithin(manifest.workingDirectory, root)) continue;
                    const QString suffix = manifest.workingDirectory.mid(root.size());
                    if (item.record.mode == QStringLiteral("acl")) {
                        *workingDirectory = QDir(item.canonicalSource).filePath(suffix.mid(1));
                    } else {
                        QString targetRoot;
                        if (!targetAbsolutePath(item.targetAlias, manifest, &targetRoot)) continue;
                        *workingDirectory = suffix.isEmpty() ? targetRoot : targetRoot + suffix;
                    }
                    mapped = true;
                    break;
                }
                if (mapped) break;
            }
            if (!mapped) {
                const HomePathMapping callerHome = mapHomeRelativePath(manifest.workingDirectory,
                                                                       manifest.declaredHome,
                                                                       manifest.canonicalHome);
                if ((callerHome.kind == HomePathKind::Relative && !callerHome.relativePath.isEmpty())
                    || isCallerHomeRoot(manifest.workingDirectory, manifest)) {
                    *workingDirectory = manifest.workingDirectory;
                    mapped = true;
                }
            }
            if (!mapped && permittedExternalPath(manifest.workingDirectory)
                && !pathWithin(manifest.workingDirectory, QStringLiteral("/tmp"))) {
                *workingDirectory = manifest.workingDirectory;
                mapped = true;
            }
            if (!mapped) {
                return setError(error, errorNumber, EACCES,
                                QStringLiteral("Selected working directory is neither projected nor in a permitted original path"));
            }
        }
    }

    // Source and detached mount descriptors are closed before privilege drop.
    for (PreparedDirectory &item : *prepared) {
        item.sourceFd.reset();
        item.stagingFd.reset();
    }
    return true;
}

bool readableDirectoryFd(int fd)
{
    // Real and effective IDs are both the target account after the checked
    // privilege drop, so faccessat's real-ID semantics match Gamescope.
    return ::faccessat(fd, ".", R_OK | X_OK, 0) == 0;
}

bool checkPlayerAccess(const LaunchManifest &manifest, std::vector<PreparedDirectory> *prepared,
                       const QString &workingDirectory, QString *error, int *errorNumber)
{
    for (const PreparedDirectory &item : *prepared) {
        if (item.record.mode == QStringLiteral("acl")) {
            const int source = SecureFs::openExistingDirNoFollow(item.canonicalSource);
            if (source < 0) {
                return setError(error, errorNumber, -source,
                                QStringLiteral("ACL source %1 is hidden from this instance namespace")
                                    .arg(item.record.source));
            }
            UniqueFd sourceFd(source);
            struct stat current{};
            if (::fstat(source, &current) != 0 || !sameInode(current, item.sourceStat)
                || !readableDirectoryFd(source)) {
                return setError(error, errorNumber, errno ? errno : EACCES,
                                QStringLiteral("Player cannot read the original ACL source %1")
                                    .arg(item.record.source));
            }
            continue;
        }
        QString target;
        if (!targetAbsolutePath(item.targetAlias, manifest, &target)) {
            return setError(error, errorNumber, EINVAL,
                            QStringLiteral("Invalid target path for source %1").arg(item.record.source));
        }
        const int fd = SecureFs::openExistingDirNoFollow(target);
        if (fd < 0) {
            return setError(error, errorNumber, -fd,
                            QStringLiteral("Player cannot traverse projected source %1: %2")
                                .arg(item.record.source, systemError(-fd)));
        }
        UniqueFd targetFd(fd);
        if (!readableDirectoryFd(fd)) {
            return setError(error, errorNumber, errno ? errno : EACCES,
                            QStringLiteral("Player cannot read projected source %1: %2")
                                .arg(item.record.source, systemError(errno)));
        }
    }
    const QByteArray working = workingDirectory.toLocal8Bit();
    if (::faccessat(AT_FDCWD, working.constData(), X_OK, 0) != 0) {
        return setError(error, errorNumber, errno,
                        QStringLiteral("Player cannot access selected working directory %1")
                            .arg(workingDirectory));
    }
    return true;
}

bool dropPrivileges(const Account &target, QString *error, int *errorNumber)
{
    if (::initgroups(target.username.toLocal8Bit().constData(), target.gid) != 0) {
        return setError(error, errorNumber, errno, QStringLiteral("Could not initialize supplementary groups"));
    }
    for (int capability = 0; capability <= CAP_LAST_CAP; ++capability) {
        if (::prctl(PR_CAPBSET_DROP, capability, 0, 0, 0) != 0 && errno != EINVAL) {
            return setError(error, errorNumber, errno, QStringLiteral("Could not drop capability bounding set"));
        }
    }
    if (::setresgid(target.gid, target.gid, target.gid) != 0
        || ::setresuid(target.uid, target.uid, target.uid) != 0) {
        return setError(error, errorNumber, errno, QStringLiteral("Could not drop instance credentials"));
    }
    if (::prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0) != 0) {
        return setError(error, errorNumber, errno, QStringLiteral("Could not clear keep-capabilities state"));
    }
    struct __user_cap_header_struct header{};
    header.version = _LINUX_CAPABILITY_VERSION_3;
    header.pid = 0;
    struct __user_cap_data_struct empty[2]{};
    if (::syscall(SYS_capset, &header, empty) != 0) {
        return setError(error, errorNumber, errno, QStringLiteral("Could not clear process capabilities"));
    }
    if (::prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) != 0) {
        return setError(error, errorNumber, errno, QStringLiteral("Could not clear ambient capabilities"));
    }
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        return setError(error, errorNumber, errno, QStringLiteral("Could not set no_new_privs"));
    }
    uid_t realUid, effectiveUid, savedUid;
    gid_t realGid, effectiveGid, savedGid;
    if (::getresuid(&realUid, &effectiveUid, &savedUid) != 0
        || ::getresgid(&realGid, &effectiveGid, &savedGid) != 0
        || realUid != target.uid || effectiveUid != target.uid || savedUid != target.uid
        || realGid != target.gid || effectiveGid != target.gid || savedGid != target.gid) {
        return setError(error, errorNumber, EPERM, QStringLiteral("Instance credential drop verification failed"));
    }
    struct __user_cap_data_struct current[2]{};
    if (::syscall(SYS_capget, &header, current) != 0
        || current[0].effective || current[0].permitted || current[0].inheritable
        || current[1].effective || current[1].permitted || current[1].inheritable) {
        return setError(error, errorNumber, EPERM, QStringLiteral("Instance capabilities remain after privilege drop"));
    }
    if (::prctl(PR_GET_KEEPCAPS, 0, 0, 0, 0) != 0) {
        return setError(error, errorNumber, EPERM, QStringLiteral("Keep-capabilities state remains enabled"));
    }
    for (int capability = 0; capability <= CAP_LAST_CAP; ++capability) {
        const int present = ::prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_IS_SET, capability, 0, 0);
        if (present > 0) return setError(error, errorNumber, EPERM, QStringLiteral("Ambient capabilities remain after privilege drop"));
        if (present < 0 && errno != EINVAL) {
            return setError(error, errorNumber, errno, QStringLiteral("Could not verify ambient capabilities"));
        }
    }
    for (int capability = 0; capability <= CAP_LAST_CAP; ++capability) {
        const int present = ::prctl(PR_CAPBSET_READ, capability, 0, 0, 0);
        if (present > 0) return setError(error, errorNumber, EPERM, QStringLiteral("Capability bounding set is not empty"));
        if (present < 0 && errno != EINVAL) {
            return setError(error, errorNumber, errno, QStringLiteral("Could not verify capability bounding set"));
        }
    }
    if (::prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1) {
        return setError(error, errorNumber, EPERM, QStringLiteral("no_new_privs verification failed"));
    }
    return true;
}

bool verifyHelperExecutable(const QString &path, QString *error, int *errorNumber)
{
    if (!QDir::isAbsolutePath(path) || path.contains(QChar::Null)) {
        return setError(error, errorNumber, EINVAL, QStringLiteral("Invalid helper executable path"));
    }
    struct stat helper{};
    struct stat running{};
    const QByteArray encoded = path.toLocal8Bit();
    if (::stat(encoded.constData(), &helper) != 0 || ::stat("/proc/self/exe", &running) != 0) {
        return setError(error, errorNumber, errno, QStringLiteral("Could not verify running helper executable"));
    }
    if (!S_ISREG(helper.st_mode) || helper.st_uid != 0 || (helper.st_mode & 0022) != 0
        || (helper.st_mode & 0111) == 0 || !sameInode(helper, running)) {
        return setError(error, errorNumber, EPERM, QStringLiteral("Running helper executable is not trusted"));
    }
    return true;
}

bool sendSystemdNotification(const QByteArray &message)
{
    const char *path = ::getenv("NOTIFY_SOCKET");
    if (!path || !*path) return false;
    const size_t pathLength = std::strlen(path);
    struct sockaddr_un address{};
    if (pathLength == 0 || pathLength >= sizeof(address.sun_path)) return false;
    const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    address.sun_family = AF_UNIX;
    socklen_t addressLength = 0;
    if (path[0] == '@') {
        std::memcpy(address.sun_path + 1, path + 1, pathLength - 1);
        addressLength = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + pathLength);
    } else {
        std::memcpy(address.sun_path, path, pathLength + 1);
        addressLength = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + pathLength + 1);
    }
    const bool sent = ::sendto(fd, message.constData(), static_cast<size_t>(message.size()), MSG_NOSIGNAL,
                               reinterpret_cast<sockaddr *>(&address), addressLength) == message.size();
    ::close(fd);
    return sent;
}

int failBootstrap(const QString &message, int errorNumber)
{
    QString safe = message;
    safe.replace(QLatin1Char('\n'), QLatin1Char(' '));
    safe.replace(QLatin1Char('\r'), QLatin1Char(' '));
    QByteArray status = QByteArrayLiteral("STATUS=") + safe.toUtf8()
        + QByteArrayLiteral("\nERRNO=") + QByteArray::number(errorNumber > 0 ? errorNumber : EIO) + '\n';
    (void)sendSystemdNotification(status);
    std::fprintf(stderr, "couchplay-helper instance preparation failed: %s\n", safe.toLocal8Bit().constData());
    return 1;
}


QMap<QString, QString> environmentForExec(const LaunchManifest &manifest,
                                          const QString &workingDirectory)
{
    // The manifest is the complete, root-protected game environment. Do not
    // leak the bootstrap/systemd environment into the unprivileged process.
    QMap<QString, QString> environment;
    for (auto it = manifest.environment.cbegin(); it != manifest.environment.cend(); ++it) {
        environment.insert(it.key(), it.value());
    }
    environment.insert(QStringLiteral("HOME"), manifest.targetDeclaredHome);
    environment.insert(QStringLiteral("USER"), manifest.targetUsername);
    environment.insert(QStringLiteral("LOGNAME"), manifest.targetUsername);
    environment.insert(QStringLiteral("PWD"), workingDirectory);
    environment.remove(QStringLiteral("NOTIFY_SOCKET"));
    environment.remove(QStringLiteral("WATCHDOG_PID"));
    environment.remove(QStringLiteral("WATCHDOG_USEC"));
    return environment;
}

bool execGamescope(const LaunchManifest &manifest, const QString &workingDirectory,
                   QString *error, int *errorNumber)
{
    const QByteArray executable = manifest.gamescopeArgv.constFirst().toLocal8Bit();
    std::vector<QByteArray> arguments;
    arguments.reserve(static_cast<size_t>(manifest.gamescopeArgv.size()));
    for (const QString &value : manifest.gamescopeArgv) arguments.push_back(value.toLocal8Bit());
    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1);
    for (QByteArray &value : arguments) argv.push_back(value.data());
    argv.push_back(nullptr);

    const QMap<QString, QString> environment = environmentForExec(manifest, workingDirectory);
    std::vector<QByteArray> entries;
    entries.reserve(static_cast<size_t>(environment.size()));
    for (auto it = environment.cbegin(); it != environment.cend(); ++it) {
        entries.push_back(it.key().toLocal8Bit() + '=' + it.value().toLocal8Bit());
    }
    std::vector<char *> envp;
    envp.reserve(entries.size() + 1);
    for (QByteArray &entry : entries) envp.push_back(entry.data());
    envp.push_back(nullptr);

    if (::chdir(workingDirectory.toLocal8Bit().constData()) != 0) {
        return setError(error, errorNumber, errno,
                        QStringLiteral("Could not enter selected working directory %1").arg(workingDirectory));
    }
    const char *notifyValue = ::getenv("NOTIFY_SOCKET");
    const bool hadNotifySocket = notifyValue != nullptr;
    const QByteArray savedNotifySocket = hadNotifySocket ? QByteArray(notifyValue) : QByteArray();
    if (!sendSystemdNotification(QByteArrayLiteral("READY=1\nSTATUS=Preparing game\n"))) {
        return setError(error, errorNumber, EIO, QStringLiteral("Could not notify systemd that preparation is ready"));
    }
    if (::unsetenv("NOTIFY_SOCKET") != 0 || ::unsetenv("WATCHDOG_PID") != 0 || ::unsetenv("WATCHDOG_USEC") != 0) {
        const int value = errno;
        if (hadNotifySocket) (void)::setenv("NOTIFY_SOCKET", savedNotifySocket.constData(), 1);
        return setError(error, errorNumber, value,
                        QStringLiteral("Could not remove systemd notification environment before game exec"));
    }
    ::execve(executable.constData(), argv.data(), envp.data());
    const int value = errno;
    if (hadNotifySocket) (void)::setenv("NOTIFY_SOCKET", savedNotifySocket.constData(), 1);
    if (errorNumber) *errorNumber = value;
    if (error) *error = QStringLiteral("Could not execute Gamescope %1: %2")
                             .arg(manifest.gamescopeArgv.constFirst(), systemError(value));
    return false;
}
}

namespace InstanceBootstrap
{
bool cleanupCopyScratchDirectories(const LaunchManifest &manifest, const QString &launchToken, QString *error)
{
    if (::geteuid() != 0 || !DirectoryPlan::isLaunchToken(launchToken)) {
        return setError(error, nullptr, EIO, QStringLiteral("Invalid copy-scratch cleanup caller or launch token"));
    }
    const int homeFd = SecureFs::openExistingDirNoFollow(manifest.targetCanonicalHome);
    if (homeFd < 0) {
        if (-homeFd == ENOENT) return true;
        return setError(error, nullptr, EIO, QStringLiteral("Could not securely open player HOME for copy-scratch cleanup: %1")
                               .arg(QString::fromLocal8Bit(strerror(-homeFd))));
    }
    struct stat homeStat{};
    if (::fstat(homeFd, &homeStat) != 0) {
        const int value = errno;
        ::close(homeFd);
        return setError(error, nullptr, EIO, QStringLiteral("Could not inspect player HOME before copy-scratch cleanup: %1")
                               .arg(QString::fromLocal8Bit(strerror(value))));
    }
    if (!S_ISDIR(homeStat.st_mode) || homeStat.st_uid != manifest.targetUid) {
        ::close(homeFd);
        return setError(error, nullptr, EIO, QStringLiteral("Player HOME identity changed before copy-scratch cleanup"));
    }

    for (qsizetype index = 0; index < manifest.directories.size(); ++index) {
        const DirectoryRecord &record = manifest.directories.at(index);
        if (record.mode != QStringLiteral("copy")) continue;
        const QString alias = defaultTargetAlias(record, manifest);
        QStringList components = alias.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (!DirectoryPlan::isSafeRelativeDirectoryPath(alias) || components.isEmpty()) {
            ::close(homeFd);
            return setError(error, nullptr, EIO, QStringLiteral("Copy target has no safe leaf for scratch cleanup"));
        }
        const QString leaf = components.takeLast();
        const QString scratchName = DirectoryPlan::copyScratchName(leaf, launchToken);
        if (scratchName.isEmpty()) {
            ::close(homeFd);
            return setError(error, nullptr, EIO, QStringLiteral("Could not derive trusted copy-scratch name"));
        }
        const int parentFd = SecureFs::openDirBelow(homeFd, components, false, 0, 0);
        if (parentFd < 0) {
            if (-parentFd == ENOENT) continue;
            const int value = -parentFd;
            ::close(homeFd);
            return setError(error, nullptr, EIO, QStringLiteral("Could not securely open copy target parent: %1")
                                   .arg(QString::fromLocal8Bit(strerror(value))));
        }
        const int removed = SecureFs::removeCopyScratchAt(parentFd, scratchName);
        ::close(parentFd);
        if (removed != 0) {
            ::close(homeFd);
            return setError(error, nullptr, EIO, QStringLiteral("Refusing or unable to remove copy scratch %1: %2")
                                   .arg(scratchName, QString::fromLocal8Bit(strerror(-removed))));
        }
    }
    ::close(homeFd);
    return true;
}


int run(const QString &token, const QString &helperExecutablePath)
{
    if (::geteuid() != 0) return failBootstrap(QStringLiteral("Instance bootstrap requires effective UID 0"), EPERM);
    if (!DirectoryPlan::isLaunchToken(token)) return failBootstrap(QStringLiteral("Invalid launch token"), EINVAL);
    QString helperError;
    int failureErrno = EIO;
    if (!verifyHelperExecutable(helperExecutablePath, &helperError, &failureErrno)) {
        return failBootstrap(helperError, failureErrno);
    }
    LaunchManifest manifest;
    QString manifestError;
    if (!DirectoryPlan::readLaunchManifest(token, &manifest, &manifestError)) {
        return failBootstrap(manifestError.isEmpty() ? QStringLiteral("Could not read launch manifest") : manifestError,
                             EINVAL);
    }

    UniqueFd instanceNamespace(::open("/proc/self/ns/mnt", O_RDONLY | O_CLOEXEC));
    UniqueFd hostNamespace(::open("/proc/1/ns/mnt", O_RDONLY | O_CLOEXEC));
    if (!instanceNamespace || !hostNamespace) {
        return failBootstrap(QStringLiteral("Could not open required mount namespace handles"), errno);
    }
    if (::setns(hostNamespace.get(), CLONE_NEWNS) != 0) {
        return failBootstrap(QStringLiteral("Could not enter host mount namespace to pin launch sources"), errno);
    }

    Account caller;
    Account target;
    std::vector<PreparedDirectory> prepared;
    QString error;
    UniqueFd userNamespaceFd;
    bool valid = verifyManifestIdentity(manifest, &caller, &target, &error, &failureErrno);
    QString overlaySource;
    if (valid) {
        for (const DirectoryRecord &record : manifest.directories) {
            if (record.mode == QStringLiteral("overlay")) {
                overlaySource = record.source;
                break;
            }
        }
    }
    if (valid && !overlaySource.isEmpty()) {
        userNamespaceFd = createMappedUserNamespace(manifest.callerUid, manifest.targetUid,
                                                     manifest.callerGid, manifest.targetGid,
                                                     &error, &failureErrno);
        if (!userNamespaceFd) {
            if (failureErrno <= 0) failureErrno = EOPNOTSUPP;
            error = QStringLiteral("Overlay ownership mapping is not supported for %1; use Copy files")
                        .arg(overlaySource);
            valid = false;
        }
    }
    if (valid) {
        valid = prepareHostDirectories(manifest, caller, userNamespaceFd.get(), &prepared,
                                       &error, &failureErrno);
    }
    userNamespaceFd.reset();
    const int capturedErrno = failureErrno;
    const QString capturedError = error;
    if (::setns(instanceNamespace.get(), CLONE_NEWNS) != 0) {
        return failBootstrap(QStringLiteral("Could not return to instance mount namespace"), errno);
    }
    hostNamespace.reset();
    if (!valid) return failBootstrap(capturedError, capturedErrno);

    QString workingDirectory;
    UniqueFd targetHomeFd;
    if (!prepareMounts(manifest, token, &prepared, &targetHomeFd, &workingDirectory, &error, &failureErrno)) {
        return failBootstrap(error, failureErrno);
    }
    if (!dropPrivileges(target, &error, &failureErrno)) return failBootstrap(error, failureErrno);
    if (!checkPlayerAccess(manifest, &prepared, workingDirectory, &error, &failureErrno)) {
        return failBootstrap(error, failureErrno);
    }
    prepared.clear();
    targetHomeFd.reset();
    instanceNamespace.reset();
    if (!execGamescope(manifest, workingDirectory, &error, &failureErrno)) {
        const int result = failBootstrap(error, failureErrno);
        ::unsetenv("NOTIFY_SOCKET");
        ::unsetenv("WATCHDOG_PID");
        ::unsetenv("WATCHDOG_USEC");
        return result;
    }
    return 0;
}
}
