// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "SecureFs.h"

#include <QDebug>
#include <QRandomGenerator>

#include <dirent.h>
#include <errno.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <string>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <vector>
#ifndef O_PATH
#define O_PATH 010000000
#endif
#ifndef O_TMPFILE
#define O_TMPFILE (020000000 | O_DIRECTORY)
#endif
#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif
namespace SecureFs
{

int openBaseDir(const QString &absolutePath)
{
    int fd = ::open(absolutePath.toLocal8Bit().constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    return fd >= 0 ? fd : -errno;
}

int openExistingDirNoFollow(const QString &absolutePath)
{
    const QStringList parts = absolutePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    int baseFd = ::open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (baseFd < 0) {
        return -errno;
    }
    int result = openDirBelow(baseFd, parts, false, 0, 0);
    ::close(baseFd);
    return result;
}

int openDirBelow(int baseFd, const QStringList &parts, bool create, uid_t uid, gid_t gid, ChownMode chownMode)
{
    int current = ::fcntl(baseFd, F_DUPFD_CLOEXEC, 3);
    if (current < 0) {
        return -errno;
    }

    const int total = static_cast<int>(parts.size());
    int index = -1;
    for (const QString &part : parts) {
        index++;
        if (part.isEmpty() || part == QStringLiteral("..")) {
            ::close(current);
            return -EINVAL;
        }
        if (part == QStringLiteral(".")) {
            continue; // refers to the base directory itself
        }
        const bool finalComponent = (index == total - 1);

        int next = ::openat(current, part.toUtf8().constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && create && (errno == ENOENT)) {
            if (::mkdirat(current, part.toUtf8().constData(), 0755) != 0) {
                int err = errno;
                ::close(current);
                return -err;
            }
            next = ::openat(current, part.toUtf8().constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (next >= 0
                && (chownMode == ChownMode::All || (chownMode == ChownMode::FinalOnly && finalComponent))
                && (::fchown(next, uid, gid) != 0)) {
                int err = errno; // ownership failure is fatal: never mutate with wrong owner
                ::close(next);
                ::close(current);
                return -err;
            }
        }
        ::close(current);
        if (next < 0) {
            return -errno;
        }
        current = next;
    }

    return current;
}

static int removeTree(int dirFd)
{
    DIR *dir = ::fdopendir(::fcntl(dirFd, F_DUPFD_CLOEXEC, 3));
    if (!dir) {
        return -errno;
    }

    struct dirent *entry;
    int result = 0;
    while ((entry = ::readdir(dir)) != nullptr) {
        const char *name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }

        struct stat st;
        // fstatat without AT_SYMLINK_NOFOLLOW would stat through links;
        // with it, S_ISLNK is observable and unlinkat removes the link only
        if (::fstatat(dirFd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            result = -errno;
            break;
        }

        if (S_ISDIR(st.st_mode)) {
            int childFd = ::openat(dirFd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (childFd < 0) {
                result = -errno;
                break;
            }
            result = removeTree(childFd);
            ::close(childFd);
            if (result != 0) {
                break;
            }
            if (::unlinkat(dirFd, name, AT_REMOVEDIR) != 0) {
                result = -errno;
                break;
            }
        } else {
            if (::unlinkat(dirFd, name, 0) != 0) {
                result = -errno;
                break;
            }
        }
    }

    ::closedir(dir);
    return result;
}

int removeTreeAt(int dirFd)
{
    return removeTree(dirFd);
}

static int copyEntry(int srcDirFd, const char *name, int dstDirFd, uid_t uid, gid_t gid);

static int copyTree(int srcDirFd, int dstDirFd, uid_t uid, gid_t gid)
{
    DIR *dir = ::fdopendir(::fcntl(srcDirFd, F_DUPFD_CLOEXEC, 3));
    if (!dir) {
        return -errno;
    }

    struct dirent *entry;
    int result = 0;
    while ((entry = ::readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        result = copyEntry(srcDirFd, entry->d_name, dstDirFd, uid, gid);
        if (result != 0) {
            break;
        }
    }

    ::closedir(dir);
    return result;
}

static int copyEntry(int srcDirFd, const char *name, int dstDirFd, uid_t uid, gid_t gid)
{
    const QString entryName = QString::fromLocal8Bit(name);

    struct stat st;
    if (::fstatat(srcDirFd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        return -errno;
    }

    if (S_ISLNK(st.st_mode)) {
        std::vector<char> target(st.st_size + 1, '\0');
        ssize_t len = ::readlinkat(srcDirFd, name, target.data(), st.st_size);
        if (len < 0) {
            return -errno;
        }
        target[len] = '\0';
        if (::symlinkat(target.data(), dstDirFd, name) != 0) {
            if (errno != EEXIST) {
                return -errno;
            }
            // Merge overwrite: replace the stale entry — a regular file, a
            // symlink, or an empty directory — so an updated link target
            // reaches the destination instead of being silently dropped
            if (::unlinkat(dstDirFd, name, 0) != 0 && ::unlinkat(dstDirFd, name, AT_REMOVEDIR) != 0) {
                return -errno;
            }
            if (::symlinkat(target.data(), dstDirFd, name) != 0) {
                return -errno;
            }
        }
        // A root-owned link inside the player's tree is confusing at best;
        // assign the link itself to the target user without following it
        if (::fchownat(dstDirFd, name, uid, gid, AT_SYMLINK_NOFOLLOW) != 0) {
            qWarning() << "SecureFs: Could not assign ownership of copied symlink"
                                               << entryName << strerror(errno);
        }
        return 0;
    }

    if (S_ISDIR(st.st_mode)) {
        int srcChild = ::openat(srcDirFd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (srcChild < 0) {
            return -errno;
        }
        if (::mkdirat(dstDirFd, name, st.st_mode & 07777) != 0 && errno != EEXIST) {
            int err = errno;
            ::close(srcChild);
            return -err;
        }
        int dstChild = ::openat(dstDirFd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (dstChild < 0) {
            int err = errno;
            ::close(srcChild);
            return -err;
        }
        ::fchown(dstChild, uid, gid);
        int result = copyTree(srcChild, dstChild, uid, gid);
        ::close(srcChild);
        ::close(dstChild);
        return result;
    }

    if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        // Quick pre-filter: the authoritative classification happens on the
        // opened FD below (a writable source can swap the entry between the
        // fstatat check and the open). O_NONBLOCK guarantees a swapped-in
        // FIFO cannot block the open.
        qWarning() << "SecureFs: refusing to copy special file" << entryName;
        return -EOPNOTSUPP;
    }

    // Open before classifying, then classify the FD itself: open FIFOs and
    // device nodes with O_NONBLOCK|O_NOFOLLOW so a swapped-in FIFO can never
    // block the synchronous helper
    int srcFile = ::openat(srcDirFd, name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (srcFile < 0) {
        return -errno;
    }

    struct stat openedSt;
    if (::fstat(srcFile, &openedSt) != 0) {
        int err = errno;
        ::close(srcFile);
        return -err;
    }
    if (!S_ISREG(openedSt.st_mode)) {
        // Whatever this is now (raced FIFO, device, socket...), we don't copy
        // it — and the open cannot have blocked thanks to O_NONBLOCK
        ::close(srcFile);
        qWarning() << "SecureFs: refusing to copy non-regular file" << entryName;
        return -EOPNOTSUPP;
    }

    int dstFile = ::openat(dstDirFd, name, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, openedSt.st_mode & 07777);
    if (dstFile < 0) {
        int err = errno;
        ::close(srcFile);
        return -err;
    }

    char buffer[65536];
    ssize_t bytesRead;
    int result = 0;
    while ((bytesRead = ::read(srcFile, buffer, sizeof(buffer))) > 0) {
        ssize_t offset = 0;
        while (offset < bytesRead) {
            ssize_t written = ::write(dstFile, buffer + offset, bytesRead - offset);
            if (written < 0) {
                result = -errno;
                break;
            }
            offset += written;
        }
        if (result != 0) {
            break;
        }
    }
    if (bytesRead < 0 && result == 0) {
        result = -errno;
    }
    ::close(srcFile);

    if (result == 0) {
        ::fchown(dstFile, uid, gid);
        ::fchmod(dstFile, openedSt.st_mode & 07777);
    }
    ::close(dstFile);
    return result;
}

int copyTreeContents(int srcDirFd, int dstDirFd, uid_t uid, gid_t gid)
{
    return copyTree(srcDirFd, dstDirFd, uid, gid);
}

int writeFileAt(int parentFd, const QString &name, const QByteArray &content, uid_t uid, gid_t gid, mode_t mode)
{
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..")
        || name.contains(QLatin1Char('/'))) {
        return -EINVAL;
    }

    const auto sameInodeIdentity = [](const struct stat &left, const struct stat &right) {
        return left.st_dev == right.st_dev && left.st_ino == right.st_ino && left.st_uid == right.st_uid
            && (left.st_mode & S_IFMT) == (right.st_mode & S_IFMT);
    };
    const auto sameFileIdentity = [&](const struct stat &left, const struct stat &right) {
        return sameInodeIdentity(left, right) && left.st_nlink == right.st_nlink;
    };

    // Build the replacement in an anonymous inode. Unlike a named O_EXCL
    // temporary, O_TMPFILE cannot be hard-linked by a user between creation
    // and the write, so no root-owned write can be redirected through a raced
    // hardlink. A bounded named fallback is used only on filesystems without
    // O_TMPFILE; it remains root-owned and mode 0600 until all writes finish.
    bool namedTemporary = false;
    QByteArray temporaryName;
    int fileFd = ::openat(parentFd, ".", O_TMPFILE | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, mode);
    if (fileFd < 0 && (errno == EOPNOTSUPP || errno == ENOTSUP || errno == EINVAL)) {
        for (int attempt = 0; attempt < 8 && fileFd < 0; ++attempt) {
            temporaryName = QStringLiteral(".couchplay-write-%1-%2-%3")
                                 .arg(static_cast<qulonglong>(::getpid()))
                                 .arg(QRandomGenerator::global()->generate64(), 0, 16)
                                 .arg(attempt)
                                 .toLocal8Bit();
            fileFd = ::openat(parentFd,
                              temporaryName.constData(),
                              O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                              0600);
        }
        namedTemporary = fileFd >= 0;
    }
    if (fileFd < 0) {
        return -errno;
    }

    struct stat temporaryStat;
    if (::fstat(fileFd, &temporaryStat) != 0 || !S_ISREG(temporaryStat.st_mode)
        || (namedTemporary && temporaryStat.st_nlink != 1)) {
        const int error = errno;
        ::close(fileFd);
        // Preserve the named sibling when its inode cannot be validated.
        return error != 0 ? -error : -EINVAL;
    }
    auto cleanupTemporaryEntry = [&]() {
        struct stat current{};
        if (::fstatat(parentFd, temporaryName.constData(), &current, AT_SYMLINK_NOFOLLOW) == 0
            && sameInodeIdentity(temporaryStat, current)) {
            (void)::unlinkat(parentFd, temporaryName.constData(), 0);
        }
    };
    auto cleanupNamedTemporary = [&]() {
        if (!namedTemporary) return;
        cleanupTemporaryEntry();
    };
    qsizetype offset = 0;
    while (offset < content.size()) {
        const ssize_t written = ::write(fileFd, content.constData() + offset, content.size() - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int error = errno;
            cleanupNamedTemporary();
            ::close(fileFd);
            return -error;
        }
        offset += written;
    }

    if (::fstat(fileFd, &temporaryStat) != 0 || (namedTemporary && temporaryStat.st_nlink != 1)) {
        const int error = errno != 0 ? errno : EMLINK;
        cleanupNamedTemporary();
        ::close(fileFd);
        return -error;
    }
    if (::fchown(fileFd, uid, gid) != 0 || ::fchmod(fileFd, mode) != 0) {
        const int error = errno;
        cleanupNamedTemporary();
        ::close(fileFd);
        return -error;
    }
    if (::fstat(fileFd, &temporaryStat) != 0 || (namedTemporary && temporaryStat.st_nlink != 1)) {
        const int error = errno != 0 ? errno : EMLINK;
        cleanupNamedTemporary();
        ::close(fileFd);
        return -error;
    }

    const QByteArray nameBytes = name.toUtf8();
    const int targetFd = ::openat(parentFd, nameBytes.constData(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
    if (targetFd < 0 && errno != ENOENT) {
        const int error = errno;
        cleanupNamedTemporary();
        ::close(fileFd);
        return -error;
    }

    struct stat targetStat{};
    const bool targetExists = targetFd >= 0;
    if (targetExists) {
        if (::fstat(targetFd, &targetStat) != 0) {
            const int error = errno;
            cleanupNamedTemporary();
            ::close(targetFd);
            ::close(fileFd);
            return -error;
        }
        if (S_ISLNK(targetStat.st_mode)) {
            cleanupNamedTemporary();
            ::close(targetFd);
            ::close(fileFd);
            return -ELOOP;
        }
        if (!S_ISREG(targetStat.st_mode)) {
            cleanupNamedTemporary();
            ::close(targetFd);
            ::close(fileFd);
            return -EINVAL;
        }
        if (targetStat.st_uid != uid) {
            cleanupNamedTemporary();
            ::close(targetFd);
            ::close(fileFd);
            return -EPERM;
        }
        if (targetStat.st_nlink != 1) {
            cleanupNamedTemporary();
            ::close(targetFd);
            ::close(fileFd);
            return -EMLINK;
        }
    }

    if (!targetExists) {
        // Atomic no-follow, no-replace install. A target created concurrently
        // yields EEXIST without touching it.
        const int installResult = namedTemporary
            ? static_cast<int>(::syscall(SYS_renameat2,
                                          parentFd,
                                          temporaryName.constData(),
                                          parentFd,
                                          nameBytes.constData(),
                                          RENAME_NOREPLACE))
            : ::linkat(fileFd, "", parentFd, nameBytes.constData(), AT_EMPTY_PATH);
        if (installResult != 0) {
            const int error = errno;
            cleanupNamedTemporary();
            ::close(fileFd);
            return -error;
        }
        ::close(fileFd);
        (void)::fsync(parentFd);
        return 0;
    }

    // Link the prepared inode under a private name, then exchange it with the
    // validated target. Post-exchange identity checks make a target swap
    // observable. If the writable directory changed either name, preserve the
    // entries for explicit recovery rather than attempting a racy rollback.
    bool linked = namedTemporary;
    for (int attempt = 0; attempt < 8 && !linked; ++attempt) {
        temporaryName = QStringLiteral(".couchplay-write-%1-%2-%3")
                             .arg(static_cast<qulonglong>(::getpid()))
                             .arg(QRandomGenerator::global()->generate64(), 0, 16)
                             .arg(attempt)
                             .toLocal8Bit();
        if (::linkat(fileFd, "", parentFd, temporaryName.constData(), AT_EMPTY_PATH) == 0) {
            linked = true;
            break;
        }
        if (errno != EEXIST) {
            const int error = errno;
            cleanupNamedTemporary();
            ::close(targetFd);
            ::close(fileFd);
            return -error;
        }
    }
    ::close(fileFd);
    if (!linked) {
        ::close(targetFd);
        return -EEXIST;
    }

    struct stat linkedStat{};
    if (::fstatat(parentFd, temporaryName.constData(), &linkedStat, AT_SYMLINK_NOFOLLOW) != 0
        || !sameInodeIdentity(temporaryStat, linkedStat)) {
        const int error = errno;
        cleanupTemporaryEntry();
        ::close(targetFd);
        return error != 0 ? -error : -EAGAIN;
    }

    const int exchangeResult = static_cast<int>(::syscall(SYS_renameat2,
                                                           parentFd,
                                                           temporaryName.constData(),
                                                           parentFd,
                                                           nameBytes.constData(),
                                                           RENAME_EXCHANGE));
    if (exchangeResult != 0) {
        const int error = errno;
        cleanupTemporaryEntry();
        ::close(targetFd);
        return -error;
    }

    struct stat installedStat{};
    struct stat displacedStat{};
    const bool targetHasReplacement = ::fstatat(parentFd, nameBytes.constData(), &installedStat, AT_SYMLINK_NOFOLLOW) == 0
        && sameInodeIdentity(temporaryStat, installedStat);
    const bool targetWasDisplaced = ::fstatat(parentFd,
                                              temporaryName.constData(),
                                              &displacedStat,
                                              AT_SYMLINK_NOFOLLOW) == 0
        && sameFileIdentity(targetStat, displacedStat);
    if (!targetHasReplacement || !targetWasDisplaced) {
        // The directory is writable by the target user. A second exchange
        // cannot safely restore an inode after either name changed: the user
        // could replace the temporary entry before the exchange. Preserve
        // both names for explicit recovery instead of moving an unverified
        // entry over the target.
        ::close(targetFd);
        return -EAGAIN;
    }

    struct stat currentTemporary{};
    if (::fstatat(parentFd, temporaryName.constData(), &currentTemporary, AT_SYMLINK_NOFOLLOW) != 0
        || !sameFileIdentity(targetStat, currentTemporary)
        || ::unlinkat(parentFd, temporaryName.constData(), 0) != 0) {
        const int error = errno;
        ::close(targetFd);
        return error != 0 ? -error : -EAGAIN;
    }
    ::close(targetFd);
    (void)::fsync(parentFd);
    return 0;
}

// ---------------------------------------------------------------------------
// FD-anchored mounting
// ---------------------------------------------------------------------------

#ifndef SYS_open_tree
#define SYS_open_tree 428
#endif
#ifndef SYS_move_mount
#define SYS_move_mount 429
#endif
#ifndef SYS_fsopen
#define SYS_fsopen 430
#endif
#ifndef SYS_fsconfig
#define SYS_fsconfig 431
#endif
#ifndef SYS_fsmount
#define SYS_fsmount 432
#endif

#ifndef OPEN_TREE_CLONE
#define OPEN_TREE_CLONE 1
#endif
#ifndef OPEN_TREE_CLOEXEC
#define OPEN_TREE_CLOEXEC 4
#endif
#ifndef FSOPEN_CLOEXEC
#define FSOPEN_CLOEXEC 1
#endif
#ifndef FSMOUNT_CLOEXEC
#define FSMOUNT_CLOEXEC 1
#endif
#ifndef FSCONFIG_SET_STRING
#define FSCONFIG_SET_STRING 1
#endif
#ifndef FSCONFIG_CMD_CREATE
#define FSCONFIG_CMD_CREATE 6
#endif
#ifndef MOVE_MOUNT_F_EMPTY_PATH
#define MOVE_MOUNT_F_EMPTY_PATH 0x00000004
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif

bool mountApiAvailable()
{
    static int cached = -1;
    if (cached < 0) {
        const int fd = static_cast<int>(::syscall(SYS_open_tree, AT_FDCWD, "/", OPEN_TREE_CLOEXEC));
        if (fd >= 0) {
            ::close(fd);
            cached = 1;
        } else {
            cached = 0;
        }
    }
    return cached == 1;
}

bool overlayMountApiAvailable()
{
    static int cached = -1;
    if (cached < 0) {
        const int fd = static_cast<int>(::syscall(SYS_fsopen, "overlay", FSOPEN_CLOEXEC));
        if (fd >= 0) {
            ::close(fd);
            cached = 1;
        } else {
            cached = 0;
        }
    }
    return cached == 1;
}

int bindMountFd(const QString &canonicalSource, int targetParentFd, const QString &leafName)
{
    // Pin the source parent with a whole-chain no-follow walk, then clone the
    // source leaf with AT_SYMLINK_NOFOLLOW so a last-instant symlink swap
    // fails instead of being followed
    const int lastSlash = canonicalSource.lastIndexOf(QLatin1Char('/'));
    const QString sourceParent = lastSlash <= 0 ? QStringLiteral("/") : canonicalSource.left(lastSlash);
    const QString sourceLeaf = canonicalSource.mid(lastSlash + 1);

    int srcParentFd = openExistingDirNoFollow(sourceParent);
    if (srcParentFd < 0) {
        return srcParentFd;
    }
    int treeFd = static_cast<int>(::syscall(SYS_open_tree, srcParentFd, sourceLeaf.toUtf8().constData(),
                                            AT_SYMLINK_NOFOLLOW | OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC));
    ::close(srcParentFd);
    if (treeFd < 0) {
        return -errno;
    }

    if (::syscall(SYS_move_mount, treeFd, "", targetParentFd, leafName.toUtf8().constData(),
                  MOVE_MOUNT_F_EMPTY_PATH)
        != 0) {
        int err = errno;
        ::close(treeFd);
        return -err;
    }
    ::close(treeFd); // the mount stays attached; the fd was only for attaching
    return 0;
}

int overlayMountFd(int sourceDirFd, const QString &upperdir, const QString &workdir,
                   int targetParentFd, const QString &leafName)
{
    // Pin the lowerdir through the caller's validated source FD: the kernel
    // resolves this magic symlink to the FD's inode, bypassing any namespace
    // path a player could swap. The FD must stay open until this returns.
    const QString lowerdir = QStringLiteral("/proc/self/fd/%1").arg(QString::number(sourceDirFd));

    int fsFd = static_cast<int>(::syscall(SYS_fsopen, "overlay", FSOPEN_CLOEXEC));
    if (fsFd < 0) {
        return -errno;
    }

    auto setString = [fsFd](const char *key, const QString &value) -> int {
        if (::syscall(SYS_fsconfig, fsFd, FSCONFIG_SET_STRING, key, value.toUtf8().constData(), 0) != 0) {
            return -errno;
        }
        return 0;
    };

    int result = setString("lowerdir", lowerdir);
    if (result == 0) {
        result = setString("upperdir", upperdir);
    }
    if (result == 0) {
        result = setString("workdir", workdir);
    }
    // The new mount API requires an explicit create command to build the
    // superblock before fsmount() is legal
    if (result == 0 && ::syscall(SYS_fsconfig, fsFd, FSCONFIG_CMD_CREATE, nullptr, nullptr, 0) != 0) {
        result = -errno;
    }
    if (result != 0) {
        ::close(fsFd);
        return result;
    }

    int mntFd = static_cast<int>(::syscall(SYS_fsmount, fsFd, FSMOUNT_CLOEXEC, 0));
    ::close(fsFd);
    if (mntFd < 0) {
        return -errno;
    }

    if (::syscall(SYS_move_mount, mntFd, "", targetParentFd, leafName.toUtf8().constData(),
                  MOVE_MOUNT_F_EMPTY_PATH)
        != 0) {
        int err = errno;
        ::close(mntFd);
        return -err;
    }
    ::close(mntFd);
    return 0;
}

int umountAtFd(int targetParentFd, const QString &leafName)
{
    const QString procPath =
        QStringLiteral("/proc/self/fd/%1/%2").arg(QString::number(targetParentFd), leafName);
    if (::umount2(procPath.toLocal8Bit().constData(), 0) != 0) {
        if (::umount2(procPath.toLocal8Bit().constData(), MNT_DETACH) != 0) {
            return -errno;
        }
    }
    return 0;
}
}
