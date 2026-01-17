// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QObject>
#include <QList>
#include <QMap>
#include <QPair>
#include <QSet>
#include <QString>
#include <QVariantMap>
#include <QFileSystemWatcher>
#include <QTimer>
#include <qqmlintegration.h>

/**
 * @brief Represents an input device (controller, keyboard, mouse)
 */
struct InputDevice {
    Q_GADGET
    Q_PROPERTY(int eventNumber MEMBER eventNumber)
    Q_PROPERTY(QString name MEMBER name)
    Q_PROPERTY(QString type MEMBER type)
    Q_PROPERTY(QString path MEMBER path)
    Q_PROPERTY(QString vendorId MEMBER vendorId)
    Q_PROPERTY(QString productId MEMBER productId)
    Q_PROPERTY(QString physPath MEMBER physPath)
    Q_PROPERTY(bool assigned MEMBER assigned)
    Q_PROPERTY(int assignedInstance MEMBER assignedInstance)
    Q_PROPERTY(bool isVirtual MEMBER isVirtual)
    Q_PROPERTY(bool isInternal MEMBER isInternal)
    Q_PROPERTY(QString stableId MEMBER stableId)

public:
    int eventNumber = -1;
    QString name;
    QString type; // "controller", "keyboard", "mouse", "other"
    QString path; // /dev/input/eventN
    QString vendorId;
    QString productId;
    QString physPath; // Physical device path (for grouping)
    QString stableId; // Stable identifier: "vendorId:productId:physPath" - survives hotplug/reboot
    bool assigned = false;
    int assignedInstance = -1;
    bool isVirtual = false;  // Virtual/software device
    bool isInternal = false; // Internal device (power buttons, etc.)
};

Q_DECLARE_METATYPE(InputDevice)

/**
 * @brief Manages input device detection and assignment
 * 
 * Reads from /proc/bus/input/devices to detect available input devices
 * and manages their assignment to gamescope instances. Monitors for
 * hotplug events to automatically detect device changes.
 */
class DeviceManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    
    Q_PROPERTY(QVariantList devices READ devicesAsVariant NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList controllers READ controllersAsVariant NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList keyboards READ keyboardsAsVariant NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList mice READ miceAsVariant NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList visibleDevices READ visibleDevicesAsVariant NOTIFY devicesChanged)
    Q_PROPERTY(bool showVirtualDevices READ showVirtualDevices WRITE setShowVirtualDevices NOTIFY showVirtualDevicesChanged)
    Q_PROPERTY(bool showInternalDevices READ showInternalDevices WRITE setShowInternalDevices NOTIFY showInternalDevicesChanged)
    Q_PROPERTY(bool hotplugEnabled READ hotplugEnabled WRITE setHotplugEnabled NOTIFY hotplugEnabledChanged)
    Q_PROPERTY(int instanceCount READ instanceCount WRITE setInstanceCount NOTIFY instanceCountChanged)
    Q_PROPERTY(QVariantList pendingDevices READ pendingDevicesAsVariant NOTIFY pendingDevicesChanged)

