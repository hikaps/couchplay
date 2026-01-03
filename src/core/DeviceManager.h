// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QObject>
#include <QList>
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

public:
    int eventNumber = -1;
    QString name;
    QString type; // "controller", "keyboard", "mouse", "other"
    QString path; // /dev/input/eventN
    QString vendorId;
    QString productId;
    QString physPath; // Physical device path (for grouping)
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
    void deviceAssigned(int eventNumber, int instanceIndex);
    void deviceAdded(int eventNumber, const QString &name);
    void deviceRemoved(int eventNumber, const QString &name);
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

    QList<InputDevice> m_devices;
    QFileSystemWatcher *m_watcher = nullptr;
    QTimer *m_debounceTimer = nullptr;
    
    bool m_showVirtualDevices = false;
    bool m_showInternalDevices = false;
    bool m_hotplugEnabled = true;
    int m_instanceCount = 2;
};
