// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "WindowManager.h"

#include <QByteArray>
#include <QDateTime>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDebug>
#include <QDir>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QUuid>

WindowManager::WindowManager(QObject *parent)
    : QObject(parent)
{
    QDBusInterface kwin(QStringLiteral("org.kde.KWin"),
                        QStringLiteral("/KWin"),
                        QStringLiteral("org.kde.KWin"),
                        QDBusConnection::sessionBus());
    m_kwinAvailable = kwin.isValid();

    if (!m_kwinAvailable) {
        qWarning() << "WindowManager: KWin D-Bus interface not available";
    } else {
        qDebug() << "WindowManager: KWin D-Bus interface available";
    }

    m_monitorTimer = new QTimer(this);
    m_monitorTimer->setInterval(MONITOR_INTERVAL_MS);
    connect(m_monitorTimer, &QTimer::timeout, this, &WindowManager::checkForNewWindows);

    m_positionTimer = new QTimer(this);
    m_positionTimer->setInterval(POSITION_CONFIRM_INTERVAL_MS);
    connect(m_positionTimer, &QTimer::timeout, this, &WindowManager::checkPositionConfirmations);

    // Use the per-app cache, which is visible to host KWin when CouchPlay runs in Flatpak.
    const QString scriptDir =
        QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).filePath(QStringLiteral("couchplay"));
    QDir staleDir(scriptDir);
    if (staleDir.exists()) {
        QStringList staleScripts =
            staleDir.entryList(QStringList{QStringLiteral("couchplay-position-*.js")}, QDir::Files);
        for (const QString &script : staleScripts) {
            staleDir.remove(script);
        }
        if (!staleScripts.isEmpty()) {
            qDebug() << "WindowManager: Cleaned up" << staleScripts.size() << "stale positioning scripts";
        }
    }
}

WindowManager::~WindowManager() = default;

bool WindowManager::isAvailable() const
{
    return m_kwinAvailable;
}

QString WindowManager::findGamescopeWindow(const QStringList &excludeWindowIds)
{
    if (!m_kwinAvailable) {
        return QString();
    }

    QStringList allWindows = findAllGamescopeWindows();

    for (const QString &uuid : allWindows) {
        if (!excludeWindowIds.contains(uuid) && !m_knownWindowIds.contains(uuid)
            && !m_reservedWindowIds.contains(uuid)) {
            return uuid;
        }
    }

    return QString();
}

QStringList WindowManager::findAllGamescopeWindows(qint64 gamescopePid)
{
    QStringList results;

    if (!m_kwinAvailable) {
        return results;
    }

    QDBusInterface windowsRunner(QStringLiteral("org.kde.KWin"),
                                 QStringLiteral("/WindowsRunner"),
                                 QStringLiteral("org.kde.krunner1"),
                                 QDBusConnection::sessionBus());

    if (!windowsRunner.isValid()) {
        qWarning() << "WindowManager: WindowsRunner interface not available";
        return results;
    }

    // Query for all windows (empty string returns all)
    QDBusMessage reply = windowsRunner.call(QStringLiteral("Match"), QString());

    if (reply.type() != QDBusMessage::ReplyMessage) {
        qWarning() << "WindowManager: Failed to query windows:" << reply.errorMessage();
        return results;
    }

    if (reply.arguments().isEmpty()) {
        return results;
    }

    // The properties dict contains complex types (e.g. icon-data with struct (iiibiiay))
    // that break QDBusArgument parsing. We only need matchId, so skip the rest.
    const QDBusArgument arg = reply.arguments().at(0).value<QDBusArgument>();

    if (arg.currentType() != QDBusArgument::ArrayType) {
        qWarning() << "WindowManager: Unexpected reply type from WindowsRunner";
        return results;
    }

    arg.beginArray();

    while (!arg.atEnd()) {
        arg.beginStructure();

        QString matchId;
        QString caption;
        QString iconName;
        quint32 relevance;
        double score;

        arg >> matchId >> caption >> iconName >> relevance >> score;

        QVariant propertiesVariant;
        arg >> propertiesVariant;

        arg.endStructure();

        if (matchId.contains(QLatin1Char('{'))) {
            QString uuid = matchId.mid(matchId.indexOf(QLatin1Char('{')));

            QVariantMap info = getWindowInfo(uuid);
            QString desktopFile = info.value(QStringLiteral("desktopFile")).toString();
            QString resourceClass = info.value(QStringLiteral("resourceClass")).toString();

            const bool matches = gamescopePid > 0
                ? info.value(QStringLiteral("pid")).toLongLong() == gamescopePid
                : desktopFile == QStringLiteral("gamescope") || resourceClass == QStringLiteral("gamescope");
            if (matches) {
                results << uuid;
                qDebug() << "WindowManager: Found gamescope window" << uuid << "caption:" << caption;
            }
        }
    }

    arg.endArray();

    return results;
}

