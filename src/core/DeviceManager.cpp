// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "DeviceManager.h"

#include <QFile>
#include <QDir>
#include <QTextStream>
#include <QRegularExpression>
#include <QDebug>

// For device identification (rumble)
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <sys/ioctl.h>

DeviceManager::DeviceManager(QObject *parent)
    : QObject(parent)
    , m_debounceTimer(new QTimer(this))
{
    // Debounce timer to avoid refreshing too frequently during hotplug
    m_debounceTimer->setSingleShot(true);
    m_debounceTimer->setInterval(500); // 500ms debounce
    connect(m_debounceTimer, &QTimer::timeout, this, &DeviceManager::onDebounceTimeout);
    
    refresh();
    setupHotplugWatcher();
}

DeviceManager::~DeviceManager() = default;

void DeviceManager::setupHotplugWatcher()
{
    if (m_watcher) {
        delete m_watcher;
        m_watcher = nullptr;
    }
    
    if (!m_hotplugEnabled) {
        return;
    }
    
    m_watcher = new QFileSystemWatcher(this);
    
    // Watch /dev/input for device changes
    if (QDir(QStringLiteral("/dev/input")).exists()) {
        m_watcher->addPath(QStringLiteral("/dev/input"));
    }
    
    // Also watch /proc/bus/input/devices for more reliable detection
    if (QFile::exists(QStringLiteral("/proc/bus/input/devices"))) {
        m_watcher->addPath(QStringLiteral("/proc/bus/input/devices"));
    }
    
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, 
            this, &DeviceManager::onInputDirectoryChanged);
    connect(m_watcher, &QFileSystemWatcher::fileChanged,
            this, &DeviceManager::onInputDirectoryChanged);
    
    qDebug() << "DeviceManager: Hotplug watcher enabled";
}

void DeviceManager::onInputDirectoryChanged()
{
    // Use debounce timer to avoid multiple rapid refreshes
    m_debounceTimer->start();
}

