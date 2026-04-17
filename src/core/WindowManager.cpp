// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "WindowManager.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QThread>

WindowManager::WindowManager(QObject *parent)
    : QObject(parent)
{
    QDBusInterface kwin(
        QStringLiteral("org.kde.KWin"),
        QStringLiteral("/KWin"),
        QStringLiteral("org.kde.KWin"),
        QDBusConnection::sessionBus()
    );
    m_kwinAvailable = kwin.isValid();
    
    if (!m_kwinAvailable) {
        qWarning() << "WindowManager: KWin D-Bus interface not available";
    } else {
        qDebug() << "WindowManager: KWin D-Bus interface available";
    }
    
    m_monitorTimer = new QTimer(this);
    m_monitorTimer->setInterval(MONITOR_INTERVAL_MS);
    connect(m_monitorTimer, &QTimer::timeout, this, &WindowManager::checkForNewWindows);

    // Clean up stale KWin scripts from previous runs
    QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    QString couchplayRuntimeDir = runtimeDir + QStringLiteral("/couchplay");
    QDir staleDir(couchplayRuntimeDir);
    if (staleDir.exists()) {
        QStringList staleScripts = staleDir.entryList(
            QStringList{QStringLiteral("couchplay-position-*.js")},
            QDir::Files);
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
        if (!excludeWindowIds.contains(uuid)) {
            return uuid;
        }
    }
    
    return QString();
}

