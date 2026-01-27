// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "DeviceManager.h"
#include "SettingsManager.h"

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
#include <cerrno>

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
    QSet<QString> oldStableIds;
    for (const auto &device : m_devices) {
        oldEventNumbers.append(device.eventNumber);
        oldDeviceNames[device.eventNumber] = device.name;
        if (!device.stableId.isEmpty()) {
            oldStableIds.insert(device.stableId);
        }
    }
    
    // Refresh devices
    m_devices.clear();
    parseDevices();
    
    // Build set of currently connected stableIds
    QSet<QString> newStableIds;
    for (const auto &device : m_devices) {
        if (!device.stableId.isEmpty()) {
            newStableIds.insert(device.stableId);
        }
    }
    
    // Check for disconnected devices that were in the cache - add to pending
    bool pendingChanged = false;
    for (auto it = m_assignmentCache.constBegin(); it != m_assignmentCache.constEnd(); ++it) {
        const QString &stableId = it.key();
        if (!newStableIds.contains(stableId)) {
            // Device was assigned but is now disconnected - add to pending if not already there
            bool alreadyPending = false;
            for (const auto &pending : m_pendingDevices) {
                if (pending[QStringLiteral("stableId")].toString() == stableId) {
                    alreadyPending = true;
                    break;
                }
            }
            if (!alreadyPending) {
                QVariantMap pending;
                pending[QStringLiteral("stableId")] = stableId;
                pending[QStringLiteral("name")] = it.value().second;
                pending[QStringLiteral("instanceIndex")] = it.value().first;
                m_pendingDevices.append(pending);
                pendingChanged = true;
                qDebug() << "DeviceManager: Device disconnected, added to pending:" 
                         << it.value().second << "for instance" << it.value().first;
            }
        }
    }
    
    // Restore assignments from persistent cache (survives across hotplug cycles)
    for (auto &device : m_devices) {
        if (m_assignmentCache.contains(device.stableId)) {
            int instanceIndex = m_assignmentCache[device.stableId].first;
            device.assigned = true;
            device.assignedInstance = instanceIndex;
            
            // Check if this is a reconnected device (wasn't present before)
            bool wasPresent = oldStableIds.contains(device.stableId);
            
            if (!wasPresent) {
                qDebug() << "DeviceManager: Device reconnected:" << device.name 
                         << "stableId:" << device.stableId
                         << "eventNumber:" << device.eventNumber;
                Q_EMIT deviceReconnected(device.stableId, device.eventNumber, instanceIndex);
                
                // Remove from pending list
                for (int i = m_pendingDevices.size() - 1; i >= 0; --i) {
                    if (m_pendingDevices[i][QStringLiteral("stableId")].toString() == device.stableId) {
                        m_pendingDevices.removeAt(i);
                        pendingChanged = true;
                        Q_EMIT deviceAutoRestored(device.name, instanceIndex);
                        break;
                    }
                }
            }
        }
    }
    
    // Detect added/removed devices for signals
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
    
    if (pendingChanged) {
        Q_EMIT pendingDevicesChanged();
    }
    
    // Check if any pending devices (from profile load) have reconnected
    checkPendingDevices();
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
        qWarning() << "DeviceManager: Failed to open /proc/bus/input/devices";
        Q_EMIT errorOccurred(QStringLiteral("Failed to open /proc/bus/input/devices"));
        return;
    }
    
    // Read entire file content (procfs files report size 0, so we must read all)
    QByteArray content = file.readAll();
    file.close();
    
    if (content.isEmpty()) {
        qWarning() << "DeviceManager: /proc/bus/input/devices is empty";
        return;
    }

    QTextStream stream(&content);
    QString currentName;
    QString currentHandlers;
    QString currentPhys;
    QString currentVendor;
    QString currentProduct;
    int currentEventNumber = -1;
    int lineCount = 0;

    static const QRegularExpression nameRegex(QStringLiteral("^N: Name=\"(.*)\"$"));
    static const QRegularExpression handlersRegex(QStringLiteral("^H: Handlers=(.*)$"));
    static const QRegularExpression eventRegex(QStringLiteral("event(\\d+)"));
    static const QRegularExpression joyRegex(QStringLiteral("js(\\d+)"));
    static const QRegularExpression physRegex(QStringLiteral("^P: Phys=(.*)$"));
    static const QRegularExpression idRegex(QStringLiteral("^I: Bus=\\w+ Vendor=(\\w+) Product=(\\w+)"));

    while (!stream.atEnd()) {
        QString line = stream.readLine();
        lineCount++;

        if (line.isEmpty()) {
            // End of device block - create device if we have valid data
            if (!currentName.isEmpty() && currentEventNumber >= 0) {
                InputDevice device;
                device.eventNumber = currentEventNumber;
                device.name = currentName;
                device.path = QStringLiteral("/dev/input/event%1").arg(currentEventNumber);
                
                // Construct joystick path if a js handler was found
                if (!currentHandlers.isEmpty()) {
                    QRegularExpressionMatch joyMatch = joyRegex.match(currentHandlers);
                    if (joyMatch.hasMatch()) {
                        device.joyPath = QStringLiteral("/dev/input/js%1").arg(joyMatch.captured(1));
                    }
                }

                device.type = detectDeviceType(currentName, currentHandlers);
                device.vendorId = currentVendor;
                device.productId = currentProduct;
                device.physPath = currentPhys;
                device.stableId = generateStableId(currentVendor, currentProduct, currentPhys);
                device.assigned = false;
                device.assignedInstance = -1;
                device.isVirtual = isVirtualDevice(currentName, currentPhys);
                device.isInternal = isInternalDevice(currentName);

                if (m_settingsManager && m_settingsManager->ignoredDevices().contains(device.stableId)) {
                    qDebug() << "DeviceManager: Ignoring device" << device.name << "stableId:" << device.stableId;
                } else {
                    m_devices.append(device);
                }
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

        // Construct joystick path if a js handler was found
        if (!currentHandlers.isEmpty()) {
            QRegularExpressionMatch joyMatch = joyRegex.match(currentHandlers);
            if (joyMatch.hasMatch()) {
                device.joyPath = QStringLiteral("/dev/input/js%1").arg(joyMatch.captured(1));
            }
        }

        device.type = detectDeviceType(currentName, currentHandlers);
        device.vendorId = currentVendor;
        device.productId = currentProduct;
        device.physPath = currentPhys;
        device.stableId = generateStableId(currentVendor, currentProduct, currentPhys);
        device.assigned = false;
        device.assignedInstance = -1;
        device.isVirtual = isVirtualDevice(currentName, currentPhys);
        device.isInternal = isInternalDevice(currentName);

        if (m_settingsManager && m_settingsManager->ignoredDevices().contains(device.stableId)) {
            qDebug() << "DeviceManager: Ignoring device" << device.name << "stableId:" << device.stableId;
        } else {
            m_devices.append(device);
        }
    }
    
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
        (lowerHandlers.contains(QStringLiteral("js")) && 
         !lowerName.contains(QStringLiteral("mouse")) && 
         !lowerName.contains(QStringLiteral("keyboard")))) {
        
        // Extra check: If it claims to be a controller but has no buttons, it's likely a wireless receiver with no controller connected
        // We can check this by trying to open the device and querying capabilities
        // However, we need to be careful about permissions. 
        // If we can't open it, we assume it's valid to avoid blocking valid devices due to permission issues.
        // But for "ghost" devices that are readable (like event8 in the user report), this check will filter them out.
        
        QString path = QStringLiteral("/dev/input/event%1").arg(handlers.section(QStringLiteral("event"), 1, 1).section(QLatin1Char(' '), 0, 0));
        if (!path.isEmpty()) {
            int fd = open(path.toLocal8Bit().constData(), O_RDONLY);
            if (fd >= 0) {
                unsigned char keyBitmask[KEY_MAX/8 + 1] = {0};
                if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyBitmask)), keyBitmask) >= 0) {
                    bool hasGamepadBtns = false;
                    // BTN_GAMEPAD is 0x130 (304)
                    for (int i = 0x130; i < 0x140; i++) {
                        if ((keyBitmask[i/8] >> (i%8)) & 1) {
                            hasGamepadBtns = true;
                            break;
                        }
                    }
                    
                    close(fd);
                    if (!hasGamepadBtns) {
                         qDebug() << "DeviceManager: Device" << name << "ignored (no gamepad buttons)";
                         return QStringLiteral("other");
                    }
                } else {
                    close(fd);
                }
            }
        }

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
            int previousInstance = m_devices[i].assignedInstance;
            m_devices[i].assigned = (instanceIndex >= 0);
            m_devices[i].assignedInstance = instanceIndex;
            
            // Update persistent assignment cache
            if (!m_devices[i].stableId.isEmpty()) {
                if (instanceIndex >= 0) {
                    m_assignmentCache[m_devices[i].stableId] = qMakePair(instanceIndex, m_devices[i].name);
                } else {
                    m_assignmentCache.remove(m_devices[i].stableId);
                }
            }
            
            Q_EMIT devicesChanged();
            Q_EMIT deviceAssigned(eventNumber, instanceIndex, previousInstance);
            qDebug() << "DeviceManager: Assigned device" << m_devices[i].name 
                     << "to instance" << instanceIndex << "(was:" << previousInstance << ")";
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
            if (!device.joyPath.isEmpty()) {
                result.append(device.joyPath);
            }
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
        int previousInstance = m_devices[deviceIndex].assignedInstance; // Will be -1 since we unassigned all above
        m_devices[deviceIndex].assigned = true;
        m_devices[deviceIndex].assignedInstance = instance;
        Q_EMIT deviceAssigned(m_devices[deviceIndex].eventNumber, instance, previousInstance);
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
    if (ioctl(fd, EVIOCGBIT(EV_FF, sizeof(features)), features) < 0) {
        qWarning() << "DeviceManager: Failed to get FF features for" << device->name << "errno:" << errno;
        close(fd);
        return;
    }

    // Create a simple rumble effect
    struct ff_effect effect;
    memset(&effect, 0, sizeof(effect));
    effect.type = FF_RUMBLE;
    effect.id = -1;
    effect.u.rumble.strong_magnitude = 0xC000;
    effect.u.rumble.weak_magnitude = 0xC000;
    effect.replay.length = 1000; // 1000ms (matched to test_rumble)
    effect.replay.delay = 0;
    
    if (ioctl(fd, EVIOCSFF, &effect) < 0) {
        qWarning() << "DeviceManager: Failed to upload rumble effect to" << device->name << "errno:" << errno;
        close(fd);
        return;
    }

    // Play the effect
    struct input_event play;
    memset(&play, 0, sizeof(play));
    play.type = EV_FF;
    play.code = effect.id;
    play.value = 1;
    
    if (write(fd, &play, sizeof(play)) < 0) {
        qWarning() << "DeviceManager: Failed to play rumble effect on" << device->name << "errno:" << errno;
        close(fd);
    } else {
        // Keep FD open for duration of effect, otherwise kernel stops it immediately
        // Use QTimer to close it asynchronously
        QTimer::singleShot(effect.replay.length + 100, [fd, id = effect.id, name = device->name]() {
            struct input_event stop;
            memset(&stop, 0, sizeof(stop));
            stop.type = EV_FF;
            stop.code = id;
            stop.value = 0;
            if (write(fd, &stop, sizeof(stop)) < 0) {
                // Best effort stop
            }
            close(fd);
        });
    }
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
    map[QStringLiteral("joyPath")] = device.joyPath;
    map[QStringLiteral("vendorId")] = device.vendorId;
    map[QStringLiteral("productId")] = device.productId;
    map[QStringLiteral("physPath")] = device.physPath;
    map[QStringLiteral("stableId")] = device.stableId;
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