void DeviceManager::onDebounceTimeout()
{
    qDebug() << "DeviceManager: Detected device change, refreshing...";
    
    // Store old device list to detect changes
    QList<int> oldEventNumbers;
    QMap<int, QString> oldDeviceNames;
    for (const auto &device : m_devices) {
        oldEventNumbers.append(device.eventNumber);
        oldDeviceNames[device.eventNumber] = device.name;
    }
    
    // Preserve assignments
    QMap<int, int> assignments;
    for (const auto &device : m_devices) {
        if (device.assigned) {
            assignments[device.eventNumber] = device.assignedInstance;
        }
    }
    
    // Refresh devices
    m_devices.clear();
    parseDevices();
    
    // Restore assignments
    for (auto &device : m_devices) {
        if (assignments.contains(device.eventNumber)) {
            device.assigned = true;
            device.assignedInstance = assignments[device.eventNumber];
        }
    }
    
    // Detect added/removed devices
    QList<int> newEventNumbers;
    for (const auto &device : m_devices) {
        newEventNumbers.append(device.eventNumber);
        
        if (!oldEventNumbers.contains(device.eventNumber)) {
            qDebug() << "DeviceManager: Device added:" << device.name;
            Q_EMIT deviceAdded(device.eventNumber, device.name);
        }
    }
    
    for (int oldEvent : oldEventNumbers) {
        if (!newEventNumbers.contains(oldEvent)) {
            qDebug() << "DeviceManager: Device removed:" << oldDeviceNames[oldEvent];
            Q_EMIT deviceRemoved(oldEvent, oldDeviceNames[oldEvent]);
        }
    }
    
    Q_EMIT devicesChanged();
}

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
    QString currentPhys;
    QString currentVendor;
    QString currentProduct;
    int currentEventNumber = -1;

    static const QRegularExpression nameRegex(QStringLiteral("^N: Name=\"(.*)\"$"));
    static const QRegularExpression handlersRegex(QStringLiteral("^H: Handlers=(.*)$"));
    static const QRegularExpression eventRegex(QStringLiteral("event(\\d+)"));
    static const QRegularExpression physRegex(QStringLiteral("^P: Phys=(.*)$"));
    static const QRegularExpression idRegex(QStringLiteral("^I: Bus=\\w+ Vendor=(\\w+) Product=(\\w+)"));

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
                device.vendorId = currentVendor;
                device.productId = currentProduct;
                device.physPath = currentPhys;
                device.assigned = false;
                device.assignedInstance = -1;
                device.isVirtual = isVirtualDevice(currentName, currentPhys);
                device.isInternal = isInternalDevice(currentName);

                m_devices.append(device);
            }

            // Reset for next device
            currentName.clear();
            currentHandlers.clear();
            currentPhys.clear();
            currentVendor.clear();
            currentProduct.clear();
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
            continue;
        }
        
        // Parse physical path
        QRegularExpressionMatch physMatch = physRegex.match(line);
        if (physMatch.hasMatch()) {
            currentPhys = physMatch.captured(1);
            continue;
        }
        
        // Parse vendor/product ID
        QRegularExpressionMatch idMatch = idRegex.match(line);
        if (idMatch.hasMatch()) {
            currentVendor = idMatch.captured(1);
            currentProduct = idMatch.captured(2);
            continue;
        }
    }

    // Handle last device if file doesn't end with empty line
    if (!currentName.isEmpty() && currentEventNumber >= 0) {
        InputDevice device;
        device.eventNumber = currentEventNumber;
        device.name = currentName;
        device.path = QStringLiteral("/dev/input/event%1").arg(currentEventNumber);
        device.type = detectDeviceType(currentName, currentHandlers);
        device.vendorId = currentVendor;
        device.productId = currentProduct;
        device.physPath = currentPhys;
        device.assigned = false;
        device.assignedInstance = -1;
        device.isVirtual = isVirtualDevice(currentName, currentPhys);
        device.isInternal = isInternalDevice(currentName);

        m_devices.append(device);
    }

    file.close();
    
    qDebug() << "DeviceManager: Found" << m_devices.size() << "input devices";
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
        lowerName.contains(QStringLiteral("wireless controller")) ||
        lowerName.contains(QStringLiteral("sony")) ||
        lowerName.contains(QStringLiteral("nintendo")) ||
        lowerName.contains(QStringLiteral("pro controller")) ||
        lowerName.contains(QStringLiteral("8bitdo")) ||
        lowerName.contains(QStringLiteral("steam controller")) ||
        lowerHandlers.contains(QStringLiteral("js"))) {
        return QStringLiteral("controller");
    }

    // Check for keyboards
    if (lowerName.contains(QStringLiteral("keyboard")) ||
        (lowerHandlers.contains(QStringLiteral("kbd")) && 
         !lowerHandlers.contains(QStringLiteral("mouse")) &&
         !lowerName.contains(QStringLiteral("button")))) {
        return QStringLiteral("keyboard");
    }

    // Check for mice
    if (lowerName.contains(QStringLiteral("mouse")) ||
        lowerName.contains(QStringLiteral("touchpad")) ||
        lowerName.contains(QStringLiteral("trackpad")) ||
        lowerName.contains(QStringLiteral("trackball")) ||
        lowerHandlers.contains(QStringLiteral("mouse"))) {
        return QStringLiteral("mouse");
    }

    return QStringLiteral("other");
}

bool DeviceManager::isVirtualDevice(const QString &name, const QString &physPath) const
{
    QString lowerName = name.toLower();
    QString lowerPhys = physPath.toLower();
    
    // Virtual devices typically have no physical path or specific patterns
    if (physPath.isEmpty()) {
        return true;
    }
    
    // Common virtual device indicators
    if (lowerName.contains(QStringLiteral("virtual")) ||
        lowerName.contains(QStringLiteral("xtest")) ||
        lowerName.contains(QStringLiteral("uinput")) ||
        lowerPhys.contains(QStringLiteral("virtual"))) {
        return true;
    }
    
    return false;
}

