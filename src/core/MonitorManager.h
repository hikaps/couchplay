// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <qqmlintegration.h>

/**
 * @brief Placeholder for monitor detection
 * Full implementation will use KScreen or Qt's screen APIs
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

    int monitorCount() const { return m_monitors.size(); }
    QVariantList monitorsAsVariant() const;

Q_SIGNALS:
    void monitorsChanged();

private:
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
};
