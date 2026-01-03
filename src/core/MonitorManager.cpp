// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

#include "MonitorManager.h"

#include <QGuiApplication>
#include <QScreen>
#include <QDebug>

MonitorManager::MonitorManager(QObject *parent)
    : QObject(parent)
{
    refresh();

    // Connect to screen changes
    connect(qApp, &QGuiApplication::screenAdded, this, &MonitorManager::refresh);
    connect(qApp, &QGuiApplication::screenRemoved, this, &MonitorManager::refresh);
}

MonitorManager::~MonitorManager() = default;

void MonitorManager::refresh()
{
    m_monitors.clear();

    QList<QScreen*> screens = QGuiApplication::screens();
    QScreen *primaryScreen = QGuiApplication::primaryScreen();

    for (int i = 0; i < screens.size(); ++i) {
        QScreen *screen = screens[i];

        MonitorInfo info;
        info.index = i;
        info.name = screen->name();
        info.connector = screen->name(); // On Linux, name usually is the connector
        info.width = screen->size().width();
        info.height = screen->size().height();
        info.refreshRate = qRound(screen->refreshRate());
        info.primary = (screen == primaryScreen);

        m_monitors.append(info);
    }

    Q_EMIT monitorsChanged();
}

QVariantList MonitorManager::monitorsAsVariant() const
{
    QVariantList list;
    for (const auto &monitor : m_monitors) {
        QVariantMap map;
        map[QStringLiteral("index")] = monitor.index;
        map[QStringLiteral("name")] = monitor.name;
        map[QStringLiteral("connector")] = monitor.connector;
        map[QStringLiteral("width")] = monitor.width;
        map[QStringLiteral("height")] = monitor.height;
        map[QStringLiteral("refreshRate")] = monitor.refreshRate;
        map[QStringLiteral("primary")] = monitor.primary;
        map[QStringLiteral("displayString")] = QStringLiteral("%1 (%2x%3 @ %4Hz)")
            .arg(monitor.name)
            .arg(monitor.width)
            .arg(monitor.height)
            .arg(monitor.refreshRate);
        list.append(map);
    }
    return list;
}
