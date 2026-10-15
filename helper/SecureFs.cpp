// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "SecureFs.h"

#include <QDebug>

#include <dirent.h>
#include <errno.h>
#include <sys/acl.h>
#include <acl/libacl.h>
#include <cstdio>
#include <linux/fs.h>
#include <linux/capability.h>
#include <algorithm>
#include <linux/mount.h>
#include <grp.h>
#include <pwd.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <QUuid>
#include <string>
#include <sys/mount.h>
#include <array>
#include <cstring>
#include <limits.h>
#include <sys/syscall.h>
#include <vector>

namespace SecureFs
{
static void logMountFailure(const char *operation, int error)
{
    qWarning().nospace() << "SecureFs: " << operation << " failed with errno " << error << ": "
                         << std::strerror(error);
}
int openBaseDir(const QString &absolutePath)
{
    int fd = ::open(absolutePath.toLocal8Bit().constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    return fd >= 0 ? fd : -errno;
}

int openExistingDirNoFollow(const QString &absolutePath)
{
    const QStringList parts = absolutePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    int baseFd = ::open("/", O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
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

        // Ancestors need search permission, not permission to list their contents.
        const int access = finalComponent ? O_RDONLY : O_PATH;
        int next = ::openat(current, part.toUtf8().constData(), access | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
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

static int negativeErrno()
{
    return errno == 0 ? -EIO : -errno;
}

static bool unsupportedAclError(int error)
{
    return error == ENOTSUP || error == ENOSYS;
}

struct TargetPrincipal {
    uid_t uid;
    gid_t primaryGid = 0;
    std::vector<gid_t> groups;
    bool resolved = false;
    int resolutionError = 0;
};

static int resolveTargetPrincipal(TargetPrincipal *principal)
{
    if (principal->resolved) return principal->resolutionError;
    principal->resolved = true;
    if (principal->uid == 0) return principal->resolutionError = -EPERM;

    long bufferSize = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    if (bufferSize < 1024) bufferSize = 16384;
    if (bufferSize > 1024 * 1024) bufferSize = 1024 * 1024;
    std::vector<char> buffer(static_cast<size_t>(bufferSize));
    struct passwd password{};
    struct passwd *found = nullptr;
    const int lookupError = ::getpwuid_r(principal->uid, &password, buffer.data(), buffer.size(), &found);
    if (lookupError != 0) return principal->resolutionError = -lookupError;
    if (!found) return principal->resolutionError = -ENOENT;

    principal->primaryGid = password.pw_gid;
    int groupCount = 16;
    principal->groups.resize(static_cast<size_t>(groupCount));
    int listedGroups = ::getgrouplist(password.pw_name, password.pw_gid, principal->groups.data(), &groupCount);
    if (listedGroups < 0) {
        if (groupCount <= 0) return principal->resolutionError = -EIO;
        principal->groups.resize(static_cast<size_t>(groupCount));
        listedGroups = ::getgrouplist(password.pw_name, password.pw_gid, principal->groups.data(), &groupCount);
        if (listedGroups < 0) return principal->resolutionError = -EIO;
    }
    principal->groups.resize(static_cast<size_t>(groupCount));
    if (std::find(principal->groups.begin(), principal->groups.end(), principal->primaryGid)
        == principal->groups.end()) {
        principal->groups.push_back(principal->primaryGid);
    }
    return principal->resolutionError = 0;
}

static void reportTargetAccessFailure(int pipeFd, int error)
{
    if (error <= 0) error = EIO;
    const char *bytes = reinterpret_cast<const char *>(&error);
    size_t remaining = sizeof(error);
    while (remaining != 0) {
        const ssize_t written = ::write(pipeFd, bytes + sizeof(error) - remaining, remaining);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) break;
        remaining -= static_cast<size_t>(written);
    }
    _exit(1);
}

static int targetReadExecuteAccess(int objectFd, const char *procPath, TargetPrincipal *principal)
{
    const int principalResult = resolveTargetPrincipal(principal);
    if (principalResult != 0) return principalResult;

    int pipes[2];
    if (::pipe2(pipes, O_CLOEXEC) != 0) return negativeErrno();
    const pid_t child = ::fork();
    if (child < 0) {
        const int error = negativeErrno();
        ::close(pipes[0]);
        ::close(pipes[1]);
        return error;
    }
    if (child == 0) {
        ::close(pipes[0]);
        if (::setgroups(principal->groups.size(), principal->groups.data()) != 0) {
            reportTargetAccessFailure(pipes[1], errno);
        }
        if (::setresgid(principal->primaryGid, principal->primaryGid, principal->primaryGid) != 0) {
            reportTargetAccessFailure(pipes[1], errno);
        }
        if (::prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0) != 0) reportTargetAccessFailure(pipes[1], errno);
        if (::setresuid(principal->uid, principal->uid, principal->uid) != 0) {
            reportTargetAccessFailure(pipes[1], errno);
        }
        struct __user_cap_header_struct capHeader{};
        struct __user_cap_data_struct capData[2]{};
        capHeader.version = _LINUX_CAPABILITY_VERSION_3;
        if (::syscall(SYS_capset, &capHeader, capData) != 0) reportTargetAccessFailure(pipes[1], errno);

        int accessResult = -1;
#ifdef SYS_faccessat2
        accessResult = static_cast<int>(::syscall(SYS_faccessat2, objectFd, "", R_OK | X_OK,
                                                  AT_EMPTY_PATH | AT_EACCESS));
        if (accessResult != 0 && errno != ENOSYS && errno != EINVAL) {
            reportTargetAccessFailure(pipes[1], errno);
        }
#endif
        if (accessResult != 0) {
            accessResult = ::faccessat(AT_FDCWD, procPath, R_OK | X_OK, AT_EACCESS);
        }
        if (accessResult != 0) reportTargetAccessFailure(pipes[1], errno);
        ::close(pipes[1]);
        _exit(0);
    }

    ::close(pipes[1]);
    int status = 0;
    pid_t waited;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    int childError = 0;
    ssize_t errorBytes;
    do {
        errorBytes = ::read(pipes[0], &childError, sizeof(childError));
    } while (errorBytes < 0 && errno == EINTR);
    ::close(pipes[0]);
    if (waited < 0) return negativeErrno();
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
    if (errorBytes == sizeof(childError) && childError > 0) return -childError;
    return -EIO;
}

static int fdProcPath(int fd, char *path, size_t pathSize)
{
    const int length = std::snprintf(path, pathSize, "/proc/self/fd/%d", fd);
    return length > 0 && static_cast<size_t>(length) < pathSize ? 0 : -EINVAL;
}

static int aclPermissionBits(acl_entry_t entry, int *bits)
{
    acl_permset_t permissions;
    errno = 0;
    if (::acl_get_permset(entry, &permissions) != 0) return negativeErrno();
    const int read = ::acl_get_perm(permissions, ACL_READ);
    const int write = ::acl_get_perm(permissions, ACL_WRITE);
    const int execute = ::acl_get_perm(permissions, ACL_EXECUTE);
    if (read < 0 || write < 0 || execute < 0) return negativeErrno();
    *bits = (read ? ACL_READ : 0) | (write ? ACL_WRITE : 0) | (execute ? ACL_EXECUTE : 0);
    return 0;
}

static int setAclPermissionBits(acl_entry_t entry, int bits)
{
    acl_permset_t permissions;
    errno = 0;
    if (::acl_get_permset(entry, &permissions) != 0) return negativeErrno();
    if (::acl_clear_perms(permissions) != 0) return negativeErrno();
    for (const int permission : {ACL_READ, ACL_WRITE, ACL_EXECUTE}) {
        if ((bits & permission) != 0 && ::acl_add_perm(permissions, permission) != 0) return negativeErrno();
    }
    return ::acl_set_permset(entry, permissions) == 0 ? 0 : negativeErrno();
}

static int appendAclEntry(acl_t &destination, acl_tag_t tag, const void *qualifier, int permissionBits)
{
    acl_entry_t entry;
    errno = 0;
    if (::acl_create_entry(&destination, &entry) != 0) return negativeErrno();
    if (::acl_set_tag_type(entry, tag) != 0) return negativeErrno();
    if ((tag == ACL_USER || tag == ACL_GROUP) && ::acl_set_qualifier(entry, qualifier) != 0) {
        return negativeErrno();
    }
    return setAclPermissionBits(entry, permissionBits);
}

static int copyAclEntry(acl_t &destination, acl_entry_t source, int permissionBits)
{
    acl_tag_t tag;
    errno = 0;
    if (::acl_get_tag_type(source, &tag) != 0) return negativeErrno();
    void *qualifier = nullptr;
    if (tag == ACL_USER || tag == ACL_GROUP) {
        errno = 0;
        qualifier = ::acl_get_qualifier(source);
        if (!qualifier) return negativeErrno();
    }
    const int result = appendAclEntry(destination, tag, qualifier, permissionBits);
    if (qualifier) ::acl_free(qualifier);
    return result;
}

static int storeAcl(int objectFd, acl_t acl, TargetPrincipal *principal, acl_type_t type)
{
    errno = EINVAL;
    if (::acl_valid(acl) != 0) return negativeErrno();
    char path[64];
    const int pathResult = fdProcPath(objectFd, path, sizeof(path));
    if (pathResult != 0) return pathResult;
    errno = 0;
    if (::acl_set_file(path, type, acl) == 0) return 0;
    const int error = errno == 0 ? EIO : errno;
    if (unsupportedAclError(error)) return targetReadExecuteAccess(objectFd, path, principal);
    return -error;
}

static int applyReadExecuteAclToFd(int objectFd, TargetPrincipal *principal, acl_type_t type = ACL_TYPE_ACCESS)
{
    const uid_t targetUid = principal->uid;
    struct stat st{};
    if (::fstat(objectFd, &st) != 0) return negativeErrno();
    if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) return -EINVAL;

    char path[64];
    const int pathResult = fdProcPath(objectFd, path, sizeof(path));
    if (pathResult != 0) return pathResult;
    errno = 0;
    acl_t original = ::acl_get_file(path, type);
    bool createDefault = false;
    if (original && type == ACL_TYPE_DEFAULT && ::acl_entries(original) == 0) {
        // Seed a missing default ACL with owner-only base entries. Copying group/other from the access ACL
        // would make new files ignore the creator's umask; the target entry added below still inherits r-x.
        ::acl_free(original);
        original = ::acl_init(3);
        int seeded = original ? 0 : negativeErrno();
        if (seeded == 0) seeded = appendAclEntry(original, ACL_USER_OBJ, nullptr, (st.st_mode >> 6) & 07);
        if (seeded == 0) seeded = appendAclEntry(original, ACL_GROUP_OBJ, nullptr, 0);
        if (seeded == 0) seeded = appendAclEntry(original, ACL_OTHER, nullptr, 0);
        if (seeded != 0) {
            if (original) ::acl_free(original);
            return seeded;
        }
        createDefault = true;
    }
    if (!original) {
        const int error = errno == 0 ? EIO : errno;
        if (unsupportedAclError(error)) return targetReadExecuteAccess(objectFd, path, principal);
        return -error;
    }

    constexpr int readExecute = ACL_READ | ACL_EXECUTE;
    acl_entry_t ownerEntry = nullptr;
    acl_entry_t groupOwnerEntry = nullptr;
    acl_entry_t targetEntry = nullptr;
    acl_entry_t maskEntry = nullptr;
    int ownerBits = 0;
    int groupOwnerBits = 0;
    int targetBits = 0;
    int maskBits = 0;
    int entryCount = 0;
    int result = 0;
    acl_entry_t entry;
    int next = ::acl_get_entry(original, ACL_FIRST_ENTRY, &entry);
    while (next == 1) {
        acl_tag_t tag;
        if (::acl_get_tag_type(entry, &tag) != 0) {
            result = negativeErrno();
            break;
        }
        int bits = 0;
        result = aclPermissionBits(entry, &bits);
        if (result != 0) break;
        ++entryCount;
        if (tag == ACL_USER_OBJ) {
            ownerEntry = entry;
            ownerBits = bits;
        } else if (tag == ACL_GROUP_OBJ) {
            groupOwnerEntry = entry;
            groupOwnerBits = bits;
        } else if (tag == ACL_MASK) {
            maskEntry = entry;
            maskBits = bits;
        } else if (tag == ACL_USER) {
            void *qualifier = ::acl_get_qualifier(entry);
            if (!qualifier) {
                result = negativeErrno();
                break;
            }
            const uid_t entryUid = *static_cast<uid_t *>(qualifier);
            ::acl_free(qualifier);
            if (entryUid == targetUid) {
                targetEntry = entry;
                targetBits = bits;
            }
        }
        next = ::acl_get_entry(original, ACL_NEXT_ENTRY, &entry);
    }
    if (result == 0 && next < 0) result = negativeErrno();
    if (result == 0 && (!ownerEntry || !groupOwnerEntry)) result = -EINVAL;
    if (result != 0) {
        ::acl_free(original);
        return result;
    }

    if (type == ACL_TYPE_ACCESS && targetUid == st.st_uid) {
        if ((ownerBits & readExecute) == readExecute) {
            ::acl_free(original);
            return 0;
        }
        result = setAclPermissionBits(ownerEntry, ownerBits | readExecute);
        if (result == 0) result = storeAcl(objectFd, original, principal, type);
        ::acl_free(original);
        return result;
    }

    const int oldMask = maskEntry ? maskBits : groupOwnerBits;
    if (targetEntry && ((targetBits & oldMask & readExecute) == readExecute)) {
        result = createDefault ? storeAcl(objectFd, original, principal, type) : 0;
        ::acl_free(original);
        return result;
    }

    const int newMask = oldMask | readExecute;
    if (targetEntry && maskEntry) {
        next = ::acl_get_entry(original, ACL_FIRST_ENTRY, &entry);
        while (next == 1) {
            acl_tag_t tag;
            if (::acl_get_tag_type(entry, &tag) != 0) {
                result = negativeErrno();
                break;
            }
            int bits = 0;
            result = aclPermissionBits(entry, &bits);
            if (result != 0) break;
            if (entry == targetEntry) {
                bits |= readExecute;
            } else if (tag == ACL_MASK) {
                bits = newMask;
            } else if (newMask != oldMask && (tag == ACL_GROUP_OBJ || tag == ACL_GROUP || tag == ACL_USER)) {
                // Widening the mask would otherwise expose previously masked-off bits.
                bits &= oldMask;
            }
            result = setAclPermissionBits(entry, bits);
            if (result != 0) break;
            next = ::acl_get_entry(original, ACL_NEXT_ENTRY, &entry);
        }
        if (result == 0 && next < 0) result = negativeErrno();
        if (result == 0) result = storeAcl(objectFd, original, principal, type);
        ::acl_free(original);
        return result;
    }
    const bool addTarget = targetEntry == nullptr;
    const bool addMask = maskEntry == nullptr;
    acl_t updated = ::acl_init(entryCount + static_cast<int>(addTarget) + static_cast<int>(addMask));
    if (!updated) {
        result = negativeErrno();
        ::acl_free(original);
        return result;
    }

    bool targetInserted = !addTarget;
    bool maskInserted = !addMask;
    next = ::acl_get_entry(original, ACL_FIRST_ENTRY, &entry);
    while (next == 1 && result == 0) {
        acl_tag_t tag;
        if (::acl_get_tag_type(entry, &tag) != 0) {
            result = negativeErrno();
            break;
        }
        if (addTarget && !targetInserted) {
            bool insertBefore = tag == ACL_GROUP_OBJ || tag == ACL_GROUP || tag == ACL_MASK || tag == ACL_OTHER;
            if (tag == ACL_USER) {
                void *qualifier = ::acl_get_qualifier(entry);
                if (!qualifier) {
                    result = negativeErrno();
                    break;
                }
                insertBefore = targetUid < *static_cast<uid_t *>(qualifier);
                ::acl_free(qualifier);
            }
            if (insertBefore) {
                result = appendAclEntry(updated, ACL_USER, &targetUid, readExecute);
                targetInserted = result == 0;
            }
        }
        if (result != 0) break;
        if (addMask && !maskInserted && tag == ACL_OTHER) {
            result = appendAclEntry(updated, ACL_MASK, nullptr, newMask);
            maskInserted = result == 0;
        }
        if (result != 0) break;

        int bits = 0;
        result = aclPermissionBits(entry, &bits);
        if (result != 0) break;
        if (tag == ACL_USER && targetEntry == entry) {
            bits |= readExecute;
        } else if (tag == ACL_MASK) {
            bits = newMask;
        } else if (newMask != oldMask && (tag == ACL_GROUP_OBJ || tag == ACL_GROUP || tag == ACL_USER)) {
            bits &= oldMask;
        }
        result = copyAclEntry(updated, entry, bits);
        next = ::acl_get_entry(original, ACL_NEXT_ENTRY, &entry);
    }
    if (result == 0 && next < 0) result = negativeErrno();
    if (result == 0 && !targetInserted) result = appendAclEntry(updated, ACL_USER, &targetUid, readExecute);
    if (result == 0 && !maskInserted) result = appendAclEntry(updated, ACL_MASK, nullptr, newMask);
    if (result == 0) result = storeAcl(objectFd, updated, principal, type);
    ::acl_free(updated);
    ::acl_free(original);
    return result;
}

int applyReadExecuteAcl(int objectFd, uid_t targetUid)
{
    TargetPrincipal principal{};
    principal.uid = targetUid;
    return applyReadExecuteAclToFd(objectFd, &principal);
}

static int applyReadExecuteAclTreeAt(int directoryFd, TargetPrincipal *principal)
{
    int aclResult = applyReadExecuteAclToFd(directoryFd, principal);
    if (aclResult == 0) aclResult = applyReadExecuteAclToFd(directoryFd, principal, ACL_TYPE_DEFAULT);
    if (aclResult != 0) return aclResult;

    const int streamFd = ::openat(directoryFd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (streamFd < 0) return negativeErrno();
    DIR *directory = ::fdopendir(streamFd);
    if (!directory) {
        const int error = negativeErrno();
        ::close(streamFd);
        return error;
    }

    int result = 0;
    while (result == 0) {
        errno = 0;
        dirent *entry = ::readdir(directory);
        if (!entry) {
            if (errno != 0) result = negativeErrno();
            break;
        }
        const char *name = entry->d_name;
        if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) continue;
        struct stat observed{};
        if (::fstatat(directoryFd, name, &observed, AT_SYMLINK_NOFOLLOW) != 0) {
            result = negativeErrno();
            break;
        }
        if (!S_ISDIR(observed.st_mode) && !S_ISREG(observed.st_mode)) continue;

        const int flags = (S_ISDIR(observed.st_mode) ? O_RDONLY | O_DIRECTORY : O_PATH)
            | O_NOFOLLOW | O_CLOEXEC;
        const int childFd = ::openat(directoryFd, name, flags);
        if (childFd < 0) {
            result = negativeErrno();
            break;
        }
        struct stat opened{};
        if (::fstat(childFd, &opened) != 0) {
            result = negativeErrno();
        } else if (opened.st_dev != observed.st_dev || opened.st_ino != observed.st_ino
                   || (opened.st_mode & S_IFMT) != (observed.st_mode & S_IFMT)) {
            result = -ESTALE;
        } else if (S_ISDIR(opened.st_mode)) {
            result = applyReadExecuteAclTreeAt(childFd, principal);
        } else {
            result = applyReadExecuteAclToFd(childFd, principal);
        }
        ::close(childFd);
    }
    ::closedir(directory);
    return result;
}

int applyReadExecuteAclTree(int directoryFd, uid_t targetUid)
{
    struct stat st{};
    if (::fstat(directoryFd, &st) != 0) return negativeErrno();
    if (!S_ISDIR(st.st_mode)) return -ENOTDIR;
    TargetPrincipal principal{};
    principal.uid = targetUid;
    return applyReadExecuteAclTreeAt(directoryFd, &principal);
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

static int removeEntryChecked(int parentFd, const char *name)
{
    struct stat st{};
    if (::fstatat(parentFd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        return errno == ENOENT ? 0 : -errno;
    }
    if (S_ISDIR(st.st_mode)) {
        const int child = ::openat(parentFd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (child < 0) return -errno;
        const int result = removeTreeAt(child);
        ::close(child);
        if (result != 0) return result;
        if (::unlinkat(parentFd, name, AT_REMOVEDIR) != 0) return -errno;
    } else if (::unlinkat(parentFd, name, 0) != 0) {
        return -errno;
    }
    return 0;
}

static int ensureDestinationDirectory(int parentFd, const char *name)
{
    struct stat st{};
    if (::fstatat(parentFd, name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        if (S_ISDIR(st.st_mode)) return 0;
        const int result = removeEntryChecked(parentFd, name);
        if (result != 0) return result;
    } else if (errno != ENOENT) {
        return -errno;
    }
    if (::mkdirat(parentFd, name, 0700) != 0 && errno != EEXIST) return -errno;
    return 0;
}

static int copyEntry(int srcDirFd, const char *name, int dstDirFd, uid_t uid, gid_t gid);

static int copyTree(int srcDirFd, int dstDirFd, uid_t uid, gid_t gid)
{
    DIR *dir = ::fdopendir(::fcntl(srcDirFd, F_DUPFD_CLOEXEC, 3));
    if (!dir) return -errno;

    int result = 0;
    while (dirent *entry = ::readdir(dir)) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        result = copyEntry(srcDirFd, entry->d_name, dstDirFd, uid, gid);
        if (result != 0) break;
    }
    ::closedir(dir);
    return result;
}

static int copyEntry(int srcDirFd, const char *name, int dstDirFd, uid_t uid, gid_t gid)
{
    struct stat st{};
    if (::fstatat(srcDirFd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) return -errno;

    if (S_ISLNK(st.st_mode)) {
        std::array<char, PATH_MAX + 1> target{};
        const ssize_t length = ::readlinkat(srcDirFd, name, target.data(), PATH_MAX);
        if (length < 0) return -errno;
        if (length == PATH_MAX) return -ENAMETOOLONG;
        target[static_cast<size_t>(length)] = '\0';
        const int removed = removeEntryChecked(dstDirFd, name);
        if (removed != 0) return removed;
        if (::symlinkat(target.data(), dstDirFd, name) != 0) return -errno;
        if (::fchownat(dstDirFd, name, uid, gid, AT_SYMLINK_NOFOLLOW) != 0) return -errno;
        return 0;
    }

    if (S_ISDIR(st.st_mode)) {
        const int srcChild = ::openat(srcDirFd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (srcChild < 0) return -errno;
        int result = ensureDestinationDirectory(dstDirFd, name);
        int dstChild = -1;
        if (result == 0) {
            dstChild = ::openat(dstDirFd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (dstChild < 0) result = -errno;
        }
        if (result == 0 && ::fchown(dstChild, uid, gid) != 0) result = -errno;
        if (result == 0) result = copyTree(srcChild, dstChild, uid, gid);
        if (result == 0 && ::fchmod(dstChild, st.st_mode & 07777) != 0) result = -errno;
        if (dstChild >= 0) ::close(dstChild);
        ::close(srcChild);
        return result;
    }

    if (!S_ISREG(st.st_mode)) {
        qWarning() << "SecureFs: refusing to copy special file" << name;
        return -EOPNOTSUPP;
    }

    const int srcFile = ::openat(srcDirFd, name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (srcFile < 0) return -errno;
    struct stat openedSt{};
    if (::fstat(srcFile, &openedSt) != 0) {
        const int error = errno;
        ::close(srcFile);
        return -error;
    }
    if (!S_ISREG(openedSt.st_mode)) {
        ::close(srcFile);
        qWarning() << "SecureFs: refusing to copy non-regular file" << name;
        return -EOPNOTSUPP;
    }

    int result = removeEntryChecked(dstDirFd, name);
    int dstFile = -1;
    if (result == 0) {
        dstFile = ::openat(dstDirFd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (dstFile < 0) result = -errno;
    }
    char buffer[65536];
    while (result == 0) {
        const ssize_t bytesRead = ::read(srcFile, buffer, sizeof(buffer));
        if (bytesRead < 0) {
            if (errno == EINTR) continue;
            result = -errno;
            break;
        }
        if (bytesRead == 0) break;
        ssize_t offset = 0;
        while (offset < bytesRead) {
            const ssize_t written = ::write(dstFile, buffer + offset,
                                            static_cast<size_t>(bytesRead - offset));
            if (written < 0) {
                if (errno == EINTR) continue;
                result = -errno;
                break;
            }
            if (written == 0) {
                result = -EIO;
                break;
            }
            offset += written;
        }
    }
    if (result == 0 && ::fchown(dstFile, uid, gid) != 0) result = -errno;
    if (result == 0 && ::fchmod(dstFile, openedSt.st_mode & 07777) != 0) result = -errno;
    if (dstFile >= 0 && ::close(dstFile) != 0 && result == 0) result = -errno;
    ::close(srcFile);
    return result;
}

int copyTreeContents(int srcDirFd, int dstDirFd, uid_t uid, gid_t gid)
{
    return copyTree(srcDirFd, dstDirFd, uid, gid);
}


static bool validLeafName(const QString &name)
{
    return !name.isEmpty() && name != QStringLiteral(".") && name != QStringLiteral("..")
        && !name.contains(QLatin1Char('/')) && !name.contains(QChar::Null);
}


int removeCopyScratchAt(int parentFd, const QString &scratchName)
{
    if (parentFd < 0 || !validLeafName(scratchName)
        || !scratchName.startsWith(QStringLiteral(".cptmp-"))) return -EINVAL;
    const QByteArray name = scratchName.toUtf8();
    struct stat parentStat{};
    if (::fstat(parentFd, &parentStat) != 0) return -errno;
    if (!S_ISDIR(parentStat.st_mode)) return -ENOTDIR;
    const auto isPrivateWrapper = [&](const struct stat &st) {
        if (!S_ISDIR(st.st_mode) || st.st_uid != 0 || st.st_dev != parentStat.st_dev) return false;
        const mode_t mode = st.st_mode & 07777;
        if (st.st_gid == 0 && mode == 0700) return true;
        // mkdir(0700) inherits SGID and the parent's group before fchown/fchmod.
        // A crash after fchown may already have reset the group to root.
        return (parentStat.st_mode & S_ISGID) && mode == 02700
            && (st.st_gid == parentStat.st_gid || st.st_gid == 0);
    };

    struct stat observed{};
    if (::fstatat(parentFd, name.constData(), &observed, AT_SYMLINK_NOFOLLOW) != 0) {
        return errno == ENOENT ? 0 : -errno;
    }
    if (!isPrivateWrapper(observed)) return -EPERM;

    const int directoryFd = ::openat(parentFd, name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directoryFd < 0) return -errno;
    struct stat opened{};
    if (::fstat(directoryFd, &opened) != 0) {
        const int error = errno;
        ::close(directoryFd);
        return -error;
    }
    if (!isPrivateWrapper(opened) || opened.st_dev != observed.st_dev || opened.st_ino != observed.st_ino
        || opened.st_uid != observed.st_uid || opened.st_gid != observed.st_gid
        || opened.st_mode != observed.st_mode) {
        ::close(directoryFd);
        return -ESTALE;
    }
    const int removedContents = removeTreeAt(directoryFd);
    ::close(directoryFd);
    if (removedContents != 0) return removedContents;

    struct stat current{};
    if (::fstatat(parentFd, name.constData(), &current, AT_SYMLINK_NOFOLLOW) != 0) {
        return errno == ENOENT ? 0 : -errno;
    }
    if (!S_ISDIR(current.st_mode) || current.st_dev != opened.st_dev || current.st_ino != opened.st_ino) {
        return -ESTALE;
    }
    if (::unlinkat(parentFd, name.constData(), AT_REMOVEDIR) != 0) return -errno;
    if (::fsync(parentFd) != 0) return -errno;
    return 0;
}

int replaceTreeContents(int srcDirFd, int stagingDirFd, int targetParentFd, const QString &leafName,
                        const QString &scratchName, uid_t uid, gid_t gid)
{
    if (srcDirFd < 0 || targetParentFd < 0 || !validLeafName(leafName) || !validLeafName(scratchName)
        || !scratchName.startsWith(QStringLiteral(".cptmp-"))) {
        return -EINVAL;
    }
    struct stat sourceStat{};
    if (::fstat(srcDirFd, &sourceStat) != 0) return -errno;
    if (!S_ISDIR(sourceStat.st_mode)) return -ENOTDIR;
    const QByteArray leaf = leafName.toUtf8();
    const QByteArray tempName = scratchName.toUtf8();
    const QByteArray payloadName = QByteArrayLiteral("payload");
    struct stat existing{};
    if (::fstatat(targetParentFd, leaf.constData(), &existing, AT_SYMLINK_NOFOLLOW) == 0) {
        if (S_ISLNK(existing.st_mode)) return -ELOOP;
    } else if (errno != ENOENT) {
        return -errno;
    }

    if (::mkdirat(targetParentFd, tempName.constData(), 0700) != 0) return -errno;
    const int wrapperFd = ::openat(targetParentFd, tempName.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (wrapperFd < 0) {
        const int error = errno;
        (void)removeCopyScratchAt(targetParentFd, scratchName);
        return -error;
    }
    struct stat wrapperPathStat{};
    struct stat wrapperStat{};
    auto discardScratch = [&](int error) {
        if (::close(wrapperFd) != 0 && error == 0) error = -errno;
        (void)removeCopyScratchAt(targetParentFd, scratchName);
        return error;
    };
    if (::fstatat(targetParentFd, tempName.constData(), &wrapperPathStat, AT_SYMLINK_NOFOLLOW) != 0) {
        return discardScratch(-errno);
    }
    if (::fstat(wrapperFd, &wrapperStat) != 0) return discardScratch(-errno);
    if (!S_ISDIR(wrapperPathStat.st_mode) || !S_ISDIR(wrapperStat.st_mode)
        || wrapperPathStat.st_dev != wrapperStat.st_dev || wrapperPathStat.st_ino != wrapperStat.st_ino
        || wrapperStat.st_uid != 0) {
        return discardScratch(wrapperStat.st_uid == 0 ? -ESTALE : -EPERM);
    }
    if (::fchown(wrapperFd, 0, 0) != 0) return discardScratch(-errno);
    if (::fchmod(wrapperFd, 0700) != 0) return discardScratch(-errno);
    struct stat normalizedPathStat{};
    struct stat normalizedStat{};
    if (::fstatat(targetParentFd, tempName.constData(), &normalizedPathStat, AT_SYMLINK_NOFOLLOW) != 0) {
        return discardScratch(-errno);
    }
    if (::fstat(wrapperFd, &normalizedStat) != 0) return discardScratch(-errno);
    if (!S_ISDIR(normalizedPathStat.st_mode) || !S_ISDIR(normalizedStat.st_mode)
        || normalizedPathStat.st_dev != normalizedStat.st_dev || normalizedPathStat.st_ino != normalizedStat.st_ino
        || normalizedStat.st_uid != 0 || normalizedStat.st_gid != 0
        || (normalizedStat.st_mode & 07777) != 0700) {
        return discardScratch(-ESTALE);
    }

    if (::mkdirat(wrapperFd, payloadName.constData(), 0700) != 0) return discardScratch(-errno);
    const int payloadFd = ::openat(wrapperFd, payloadName.constData(),
                                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (payloadFd < 0) return discardScratch(-errno);
    struct stat payloadStat{};
    if (::fstat(payloadFd, &payloadStat) != 0 || !S_ISDIR(payloadStat.st_mode)
        || payloadStat.st_uid != 0 || payloadStat.st_gid != 0 || payloadStat.st_dev != wrapperStat.st_dev
        || ::fchown(payloadFd, 0, 0) != 0 || ::fchmod(payloadFd, 0700) != 0) {
        const int error = errno ? errno : ESTALE;
        ::close(payloadFd);
        return discardScratch(-error);
    }

    int result = copyTreeContents(srcDirFd, payloadFd, uid, gid);
    if (result == 0 && stagingDirFd >= 0) result = copyTreeContents(stagingDirFd, payloadFd, uid, gid);
    if (result == 0 && ::fchown(payloadFd, uid, gid) != 0) result = -errno;
    if (result == 0 && ::fchmod(payloadFd, sourceStat.st_mode & 07777) != 0) result = -errno;
    struct stat finalizedPayload{};
    struct stat payloadPathStat{};
    if (result == 0 && (::fstat(payloadFd, &finalizedPayload) != 0
                        || ::fstatat(wrapperFd, payloadName.constData(), &payloadPathStat, AT_SYMLINK_NOFOLLOW) != 0)) {
        result = -errno;
    }
    if (result == 0 && (!S_ISDIR(finalizedPayload.st_mode) || finalizedPayload.st_uid != uid
                        || finalizedPayload.st_gid != gid
                        || (finalizedPayload.st_mode & 07777) != (sourceStat.st_mode & 07777)
                        || finalizedPayload.st_dev != payloadStat.st_dev || finalizedPayload.st_ino != payloadStat.st_ino
                        || payloadPathStat.st_dev != finalizedPayload.st_dev
                        || payloadPathStat.st_ino != finalizedPayload.st_ino)) {
        result = -ESTALE;
    }
    if (::close(payloadFd) != 0 && result == 0) result = -errno;
    if (result != 0) return discardScratch(result);

    if (::fstatat(targetParentFd, tempName.constData(), &wrapperPathStat, AT_SYMLINK_NOFOLLOW) != 0) {
        return discardScratch(-errno);
    }
    if (!S_ISDIR(wrapperPathStat.st_mode) || wrapperPathStat.st_uid != 0 || wrapperPathStat.st_gid != 0
        || (wrapperPathStat.st_mode & 07777) != 0700 || wrapperPathStat.st_dev != wrapperStat.st_dev
        || wrapperPathStat.st_ino != wrapperStat.st_ino) {
        return discardScratch(-ESTALE);
    }

    const bool hadExisting = ::fstatat(targetParentFd, leaf.constData(), &existing, AT_SYMLINK_NOFOLLOW) == 0;
    if (!hadExisting && errno != ENOENT) return discardScratch(-errno);
    if (hadExisting && S_ISLNK(existing.st_mode)) return discardScratch(-ELOOP);
    constexpr unsigned int renameNoReplace = 1;
    constexpr unsigned int renameExchange = 2;
    const unsigned int renameFlags = hadExisting ? renameExchange : renameNoReplace;
#ifdef SYS_renameat2
    if (::syscall(SYS_renameat2, wrapperFd, payloadName.constData(), targetParentFd, leaf.constData(),
                  renameFlags) != 0) {
        return discardScratch(-errno);
    }
#else
    return discardScratch(-ENOSYS);
#endif
    ::close(wrapperFd);
    (void)removeCopyScratchAt(targetParentFd, scratchName);
    return 0;
}


int writeFileAt(int parentFd, const QString &name, const QByteArray &content, uid_t uid, gid_t gid, mode_t mode)
{
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..")
        || name.contains(QLatin1Char('/'))) {
        return -EINVAL;
    }

    const QByteArray nameBytes = name.toUtf8();
    const int fileFd =
        ::openat(parentFd, nameBytes.constData(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, mode);
    if (fileFd < 0) {
        return -errno;
    }

    qsizetype offset = 0;
    while (offset < content.size()) {
        const ssize_t written = ::write(fileFd, content.constData() + offset, content.size() - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int error = errno;
            ::close(fileFd);
            return -error;
        }
        offset += written;
    }

    if (::fchown(fileFd, uid, gid) != 0) {
        const int error = errno;
        ::close(fileFd);
        return -error;
    }
    if (::fchmod(fileFd, mode) != 0) {
        const int error = errno;
        ::close(fileFd);
        return -error;
    }

    ::close(fileFd);
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
#ifndef SYS_mount_setattr
#define SYS_mount_setattr 442
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
#ifndef MOVE_MOUNT_T_EMPTY_PATH
#define MOVE_MOUNT_T_EMPTY_PATH 0x00000040
#endif
#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
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

int cloneMountFd(int sourceDirFd)
{
    if (sourceDirFd < 0) return -EINVAL;
    const int sourcePathFd = ::openat(sourceDirFd, ".", O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (sourcePathFd < 0) {
        const int error = errno;
        logMountFailure("source O_PATH openat", error);
        return -error;
    }
    // Clone in the host namespace, then sever inherited propagation before
    // attaching it anywhere. open_tree clones retain shared peer groups.
    const int mountFd = static_cast<int>(::syscall(SYS_open_tree, sourcePathFd, "",
                                                    AT_EMPTY_PATH | OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC));
    const int error = errno;
    ::close(sourcePathFd);
    if (mountFd < 0) {
        logMountFailure("open_tree source clone", error);
        return -error;
    }
    struct mount_attr attributes{};
    attributes.propagation = MS_PRIVATE;
    if (::syscall(SYS_mount_setattr, mountFd, "", AT_EMPTY_PATH,
                  &attributes, sizeof(attributes)) != 0) {
        const int privateError = errno;
        logMountFailure("source clone private propagation", privateError);
        ::close(mountFd);
        return -privateError;
    }
    return mountFd;
}

int bindMountFd(int sourceMountFd, int targetParentFd, const QString &leafName)
{
    if (sourceMountFd < 0 || targetParentFd < 0 || !validLeafName(leafName)) return -EINVAL;
    const QByteArray leaf = leafName.toUtf8();
    const int targetFd = ::openat(targetParentFd, leaf.constData(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (targetFd < 0) {
        const int error = errno;
        logMountFailure("bind target openat", error);
        return -error;
    }
    if (::syscall(SYS_move_mount, sourceMountFd, "", targetFd, "",
                  MOVE_MOUNT_F_EMPTY_PATH | MOVE_MOUNT_T_EMPTY_PATH) != 0) {
        const int error = errno;
        logMountFailure("bind move_mount", error);
        ::close(targetFd);
        return -error;
    }
    ::close(targetFd);
    return 0;
}

#ifndef MOUNT_ATTR_IDMAP
#define MOUNT_ATTR_IDMAP 0x00100000
#endif

int cloneIdmappedLowerFd(int sourceDirFd, int userNamespaceFd)
{
    if (sourceDirFd < 0 || userNamespaceFd < 0) return -EINVAL;
    const int mountFd = cloneMountFd(sourceDirFd);
    if (mountFd < 0) return mountFd;
    struct mount_attr attributes{};
    attributes.attr_set = MOUNT_ATTR_IDMAP | MOUNT_ATTR_RDONLY;
    attributes.userns_fd = static_cast<__u64>(userNamespaceFd);
    if (::syscall(SYS_mount_setattr, mountFd, "", AT_EMPTY_PATH,
                  &attributes, sizeof(attributes)) != 0) {
        const int error = errno;
        logMountFailure("idmapped lower mount_setattr", error);
        ::close(mountFd);
        return -error;
    }
    return mountFd;
}
int overlayMountFd(int sourceMountFd, const QString &upperdir, const QString &workdir,
                   int targetParentFd, const QString &leafName)
{
    if (sourceMountFd < 0 || targetParentFd < 0 || !validLeafName(leafName)) return -EINVAL;

    const QByteArray leaf = leafName.toUtf8();
    const int targetFd = ::openat(targetParentFd, leaf.constData(),
                                  O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (targetFd < 0) {
        const int error = errno;
        logMountFailure("overlay target openat", error);
        return -error;
    }
    struct stat targetStat{};
    if (::fstat(targetFd, &targetStat) != 0) {
        const int error = errno;
        ::close(targetFd);
        return -error;
    }
    if (!S_ISDIR(targetStat.st_mode)) {
        ::close(targetFd);
        return -ENOTDIR;
    }
    if (::syscall(SYS_move_mount, sourceMountFd, "", targetFd, "",
                  MOVE_MOUNT_F_EMPTY_PATH | MOVE_MOUNT_T_EMPTY_PATH) != 0) {
        const int error = errno;
        ::close(targetFd);
        logMountFailure("overlay lower move_mount", error);
        return -error;
    }

    const int lowerFd = ::openat(targetParentFd, leaf.constData(),
                                 O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (lowerFd < 0) {
        const int error = errno;
        ::close(targetFd);
        logMountFailure("overlay attached lower openat", error);
        return -error;
    }
    struct stat sourceStat{};
    struct stat lowerStat{};
    if (::fstat(sourceMountFd, &sourceStat) != 0 || ::fstat(lowerFd, &lowerStat) != 0) {
        const int error = errno;
        ::close(lowerFd);
        ::close(targetFd);
        return -error;
    }
    if (sourceStat.st_dev != lowerStat.st_dev || sourceStat.st_ino != lowerStat.st_ino) {
        ::close(lowerFd);
        ::close(targetFd);
        return -ESTALE;
    }

    const int fsFd = static_cast<int>(::syscall(SYS_fsopen, "overlay", FSOPEN_CLOEXEC));
    if (fsFd < 0) {
        const int error = errno;
        ::close(lowerFd);
        ::close(targetFd);
        logMountFailure("overlay fsopen", error);
        return -error;
    }
    auto setString = [fsFd](const char *key, const QByteArray &value) -> int {
        if (::syscall(SYS_fsconfig, fsFd, FSCONFIG_SET_STRING, key, value.constData(), 0) != 0) {
            const int error = errno;
            logMountFailure(key, error);
            return -error;
        }
        return 0;
    };
    const QByteArray lowerPath = QByteArrayLiteral("/proc/self/fd/") + QByteArray::number(lowerFd);
    int result = setString("lowerdir", lowerPath);
    if (result == 0) result = setString("upperdir", upperdir.toUtf8());
    if (result == 0) result = setString("workdir", workdir.toUtf8());
    if (result == 0 && ::syscall(SYS_fsconfig, fsFd, FSCONFIG_CMD_CREATE, nullptr, nullptr, 0) != 0) {
        const int error = errno;
        logMountFailure("overlay fsconfig CREATE", error);
        result = -error;
    }
    if (result != 0) {
        ::close(fsFd);
        ::close(lowerFd);
        ::close(targetFd);
        return result;
    }
    const int mountFd = static_cast<int>(::syscall(SYS_fsmount, fsFd, FSMOUNT_CLOEXEC, 0));
    ::close(fsFd);
    if (mountFd < 0) {
        const int error = errno;
        ::close(lowerFd);
        ::close(targetFd);
        logMountFailure("overlay fsmount", error);
        return -error;
    }
    if (::syscall(SYS_move_mount, mountFd, "", targetFd, "",
                  MOVE_MOUNT_F_EMPTY_PATH | MOVE_MOUNT_T_EMPTY_PATH) != 0) {
        const int error = errno;
        ::close(mountFd);
        ::close(lowerFd);
        ::close(targetFd);
        logMountFailure("overlay move_mount", error);
        return -error;
    }
    ::close(mountFd);
    ::close(lowerFd);
    ::close(targetFd);
    return 0;
}

}