QVariantMap WindowManager::getWindowInfo(const QString &windowId)
{
    QVariantMap info;

    if (!m_kwinAvailable || windowId.isEmpty()) {
        return info;
    }

    QDBusInterface kwin(QStringLiteral("org.kde.KWin"),
                        QStringLiteral("/KWin"),
                        QStringLiteral("org.kde.KWin"),
                        QDBusConnection::sessionBus());
    kwin.setTimeout(5000);

    if (!kwin.isValid()) {
        qWarning() << "WindowManager: KWin interface not available";
        return info;
    }

    QDBusReply<QVariantMap> reply = kwin.call(QStringLiteral("getWindowInfo"), windowId);

    if (!reply.isValid()) {
        qWarning() << "WindowManager: Failed to get window info for" << windowId << ":" << reply.error().message();
        return info;
    }

    info = reply.value();

    // Convert double values to int for geometry (KWin returns doubles)
    if (info.contains(QStringLiteral("x"))) {
        info[QStringLiteral("x")] = info.value(QStringLiteral("x")).toInt();
    }
    if (info.contains(QStringLiteral("y"))) {
        info[QStringLiteral("y")] = info.value(QStringLiteral("y")).toInt();
    }
    if (info.contains(QStringLiteral("width"))) {
        info[QStringLiteral("width")] = info.value(QStringLiteral("width")).toInt();
    }
    if (info.contains(QStringLiteral("height"))) {
        info[QStringLiteral("height")] = info.value(QStringLiteral("height")).toInt();
    }

    return info;
}

bool WindowManager::positionWindow(const QString &windowId, const QRect &geometry, bool borderless)
{
    if (!m_kwinAvailable || windowId.isEmpty()) {
        Q_EMIT positioningFailed(windowId, QStringLiteral("KWin not available or invalid window ID"));
        return false;
    }

    qDebug() << "WindowManager: Positioning window" << windowId << "to" << geometry;
    if (!executePositionScript(windowId, geometry, borderless)) {
        return false;
    }

    PendingPosition pending;
    // A newer direct placement supersedes any unconfirmed direct request for this window.
    for (int i = m_pendingPositions.size() - 1; i >= 0; --i) {
        const PendingPosition &existing = m_pendingPositions.at(i);
        if (!existing.queued && existing.windowId == windowId) {
            m_pendingPositions.removeAt(i);
        }
    }

    pending.requestId = 0;
    pending.windowId = windowId;
    pending.geometry = geometry;
    pending.queued = false;
    pending.expiresAt = QDateTime::currentMSecsSinceEpoch() + POSITION_CONFIRM_TIMEOUT_MS;
    m_pendingPositions.append(pending);
    if (!m_reservedWindowIds.contains(windowId)) {
        m_reservedWindowIds.append(windowId);
    }
    m_positionTimer->start();
    return true;
}

void WindowManager::queuePositionRequest(int requestId,
                                         qint64 gamescopePid,
                                         const QRect &geometry,
                                         const QStringList &excludeWindowIds,
                                         bool borderless,
                                         int timeoutMs)
{
    if (!m_kwinAvailable) {
        qWarning() << "WindowManager: Cannot queue position request - KWin not available";
        Q_EMIT positioningTimedOut(requestId);
        return;
    }

    cancelPositionRequest(requestId);

    PositionRequest request;
    request.requestId = requestId;
    request.gamescopePid = gamescopePid;
    request.geometry = geometry;
    request.excludeWindowIds = excludeWindowIds;
    request.borderless = borderless;
    request.expiresAt = QDateTime::currentMSecsSinceEpoch() + timeoutMs;

    m_pendingRequests.append(request);

    qDebug() << "WindowManager: Queued position request" << requestId << "geometry:" << geometry << "excluding"
             << excludeWindowIds.size() << "windows"
             << "timeout:" << timeoutMs << "ms";

    startMonitoring();
    checkForNewWindows();
}

