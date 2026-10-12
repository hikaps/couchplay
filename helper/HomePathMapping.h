// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#pragma once

#include <QDir>
#include <QString>

#include <initializer_list>

inline bool pathHasDotDotComponent(const QString &path)
{
    qsizetype start = 0;
    while (start < path.size()) {
        const qsizetype slash = path.indexOf(QLatin1Char('/'), start);
        const qsizetype end = slash < 0 ? path.size() : slash;
        if (end - start == 2 && path.at(start) == QLatin1Char('.') && path.at(start + 1) == QLatin1Char('.')) {
            return true;
        }
        start = end + 1;
    }
    return false;
}

enum class HomePathKind {
    Relative,
    External,
    Invalid
};

struct HomePathMapping
{
    HomePathKind kind;
    QString relativePath;
};

// A home-relative source must be a strict descendant of either spelling of
// the account home. Reject traversal before normalization; never reclassify
// an invalid path as an external directory.
inline HomePathMapping mapHomeRelativePath(const QString &path,
                                          const QString &declaredHome,
                                          const QString &canonicalHome)
{
    if (!QDir::isAbsolutePath(path) || pathHasDotDotComponent(path)) {
        return {HomePathKind::Invalid, {}};
    }
    const QString source = QDir::cleanPath(path);
    for (const QString *home : {&declaredHome, &canonicalHome}) {
        if (home->isEmpty() || !QDir::isAbsolutePath(*home)) {
            continue;
        }
        const QString root = QDir::cleanPath(*home);
        if (source == root) {
            return {HomePathKind::Invalid, {}};
        }
        if (root == QStringLiteral("/")) {
            return {HomePathKind::Relative, source.mid(1)};
        }
        if (source.startsWith(root) && source.size() > root.size() && source.at(root.size()) == QLatin1Char('/')) {
            return {HomePathKind::Relative, source.mid(root.size() + 1)};
        }
    }
    return {HomePathKind::External, {}};
}
