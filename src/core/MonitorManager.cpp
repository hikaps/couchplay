// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

#include "MonitorManager.h"

#include "HostProcess.h"

#include <QDebug>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QScreen>

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
    // A scale change resizes the screen's logical geometry
    for (QScreen *screen : QGuiApplication::screens()) {
        connect(screen, &QScreen::geometryChanged, this, &MonitorManager::refresh, Qt::UniqueConnection);
    }
    rebuild();
    queryHostScales();
}

void MonitorManager::rebuild()
{
    m_monitors.clear();

    QList<QScreen *> screens = QGuiApplication::screens();
    QScreen *primaryScreen = QGuiApplication::primaryScreen();

    for (int i = 0; i < screens.size(); ++i) {
        QScreen *screen = screens[i];

        MonitorInfo info;
        info.index = i;
        info.name = screen->name();
        info.connector = screen->name(); // On Linux, name usually is the connector
        // QScreen::size() is in logical pixels; at 250% scale a 4K panel reports 1536x864.
        const qreal scale = scaleFor(info.connector);
        info.width = qRound(screen->size().width() * scale);
        info.height = qRound(screen->size().height() * scale);
        info.refreshRate = qRound(screen->refreshRate());
        info.primary = (screen == primaryScreen);

        m_monitors.append(info);
    }

    Q_EMIT monitorsChanged();
}

void MonitorManager::queryHostScales()
{
    if (!m_scaleQuery) {
        m_scaleQuery = new QProcess(this);
        auto finishQuery = [this]() {
            if (m_scaleQueryPending) {
                m_scaleQueryPending = false;
                queryHostScales();
            }
        };
        connect(m_scaleQuery,
                &QProcess::finished,
                this,
                [this, finishQuery](int exitCode, QProcess::ExitStatus exitStatus) {
                    if (exitStatus == QProcess::NormalExit && exitCode == 0) {
                        const QHash<QString, qreal> scales = parseKScreenScales(m_scaleQuery->readAllStandardOutput());
                        if (scales != m_hostScales) {
                            m_hostScales = scales;
                            rebuild();
                        }
                    } else {
                        qDebug() << "kscreen-doctor failed; using Qt's screen scale";
                    }
                    finishQuery();
                });
        connect(m_scaleQuery, &QProcess::errorOccurred, this, [finishQuery](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                qDebug() << "kscreen-doctor unavailable; using Qt's screen scale";
                finishQuery();
            }
        });
    }
    if (m_scaleQuery->state() != QProcess::NotRunning) {
        m_scaleQueryPending = true;
        return;
    }
    HostProcess::configure(*m_scaleQuery, QStringLiteral("kscreen-doctor"), {QStringLiteral("-j")});
    m_scaleQuery->start();
}

qreal MonitorManager::scaleFor(const QString &connector) const
{
    const auto known = m_hostScales.constFind(connector);
    if (known != m_hostScales.cend()) {
        return *known;
    }
    for (QScreen *screen : QGuiApplication::screens()) {
        if (screen->name() == connector) {
            return screen->devicePixelRatio();
        }
    }
    return 1.0;
}

void MonitorManager::waitForHostScales(int msecs)
{
    if (m_scaleQuery && m_scaleQuery->state() != QProcess::NotRunning) {
        m_scaleQuery->waitForFinished(msecs);
    }
}

QHash<QString, qreal> MonitorManager::parseKScreenScales(const QByteArray &json)
{
    QHash<QString, qreal> scales;
    const QJsonArray outputs = QJsonDocument::fromJson(json).object().value(QStringLiteral("outputs")).toArray();
    for (const QJsonValue &value : outputs) {
        const QJsonObject output = value.toObject();
        const QString name = output.value(QStringLiteral("name")).toString();
        const double scale = output.value(QStringLiteral("scale")).toDouble();
        if (output.value(QStringLiteral("connected")).toBool() && !name.isEmpty() && scale > 0) {
            scales.insert(name, scale);
        }
    }
    return scales;
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