void WindowManager::cancelPositionRequest(int requestId)
{
    if (m_emittingQueuedRequest && m_emittingRequestId == requestId) {
        m_emittingRequestCancelled = true;
    }
    for (int i = 0; i < m_pendingRequests.size(); ++i) {
        if (m_pendingRequests[i].requestId == requestId) {
            m_pendingRequests.removeAt(i);
            qDebug() << "WindowManager: Cancelled position request" << requestId;
            break;
        }
    }

    for (int i = m_pendingPositions.size() - 1; i >= 0; --i) {
        if (m_pendingPositions[i].queued && m_pendingPositions[i].requestId == requestId) {
            const QString windowId = m_pendingPositions[i].windowId;
            m_pendingPositions.removeAt(i);
            releaseWindowReservation(windowId);
            qDebug() << "WindowManager: Cancelled positioning confirmation for request" << requestId;
        }
    }

    stopMonitoringIfEmpty();
    stopPositionConfirmationIfEmpty();
}

void WindowManager::cancelAllRequests()
{
    ++m_cancelAllGeneration;
    int count = m_pendingRequests.size() + m_pendingPositions.size();
    m_pendingRequests.clear();
    m_pendingPositions.clear();
    m_knownWindowIds.clear();
    m_reservedWindowIds.clear();
    stopMonitoringIfEmpty();
    stopPositionConfirmationIfEmpty();

    qDebug() << "WindowManager: Cancelled all" << count << "pending requests";
}

bool WindowManager::hasPendingRequests() const
{
    return !m_pendingRequests.isEmpty() || !m_pendingPositions.isEmpty();
}