void DeviceManager::setSettingsManager(SettingsManager *manager)
{
    if (m_settingsManager != manager) {
        if (m_settingsManager) {
            disconnect(m_settingsManager, &SettingsManager::ignoredDevicesChanged, this, &DeviceManager::refresh);
        }
        m_settingsManager = manager;
        if (m_settingsManager) {
            connect(m_settingsManager, &SettingsManager::ignoredDevicesChanged, this, &DeviceManager::onIgnoredDevicesChanged);
        }
        Q_EMIT settingsManagerChanged();
        refresh();
    }
}

void DeviceManager::onIgnoredDevicesChanged()
{
    refresh();
}

void DeviceManager::ignoreDevice(const QString &stableId)
{
    if (m_settingsManager) {
        m_settingsManager->addIgnoredDevice(stableId);
    }
}

void DeviceManager::unignoreDevice(const QString &stableId)
{
    if (m_settingsManager) {
        m_settingsManager->removeIgnoredDevice(stableId);
    }
}

QString DeviceManager::generateStableId(const QString &vendorId, const QString &productId, const QString &physPath)
{
    // Create a stable identifier from hardware properties
    // Format: "vendorId:productId:physPath"
    // This survives hotplug events and reboots (as long as device is plugged into same port)
    if (vendorId.isEmpty() && productId.isEmpty() && physPath.isEmpty()) {
        return QString();
    }
    return QStringLiteral("%1:%2:%3").arg(vendorId, productId, physPath);
}

