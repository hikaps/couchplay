// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 hikaps

#pragma once

#include <QString>

namespace PlayerFiles
{
/**
 * Copy the contents of @p src into @p dst (created if missing), merging over existing files. Modes are
 * preserved and symlinks are recreated as symlinks, never followed.
 * @return false when anything could not be copied; @p error then describes the first failure.
 */
bool copyTreeContents(const QString &src, const QString &dst, QString *error = nullptr);

/**
 * Replace @p dest with a fresh copy of @p source, then lay @p stagingDir (may be empty or missing) over it.
 * The copy is built next to @p dest first, so a failure leaves the previous @p dest untouched.
 */
bool replaceTree(const QString &source, const QString &dest, const QString &stagingDir, QString *error = nullptr);

/**
 * Stable directory name for a shared data directory: the path relative to @p home (or without the leading
 * slash when outside it) with '/' → '_', plus a short hash.
 */
QString dataDirectoryStagingSlug(const QString &dirPath, const QString &home);
}
