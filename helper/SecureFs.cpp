// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "SecureFs.h"

#include <QDir>

#include <dirent.h>
#include <errno.h>
#include <string>
#include <vector>

namespace SecureFs
{

int openBaseDir(const QString &absolutePath)
{
    int fd = ::open(absolutePath.toLocal8Bit().constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    return fd >= 0 ? fd : -errno;
}

int openDirBelow(int baseFd, const QStringList &parts, bool create, uid_t uid, gid_t gid)
{
    int current = ::fcntl(baseFd, F_DUPFD_CLOEXEC, 3);
    if (current < 0) {
        return -errno;
    }

    for (const QString &part : parts) {
        if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..")) {
            ::close(current);
            return -EINVAL;
        }

        int next = ::openat(current, part.toUtf8().constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && create && (errno == ENOENT)) {
            if (::mkdirat(current, part.toUtf8().constData(), 0755) != 0) {
                int err = errno;
                ::close(current);
                return -err;
            }
            next = ::openat(current, part.toUtf8().constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (next >= 0 && (::fchown(next, uid, gid) != 0)) {
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
        ::symlinkat(target.data(), dstDirFd, name); // may EEXIST on merge — non-fatal
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

    // Regular file (and anything else treated as opaque bytes)
    int srcFile = ::openat(srcDirFd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (srcFile < 0) {
        return -errno;
    }
    int dstFile = ::openat(dstDirFd, name, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, st.st_mode & 07777);
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
        ::fchmod(dstFile, st.st_mode & 07777);
    }
    ::close(dstFile);
    return result;
}

int copyTreeContents(int srcDirFd, int dstDirFd, uid_t uid, gid_t gid)
{
    return copyTree(srcDirFd, dstDirFd, uid, gid);
}
}