int DeviceManager::findDeviceByStableId(const QString &stableId) const
{
    if (stableId.isEmpty()) {
        return -1;
    }
    
    for (const auto &device : m_devices) {
        if (device.stableId == stableId) {
            return device.eventNumber;
        }
    }
    return -1;
}

bool DeviceManager::assignDeviceByStableId(const QString &stableId, int instanceIndex)
{
    int eventNumber = findDeviceByStableId(stableId);
    if (eventNumber < 0) {
        qDebug() << "DeviceManager: Device with stableId" << stableId << "not found";
        return false;
    }
    return assignDevice(eventNumber, instanceIndex);
}

QStringList DeviceManager::getStableIdsForInstance(int instanceIndex) const
{
    QStringList result;
    for (const auto &device : m_devices) {
        if (device.assignedInstance == instanceIndex && !device.stableId.isEmpty()) {
            result.append(device.stableId);
        }
    }
    return result;
}

QStringList DeviceManager::getDeviceNamesForInstance(int instanceIndex) const
{
    QStringList result;
    for (const auto &device : m_devices) {
        if (device.assignedInstance == instanceIndex) {
            result.append(device.name);
        }
    }
    return result;
}

void DeviceManager::restoreAssignmentsFromStableIds(int instanceIndex, const QStringList &stableIds, const QStringList &names)
{
    for (int i = 0; i < stableIds.size(); ++i) {
        const QString &stableId = stableIds[i];
        QString name = (i < names.size()) ? names[i] : stableId;
        
        if (assignDeviceByStableId(stableId, instanceIndex)) {
            qDebug() << "DeviceManager: Restored device" << name << "to instance" << instanceIndex;
        } else {
            // Device not connected - add to pending list
            qDebug() << "DeviceManager: Device" << name << "not connected, adding to pending list";
            
            QVariantMap pending;
            pending[QStringLiteral("stableId")] = stableId;
            pending[QStringLiteral("name")] = name;
            pending[QStringLiteral("instanceIndex")] = instanceIndex;
            m_pendingDevices.append(pending);
        }
    }
    
    if (!m_pendingDevices.isEmpty()) {
        Q_EMIT pendingDevicesChanged();
    }
}