QStringList WindowManager::findAllGamescopeWindows()
{
    QStringList results;
    
    if (!m_kwinAvailable) {
        return results;
    }

    QDBusInterface windowsRunner(
        QStringLiteral("org.kde.KWin"),
        QStringLiteral("/WindowsRunner"),
        QStringLiteral("org.kde.krunner1"),
        QDBusConnection::sessionBus()
    );
    
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
            
            if (desktopFile == QStringLiteral("gamescope") || 
                resourceClass == QStringLiteral("gamescope")) {
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

    QDBusInterface kwin(
        QStringLiteral("org.kde.KWin"),
        QStringLiteral("/KWin"),
        QStringLiteral("org.kde.KWin"),
        QDBusConnection::sessionBus()
    );
    
    if (!kwin.isValid()) {
        qWarning() << "WindowManager: KWin interface not available";
        return info;
    }
    
    QDBusReply<QVariantMap> reply = kwin.call(QStringLiteral("getWindowInfo"), windowId);
    
    if (!reply.isValid()) {
        qWarning() << "WindowManager: Failed to get window info for" << windowId 
                   << ":" << reply.error().message();
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

bool WindowManager::positionWindow(const QString &windowId, const QRect &geometry)
{
    if (!m_kwinAvailable || windowId.isEmpty()) {
        Q_EMIT positioningFailed(windowId, QStringLiteral("KWin not available or invalid window ID"));
        return false;
    }

    qDebug() << "WindowManager: Positioning window" << windowId << "to" << geometry;

    return executePositionScript(windowId, geometry);
}

void WindowManager::queuePositionRequest(int requestId, const QRect &geometry,
                                          const QStringList &excludeWindowIds,
                                          int timeoutMs)
{
    if (!m_kwinAvailable) {
        qWarning() << "WindowManager: Cannot queue position request - KWin not available";
        Q_EMIT positioningTimedOut(requestId);
        return;
    }
    
    for (int i = 0; i < m_pendingRequests.size(); ++i) {
        if (m_pendingRequests[i].requestId == requestId) {
            qWarning() << "WindowManager: Replacing existing request" << requestId;
            m_pendingRequests.removeAt(i);
            break;
        }
    }
    
    PositionRequest request;
    request.requestId = requestId;
    request.geometry = geometry;
    request.excludeWindowIds = excludeWindowIds;
    request.expiresAt = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
    
    m_pendingRequests.append(request);
    
    qDebug() << "WindowManager: Queued position request" << requestId 
             << "geometry:" << geometry
             << "excluding" << excludeWindowIds.size() << "windows"
             << "timeout:" << timeoutMs << "ms";
    
    startMonitoring();
    checkForNewWindows();
}

void WindowManager::cancelPositionRequest(int requestId)
{
    for (int i = 0; i < m_pendingRequests.size(); ++i) {
        if (m_pendingRequests[i].requestId == requestId) {
            m_pendingRequests.removeAt(i);
            qDebug() << "WindowManager: Cancelled position request" << requestId;
            break;
        }
    }
    
    stopMonitoringIfEmpty();
}

void WindowManager::cancelAllRequests()
{
    int count = m_pendingRequests.size();
    m_pendingRequests.clear();
    m_knownWindowIds.clear();
    stopMonitoringIfEmpty();
    
    qDebug() << "WindowManager: Cancelled all" << count << "pending requests";
}

bool WindowManager::hasPendingRequests() const
{
    return !m_pendingRequests.isEmpty();
}

void WindowManager::checkForNewWindows()
{
    if (m_pendingRequests.isEmpty()) {
        stopMonitoringIfEmpty();
        return;
    }
    
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    
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
    
    QStringList currentWindows = findAllGamescopeWindows();
    
    // FIFO order — elements removed during iteration
    int i = 0;
    while (i < m_pendingRequests.size() && !currentWindows.isEmpty()) {
        const PositionRequest &request = m_pendingRequests[i];
        
        QString matchedWindowId;
        for (const QString &windowId : currentWindows) {
            if (!request.excludeWindowIds.contains(windowId) && 
                !m_knownWindowIds.contains(windowId)) {
                matchedWindowId = windowId;
                break;
            }
        }
        
        if (!matchedWindowId.isEmpty()) {
            qDebug() << "WindowManager: Matched window" << matchedWindowId 
                     << "to request" << request.requestId;
            
            bool success = positionWindow(matchedWindowId, request.geometry);
            
            m_knownWindowIds.append(matchedWindowId);
            
            currentWindows.removeAll(matchedWindowId);
            
            int requestId = request.requestId;
            m_pendingRequests.removeAt(i);
            
            if (success) {
                Q_EMIT gamescopeWindowPositioned(requestId, matchedWindowId);
            } else {
                Q_EMIT positioningTimedOut(requestId);
            }
            // Don't increment i since we removed the current element
        } else {
            ++i;
        }
    }
    
    stopMonitoringIfEmpty();
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

bool WindowManager::executePositionScript(const QString &windowId, const QRect &geometry)
{
    QString scriptContent = QStringLiteral(R"(
(function() {
    var targetUuid = "%1";
    var targetX = %2;
    var targetY = %3;
    var targetW = %4;
    var targetH = %5;
    
    var windows = workspace.windowList();
    for (var i = 0; i < windows.length; i++) {
        var win = windows[i];
        if (win.internalId.toString() === targetUuid) {
            win.frameGeometry = {x: targetX, y: targetY, width: targetW, height: targetH};
            win.noBorder = true;
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
        .arg(geometry.height());
    
    // Use XDG_RUNTIME_DIR — auto-cleaned on session end, not shared across users
    QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    QString couchplayRuntimeDir = runtimeDir + QStringLiteral("/couchplay");
    QDir().mkpath(couchplayRuntimeDir);
    QString scriptPath = couchplayRuntimeDir + QStringLiteral("/couchplay-position-%1.js")
        .arg(QString::number(reinterpret_cast<quintptr>(this), 16));
    
    QFile scriptFile(scriptPath);
    if (!scriptFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "WindowManager: Failed to create temporary script file:" << scriptPath;
        Q_EMIT positioningFailed(windowId, QStringLiteral("Failed to create script file"));
        return false;
    }
    
    scriptFile.write(scriptContent.toUtf8());
    scriptFile.close();
    
    QDBusInterface scripting(
        QStringLiteral("org.kde.KWin"),
        QStringLiteral("/Scripting"),
        QStringLiteral("org.kde.kwin.Scripting"),
        QDBusConnection::sessionBus()
    );
    
    if (!scripting.isValid()) {
        qWarning() << "WindowManager: KWin Scripting interface not available";
        QFile::remove(scriptPath);
        Q_EMIT positioningFailed(windowId, QStringLiteral("KWin Scripting not available"));
        return false;
    }
    
    QString pluginName = QStringLiteral("couchplay-position-%1").arg(
        QString::number(QDateTime::currentMSecsSinceEpoch()));
    
    QDBusReply<int> loadReply = scripting.call(QStringLiteral("loadScript"), scriptPath, pluginName);
    
    if (!loadReply.isValid()) {
        qWarning() << "WindowManager: Failed to load positioning script:" << loadReply.error().message();
        QFile::remove(scriptPath);
        Q_EMIT positioningFailed(windowId, QStringLiteral("Failed to load script: %1").arg(loadReply.error().message()));
        return false;
    }
    
    int scriptId = loadReply.value();
    qDebug() << "WindowManager: Loaded positioning script with ID" << scriptId;
    
    scripting.call(QStringLiteral("start"));
    
    // Give the script a moment to execute - use QTimer for non-blocking delay would be better
    // but for simplicity we use a short blocking wait here
    QThread::msleep(100);
    
    scripting.call(QStringLiteral("unloadScript"), pluginName);
    
    QFile::remove(scriptPath);
    
    Q_EMIT windowPositioned(windowId, geometry);
    qDebug() << "WindowManager: Successfully positioned window" << windowId;
    return true;
}