void WindowManager::checkForNewWindows()
{
    if (m_pendingRequests.isEmpty()) {
        stopMonitoringIfEmpty();
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<int> expiredRequestIds;
    for (int i = m_pendingRequests.size() - 1; i >= 0; --i) {
        if (m_pendingRequests[i].expiresAt <= now) {
            expiredRequestIds.append(m_pendingRequests[i].requestId);
            m_pendingRequests.removeAt(i);
        }
    }

    for (int requestId : expiredRequestIds) {
        qWarning() << "WindowManager: Position request" << requestId << "timed out";
        Q_EMIT positioningTimedOut(requestId);
    }

    if (m_pendingRequests.isEmpty()) {
        stopMonitoringIfEmpty();
        return;
    }

    QStringList attemptedWindowIds;
    int i = 0;
    while (i < m_pendingRequests.size()) {
        const PositionRequest request = m_pendingRequests[i];
        const QStringList currentWindows = findAllGamescopeWindows(request.gamescopePid);
        QString matchedWindowId;
        for (const QString &windowId : currentWindows) {
            if (!request.excludeWindowIds.contains(windowId) && !m_knownWindowIds.contains(windowId)
                && !m_reservedWindowIds.contains(windowId) && !attemptedWindowIds.contains(windowId)) {
                matchedWindowId = windowId;
                break;
            }
        }

        if (matchedWindowId.isEmpty()) {
            ++i;
            continue;
        }

        qDebug() << "WindowManager: Matched window" << matchedWindowId << "to request" << request.requestId;
        attemptedWindowIds.append(matchedWindowId);
        if (!executePositionScript(matchedWindowId, request.geometry, request.borderless)) {
            for (int pendingIndex = 0; pendingIndex < m_pendingRequests.size(); ++pendingIndex) {
                const PositionRequest &pending = m_pendingRequests[pendingIndex];
                if (pending.requestId == request.requestId && pending.expiresAt == request.expiresAt) {
                    m_pendingRequests.removeAt(pendingIndex);
                    Q_EMIT positioningTimedOut(request.requestId);
                    break;
                }
            }
            ++i;
            continue;
        }

        m_pendingRequests.removeAt(i);
        if (!m_reservedWindowIds.contains(matchedWindowId)) {
            m_reservedWindowIds.append(matchedWindowId);
        }
        PendingPosition pending;
        pending.requestId = request.requestId;
        pending.windowId = matchedWindowId;
        pending.geometry = request.geometry;
        pending.queued = true;
        pending.expiresAt = request.expiresAt;
        m_pendingPositions.append(pending);
        m_positionTimer->start();
    }

    stopMonitoringIfEmpty();
}

void WindowManager::checkPositionConfirmations()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int i = m_pendingPositions.size() - 1; i >= 0; --i) {
        const PendingPosition &pending = m_pendingPositions[i];
        const bool expired = pending.expiresAt <= now;
        bool geometryConfirmed = false;
        if (!expired) {
            const QVariantMap actual = getWindowInfo(pending.windowId);
            geometryConfirmed = actual.contains(QStringLiteral("x")) && actual.contains(QStringLiteral("y"))
                && actual.contains(QStringLiteral("width")) && actual.contains(QStringLiteral("height"))
                && actual.value(QStringLiteral("x")).toInt() == pending.geometry.x()
                && actual.value(QStringLiteral("y")).toInt() == pending.geometry.y()
                && actual.value(QStringLiteral("width")).toInt() == pending.geometry.width()
                && actual.value(QStringLiteral("height")).toInt() == pending.geometry.height();
        }

        if (!expired && !geometryConfirmed) {
            continue;
        }

        const PendingPosition completed = m_pendingPositions.takeAt(i);
        releaseWindowReservation(completed.windowId);
        if (geometryConfirmed) {
            if (!m_knownWindowIds.contains(completed.windowId)) {
                m_knownWindowIds.append(completed.windowId);
            }
        }
        stopPositionConfirmationIfEmpty();

        const quint64 cancelAllGeneration = m_cancelAllGeneration;
        m_emittingQueuedRequest = completed.queued;
        m_emittingRequestId = completed.requestId;
        m_emittingRequestCancelled = false;
        if (geometryConfirmed) {
            qDebug() << "WindowManager: KWin confirmed frame geometry for window" << completed.windowId
                     << completed.geometry;
            Q_EMIT windowPositioned(completed.windowId, completed.geometry);
            if (completed.queued && cancelAllGeneration == m_cancelAllGeneration && !m_emittingRequestCancelled) {
                Q_EMIT gamescopeWindowPositioned(completed.requestId, completed.windowId);
            }
        } else {
            qWarning() << "WindowManager: Geometry confirmation timed out for window" << completed.windowId;
            if (completed.queued) {
                Q_EMIT positioningTimedOut(completed.requestId);
            } else {
                Q_EMIT positioningFailed(completed.windowId,
                                         QStringLiteral("Timed out waiting for requested frame geometry"));
            }
        }
        m_emittingQueuedRequest = false;
        m_emittingRequestCancelled = false;
        return;
    }

    stopPositionConfirmationIfEmpty();
}

void WindowManager::stopPositionConfirmationIfEmpty()
{
    if (m_pendingPositions.isEmpty() && m_positionTimer->isActive()) {
        m_positionTimer->stop();
    }
}

void WindowManager::releaseWindowReservation(const QString &windowId)
{
    for (const PendingPosition &pending : m_pendingPositions) {
        if (pending.windowId == windowId) {
            return;
        }
    }
    m_reservedWindowIds.removeAll(windowId);
}

void WindowManager::startMonitoring()
{
    if (!m_monitorTimer->isActive()) {
        qDebug() << "WindowManager: Starting window monitoring (interval:" << MONITOR_INTERVAL_MS << "ms)";
        m_monitorTimer->start();
    }
}

void WindowManager::stopMonitoringIfEmpty()
{
    if (m_pendingRequests.isEmpty() && m_monitorTimer->isActive()) {
        qDebug() << "WindowManager: Stopping window monitoring (no pending requests)";
        m_monitorTimer->stop();
    }
}

