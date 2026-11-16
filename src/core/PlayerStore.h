// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 hikaps

#pragma once

#include <QObject>
#include <qqmlintegration.h>
#include <QString>
#include <QStringList>

/**
 * @brief Per-player storage for rootless sandboxed players
 *
 * A player is a directory below the player root, not a Linux account:
 *   players/<name>/home/                 bound at $HOME inside the player's sandbox
 *   players/<name>/layers/<key>/{upper,work}   overlay storage for shared game libraries
 *
 * The root is `<AppDataLocation>/players` under Flatpak and `~/.local/share/couchplay/players`
 * natively, so it is the same host-visible location either way.
 */
class PlayerStore : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QStringList players READ players NOTIFY playersChanged)

public:
    explicit PlayerStore(QObject *parent = nullptr);

    /** Players currently on disk, sorted. */
    QStringList players() const
    {
        return m_players;
    }

    /** Root directory holding all players. */
    static QString rootDir();

    /** `^[a-z_][a-z0-9_-]{0,31}$`; also rules out path traversal. */
    Q_INVOKABLE static bool isValidName(const QString &name);
    Q_INVOKABLE bool exists(const QString &name) const;

    Q_INVOKABLE void refresh();
    Q_INVOKABLE bool createPlayer(const QString &name);
    /** Removes the player's directory and its Sunshine state (pairing). */
    Q_INVOKABLE bool deletePlayer(const QString &name);

    /** Host path of the player's home (not created). Empty for an invalid name. */
    static QString homeDir(const QString &name);
    /** Host path of the overlay layer for @p source; contains `upper` and `work`. Empty for an invalid name. */
    static QString layerDir(const QString &name, const QString &source);

Q_SIGNALS:
    void playersChanged();
    void playerCreated(const QString &name);
    void playerDeleted(const QString &name);
    void errorOccurred(const QString &message);

private:
    QStringList m_players;
};