public:
    explicit DeviceManager(QObject *parent = nullptr);
    ~DeviceManager() override;

    /**
     * @brief Refresh the list of input devices
     */
    Q_INVOKABLE void refresh();

    /**
     * @brief Assign a device to a specific instance
     * @param eventNumber The event number of the device
     * @param instanceIndex The instance to assign to (-1 to unassign)
     * @return true if successful
     */
    Q_INVOKABLE bool assignDevice(int eventNumber, int instanceIndex);

    /**
     * @brief Unassign all devices
     */
    Q_INVOKABLE void unassignAll();

    /**
     * @brief Get devices assigned to a specific instance
     * @param instanceIndex The instance index
     * @return List of device event numbers
     */
    Q_INVOKABLE QList<int> getDevicesForInstance(int instanceIndex) const;

    /**
     * @brief Get device paths for gamescope --input-device argument
     * @param instanceIndex The instance index
     * @return List of device paths
     */
    Q_INVOKABLE QStringList getDevicePathsForInstance(int instanceIndex) const;

    /**
     * @brief Auto-assign controllers to instances (one per instance)
     * @return Number of controllers assigned
     */
    Q_INVOKABLE int autoAssignControllers();

    /**
     * @brief Test/identify a device (trigger rumble or LED if supported)
     * @param eventNumber The event number of the device
     */
    Q_INVOKABLE void identifyDevice(int eventNumber);

    /**
     * @brief Get a device by event number
     * @param eventNumber The event number
     * @return Device info as QVariantMap, or empty if not found
     */
    Q_INVOKABLE QVariantMap getDevice(int eventNumber) const;

    /**
     * @brief Generate a stable device identifier from hardware properties
     * @param vendorId Vendor ID (e.g., "045e")
     * @param productId Product ID (e.g., "028e")
     * @param physPath Physical path (e.g., "usb-0000:00:14.0-2.4/input0")
     * @return Stable ID in format "vendorId:productId:physPath"
     */
    static QString generateStableId(const QString &vendorId, const QString &productId, const QString &physPath);

    /**
     * @brief Find a device by its stable ID
     * @param stableId The stable ID to search for
     * @return Event number of the device, or -1 if not found
     */
    Q_INVOKABLE int findDeviceByStableId(const QString &stableId) const;

    /**
     * @brief Assign a device to an instance using its stable ID
     * @param stableId The stable ID of the device
     * @param instanceIndex The instance to assign to (-1 to unassign)
     * @return true if successful
     */
    Q_INVOKABLE bool assignDeviceByStableId(const QString &stableId, int instanceIndex);

    /**
     * @brief Get stable IDs for devices assigned to an instance
     * @param instanceIndex The instance index
     * @return List of stable IDs
     */
    Q_INVOKABLE QStringList getStableIdsForInstance(int instanceIndex) const;

    /**
     * @brief Get friendly names for devices assigned to an instance
     * @param instanceIndex The instance index
     * @return List of device names (parallel to getStableIdsForInstance)
     */
    Q_INVOKABLE QStringList getDeviceNamesForInstance(int instanceIndex) const;

    /**
     * @brief Restore device assignments from a list of stable IDs
     * 
     * Devices that cannot be found are added to the pending devices list
     * and will be auto-assigned when they reconnect.
     * 
     * @param instanceIndex The instance to assign devices to
     * @param stableIds List of stable IDs to assign
     * @param names List of friendly names (parallel to stableIds)
     */
    Q_INVOKABLE void restoreAssignmentsFromStableIds(int instanceIndex, const QStringList &stableIds, const QStringList &names);

    /**
     * @brief Clear pending devices for a specific instance
     * @param instanceIndex The instance index (-1 to clear all)
     */
    Q_INVOKABLE void clearPendingDevicesForInstance(int instanceIndex);

    /**
     * @brief Get pending devices as QVariantList for QML
     * @return List of pending device info maps
     */
    QVariantList pendingDevicesAsVariant() const;

    // Property getters
    QList<InputDevice> devices() const { return m_devices; }
    QVariantList devicesAsVariant() const;
    QVariantList controllersAsVariant() const;
    QVariantList keyboardsAsVariant() const;
    QVariantList miceAsVariant() const;
    QVariantList visibleDevicesAsVariant() const;
    
    bool showVirtualDevices() const { return m_showVirtualDevices; }
    void setShowVirtualDevices(bool show);
    
    bool showInternalDevices() const { return m_showInternalDevices; }
    void setShowInternalDevices(bool show);
    
    bool hotplugEnabled() const { return m_hotplugEnabled; }
    void setHotplugEnabled(bool enabled);
    
    int instanceCount() const { return m_instanceCount; }
    void setInstanceCount(int count);

Q_SIGNALS:
    void devicesChanged();
    /**
     * @brief Emitted when a device is assigned or unassigned
     * @param eventNumber The event number of the device
     * @param instanceIndex The new instance index (-1 if unassigned)
     * @param previousInstanceIndex The previous instance index (-1 if wasn't assigned)
     */
    void deviceAssigned(int eventNumber, int instanceIndex, int previousInstanceIndex);
    void deviceAdded(int eventNumber, const QString &name);
    void deviceRemoved(int eventNumber, const QString &name);
    /**
     * @brief Emitted when a previously assigned device reconnects
     * @param stableId The stable ID of the device
     * @param eventNumber The new event number (may differ from before disconnect)
     * @param instanceIndex The instance the device was assigned to
     */
    void deviceReconnected(const QString &stableId, int eventNumber, int instanceIndex);
    /**
     * @brief Emitted when a pending (missing) device is auto-restored on reconnect
     * @param name The friendly name of the device
     * @param instanceIndex The instance the device was restored to
     */
    void deviceAutoRestored(const QString &name, int instanceIndex);
    /**
     * @brief Emitted when the pending devices list changes
     */
    void pendingDevicesChanged();
    void errorOccurred(const QString &message);
    void showVirtualDevicesChanged();
    void showInternalDevicesChanged();
    void hotplugEnabledChanged();
    void instanceCountChanged();

private Q_SLOTS:
    void onInputDirectoryChanged();
    void onDebounceTimeout();

private:
    void parseDevices();
    QString detectDeviceType(const QString &name, const QString &handlers) const;
    bool isVirtualDevice(const QString &name, const QString &physPath) const;
    bool isInternalDevice(const QString &name) const;
    QVariantMap deviceToVariantMap(const InputDevice &device) const;
    void setupHotplugWatcher();
    void checkPendingDevices();

    QList<InputDevice> m_devices;
    QFileSystemWatcher *m_watcher = nullptr;
    QTimer *m_debounceTimer = nullptr;
    
    // Persistent assignment cache: survives across hotplug cycles
    // Maps stableId -> {instanceIndex, deviceName}
    QMap<QString, QPair<int, QString>> m_assignmentCache;
    
    // Pending devices: devices that were expected from profile but not connected
    // Format: {stableId, name, instanceIndex}
    QList<QVariantMap> m_pendingDevices;
    
    bool m_showVirtualDevices = false;
    bool m_showInternalDevices = false;
    bool m_hotplugEnabled = true;
    int m_instanceCount = 2;
};
