// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include "../dbus/CouchPlayHelperClient.h"

#include <pwd.h>
#include <unistd.h>

#include <QString>
#include <QVariantMap>

struct UserIdentity {
    bool valid = false;
    uint uid = 0;
    uint gid = 0;
    QString home;
};

inline UserIdentity resolveUserIdentity(const QString &username, CouchPlayHelperClient *helper)
{
    UserIdentity id;

    if (helper && helper->isAvailable()) {
        const QVariantMap m = helper->getUserInfo(username);
        if (!m.isEmpty()) {
            id.valid = true;
            id.uid = m.value(QStringLiteral("uid")).toUInt();
            id.gid = m.value(QStringLiteral("gid")).toUInt();
            id.home = m.value(QStringLiteral("home")).toString();
        }
        return id;
    }

    if (struct passwd *pw = getpwnam(username.toLocal8Bit().constData())) {
        id.valid = true;
        id.uid = pw->pw_uid;
        id.gid = pw->pw_gid;
        id.home = QString::fromLocal8Bit(pw->pw_dir);
    }

    return id;
}

// Resolve the compositor's own home directory. Under Flatpak the sandbox's
// getpwuid(getuid()) cannot see CouchPlay-created host accounts and may
// return a sandbox-local entry, so prefer the root helper's host-side answer
// before falling back to the process-local lookup.
inline QString resolveCompositorHome(CouchPlayHelperClient *helper)
{
    if (helper && helper->isAvailable()) {
        const QString home = helper->getUserHomeByUid(static_cast<uint>(::getuid()));
        if (!home.isEmpty()) {
            return home;
        }
    }
    if (struct passwd *pw = ::getpwuid(::getuid())) {
        return QString::fromLocal8Bit(pw->pw_dir);
    }
    return QString();
}
