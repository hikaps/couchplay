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
bool copyTreeContents(const QString &src, const QString &dst, QString *error)
{
    const QDir srcDir(src);
    if (!srcDir.exists()) {
        return fail(error, QStringLiteral("Source folder is missing: %1").arg(src));
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
            if (!copyTreeContents(entry.filePath(), target, error)) {
                ok = false;
            }
            QFile::setPermissions(target, entry.permissions());
        } else {
            QFile::remove(target);
            if (!QFile::copy(entry.filePath(), target)) {
                ok = fail(error, QStringLiteral("Could not copy %1").arg(entry.filePath()));
            }
        }
    }
    return ok;
}

bool replaceTree(const QString &source, const QString &dest, const QString &stagingDir, QString *error)
{
    const QString fresh = dest + QStringLiteral(".couchplay-new");
    const QString old = dest + QStringLiteral(".couchplay-old");

    QDir(fresh).removeRecursively();
    QDir(old).removeRecursively();
    QDir().mkpath(QFileInfo(dest).absolutePath());

    bool ok = copyTreeContents(source, fresh, error);
    if (ok && !stagingDir.isEmpty() && QFileInfo(stagingDir).isDir()) {
        ok = copyTreeContents(stagingDir, fresh, error);
    }
    if (!ok) {
        QDir(fresh).removeRecursively();
        return false;
    }

    const bool hadDest = QFileInfo::exists(dest);
    if (hadDest && !QDir().rename(dest, old)) {
        QDir(fresh).removeRecursively();
        return fail(error, QStringLiteral("Could not replace %1").arg(dest));
    }
    if (!QDir().rename(fresh, dest)) {
        if (hadDest) {
            QDir().rename(old, dest);
        }
        QDir(fresh).removeRecursively();
        return fail(error, QStringLiteral("Could not replace %1").arg(dest));
    }
    QDir(old).removeRecursively();
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
