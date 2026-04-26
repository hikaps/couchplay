// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QObject>
#include <QSet>
#include <QString>

class QFileSystemWatcher;
class QTimer;

/**
 * @brief Watches for new input devices appearing during a session
 *
 * Used to detect virtual devices created by Steam Input so they can
 * be isolated to the creating user via device ownership transfer.
 */
class VirtualDeviceWatcher : public QObject
{
    Q_OBJECT

public:
    explicit VirtualDeviceWatcher(QObject *parent = nullptr);
    ~VirtualDeviceWatcher() override;

    /**
     * @brief Start watching for new virtual devices
     * @param knownEventNumbers Event numbers already known at session start
     */
    void startWatching(const QSet<int> &knownEventNumbers);

    /**
     * @brief Stop watching and clear state
     */
    void stopWatching();

    /**
     * @brief Check if currently watching
     */
    bool isWatching() const
    {
        return m_watching;
    }

Q_SIGNALS:
    /**
     * @brief Emitted when a new virtual device appears
     * @param eventNumber The event number of the new device
     * @param devicePath Full path (e.g., /dev/input/event15)
     * @param deviceName Device name from /sys/class/input
     */
    void virtualDeviceAppeared(int eventNumber, const QString &devicePath, const QString &deviceName);

private Q_SLOTS:
    void onInputDirectoryChanged();
    void onDebounceTimeout();

private:
    void checkForNewDevices();
    QString getDeviceName(int eventNumber) const;
    bool isVirtualDevice(int eventNumber, const QString &name) const;

    QFileSystemWatcher *m_watcher = nullptr;
    QTimer *m_debounceTimer = nullptr;
    QSet<int> m_knownEventNumbers;
    bool m_watching = false;
};
