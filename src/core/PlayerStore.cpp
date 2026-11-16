// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 hikaps

#include "PlayerStore.h"

#include "HostProcess.h"
#include "PlayerFiles.h"
#include "SunshineConfig.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>

#include <KLocalizedString>

PlayerStore::PlayerStore(QObject *parent)
    : QObject(parent)
{
    refresh();
}

QString PlayerStore::rootDir()
{
    if (HostProcess::runningInFlatpak()) {
        return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/players");
    }
    return QDir::homePath() + QStringLiteral("/.local/share/couchplay/players");
}

bool PlayerStore::isValidName(const QString &name)
{
    static const QRegularExpression re(QStringLiteral("\\A[a-z_][a-z0-9_-]{0,31}\\z"));
    return re.match(name).hasMatch();
}

bool PlayerStore::exists(const QString &name) const
{
    return isValidName(name) && QFileInfo(homeDir(name)).isDir();
}

void PlayerStore::refresh()
{
    QStringList found;
    const QDir root(rootDir());
    for (const QString &entry : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (isValidName(entry) && QFileInfo(homeDir(entry)).isDir()) {
            found << entry;
        }
    }
    if (found != m_players) {
        m_players = found;
        Q_EMIT playersChanged();
    }
}

QString PlayerStore::homeDir(const QString &name)
{
    if (!isValidName(name)) {
        return {};
    }
    return rootDir() + QLatin1Char('/') + name + QStringLiteral("/home");
}

QString PlayerStore::layerDir(const QString &name, const QString &source)
{
    if (!isValidName(name)) {
        return {};
    }
    const QByteArray digest =
        QCryptographicHash::hash(QDir::cleanPath(source).toUtf8(), QCryptographicHash::Sha256).toHex().left(16);
    return rootDir() + QLatin1Char('/') + name + QStringLiteral("/layers/") + QString::fromLatin1(digest);
}

bool PlayerStore::createPlayer(const QString &name)
{
    if (!isValidName(name)) {
        Q_EMIT errorOccurred(i18n("Invalid player name. Use lowercase letters, digits, '-' and '_' (max 32)."));
        return false;
    }
    if (exists(name)) {
        Q_EMIT errorOccurred(i18n("Player \"%1\" already exists.", name));
        return false;
    }

    const QString playerDir = rootDir() + QLatin1Char('/') + name;
    // Parents get 0700 so other local users cannot read player data.
    QDir().mkpath(rootDir());
    QFile::setPermissions(rootDir(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    if (!QDir().mkpath(homeDir(name))) {
        Q_EMIT errorOccurred(i18n("Could not create %1", playerDir));
        return false;
    }
    QFile::setPermissions(playerDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    refresh();
    Q_EMIT playerCreated(name);
    return true;
}

bool PlayerStore::deletePlayer(const QString &name)
{
    if (!isValidName(name)) {
        Q_EMIT errorOccurred(i18n("Invalid player name."));
        return false;
    }

    const QString playerPath = rootDir() + QLatin1Char('/') + name;
    if (QFileInfo::exists(playerPath) && !PlayerFiles::removeTree(playerPath)) {
        Q_EMIT errorOccurred(i18n("Could not delete the files of player \"%1\".", name));
        refresh();
        return false;
    }
    SunshineConfig::removeState(name);

    refresh();
    Q_EMIT playerDeleted(name);
    return true;
}
