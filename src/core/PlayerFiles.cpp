// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 hikaps

#include "PlayerFiles.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <climits>
#include <unistd.h>

namespace
{
bool fail(QString *error, const QString &message)
{
    if (error && error->isEmpty()) {
        *error = message;
    }
    return false;
}
}

namespace PlayerFiles
{
bool hasSymlinkUnder(const QString &base, const QString &path)
{
    const QString cleanBase = QDir::cleanPath(base);
    const QString cleanPath = QDir::cleanPath(path);
    if (cleanPath != cleanBase && !cleanPath.startsWith(cleanBase + QLatin1Char('/'))) {
        return true;
    }
    QString current = cleanBase;
    const QStringList parts = cleanPath.mid(cleanBase.size()).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        current += QLatin1Char('/') + part;
        if (QFileInfo(current).isSymLink()) {
            return true;
        }
    }
    return false;
}

bool removeTree(const QString &path)
{
    const QFileInfo info(path);
    if (info.isSymLink() || !info.isDir()) {
        return !info.exists() && !info.isSymLink() ? true : QFile::remove(path);
    }
    QFile::setPermissions(path, info.permissions() | QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                    | QFileDevice::ExeOwner);
    bool ok = true;
    const QFileInfoList entries =
        QDir(path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo &entry : entries) {
        ok = removeTree(entry.filePath()) && ok;
    }
    return QDir().rmdir(path) && ok;
}

bool copyTreeContents(const QString &src, const QString &dst, QString *error)
{
    const QDir srcDir(src);
    if (!srcDir.exists()) {
        return fail(error, QStringLiteral("Source folder is missing: %1").arg(src));
    }
    // entryInfoList() is empty for an unreadable directory, which would read as an empty successful copy
    if (!QFileInfo(src).isReadable() || !QFileInfo(src).isExecutable()) {
        return fail(error, QStringLiteral("Source folder is not readable: %1").arg(src));
    }
    if (!QDir().mkpath(dst)) {
        return fail(error, QStringLiteral("Could not create %1").arg(dst));
    }

    bool ok = true;
    const QFileInfoList entries =
        srcDir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo &entry : entries) {
        const QString target = dst + QLatin1Char('/') + entry.fileName();
        if (entry.isSymLink()) {
            QFile::remove(target);
            char buffer[PATH_MAX];
            const ssize_t length = ::readlink(QFile::encodeName(entry.filePath()).constData(), buffer, sizeof(buffer) - 1);
            if (length < 0) {
                ok = fail(error, QStringLiteral("Could not read link %1").arg(entry.filePath()));
                continue;
            }
            buffer[length] = '\0';
            if (::symlink(buffer, QFile::encodeName(target).constData()) != 0) {
                ok = fail(error, QStringLiteral("Could not copy link %1").arg(entry.filePath()));
            }
        } else if (entry.isDir()) {
            // A symlink already sitting at the destination must not redirect the copy elsewhere.
            if (QFileInfo(target).isSymLink()) {
                QFile::remove(target);
            }
            if (!copyTreeContents(entry.filePath(), target, error)) {
                ok = false;
            }
            // Keep the owner able to read/write/enter so later overlays and removal still work.
            QFile::setPermissions(target,
                                  entry.permissions() | QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                      | QFileDevice::ExeOwner);
        } else if (entry.isFile()) {
            QFile::remove(target);
            if (!QFile::copy(entry.filePath(), target)) {
                ok = fail(error, QStringLiteral("Could not copy %1").arg(entry.filePath()));
            }
        }
        // FIFOs, sockets and device nodes are skipped: copying them would block or fail.
    }
    return ok;
}

bool replaceTree(const QString &source, const QString &dest, const QString &stagingDir, QString *error)
{
    const QString fresh = dest + QStringLiteral(".couchplay-new");
    const QString old = dest + QStringLiteral(".couchplay-old");

    // A crash between the two renames below leaves the previous copy only in "old": bring it back first
    if (!QFileInfo::exists(dest) && QFileInfo(old).isDir()) {
        QDir().rename(old, dest);
    }
    removeTree(fresh);
    removeTree(old);
    QDir().mkpath(QFileInfo(dest).absolutePath());

    bool ok = copyTreeContents(source, fresh, error);
    if (ok && !stagingDir.isEmpty() && QFileInfo(stagingDir).isDir()) {
        ok = copyTreeContents(stagingDir, fresh, error);
    }
    if (!ok) {
        removeTree(fresh);
        return false;
    }

    const bool hadDest = QFileInfo::exists(dest);
    if (hadDest && !QDir().rename(dest, old)) {
        removeTree(fresh);
        return fail(error, QStringLiteral("Could not replace %1").arg(dest));
    }
    if (!QDir().rename(fresh, dest)) {
        if (hadDest) {
            QDir().rename(old, dest);
        }
        removeTree(fresh);
        return fail(error, QStringLiteral("Could not replace %1").arg(dest));
    }
    removeTree(old);
    return true;
}

QString dataDirectoryStagingSlug(const QString &dirPath, const QString &home)
{
    QString relative;
    if (!home.isEmpty() && dirPath.startsWith(home + QLatin1Char('/'))) {
        relative = dirPath.mid(home.length() + 1);
    } else {
        relative = dirPath;
        while (relative.startsWith(QLatin1Char('/'))) {
            relative.remove(0, 1);
        }
    }
    QString slug = relative;
    slug.replace(QLatin1Char('/'), QLatin1Char('_'));
    const QByteArray hash = QCryptographicHash::hash(relative.toUtf8(), QCryptographicHash::Sha256).toHex().left(8);
    slug += QLatin1Char('-') + QString::fromLatin1(hash);
    return slug;
}
}
