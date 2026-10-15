// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace SecureFs
{
// Monotonic count of entries/chunks processed by long tree operations; lock-free, async-signal-safe to read.
// Call shareProgressAcrossFork() once, before forking, so forked tree walkers report into the same counter.
uint64_t progressCount();
void noteProgress();
void shareProgressAcrossFork();
enum class ChownMode {
    All,
    FinalOnly,
};

int openDirBelow(int baseFd, const QStringList &parts, bool create, uid_t uid, gid_t gid,
                 ChownMode chownMode = ChownMode::All);
int openExistingDirNoFollow(const QString &absolutePath);
int removeTreeAt(int dirFd);
int copyTreeContents(int srcDirFd, int dstDirFd, uid_t uid, gid_t gid);
int replaceTreeContents(int srcDirFd, int stagingDirFd, int targetParentFd, const QString &leafName,
                        const QString &scratchName, uid_t uid, gid_t gid);
int removeCopyScratchAt(int parentFd, const QString &scratchName);
int writeFileAt(int parentFd, const QString &name, const QByteArray &content, uid_t uid, gid_t gid, mode_t mode = 0644);
int applyReadExecuteAcl(int objectFd, uid_t targetUid);
int applyReadExecuteAclTree(int directoryFd, uid_t targetUid);

bool mountApiAvailable();
bool overlayMountApiAvailable();
int cloneMountFd(int sourceDirFd);
int bindMountFd(int sourceMountFd, int targetParentFd, const QString &leafName);
int cloneIdmappedLowerFd(int sourceDirFd, int userNamespaceFd);
int overlayMountFd(int sourceMountFd, const QString &upperdir, const QString &workdir,
                   int targetParentFd, const QString &leafName);
int openBaseDir(const QString &absolutePath);
}
