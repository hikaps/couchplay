// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

#pragma once

#include <QObject>
#include <QList>
#include <QString>
#include <QVariantMap>
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
    Q_PROPERTY(bool assigned MEMBER assigned)
    Q_PROPERTY(int assignedInstance MEMBER assignedInstance)

public:
    int eventNumber = -1;
    QString name;
    QString type; // "controller", "keyboard", "mouse", "other"
    QString path; // /dev/input/eventN
    bool assigned = false;
    int assignedInstance = -1;
};

Q_DECLARE_METATYPE(InputDevice)

/**
 * @brief Manages input device detection and assignment
 * 
 * Reads from /proc/bus/input/devices to detect available input devices
 * and manages their assignment to gamescope instances.
 */
class DeviceManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantList devices READ devicesAsVariant NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList controllers READ controllersAsVariant NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList keyboards READ keyboardsAsVariant NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList mice READ miceAsVariant NOTIFY devicesChanged)

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

    // Property getters
    QList<InputDevice> devices() const { return m_devices; }
    QVariantList devicesAsVariant() const;
    QVariantList controllersAsVariant() const;
    QVariantList keyboardsAsVariant() const;
    QVariantList miceAsVariant() const;

Q_SIGNALS:
    void devicesChanged();
    void deviceAssigned(int eventNumber, int instanceIndex);
    void errorOccurred(const QString &message);

private:
    void parseDevices();
    QString detectDeviceType(const QString &name, const QString &handlers) const;

    QList<InputDevice> m_devices;
};