bool WindowManager::executePositionScript(const QString &windowId, const QRect &geometry, bool borderless)
{
    constexpr int dbusTimeoutMs = 5000;
    auto fail = [&](const QString &message) {
        qWarning() << "WindowManager: Failed to position window" << windowId << ":" << message;
        Q_EMIT positioningFailed(windowId, message);
        return false;
    };

    QString borderlessValue = borderless ? QStringLiteral("true") : QStringLiteral("false");
    QString scriptContent = QStringLiteral(R"(
(function() {
    var targetUuid = "%1";
    var targetX = %2;
    var targetY = %3;
    var targetW = %4;
    var targetH = %5;
    var borderless = %6;

    var windows = workspace.windowList();
    for (var i = 0; i < windows.length; i++) {
        var win = windows[i];
        if (win.internalId.toString() === targetUuid) {
            if (win.fullScreen) {
                win.fullScreen = false;
            }
            win.setMaximize(false, false);
            win.noBorder = borderless;
            win.frameGeometry = {x: targetX, y: targetY, width: targetW, height: targetH};
            win.keepAbove = true;
            win.skipTaskbar = true;
            win.skipPager = true;
            break;
        }
    }
})();
)")
                                .arg(windowId)
                                .arg(geometry.x())
                                .arg(geometry.y())
                                .arg(geometry.width())
                                .arg(geometry.height())
                                .arg(borderlessValue);

    const QString scriptDir =
        QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).filePath(QStringLiteral("couchplay"));
    if (!QDir().mkpath(scriptDir)) {
        return fail(QStringLiteral("Failed to create host-visible script directory"));
    }

    QTemporaryFile scriptFile(QDir(scriptDir).filePath(QStringLiteral("couchplay-position-XXXXXX.js")));
    if (!scriptFile.open()) {
        return fail(QStringLiteral("Failed to create positioning script"));
    }
    const QByteArray scriptBytes = scriptContent.toUtf8();
    if (scriptFile.write(scriptBytes) != scriptBytes.size() || !scriptFile.flush()) {
        return fail(QStringLiteral("Failed to write positioning script"));
    }
    scriptFile.close();
    const QString scriptPath = scriptFile.fileName();

    QDBusInterface scripting(QStringLiteral("org.kde.KWin"),
                             QStringLiteral("/Scripting"),
                             QStringLiteral("org.kde.kwin.Scripting"),
                             QDBusConnection::sessionBus());
    scripting.setTimeout(dbusTimeoutMs);
    if (!scripting.isValid()) {
        return fail(QStringLiteral("KWin Scripting not available"));
    }

    const QString pluginName =
        QStringLiteral("couchplay-position-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QDBusReply<int> loadReply = scripting.call(QStringLiteral("loadScript"), scriptPath, pluginName);
    if (!loadReply.isValid()) {
        return fail(QStringLiteral("Failed to load script: %1").arg(loadReply.error().message()));
    }

    const int scriptId = loadReply.value();
    if (scriptId < 0) {
        return fail(QStringLiteral("KWin returned an invalid script ID"));
    }
    qDebug() << "WindowManager: Loaded positioning script with ID" << scriptId;

    auto unloadScript = [&]() {
        const QDBusMessage reply = scripting.call(QStringLiteral("unloadScript"), pluginName);
        if (reply.type() != QDBusMessage::ReplyMessage) {
            qWarning() << "WindowManager: Failed to unload positioning script" << pluginName << ":"
                       << reply.errorMessage();
        }
    };

    QDBusInterface script(QStringLiteral("org.kde.KWin"),
                          QStringLiteral("/Scripting/Script%1").arg(scriptId),
                          QStringLiteral("org.kde.kwin.Script"),
                          QDBusConnection::sessionBus());
    script.setTimeout(dbusTimeoutMs);
    if (!script.isValid()) {
        unloadScript();
        return fail(QStringLiteral("KWin did not expose the loaded script object"));
    }

    const QDBusMessage runReply = script.call(QStringLiteral("run"));
    if (runReply.type() != QDBusMessage::ReplyMessage) {
        const QString error = runReply.errorMessage();
        unloadScript();
        return fail(QStringLiteral("Failed to run positioning script: %1").arg(error));
    }

    unloadScript();
    qDebug() << "WindowManager: KWin accepted positioning script for window" << windowId << geometry;
    return true;
}
