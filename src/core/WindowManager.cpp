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
    // Check if KWin is available
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
    
    // Create the monitoring timer (but don't start it yet)
    m_monitorTimer = new QTimer(this);
    m_monitorTimer->setInterval(MONITOR_INTERVAL_MS);
    connect(m_monitorTimer, &QTimer::timeout, this, &WindowManager::checkForNewWindows);
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

    // Use WindowsRunner to search for windows
    // The Match method returns windows matching a search query
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
    
    // Parse the reply - it's an array of structs: a(sssuda{sv})
    // Each struct: (id, caption, iconName, relevance, score, properties)
    // The id format is "0_{uuid}" - we need to extract the uuid and check if it's gamescope
    // 
    // The properties dict contains complex types (like icon-data with struct (iiibiiay))
    // that can cause issues with QDBusArgument parsing. We only need the matchId,
    // so we extract just what we need and skip the rest.
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
        
        // Read the first 5 fields we care about
        arg >> matchId >> caption >> iconName >> relevance >> score;
        
        // Skip the properties dict - it contains complex types that are hard to parse
        // We use QVariant to consume it without fully deserializing
        QVariant propertiesVariant;
        arg >> propertiesVariant;
        
        arg.endStructure();
        
        // matchId format is "0_{uuid}" - extract the uuid
        if (matchId.contains(QLatin1Char('{'))) {
            QString uuid = matchId.mid(matchId.indexOf(QLatin1Char('{')));
            
            // Get window info to check if it's a gamescope window
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

    // Use KWin scripting to position the window
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
    
    // Check if a request with this ID already exists
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
    
    // Start monitoring if not already running
    startMonitoring();
    
    // Do an immediate check in case the window is already there
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
    
    // Check for expired requests first
    QList<int> expiredRequestIds;
    for (int i = m_pendingRequests.size() - 1; i >= 0; --i) {
        if (m_pendingRequests[i].expiresAt <= now) {
            expiredRequestIds.append(m_pendingRequests[i].requestId);
            m_pendingRequests.removeAt(i);
        }
    }
    
    // Emit timeout signals for expired requests
    for (int requestId : expiredRequestIds) {
        qWarning() << "WindowManager: Position request" << requestId << "timed out";
        Q_EMIT positioningTimedOut(requestId);
    }
    
    if (m_pendingRequests.isEmpty()) {
        stopMonitoringIfEmpty();
        return;
    }
    
    // Find all current gamescope windows
    QStringList currentWindows = findAllGamescopeWindows();
    
    // Process pending requests in order (FIFO)
    // We iterate carefully since we may remove elements
    int i = 0;
    while (i < m_pendingRequests.size() && !currentWindows.isEmpty()) {
        const PositionRequest &request = m_pendingRequests[i];
        
        // Find a window that matches this request (not in exclude list and not already known)
        QString matchedWindowId;
        for (const QString &windowId : currentWindows) {
            if (!request.excludeWindowIds.contains(windowId) && 
                !m_knownWindowIds.contains(windowId)) {
                matchedWindowId = windowId;
                break;
            }
        }
        
        if (!matchedWindowId.isEmpty()) {
            // Found a match - position the window
            qDebug() << "WindowManager: Matched window" << matchedWindowId 
                     << "to request" << request.requestId;
            
            bool success = positionWindow(matchedWindowId, request.geometry);
            
            // Track this window as known (positioned)
            m_knownWindowIds.append(matchedWindowId);
            
            // Remove from available windows for subsequent requests
            currentWindows.removeAll(matchedWindowId);
            
            // Remove this request and emit signal
            int requestId = request.requestId;
            m_pendingRequests.removeAt(i);
            
            if (success) {
                Q_EMIT gamescopeWindowPositioned(requestId, matchedWindowId);
            } else {
                Q_EMIT positioningTimedOut(requestId);
            }
            
            // Don't increment i since we removed the current element
        } else {
            // No match for this request, try next
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
    // Create a temporary KWin script to position the window
    // KWin scripts use JavaScript and can access window properties
    
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
            // Set the frame geometry using a plain JavaScript object
            win.frameGeometry = {x: targetX, y: targetY, width: targetW, height: targetH};
            // Remove decorations for cleaner positioning
            win.noBorder = true;
            // Hide from taskbar/pager but keep in Alt+Tab for emergency access
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
    
    // Write script to a temporary file
    QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QString scriptPath = tempDir + QStringLiteral("/couchplay-position-%1.js")
        .arg(QString::number(reinterpret_cast<quintptr>(this), 16));
    
    QFile scriptFile(scriptPath);
    if (!scriptFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "WindowManager: Failed to create temporary script file:" << scriptPath;
        Q_EMIT positioningFailed(windowId, QStringLiteral("Failed to create script file"));
        return false;
    }
    
    scriptFile.write(scriptContent.toUtf8());
    scriptFile.close();
    
    // Load the script via KWin D-Bus
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
    
    // Generate a unique plugin name for this positioning operation
    QString pluginName = QStringLiteral("couchplay-position-%1").arg(
        QString::number(QDateTime::currentMSecsSinceEpoch()));
    
    // Load the script
    QDBusReply<int> loadReply = scripting.call(QStringLiteral("loadScript"), scriptPath, pluginName);
    
    if (!loadReply.isValid()) {
        qWarning() << "WindowManager: Failed to load positioning script:" << loadReply.error().message();
        QFile::remove(scriptPath);
        Q_EMIT positioningFailed(windowId, QStringLiteral("Failed to load script: %1").arg(loadReply.error().message()));
        return false;
    }
    
    int scriptId = loadReply.value();
    qDebug() << "WindowManager: Loaded positioning script with ID" << scriptId;
    
    // Start the scripts (this actually executes them)
    scripting.call(QStringLiteral("start"));
    
    // Give the script a moment to execute - use QTimer for non-blocking delay would be better
    // but for simplicity we use a short blocking wait here
    QThread::msleep(100);
    
    // Unload the script
    scripting.call(QStringLiteral("unloadScript"), pluginName);
    
    // Clean up the temporary file
    QFile::remove(scriptPath);
    
    Q_EMIT windowPositioned(windowId, geometry);
    qDebug() << "WindowManager: Successfully positioned window" << windowId;
    return true;
}
