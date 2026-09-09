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
 * Which created components get fchown'd to uid/gid.
 * All: every created component (used below a user's home).
 * FinalOnly: only the final component (used below root-owned storage, where
 * intermediates must stay root-owned so a user cannot alter the chain).
 */
enum class ChownMode {
    All,
    FinalOnly,
};

/**
 * Open (and optionally create) the directory at `parts` below `baseFd`.
 * Never follows symlinks: any existing component that is a symlink fails
 * with ELOOP. Missing components are created (mode 0755); created
 * components are fchown'd to uid/gid per chownMode.
 *
 * @param baseFd FD of the trusted base directory
 * @param parts Path components below baseFd ("." components are skipped)
 * @param create Create missing components instead of failing with ENOENT
 * @param uid/gid Ownership for created components
 * @param chownMode Which created components receive the ownership
 * @return FD of the final directory (caller closes), or -errno
 */
int openDirBelow(int baseFd, const QStringList &parts, bool create, uid_t uid, gid_t gid,
                 ChownMode chownMode = ChownMode::All);

/**
 * Open an existing absolute directory by walking every component from "/"
 * with openat(O_NOFOLLOW). Unlike openBaseDir (which only protects the
 * final component), this pins the whole chain: it fails closed if any
 * ancestor is (or has just become) a symlink. Intended to re-open a
 * freshly canonicalized path.
 *
 * @return FD of the directory (caller closes), or -errno
 */
int openExistingDirNoFollow(const QString &absolutePath);

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

// ---------------------------------------------------------------------------
// FD-anchored mounting (Linux mount API, kernel 5.2+; overlayfs 5.11+)
//
// mount(8) resolves source and target by name at execution time, so a user
// who can replace a validated ancestor with a symlink can race a root mount
// onto an arbitrary path. These helpers keep verified directory FDs pinned
// through the operation: the attach point is a (parentFd, leafName) pair,
// the parent FD is a no-follow walk anchored at "/", and move_mount does not
// follow symlinks on the leaf by default — a swapped-in symlink fails the
// mount instead of redirecting it.
// ---------------------------------------------------------------------------

/**
 * Probe whether the new mount API (fsopen/open_tree/move_mount) is available
 * to this process. Cached: the first call performs the probe.
 */
bool mountApiAvailable();

/**
 * Bind-mount canonicalSource at (targetParentFd, leafName).
 * The source is cloned via open_tree with AT_SYMLINK_NOFOLLOW below a
 * no-follow-anchored parent, then attached with move_mount.
 * @return 0 on success, -errno on failure
 */
int bindMountFd(const QString &canonicalSource, int targetParentFd, const QString &leafName);

/**
 * Mount an overlay filesystem at (targetParentFd, leafName) via
 * fsopen/fsconfig/fsmount + move_mount. The lowerdir is passed as
 * /proc/self/fd/<sourceDirFd> — the caller pins the validated source with a
 * no-follow walk and keeps the FD open until this call returns, so a mutable
 * source path cannot be swapped after validation (a renamed source still
 * resolves to the same inode; a deleted one fails the mount closed).
 * upperdir/workdir are plain strings and must live under non-player-writable
 * storage (see SetupOverlayMount). The superblock is created with
 * FSCONFIG_CMD_CREATE before fsmount, as the new mount API requires.
 * @return 0 on success, -errno on failure
 */
int overlayMountFd(int sourceDirFd, const QString &upperdir, const QString &workdir,
                   int targetParentFd, const QString &leafName);

/**
 * Unmount whatever is attached at (targetParentFd, leafName) through the
 * pinned parent FD via /proc/self/fd — immune to ancestor swaps.
 * Tries a clean unmount, then a lazy detach.
 * @return 0 on success, -errno on failure
 */
int umountAtFd(int targetParentFd, const QString &leafName);

/**
 * Convenience: open the user's home directory (trusted — owned/maintained
 * by the system, not writable by the user's own privilege) as the anchor
 * for the calls above. Returns FD or -errno.
 */
int openBaseDir(const QString &absolutePath);
}