bool DeviceManager::isInternalDevice(const QString &name) const
{
    QString lowerName = name.toLower();
    
    // Internal system devices
    if (lowerName.contains(QStringLiteral("power button")) ||
        lowerName.contains(QStringLiteral("sleep button")) ||
        lowerName.contains(QStringLiteral("lid switch")) ||
        lowerName.contains(QStringLiteral("video bus")) ||
        lowerName.contains(QStringLiteral("pc speaker")) ||
        lowerName.contains(QStringLiteral("acpi")) ||
        lowerName.contains(QStringLiteral("at translated")) ||
        lowerName.contains(QStringLiteral("intel hid")) ||
        lowerName.contains(QStringLiteral("wireless hotkeys")) ||
        lowerName.contains(QStringLiteral("wmi"))) {
        return true;
    }
    
    return false;
}

bool DeviceManager::assignDevice(int eventNumber, int instanceIndex)
{
    for (int i = 0; i < m_devices.size(); ++i) {
        if (m_devices[i].eventNumber == eventNumber) {
            m_devices[i].assigned = (instanceIndex >= 0);
            m_devices[i].assignedInstance = instanceIndex;
            Q_EMIT devicesChanged();
            Q_EMIT deviceAssigned(eventNumber, instanceIndex);
            qDebug() << "DeviceManager: Assigned device" << m_devices[i].name 
                     << "to instance" << instanceIndex;
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
    qDebug() << "DeviceManager: Unassigned all devices";
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

QStringList DeviceManager::getDevicePathsForInstance(int instanceIndex) const
{
    QStringList result;
    for (const auto &device : m_devices) {
        if (device.assignedInstance == instanceIndex) {
            result.append(device.path);
        }
    }
    return result;
}

int DeviceManager::autoAssignControllers()
{
    // First, unassign all controllers
    for (auto &device : m_devices) {
        if (device.type == QStringLiteral("controller")) {
            device.assigned = false;
            device.assignedInstance = -1;
        }
    }
    
    // Get list of non-virtual controllers
    QList<int> controllerIndices;
    for (int i = 0; i < m_devices.size(); ++i) {
        if (m_devices[i].type == QStringLiteral("controller") && 
            !m_devices[i].isVirtual) {
            controllerIndices.append(i);
        }
    }
    
    // Assign one controller per instance
    int assignedCount = 0;
    for (int instance = 0; instance < m_instanceCount && assignedCount < controllerIndices.size(); ++instance) {
        int deviceIndex = controllerIndices[assignedCount];
        m_devices[deviceIndex].assigned = true;
        m_devices[deviceIndex].assignedInstance = instance;
        Q_EMIT deviceAssigned(m_devices[deviceIndex].eventNumber, instance);
        assignedCount++;
    }
    
    Q_EMIT devicesChanged();
    qDebug() << "DeviceManager: Auto-assigned" << assignedCount << "controllers";
    return assignedCount;
}

void DeviceManager::identifyDevice(int eventNumber)
{
    // Find the device
    const InputDevice *device = nullptr;
    for (const auto &d : m_devices) {
        if (d.eventNumber == eventNumber) {
            device = &d;
            break;
        }
    }
    
    if (!device) {
        Q_EMIT errorOccurred(QStringLiteral("Device not found"));
        return;
    }
    
    // Only controllers support rumble
    if (device->type != QStringLiteral("controller")) {
        qDebug() << "DeviceManager: Device" << device->name << "does not support identification";
        return;
    }
    
    // Try to trigger rumble using force feedback
    int fd = open(device->path.toLocal8Bit().constData(), O_RDWR);
    if (fd < 0) {
        qDebug() << "DeviceManager: Cannot open device for identification:" << device->path;
        return;
    }
    
    // Check for force feedback support
    unsigned long features[4] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_FF, sizeof(features)), features) >= 0) {
        // Create a simple rumble effect
        struct ff_effect effect;
        memset(&effect, 0, sizeof(effect));
        effect.type = FF_RUMBLE;
        effect.id = -1;
        effect.u.rumble.strong_magnitude = 0xC000;
        effect.u.rumble.weak_magnitude = 0xC000;
        effect.replay.length = 500; // 500ms
        effect.replay.delay = 0;
        
        if (ioctl(fd, EVIOCSFF, &effect) >= 0) {
            // Play the effect
            struct input_event play;
            memset(&play, 0, sizeof(play));
            play.type = EV_FF;
            play.code = effect.id;
            play.value = 1;
            
            if (write(fd, &play, sizeof(play)) > 0) {
                qDebug() << "DeviceManager: Triggered rumble on" << device->name;
            }
        }
    }
    
    close(fd);
}

