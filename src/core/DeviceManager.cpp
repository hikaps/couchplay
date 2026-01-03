// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

#include "DeviceManager.h"

#include <QFile>
#include <QTextStream>
#include <QRegularExpression>
#include <QDebug>

DeviceManager::DeviceManager(QObject *parent)
    : QObject(parent)
{
    refresh();
}

DeviceManager::~DeviceManager() = default;

void DeviceManager::refresh()
{
    m_devices.clear();
    parseDevices();
    Q_EMIT devicesChanged();
}

void DeviceManager::parseDevices()
{
    QFile file(QStringLiteral("/proc/bus/input/devices"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to open /proc/bus/input/devices"));
        return;
    }

    QTextStream stream(&file);
    QString currentName;
    QString currentHandlers;
    int currentEventNumber = -1;

    static const QRegularExpression nameRegex(QStringLiteral("^N: Name=\"(.*)\"$"));
    static const QRegularExpression handlersRegex(QStringLiteral("^H: Handlers=(.*)$"));
    static const QRegularExpression eventRegex(QStringLiteral("event(\\d+)"));

    while (!stream.atEnd()) {
        QString line = stream.readLine();

        if (line.isEmpty()) {
            // End of device block - create device if we have valid data
            if (!currentName.isEmpty() && currentEventNumber >= 0) {
                InputDevice device;
                device.eventNumber = currentEventNumber;
                device.name = currentName;
                device.path = QStringLiteral("/dev/input/event%1").arg(currentEventNumber);
                device.type = detectDeviceType(currentName, currentHandlers);
                device.assigned = false;
                device.assignedInstance = -1;

                m_devices.append(device);
            }

            // Reset for next device
            currentName.clear();
            currentHandlers.clear();
            currentEventNumber = -1;
            continue;
        }

        // Parse name
        QRegularExpressionMatch nameMatch = nameRegex.match(line);
        if (nameMatch.hasMatch()) {
            currentName = nameMatch.captured(1);
            continue;
        }

        // Parse handlers
        QRegularExpressionMatch handlersMatch = handlersRegex.match(line);
        if (handlersMatch.hasMatch()) {
            currentHandlers = handlersMatch.captured(1);

            // Extract event number
            QRegularExpressionMatch eventMatch = eventRegex.match(currentHandlers);
            if (eventMatch.hasMatch()) {
                currentEventNumber = eventMatch.captured(1).toInt();
            }
        }
    }

    // Handle last device if file doesn't end with empty line
    if (!currentName.isEmpty() && currentEventNumber >= 0) {
        InputDevice device;
        device.eventNumber = currentEventNumber;
        device.name = currentName;
        device.path = QStringLiteral("/dev/input/event%1").arg(currentEventNumber);
        device.type = detectDeviceType(currentName, currentHandlers);
        device.assigned = false;
        device.assignedInstance = -1;

        m_devices.append(device);
    }

    file.close();
}

QString DeviceManager::detectDeviceType(const QString &name, const QString &handlers) const
{
    QString lowerName = name.toLower();
    QString lowerHandlers = handlers.toLower();

    // Check for controllers/gamepads
    if (lowerName.contains(QStringLiteral("xbox")) ||
        lowerName.contains(QStringLiteral("controller")) ||
        lowerName.contains(QStringLiteral("gamepad")) ||
        lowerName.contains(QStringLiteral("joystick")) ||
        lowerName.contains(QStringLiteral("dualshock")) ||
        lowerName.contains(QStringLiteral("dualsense")) ||
        lowerName.contains(QStringLiteral("sony")) ||
        lowerName.contains(QStringLiteral("nintendo")) ||
        lowerName.contains(QStringLiteral("pro controller")) ||
        lowerHandlers.contains(QStringLiteral("js"))) {
        return QStringLiteral("controller");
    }

    // Check for keyboards
    if (lowerName.contains(QStringLiteral("keyboard")) ||
        (lowerHandlers.contains(QStringLiteral("kbd")) && !lowerHandlers.contains(QStringLiteral("mouse")))) {
        return QStringLiteral("keyboard");
    }

    // Check for mice
    if (lowerName.contains(QStringLiteral("mouse")) ||
        lowerName.contains(QStringLiteral("touchpad")) ||
        lowerName.contains(QStringLiteral("trackpad")) ||
        lowerHandlers.contains(QStringLiteral("mouse"))) {
        return QStringLiteral("mouse");
    }

    return QStringLiteral("other");
}

bool DeviceManager::assignDevice(int eventNumber, int instanceIndex)
{
    for (int i = 0; i < m_devices.size(); ++i) {
        if (m_devices[i].eventNumber == eventNumber) {
            m_devices[i].assigned = (instanceIndex >= 0);
            m_devices[i].assignedInstance = instanceIndex;
            Q_EMIT devicesChanged();
            Q_EMIT deviceAssigned(eventNumber, instanceIndex);
            return true;
        }
    }

    Q_EMIT errorOccurred(QStringLiteral("Device event%1 not found").arg(eventNumber));
    return false;
}

void DeviceManager::unassignAll()
{
    for (int i = 0; i < m_devices.size(); ++i) {
        m_devices[i].assigned = false;
        m_devices[i].assignedInstance = -1;
    }
    Q_EMIT devicesChanged();
}

QList<int> DeviceManager::getDevicesForInstance(int instanceIndex) const
{
    QList<int> result;
    for (const auto &device : m_devices) {
        if (device.assignedInstance == instanceIndex) {
            result.append(device.eventNumber);
        }
    }
    return result;
}

QVariantList DeviceManager::devicesAsVariant() const
{
    QVariantList list;
    for (const auto &device : m_devices) {
        QVariantMap map;
        map[QStringLiteral("eventNumber")] = device.eventNumber;
        map[QStringLiteral("name")] = device.name;
        map[QStringLiteral("type")] = device.type;
        map[QStringLiteral("path")] = device.path;
        map[QStringLiteral("assigned")] = device.assigned;
        map[QStringLiteral("assignedInstance")] = device.assignedInstance;
        list.append(map);
    }
    return list;
}

QVariantList DeviceManager::controllersAsVariant() const
{
    QVariantList list;
    for (const auto &device : m_devices) {
        if (device.type == QStringLiteral("controller")) {
            QVariantMap map;
            map[QStringLiteral("eventNumber")] = device.eventNumber;
            map[QStringLiteral("name")] = device.name;
            map[QStringLiteral("type")] = device.type;
            map[QStringLiteral("path")] = device.path;
            map[QStringLiteral("assigned")] = device.assigned;
            map[QStringLiteral("assignedInstance")] = device.assignedInstance;
            list.append(map);
        }
    }
    return list;
}

QVariantList DeviceManager::keyboardsAsVariant() const
{
    QVariantList list;
    for (const auto &device : m_devices) {
        if (device.type == QStringLiteral("keyboard")) {
            QVariantMap map;
            map[QStringLiteral("eventNumber")] = device.eventNumber;
            map[QStringLiteral("name")] = device.name;
            map[QStringLiteral("type")] = device.type;
            map[QStringLiteral("path")] = device.path;
            map[QStringLiteral("assigned")] = device.assigned;
            map[QStringLiteral("assignedInstance")] = device.assignedInstance;
            list.append(map);
        }
    }
    return list;
}

QVariantList DeviceManager::miceAsVariant() const
{
    QVariantList list;
    for (const auto &device : m_devices) {
        if (device.type == QStringLiteral("mouse")) {
            QVariantMap map;
            map[QStringLiteral("eventNumber")] = device.eventNumber;
            map[QStringLiteral("name")] = device.name;
            map[QStringLiteral("type")] = device.type;
            map[QStringLiteral("path")] = device.path;
            map[QStringLiteral("assigned")] = device.assigned;
            map[QStringLiteral("assignedInstance")] = device.assignedInstance;
            list.append(map);
        }
    }
    return list;
}
