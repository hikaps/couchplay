// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QString>
#include <QStringList>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

/**
 * SecureFs - Race-safe, FD-relative filesystem mutations below a trusted root
 *
 * All operations walk path components with openat(O_NOFOLLOW), so a user who
 * owns directories below the root cannot swap an ancestor for a symlink
 * between validation and use. Callers pass a baseFd opened on a trusted
 * directory (e.g. the target user's home) and a component list relative to
 * it; every returned/created directory is referenced by FD, and ownership is
 * applied with fchown on the FD rather than chown on a path.
 *
 * Functions return 0 on success or -errno on failure.
 */
namespace SecureFs
{

/**
 * Open (and optionally create) the directory at `parts` below `baseFd`.
 * Never follows symlinks: any existing component that is a symlink fails
 * with ELOOP. Missing components are created (mode 0755) and fchown'd to
 * uid/gid when `create` is true.
 *
 * @param baseFd FD of the trusted base directory
 * @param parts Path components below baseFd
 * @param create Create missing components instead of failing with ENOENT
 * @param uid/gid Ownership for created components
 * @return FD of the final directory (caller closes), or -errno
 */
int openDirBelow(int baseFd, const QStringList &parts, bool create, uid_t uid, gid_t gid);

/**
 * Recursively remove the directory referenced by dirFd (the directory
 * itself is unlinked from its parent using unlinkAt below, not through
 * this call). Contents are removed with unlinkat(AT_REMOVEDIR) — symlinked
 * entries are unlinked as links, never followed.
 * Does not close dirFd.
 */
int removeTreeAt(int dirFd);

/**
 * Recursively copy the contents of srcDirFd into dstDirFd (merge semantics:
 * existing files are overwritten, extra files in dst are kept). New files
 * and directories are fchown'd to uid/gid; symlinks are recreated with
 * symlinkat relative to dstDirFd, never followed.
 */
int copyTreeContents(int srcDirFd, int dstDirFd, uid_t uid, gid_t gid);

/**
 * Convenience: open the user's home directory (trusted — owned/maintained
 * by the system, not writable by the user's own privilege) as the anchor
 * for the calls above. Returns FD or -errno.
 */
int openBaseDir(const QString &absolutePath);
}