QVariantMap DeviceManager::getDevice(int eventNumber) const
{
    for (const auto &device : m_devices) {
        if (device.eventNumber == eventNumber) {
            return deviceToVariantMap(device);
        }
    }
    return QVariantMap();
}

QVariantMap DeviceManager::deviceToVariantMap(const InputDevice &device) const
{
    QVariantMap map;
    map[QStringLiteral("eventNumber")] = device.eventNumber;
    map[QStringLiteral("name")] = device.name;
    map[QStringLiteral("type")] = device.type;
    map[QStringLiteral("path")] = device.path;
    map[QStringLiteral("vendorId")] = device.vendorId;
    map[QStringLiteral("productId")] = device.productId;
    map[QStringLiteral("physPath")] = device.physPath;
    map[QStringLiteral("assigned")] = device.assigned;
    map[QStringLiteral("assignedInstance")] = device.assignedInstance;
    map[QStringLiteral("isVirtual")] = device.isVirtual;
    map[QStringLiteral("isInternal")] = device.isInternal;
    return map;
}

QVariantList DeviceManager::devicesAsVariant() const
{
    QVariantList list;
    for (const auto &device : m_devices) {
        list.append(deviceToVariantMap(device));
    }
    return list;
}

QVariantList DeviceManager::visibleDevicesAsVariant() const
{
    QVariantList list;
    for (const auto &device : m_devices) {
        // Filter based on settings
        if (!m_showVirtualDevices && device.isVirtual) {
            continue;
        }
        if (!m_showInternalDevices && device.isInternal) {
            continue;
        }
        // Always hide "other" type devices
        if (device.type == QStringLiteral("other")) {
            continue;
        }
        
        list.append(deviceToVariantMap(device));
    }
    return list;
}

QVariantList DeviceManager::controllersAsVariant() const
{
    QVariantList list;
    for (const auto &device : m_devices) {
        if (device.type == QStringLiteral("controller")) {
            if (!m_showVirtualDevices && device.isVirtual) {
                continue;
            }
            list.append(deviceToVariantMap(device));
        }
    }
    return list;
}

QVariantList DeviceManager::keyboardsAsVariant() const
{
    QVariantList list;
    for (const auto &device : m_devices) {
        if (device.type == QStringLiteral("keyboard")) {
            if (!m_showVirtualDevices && device.isVirtual) {
                continue;
            }
            if (!m_showInternalDevices && device.isInternal) {
                continue;
            }
            list.append(deviceToVariantMap(device));
        }
    }
    return list;
}

QVariantList DeviceManager::miceAsVariant() const
{
    QVariantList list;
    for (const auto &device : m_devices) {
        if (device.type == QStringLiteral("mouse")) {
            if (!m_showVirtualDevices && device.isVirtual) {
                continue;
            }
            list.append(deviceToVariantMap(device));
        }
    }
    return list;
}

void DeviceManager::setShowVirtualDevices(bool show)
{
    if (m_showVirtualDevices != show) {
        m_showVirtualDevices = show;
        Q_EMIT showVirtualDevicesChanged();
        Q_EMIT devicesChanged();
    }
}

void DeviceManager::setShowInternalDevices(bool show)
{
    if (m_showInternalDevices != show) {
        m_showInternalDevices = show;
        Q_EMIT showInternalDevicesChanged();
        Q_EMIT devicesChanged();
    }
}

void DeviceManager::setHotplugEnabled(bool enabled)
{
    if (m_hotplugEnabled != enabled) {
        m_hotplugEnabled = enabled;
        setupHotplugWatcher();
        Q_EMIT hotplugEnabledChanged();
    }
}

void DeviceManager::setInstanceCount(int count)
{
    if (m_instanceCount != count && count > 0 && count <= 4) {
        m_instanceCount = count;
        Q_EMIT instanceCountChanged();
    }
}
