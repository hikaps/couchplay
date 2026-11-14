// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

#pragma once

#include <QHash>
#include <QObject>
#include <qqmlintegration.h>
#include <QString>
#include <QVariantMap>

class QProcess;

/**
 * @brief Connected screens with their physical pixel sizes
 *
 * Qt rounds a Wayland output's fractional scale up (150% reports a device pixel ratio of 2), so the real scale is
 * read from the host's `kscreen-doctor -j`. Until it has answered, or where it is not installed, Qt's ratio is used.
 */
class MonitorManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantList monitors READ monitorsAsVariant NOTIFY monitorsChanged)
    Q_PROPERTY(int monitorCount READ monitorCount NOTIFY monitorsChanged)

public:
    explicit MonitorManager(QObject *parent = nullptr);
    ~MonitorManager() override;

    Q_INVOKABLE void refresh();

    /** Physical pixels per logical pixel of the output named @p connector (1.0 when unknown). */
    qreal scaleFor(const QString &connector) const;

    /** Block for at most @p msecs while a scale query started earlier is still running (e.g. right after startup). */
    void waitForHostScales(int msecs = 2000);

    /** Output name to scale, from `kscreen-doctor -j` output; outputs that are not connected are left out. */
    static QHash<QString, qreal> parseKScreenScales(const QByteArray &json);

    int monitorCount() const
    {
        return m_monitors.size();
    }
    QVariantList monitorsAsVariant() const;

Q_SIGNALS:
    void monitorsChanged();

private:
    void rebuild();
    void queryHostScales();

    struct MonitorInfo {
        int index;
        QString name;
        QString connector; // e.g., HDMI-A-1, DP-1
        int width;
        int height;
        int refreshRate;
        bool primary;
    };

    QList<MonitorInfo> m_monitors;
    QHash<QString, qreal> m_hostScales;
    QProcess *m_scaleQuery = nullptr;
    bool m_scaleQueryPending = false;
};