void DeviceManager::clearPendingDevicesForInstance(int instanceIndex)
{
    if (instanceIndex < 0) {
        // Clear all
        m_pendingDevices.clear();
    } else {
        // Remove only for specific instance
        m_pendingDevices.erase(
            std::remove_if(m_pendingDevices.begin(), m_pendingDevices.end(),
                [instanceIndex](const QVariantMap &p) {
                    return p[QStringLiteral("instanceIndex")].toInt() == instanceIndex;
                }),
            m_pendingDevices.end());
    }
    Q_EMIT pendingDevicesChanged();
}

QVariantList DeviceManager::pendingDevicesAsVariant() const
{
    QVariantList result;
    for (const auto &pending : m_pendingDevices) {
        result.append(pending);
    }
    return result;
}

void DeviceManager::checkPendingDevices()
{
    if (m_pendingDevices.isEmpty()) {
        return;
    }
    
    bool changed = false;
    QList<QVariantMap> stillPending;
    
    for (const auto &pending : m_pendingDevices) {
        QString stableId = pending[QStringLiteral("stableId")].toString();
        QString name = pending[QStringLiteral("name")].toString();
        int instanceIndex = pending[QStringLiteral("instanceIndex")].toInt();
        
        int eventNumber = findDeviceByStableId(stableId);
        if (eventNumber >= 0) {
            // Device reconnected! Assign it
            if (assignDevice(eventNumber, instanceIndex)) {
                qDebug() << "DeviceManager: Auto-restored device" << name << "to instance" << instanceIndex;
                Q_EMIT deviceAutoRestored(name, instanceIndex);
                changed = true;
            } else {
                stillPending.append(pending);
            }
        } else {
            stillPending.append(pending);
        }
    }
    
    if (changed) {
        m_pendingDevices = stillPending;
        Q_EMIT pendingDevicesChanged();
    }
}
