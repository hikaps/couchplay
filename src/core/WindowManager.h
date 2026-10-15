// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QMap>
#include <QObject>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QtQml/qqmlregistration.h>
#include <QVariantMap>

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
     * @return true if KWin accepted the positioning script; final geometry confirmation is asynchronous and
     *         reported by windowPositioned or positioningFailed
     */
    Q_INVOKABLE bool positionWindow(const QString &windowId, const QRect &geometry, bool borderless = false);

    /**
     * @brief Queue a positioning request that will be fulfilled when a gamescope window appears
     * @param requestId Unique identifier for this request (e.g., instance index)
     * @param gamescopePid Host PID returned by the helper, used to identify the instance window
     * @param geometry The target geometry for the window
     * @param excludeWindowIds Window IDs to exclude (already positioned windows)
     * @param timeoutMs Maximum time to find and confirm the window (default 60 seconds)
     *
     * The matched window remains reserved while KWin applies the geometry.
     * gamescopeWindowPositioned is emitted only after KWin reports the requested geometry.
     * If the timeout expires, positioningTimedOut is emitted.
     * A PID-scoped KWin listener keeps replacement windows in this slot until cancellation.
     */
    Q_INVOKABLE void queuePositionRequest(int requestId,
                                          qint64 gamescopePid,
                                          const QRect &geometry,
                                          const QStringList &excludeWindowIds,
                                          bool borderless,
                                          int timeoutMs = 60000);

    /**
     * @brief Cancel a positioning request and its replacement-window listener
     * @param requestId The request ID to cancel
     */
    Q_INVOKABLE void cancelPositionRequest(int requestId);

    /**
     * @brief Cancel all positioning requests and replacement-window listeners
     */
    Q_INVOKABLE void cancelAllRequests();

    /**
     * @brief Get gamescope windows by host PID, or by application identity when no PID is supplied
     * @param gamescopePid Host Gamescope PID (0 uses the gamescope application identity)
     * @return List of window UUIDs
     */
    Q_INVOKABLE QStringList findAllGamescopeWindows(qint64 gamescopePid = 0);

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
     * @brief Emitted after KWin confirms the requested window geometry
     */
    void windowPositioned(const QString &windowId, const QRect &geometry);

    /**
     * @brief Emitted after a queued gamescope window's geometry is confirmed
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
    void checkPositionConfirmations();

private:
    struct PositionRequest {
        int requestId;
        QRect geometry;
        QStringList excludeWindowIds;
        bool borderless;
        qint64 expiresAt;
        qint64 gamescopePid;
    };

    struct PendingPosition {
        int requestId;
        QString windowId;
        QRect geometry;
        bool queued;
        qint64 expiresAt;
        qint64 gamescopePid = 0;
    };

    /**
     * @brief Execute a KWin script to position a specific window
     */
    bool executePositionScript(const QString &windowId, const QRect &geometry, bool borderless,
                               const PositionRequest *request = nullptr);
    void unloadPositionScript(const QString &pluginName);
    void stopPositionConfirmationIfEmpty();
    void releaseWindowReservation(const QString &windowId);

    /**
     * @brief Start the monitoring timer if not already running
     */
    void startMonitoring();

    /**
     * @brief Stop the monitoring timer if no pending requests
     */
    void stopMonitoringIfEmpty();

    QTimer *m_monitorTimer = nullptr;
    QTimer *m_positionTimer = nullptr;
    QList<PositionRequest> m_pendingRequests;
    QList<PendingPosition> m_pendingPositions;
    QMap<int, QString> m_positionScripts; // PID-scoped listeners live until request/session cancellation.
    QStringList m_knownWindowIds; // Windows confirmed at their requested geometry
    QStringList m_reservedWindowIds; // Windows assigned to an in-flight positioning request
    bool m_kwinAvailable = false;
    quint64 m_cancelAllGeneration = 0;
    int m_emittingRequestId = 0;
    bool m_emittingQueuedRequest = false;
    bool m_emittingRequestCancelled = false;

    static constexpr int MONITOR_INTERVAL_MS = 2000; // Poll every 2 seconds
    static constexpr int POSITION_CONFIRM_INTERVAL_MS = 100;
    static constexpr int POSITION_CONFIRM_TIMEOUT_MS = 60000;
};
