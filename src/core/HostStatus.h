// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 hikaps

#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <qqmlintegration.h>

/**
 * @brief What the host must provide for player sandboxes, plus leftovers of the old root helper.
 *
 * One host-side shell probe (through HostProcess, so it also sees the real host from the Flatpak) checks that
 * bwrap, dbus-run-session and gamescope exist, that /dev/uinput is writable, and whether the old helper service,
 * binary or `couchplay` group is still installed. The probe runs asynchronously; until it finishes nothing is
 * reported missing.
 */
class HostStatus : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    /** Human-readable entries such as "bwrap (package: bubblewrap)". */
    Q_PROPERTY(QStringList missingTools READ missingTools NOTIFY changed)
    Q_PROPERTY(bool uinputWritable READ uinputWritable NOTIFY changed)
    Q_PROPERTY(bool legacyHelperFound READ legacyHelperFound NOTIFY changed)
    /** The migration command for this user and install type (Flatpak or native). */
    Q_PROPERTY(QString migrationCommand READ migrationCommand CONSTANT)

public:
    struct Result {
        QStringList missingTools;
        bool uinputWritable = true;
        bool legacyHelperFound = false;
    };

    explicit HostStatus(QObject *parent = nullptr);

    QStringList missingTools() const { return m_result.missingTools; }
    bool uinputWritable() const { return m_result.uinputWritable; }
    bool legacyHelperFound() const { return m_result.legacyHelperFound; }
    QString migrationCommand() const;

    Q_INVOKABLE void refresh();

    /** Shell script run on the host; prints `missing <tool>`, `nouinput` and `legacy` lines. */
    static QString probeScript();
    static Result parse(const QString &output);

Q_SIGNALS:
    void changed();

private:
    QProcess m_process;
    Result m_result;
};
