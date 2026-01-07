// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QObject>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

/**
 * @brief Manages window positioning via KWin D-Bus interface
 * 
 * This class provides methods to find and position gamescope windows
 * using KDE's KWin window manager. It uses the org.kde.KWin D-Bus
 * interface to query window information and a KWin script to
 * set window geometry.
 * 
 * Supports event-driven positioning through a queue system where
 * positioning requests are queued and fulfilled as windows appear.
 */
class WindowManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit WindowManager(QObject *parent = nullptr);
    ~WindowManager() override;

    /**
     * @brief Find a gamescope window, optionally excluding already-known windows
     * @param excludeWindowIds Window IDs to exclude from search
     * @return Window UUID if found, empty string otherwise
     */
    Q_INVOKABLE QString findGamescopeWindow(const QStringList &excludeWindowIds = {});

    /**
     * @brief Get information about a window
     * @param windowId The window UUID from KWin
     * @return Map containing window properties (x, y, width, height, caption, etc.)
     */
    Q_INVOKABLE QVariantMap getWindowInfo(const QString &windowId);

    /**
     * @brief Position a window at the specified geometry
     * @param windowId The window UUID from KWin
     * @param geometry The target geometry (x, y, width, height)
     * @return true if successful
     */
    Q_INVOKABLE bool positionWindow(const QString &windowId, const QRect &geometry);

    /**
     * @brief Queue a positioning request that will be fulfilled when a gamescope window appears
     * @param requestId Unique identifier for this request (e.g., instance index)
     * @param geometry The target geometry for the window
     * @param excludeWindowIds Window IDs to exclude (already positioned windows)
     * @param timeoutMs Maximum time to wait for the window (default 60 seconds)
     * 
     * When a matching gamescope window appears (not in excludeWindowIds), it will be
     * positioned and the gamescopeWindowPositioned signal will be emitted.
     * If the timeout expires, positioningFailed will be emitted.
     */
    Q_INVOKABLE void queuePositionRequest(int requestId, const QRect &geometry,
                                           const QStringList &excludeWindowIds,
                                           int timeoutMs = 60000);

    /**
     * @brief Cancel a pending positioning request
     * @param requestId The request ID to cancel
     */
    Q_INVOKABLE void cancelPositionRequest(int requestId);

    /**
     * @brief Cancel all pending positioning requests
     */
    Q_INVOKABLE void cancelAllRequests();

    /**
     * @brief Get all gamescope windows currently open
     * @return List of window UUIDs
     */
    Q_INVOKABLE QStringList findAllGamescopeWindows();

    /**
     * @brief Check if KWin is available
     * @return true if KWin D-Bus interface is accessible
     */
    Q_INVOKABLE bool isAvailable() const;

    /**
     * @brief Check if there are pending positioning requests
     */
    Q_INVOKABLE bool hasPendingRequests() const;

Q_SIGNALS:
    /**
     * @brief Emitted when a window is successfully positioned
     */
    void windowPositioned(const QString &windowId, const QRect &geometry);

    /**
     * @brief Emitted when a queued gamescope window is positioned
     * @param requestId The request ID that was fulfilled
     * @param windowId The window that was positioned
     */
    void gamescopeWindowPositioned(int requestId, const QString &windowId);

    /**
     * @brief Emitted when window positioning fails
     */
    void positioningFailed(const QString &windowId, const QString &error);

    /**
     * @brief Emitted when a queued request times out
     */
    void positioningTimedOut(int requestId);

private Q_SLOTS:
    /**
     * @brief Called by timer to check for new gamescope windows
     */
    void checkForNewWindows();

private:
    struct PositionRequest {
        int requestId;
        QRect geometry;
        QStringList excludeWindowIds;
        qint64 expiresAt;  // milliseconds since epoch
    };

    /**
     * @brief Execute a KWin script to position a specific window
     */
    bool executePositionScript(const QString &windowId, const QRect &geometry);

    /**
     * @brief Start the monitoring timer if not already running
     */
    void startMonitoring();

    /**
     * @brief Stop the monitoring timer if no pending requests
     */
    void stopMonitoringIfEmpty();

    QTimer *m_monitorTimer = nullptr;
    QList<PositionRequest> m_pendingRequests;
    QStringList m_knownWindowIds;  // Windows we've already seen/positioned
    bool m_kwinAvailable = false;
    
    static constexpr int MONITOR_INTERVAL_MS = 2000;  // Poll every 2 seconds
};
