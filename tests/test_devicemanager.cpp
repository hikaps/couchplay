// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QSet>
#include <QTemporaryFile>
#include <QTest>
#include <linux/input.h>

#define private public
#include "DeviceManager.h"
#undef private
#include "SettingsManager.h"

// Helper macro for QVariantMap key access with proper QString conversion
#define KEY(x) QStringLiteral(x)

class TestDeviceManager : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();

    // Basic functionality tests
    void testInitialization();
    void testRefresh();
    void testDeviceAssignment();
    void testUnassignAll();
    void testAutoAssignControllers();
    void testGetDevicesForInstance();
    void testGetDevicePathsForInstance();
    void testIdentifyDevice();
    void testGetDevice();

    // Property tests
    void testShowVirtualDevices();
    void testShowInternalDevices();
    void testHotplugEnabled();
    void testInstanceCount();

    // Filtering tests
    void testDeviceFiltering();
    void testControllersProperty();
    void testKeyboardsProperty();
    void testMiceProperty();
    void testVisibleDevicesProperty();

    // Signal tests
    void testDevicesChangedSignal();
    void testDeviceAssignedSignal();
    void testInstanceCountChangedSignal();

    // Stable ID tests
    void testGenerateStableId();
    void testFindDeviceByStableId();
    void testAssignDeviceByStableId();
    void testGetStableIdsForInstance();
    void testRestoreAssignmentsFromStableIds();
    void testStableIdInDeviceVariantMap();

    // Blacklist tests
    void testIgnoredDevices();
    void testIsVirtualDevice();
    void testSteamControllerDetection();
    void testPlayStationControllerGrouping();
    void testPlayStationBluetoothControllerGrouping();

private:
    DeviceManager *m_deviceManager = nullptr;
};

void TestDeviceManager::initTestCase()
{
    m_deviceManager = new DeviceManager(this);
}

void TestDeviceManager::cleanupTestCase()
{
    delete m_deviceManager;
    m_deviceManager = nullptr;
}

void TestDeviceManager::testInitialization()
{
    // DeviceManager should initialize with sensible defaults
    QVERIFY(m_deviceManager != nullptr);
    QCOMPARE(m_deviceManager->instanceCount(), 2);
    QCOMPARE(m_deviceManager->showVirtualDevices(), false);
    QCOMPARE(m_deviceManager->showInternalDevices(), false);
    QCOMPARE(m_deviceManager->hotplugEnabled(), true);
}

void TestDeviceManager::testRefresh()
{
    QSignalSpy spy(m_deviceManager, &DeviceManager::devicesChanged);
    m_deviceManager->refresh();

    // Refresh should emit devicesChanged signal
    QCOMPARE(spy.count(), 1);
}

void TestDeviceManager::testDeviceAssignment()
{
    // Try to assign a non-existent device (should return false)
    bool result = m_deviceManager->assignDevice(-1, 0);
    QCOMPARE(result, false);

    // If there are devices, try to assign one
    QVariantList devices = m_deviceManager->devicesAsVariant();
    if (!devices.isEmpty()) {
        QVariantMap device = devices.first().toMap();
        int eventNumber = device.value(KEY("eventNumber")).toInt();

        // Assign to instance 0
        result = m_deviceManager->assignDevice(eventNumber, 0);
        QCOMPARE(result, true);

        // Check that device is now assigned
        QVariantMap updatedDevice = m_deviceManager->getDevice(eventNumber);
        QCOMPARE(updatedDevice.value(KEY("assigned")).toBool(), true);
        QCOMPARE(updatedDevice.value(KEY("assignedInstance")).toInt(), 0);

        // Unassign (assign to -1)
        result = m_deviceManager->assignDevice(eventNumber, -1);
        QCOMPARE(result, true);

        // Check that device is now unassigned
        updatedDevice = m_deviceManager->getDevice(eventNumber);
        QCOMPARE(updatedDevice.value(KEY("assigned")).toBool(), false);
        QCOMPARE(updatedDevice.value(KEY("assignedInstance")).toInt(), -1);
    }
}

void TestDeviceManager::testUnassignAll()
{
    QVariantList devices = m_deviceManager->devicesAsVariant();

    // Assign some devices first
    for (int i = 0; i < qMin(devices.size(), 2); ++i) {
        QVariantMap device = devices.at(i).toMap();
        int eventNumber = device.value(KEY("eventNumber")).toInt();
        m_deviceManager->assignDevice(eventNumber, i);
    }

    // Unassign all
    m_deviceManager->unassignAll();

    // Check that no devices are assigned
    devices = m_deviceManager->devicesAsVariant();
    for (const QVariant &v : devices) {
        QVariantMap device = v.toMap();
        QCOMPARE(device.value(KEY("assigned")).toBool(), false);
        QCOMPARE(device.value(KEY("assignedInstance")).toInt(), -1);
    }
}

void TestDeviceManager::testAutoAssignControllers()
{
    // First unassign all
    m_deviceManager->unassignAll();

    // Auto-assign controllers
    int count = m_deviceManager->autoAssignControllers();

    // Count should be >= 0 and <= instanceCount
    QVERIFY(count >= 0);
    QVERIFY(count <= m_deviceManager->instanceCount());

    // Each assigned controller should have a unique instance
    QVariantList controllers = m_deviceManager->controllersAsVariant();
    QSet<int> assignedInstances;
    for (const QVariant &v : controllers) {
        QVariantMap controller = v.toMap();
        if (controller.value(KEY("assigned")).toBool()) {
            int instance = controller.value(KEY("assignedInstance")).toInt();
            QVERIFY(!assignedInstances.contains(instance));
            assignedInstances.insert(instance);
        }
    }
}

void TestDeviceManager::testGetDevicesForInstance()
{
    m_deviceManager->unassignAll();

    // Assign a few devices to instance 0
    QVariantList devices = m_deviceManager->devicesAsVariant();
    int assignedCount = 0;
    for (int i = 0; i < qMin(devices.size(), 3); ++i) {
        QVariantMap device = devices.at(i).toMap();
        int eventNumber = device.value(KEY("eventNumber")).toInt();
        if (m_deviceManager->assignDevice(eventNumber, 0)) {
            assignedCount++;
        }
    }

    // Get devices for instance 0
    QList<int> instanceDevices = m_deviceManager->getDevicesForInstance(0);
    QVERIFY(instanceDevices.size() >= assignedCount);

    // Get devices for unassigned instance
    QList<int> emptyList = m_deviceManager->getDevicesForInstance(1);
    QCOMPARE(emptyList.size(), 0);
}

void TestDeviceManager::testGetDevicePathsForInstance()
{
    m_deviceManager->unassignAll();

    // Requires real input devices; skip in environments without them (CI).
    QVariantList devices = m_deviceManager->devicesAsVariant();
    if (devices.size() < 2) {
        QSKIP("Not enough input devices to test getDevicePathsForInstance");
    }
    for (int i = 0; i < qMin(devices.size(), 2); ++i) {
        QVariantMap device = devices.at(i).toMap();
        int eventNumber = device.value(KEY("eventNumber")).toInt();
        m_deviceManager->assignDevice(eventNumber, 0);
    }

    // Get paths for instance 0
    QStringList paths = m_deviceManager->getDevicePathsForInstance(0);

    // No usable input devices in some environments (CI container) -> skip.
    if (paths.isEmpty()) {
        QSKIP("No device paths resolved for the instance (no usable input devices)");
    }
    // getDevicePathsForInstance returns /dev/input/eventN and/or /dev/input/jsN;
    // both are valid. Assert each is a real input-device path (don't skip on the
    // joydev paths real controllers expose).
    for (const QString &path : paths) {
        QVERIFY2(path.startsWith(QStringLiteral("/dev/input/")),
                 qPrintable(QStringLiteral("Unexpected device path: %1").arg(path)));
    }
}

void TestDeviceManager::testIdentifyDevice()
{
    // Just test that this doesn't crash (actual rumble requires hardware)
    QVariantList controllers = m_deviceManager->controllersAsVariant();
    if (!controllers.isEmpty()) {
        QVariantMap controller = controllers.first().toMap();
        int eventNumber = controller.value(KEY("eventNumber")).toInt();
        m_deviceManager->identifyDevice(eventNumber);
    }
    QVERIFY(true);
}

void TestDeviceManager::testGetDevice()
{
    // Test getting a non-existent device
    QVariantMap noDevice = m_deviceManager->getDevice(-1);
    QVERIFY(noDevice.isEmpty());

    // Test getting an existing device
    QVariantList devices = m_deviceManager->devicesAsVariant();
    if (!devices.isEmpty()) {
        QVariantMap device = devices.first().toMap();
        int eventNumber = device.value(KEY("eventNumber")).toInt();

        QVariantMap retrievedDevice = m_deviceManager->getDevice(eventNumber);
        QVERIFY(!retrievedDevice.isEmpty());
        QCOMPARE(retrievedDevice.value(KEY("eventNumber")).toInt(), eventNumber);
        QVERIFY(retrievedDevice.contains(KEY("name")));
        QVERIFY(retrievedDevice.contains(KEY("type")));
        QVERIFY(retrievedDevice.contains(KEY("path")));
    }
}

void TestDeviceManager::testShowVirtualDevices()
{
    QSignalSpy spy(m_deviceManager, &DeviceManager::showVirtualDevicesChanged);

    bool initial = m_deviceManager->showVirtualDevices();
    m_deviceManager->setShowVirtualDevices(!initial);

    QCOMPARE(spy.count(), 1);
    QCOMPARE(m_deviceManager->showVirtualDevices(), !initial);

    // Reset to initial value
    m_deviceManager->setShowVirtualDevices(initial);
}

void TestDeviceManager::testShowInternalDevices()
{
    QSignalSpy spy(m_deviceManager, &DeviceManager::showInternalDevicesChanged);

    bool initial = m_deviceManager->showInternalDevices();
    m_deviceManager->setShowInternalDevices(!initial);

    QCOMPARE(spy.count(), 1);
    QCOMPARE(m_deviceManager->showInternalDevices(), !initial);

    // Reset to initial value
    m_deviceManager->setShowInternalDevices(initial);
}

void TestDeviceManager::testHotplugEnabled()
{
    QSignalSpy spy(m_deviceManager, &DeviceManager::hotplugEnabledChanged);

    bool initial = m_deviceManager->hotplugEnabled();
    m_deviceManager->setHotplugEnabled(!initial);

    QCOMPARE(spy.count(), 1);
    QCOMPARE(m_deviceManager->hotplugEnabled(), !initial);

    // Reset to initial value
    m_deviceManager->setHotplugEnabled(initial);
}

void TestDeviceManager::testInstanceCount()
{
    QSignalSpy spy(m_deviceManager, &DeviceManager::instanceCountChanged);

    int initial = m_deviceManager->instanceCount();
    m_deviceManager->setInstanceCount(3);

    if (initial != 3) {
        QCOMPARE(spy.count(), 1);
    }
    QCOMPARE(m_deviceManager->instanceCount(), 3);

    // Test bounds (should be clamped to 1-4)
    m_deviceManager->setInstanceCount(0);
    QVERIFY(m_deviceManager->instanceCount() > 0);

    m_deviceManager->setInstanceCount(5);
    QVERIFY(m_deviceManager->instanceCount() <= 4);

    // Reset to initial value
    m_deviceManager->setInstanceCount(initial);
}

void TestDeviceManager::testDeviceFiltering()
{
    // When showVirtualDevices is false, virtual devices should be filtered
    m_deviceManager->setShowVirtualDevices(false);

    QVariantList visible = m_deviceManager->visibleDevicesAsVariant();
    for (const QVariant &v : visible) {
        QVariantMap device = v.toMap();
        QCOMPARE(device.value(KEY("isVirtual")).toBool(), false);
    }

    // When showInternalDevices is false, internal devices should be filtered
    m_deviceManager->setShowInternalDevices(false);

    visible = m_deviceManager->visibleDevicesAsVariant();
    for (const QVariant &v : visible) {
        QVariantMap device = v.toMap();
        QCOMPARE(device.value(KEY("isInternal")).toBool(), false);
    }
}

void TestDeviceManager::testControllersProperty()
{
    QVariantList controllers = m_deviceManager->controllersAsVariant();

    // All items should be of type "controller"
    for (const QVariant &v : controllers) {
        QVariantMap controller = v.toMap();
        QCOMPARE(controller.value(KEY("type")).toString(), QStringLiteral("controller"));
    }
}

void TestDeviceManager::testKeyboardsProperty()
{
    QVariantList keyboards = m_deviceManager->keyboardsAsVariant();

    // All items should be of type "keyboard"
    for (const QVariant &v : keyboards) {
        QVariantMap keyboard = v.toMap();
        QCOMPARE(keyboard.value(KEY("type")).toString(), QStringLiteral("keyboard"));
    }
}

void TestDeviceManager::testMiceProperty()
{
    QVariantList mice = m_deviceManager->miceAsVariant();

    // All items should be of type "mouse"
    for (const QVariant &v : mice) {
        QVariantMap mouse = v.toMap();
        QCOMPARE(mouse.value(KEY("type")).toString(), QStringLiteral("mouse"));
    }
}

void TestDeviceManager::testVisibleDevicesProperty()
{
    // Visible devices should not include "other" type
    QVariantList visible = m_deviceManager->visibleDevicesAsVariant();

    for (const QVariant &v : visible) {
        QVariantMap device = v.toMap();
        QVERIFY(device.value(KEY("type")).toString() != QStringLiteral("other"));
    }
}

void TestDeviceManager::testDevicesChangedSignal()
{
    QSignalSpy spy(m_deviceManager, &DeviceManager::devicesChanged);

    // Toggle showVirtualDevices (should emit devicesChanged)
    bool initial = m_deviceManager->showVirtualDevices();
    m_deviceManager->setShowVirtualDevices(!initial);

    QVERIFY(spy.count() >= 1);

    // Reset
    m_deviceManager->setShowVirtualDevices(initial);
}

void TestDeviceManager::testDeviceAssignedSignal()
{
    QSignalSpy spy(m_deviceManager, &DeviceManager::deviceAssigned);

    QVariantList devices = m_deviceManager->devicesAsVariant();
    if (!devices.isEmpty()) {
        QVariantMap device = devices.first().toMap();
        int eventNumber = device.value(KEY("eventNumber")).toInt();

        m_deviceManager->assignDevice(eventNumber, 0);

        QCOMPARE(spy.count(), 1);
        QList<QVariant> args = spy.takeFirst();
        QCOMPARE(args.at(0).toInt(), eventNumber);
        QCOMPARE(args.at(1).toInt(), 0);

        // Unassign
        m_deviceManager->assignDevice(eventNumber, -1);
    }
}

void TestDeviceManager::testInstanceCountChangedSignal()
{
    QSignalSpy spy(m_deviceManager, &DeviceManager::instanceCountChanged);

    int initial = m_deviceManager->instanceCount();
    int newValue = (initial == 2) ? 3 : 2;

    m_deviceManager->setInstanceCount(newValue);
    QCOMPARE(spy.count(), 1);

    // Reset
    m_deviceManager->setInstanceCount(initial);
}

void TestDeviceManager::testGenerateStableId()
{
    // Test with all components
    QString stableId = DeviceManager::generateStableId(QStringLiteral("045e"),
                                                       QStringLiteral("028e"),
                                                       QStringLiteral("usb-0000:00:14.0-2.4/input0"));
    QCOMPARE(stableId, QStringLiteral("045e:028e:usb-0000:00:14.0-2.4/input0"));

    // Test with empty components - should return empty string
    QString emptyId = DeviceManager::generateStableId(QString(), QString(), QString());
    QVERIFY(emptyId.isEmpty());

    // Test with partial components - should still generate ID
    QString partialId = DeviceManager::generateStableId(QStringLiteral("045e"), QString(), QString());
    QVERIFY(!partialId.isEmpty());
    QVERIFY(partialId.startsWith(QStringLiteral("045e:")));

    // Test with uniq component
    QString withUniq = DeviceManager::generateStableId(QStringLiteral("054c"),
                                                       QStringLiteral("0ce6"),
                                                       QString(),
                                                       QStringLiteral("14:3a:9a:86:02:df"));
    QCOMPARE(withUniq, QStringLiteral("054c:0ce6::14:3a:9a:86:02:df"));
}

void TestDeviceManager::testFindDeviceByStableId()
{
    // Test finding non-existent device
    int notFound = m_deviceManager->findDeviceByStableId(QStringLiteral("nonexistent:id:here"));
    QCOMPARE(notFound, -1);

    // Test with empty stableId
    int emptyResult = m_deviceManager->findDeviceByStableId(QString());
    QCOMPARE(emptyResult, -1);

    // If devices exist, try to find one by its stableId
    QVariantList devices = m_deviceManager->devicesAsVariant();
    if (!devices.isEmpty()) {
        QVariantMap device = devices.first().toMap();
        QString stableId = device.value(KEY("stableId")).toString();
        int eventNumber = device.value(KEY("eventNumber")).toInt();

        if (!stableId.isEmpty()) {
            int foundEvent = m_deviceManager->findDeviceByStableId(stableId);
            QCOMPARE(foundEvent, eventNumber);
        }
    }
}

void TestDeviceManager::testAssignDeviceByStableId()
{
    m_deviceManager->unassignAll();

    // Test assigning non-existent device
    bool result = m_deviceManager->assignDeviceByStableId(QStringLiteral("nonexistent:id:here"), 0);
    QCOMPARE(result, false);

    // If devices exist, try to assign one by stableId
    QVariantList devices = m_deviceManager->devicesAsVariant();
    if (!devices.isEmpty()) {
        QVariantMap device = devices.first().toMap();
        QString stableId = device.value(KEY("stableId")).toString();
        int eventNumber = device.value(KEY("eventNumber")).toInt();

        if (!stableId.isEmpty()) {
            result = m_deviceManager->assignDeviceByStableId(stableId, 0);
            QCOMPARE(result, true);

            // Verify assignment
            QVariantMap updatedDevice = m_deviceManager->getDevice(eventNumber);
            QCOMPARE(updatedDevice.value(KEY("assigned")).toBool(), true);
            QCOMPARE(updatedDevice.value(KEY("assignedInstance")).toInt(), 0);

            // Unassign
            m_deviceManager->assignDevice(eventNumber, -1);
        }
    }
}

void TestDeviceManager::testGetStableIdsForInstance()
{
    m_deviceManager->unassignAll();

    // Get stable IDs for empty instance - should be empty
    QStringList emptyList = m_deviceManager->getStableIdsForInstance(0);
    QCOMPARE(emptyList.size(), 0);

    // Assign some devices to instance 0
    QVariantList devices = m_deviceManager->devicesAsVariant();
    QStringList expectedStableIds;

    for (int i = 0; i < qMin(devices.size(), 2); ++i) {
        QVariantMap device = devices.at(i).toMap();
        int eventNumber = device.value(KEY("eventNumber")).toInt();
        QString stableId = device.value(KEY("stableId")).toString();

        m_deviceManager->assignDevice(eventNumber, 0);
        if (!stableId.isEmpty()) {
            expectedStableIds.append(stableId);
        }
    }

    // Get stable IDs for instance 0
    QStringList stableIds = m_deviceManager->getStableIdsForInstance(0);
    QVERIFY(stableIds.size() >= expectedStableIds.size());

    for (const QString &id : expectedStableIds) {
        QVERIFY(stableIds.contains(id));
    }

    // Cleanup
    m_deviceManager->unassignAll();
}

void TestDeviceManager::testRestoreAssignmentsFromStableIds()
{
    m_deviceManager->unassignAll();

    // Clear any pending devices first
    m_deviceManager->clearPendingDevicesForInstance(-1);

    // Test with empty list - no assignments should happen
    m_deviceManager->restoreAssignmentsFromStableIds(0, QStringList(), QStringList());
    QList<int> assignedDevices = m_deviceManager->getDevicesForInstance(0);
    QCOMPARE(assignedDevices.size(), 0);

    // Test with non-existent stable IDs - should add to pending list
    QStringList fakeIds;
    QStringList fakeNames;
    fakeIds << QStringLiteral("fake:id:1") << QStringLiteral("fake:id:2");
    fakeNames << QStringLiteral("Fake Device 1") << QStringLiteral("Fake Device 2");
    m_deviceManager->restoreAssignmentsFromStableIds(0, fakeIds, fakeNames);
    assignedDevices = m_deviceManager->getDevicesForInstance(0);
    QCOMPARE(assignedDevices.size(), 0);

    // Pending devices should have 2 entries
    QVariantList pending = m_deviceManager->pendingDevicesAsVariant();
    QCOMPARE(pending.size(), 2);

    // Clear pending for cleanup
    m_deviceManager->clearPendingDevicesForInstance(-1);

    // If devices exist, test restoring real stable IDs
    QVariantList devices = m_deviceManager->devicesAsVariant();
    if (devices.size() >= 2) {
        QStringList realStableIds;
        QStringList realNames;
        for (int i = 0; i < 2; ++i) {
            QVariantMap device = devices.at(i).toMap();
            QString stableId = device.value(KEY("stableId")).toString();
            QString name = device.value(KEY("name")).toString();
            if (!stableId.isEmpty()) {
                realStableIds.append(stableId);
                realNames.append(name);
            }
        }

        if (!realStableIds.isEmpty()) {
            m_deviceManager->restoreAssignmentsFromStableIds(1, realStableIds, realNames);

            // Verify assignments
            assignedDevices = m_deviceManager->getDevicesForInstance(1);
            QCOMPARE(assignedDevices.size(), realStableIds.size());

            // Cleanup
            m_deviceManager->unassignAll();
        }
    }
}

void TestDeviceManager::testStableIdInDeviceVariantMap()
{
    // Verify that stableId is included in device variant map
    QVariantList devices = m_deviceManager->devicesAsVariant();
    if (!devices.isEmpty()) {
        QVariantMap device = devices.first().toMap();
        QVERIFY(device.contains(KEY("stableId")));

        // stableId should be a string
        QVERIFY(device.value(KEY("stableId")).canConvert<QString>());

        // Get device by eventNumber and verify stableId is present
        int eventNumber = device.value(KEY("eventNumber")).toInt();
        QVariantMap retrieved = m_deviceManager->getDevice(eventNumber);
        QVERIFY(retrieved.contains(KEY("stableId")));

        // stableIds should match
        QCOMPARE(device.value(KEY("stableId")).toString(), retrieved.value(KEY("stableId")).toString());
    }
}

void TestDeviceManager::testIgnoredDevices()
{
    // Create a SettingsManager
    SettingsManager settingsManager;
    m_deviceManager->setSettingsManager(&settingsManager);

    // Get initial device count
    m_deviceManager->refresh();
    QVariantList devices = m_deviceManager->devicesAsVariant();
    int initialCount = devices.size();

    if (initialCount > 0) {
        // Pick a device to ignore
        QVariantMap device = devices.first().toMap();
        QString stableId = device.value(KEY("stableId")).toString();

        if (!stableId.isEmpty()) {
            m_deviceManager->ignoreDevice(stableId);

            QVERIFY(settingsManager.ignoredDevices().contains(stableId));

            m_deviceManager->refresh();

            QVariantList newDevices = m_deviceManager->devicesAsVariant();
            QCOMPARE(newDevices.size(), initialCount - 1);

            for (const QVariant &v : newDevices) {
                QVariantMap d = v.toMap();
                QVERIFY(d.value(KEY("stableId")).toString() != stableId);
            }

            m_deviceManager->unignoreDevice(stableId);

            QVERIFY(!settingsManager.ignoredDevices().contains(stableId));

            m_deviceManager->refresh();

            QVariantList restoredDevices = m_deviceManager->devicesAsVariant();
            QCOMPARE(restoredDevices.size(), initialCount);
        }
    }

    m_deviceManager->setSettingsManager(nullptr);
}

void TestDeviceManager::testIsVirtualDevice()
{
    // Test standard cases
    QVERIFY(m_deviceManager->isVirtualDevice(QStringLiteral("Virtual Controller"), QString()));
    QVERIFY(m_deviceManager->isVirtualDevice(QStringLiteral("Standard Joystick"), QStringLiteral("/virtual/input0")));
    QVERIFY(!m_deviceManager->isVirtualDevice(QStringLiteral("Standard Controller"), QStringLiteral("/dev/input0")));

    // Test Bluetooth/USB devices with empty physical path (should NOT be virtual)
    QVERIFY(!m_deviceManager->isVirtualDevice(QStringLiteral("DualSense Wireless Controller"), QString(), QStringLiteral("0005")));
    QVERIFY(!m_deviceManager->isVirtualDevice(QStringLiteral("USB Controller"), QString(), QStringLiteral("0003")));

    // Test Steam-created virtual controllers emulating Xbox or PlayStation controllers (empty physical path, bus 0003)
    QVERIFY(m_deviceManager->isVirtualDevice(QStringLiteral("Microsoft X-Box 360 pad 0"), QString(), QStringLiteral("0003")));
    QVERIFY(m_deviceManager->isVirtualDevice(QStringLiteral("Sony Interactive Entertainment Wireless Controller"), QString(), QStringLiteral("0003")));

    // Test other devices with empty physical path (should be virtual)
    QVERIFY(m_deviceManager->isVirtualDevice(QStringLiteral("Keyboard"), QString(), QStringLiteral("0006")));
    QVERIFY(m_deviceManager->isVirtualDevice(QStringLiteral("Keyboard"), QString(), QString()));
}

class MockDeviceManager : public DeviceManager
{
public:
    explicit MockDeviceManager(QObject *parent = nullptr) : DeviceManager(parent) {}

    // Mock control flags/variables
    mutable bool mockIoctlSuccess = false;
    mutable QByteArray mockKeyBitmask;
    mutable QString expectedOpenedPath;
    mutable bool wasOpened = false;
    mutable QSet<int> mockConnectedSlots;

protected:
    int openDevice(const QString &path, int flags) const override
    {
        Q_UNUSED(flags);
        if (path == expectedOpenedPath) {
            wasOpened = true;
            return 999; // Return dummy fd for expected controller
        }
        if (path.contains(QStringLiteral("event"))) {
            return 888; // Return dummy fd for non-controller event files
        }
        // Avoid opening real devices in the unit test
        return -1;
    }

    int ioctlDevice(int fd, unsigned long request, void *arg) const override
    {
        Q_UNUSED(request);
        if (fd == 999) {
            if (mockIoctlSuccess) {
                int size = KEY_MAX / 8 + 1;
                unsigned char *bitmask = static_cast<unsigned char *>(arg);
                memset(bitmask, 0, size);
                int copySize = qMin(size, mockKeyBitmask.size());
                memcpy(bitmask, mockKeyBitmask.constData(), copySize);
                return 0;
            }
            return -1;
        }
        if (fd == 888) {
            // Return success but an empty bitmask (no buttons)
            int size = KEY_MAX / 8 + 1;
            unsigned char *bitmask = static_cast<unsigned char *>(arg);
            memset(bitmask, 0, size);
            return 0;
        }
        return -1;
    }

    void closeDevice(int fd) const override
    {
        Q_UNUSED(fd);
    }

    bool isSlotConnected(int eventNumber) const override
    {
        if (mockConnectedSlots.isEmpty()) {
            return true;
        }
        return mockConnectedSlots.contains(eventNumber);
    }
};

void TestDeviceManager::testSteamControllerDetection()
{
    // Create a temporary file to mock /proc/bus/input/devices
    QTemporaryFile mockDevicesFile;
    QVERIFY(mockDevicesFile.open());

    // Write a standard controller and a Steam Controller (in Lizard mode, no gamepad buttons)
    QTextStream out(&mockDevicesFile);
    
    // Write 4 slots of Steam Controller Puck (each has Mouse + Keyboard)
    // Slot 1
    out << "I: Bus=0003 Vendor=28de Product=1304 Version=0111\n";
    out << "N: Name=\"Valve Software Steam Controller Puck Mouse\"\n";
    out << "P: Phys=usb-0000:12:00.4-1/input2\n";
    out << "H: Handlers=event2 mouse0\n\n";

    out << "I: Bus=0003 Vendor=28de Product=1304 Version=0111\n";
    out << "N: Name=\"Valve Software Steam Controller Puck Keyboard\"\n";
    out << "P: Phys=usb-0000:12:00.4-1/input2\n";
    out << "H: Handlers=sysrq kbd event3\n\n";

    // Actual Gamepad Device (event10)
    out << "I: Bus=0003 Vendor=28de Product=1102 Version=0011\n";
    out << "N: Name=\"Valve Software Steam Controller\"\n";
    out << "P: Phys=usb-0000:12:00.4-1/input2/input0\n";
    out << "H: Handlers=event10 js0\n\n";

    // Slot 2
    out << "I: Bus=0003 Vendor=28de Product=1304 Version=0111\n";
    out << "N: Name=\"Valve Software Steam Controller Puck Mouse\"\n";
    out << "P: Phys=usb-0000:12:00.4-1/input3\n";
    out << "H: Handlers=event4 mouse1\n\n";

    out << "I: Bus=0003 Vendor=28de Product=1304 Version=0111\n";
    out << "N: Name=\"Valve Software Steam Controller Puck Keyboard\"\n";
    out << "P: Phys=usb-0000:12:00.4-1/input3\n";
    out << "H: Handlers=sysrq kbd event5\n\n";

    // Slot 3
    out << "I: Bus=0003 Vendor=28de Product=1304 Version=0111\n";
    out << "N: Name=\"Valve Software Steam Controller Puck Mouse\"\n";
    out << "P: Phys=usb-0000:12:00.4-1/input4\n";
    out << "H: Handlers=event6 mouse2\n\n";

    out << "I: Bus=0003 Vendor=28de Product=1304 Version=0111\n";
    out << "N: Name=\"Valve Software Steam Controller Puck Keyboard\"\n";
    out << "P: Phys=usb-0000:12:00.4-1/input4\n";
    out << "H: Handlers=sysrq kbd event7\n\n";

    // Slot 4
    out << "I: Bus=0003 Vendor=28de Product=1304 Version=0111\n";
    out << "N: Name=\"Valve Software Steam Controller Puck Mouse\"\n";
    out << "P: Phys=usb-0000:12:00.4-1/input5\n";
    out << "H: Handlers=event8 mouse3\n\n";

    out << "I: Bus=0003 Vendor=28de Product=1304 Version=0111\n";
    out << "N: Name=\"Valve Software Steam Controller Puck Keyboard\"\n";
    out << "P: Phys=usb-0000:12:00.4-1/input5\n";
    out << "H: Handlers=sysrq kbd event9\n\n";

    // Xbox Controller
    out << "I: Bus=0003 Vendor=045e Product=028e Version=0110\n";
    out << "N: Name=\"Microsoft X-Box 360 pad\"\n";
    out << "P: Phys=usb-0000:00:14.0-2/input0\n";
    out << "H: Handlers=event8888 js0\n\n";
    
    out.flush();
    mockDevicesFile.close();

    // Set mock env var
    qputenv("COUCHPLAY_MOCK_DEVICES_FILE", mockDevicesFile.fileName().toLocal8Bit());

    // Create MockDeviceManager
    MockDeviceManager mockManager;
    
    // Mock only Slot 1 connected, others disconnected
    mockManager.mockConnectedSlots = {2, 3, 10};

    // Set up mock responses:
    // For event8888 (Xbox pad), we mock ioctl to succeed and return gamepad buttons
    // So it should be detected as a controller
    mockManager.expectedOpenedPath = QStringLiteral("/dev/input/event8888");
    mockManager.mockIoctlSuccess = true;
    
    // BTN_GAMEPAD is 0x130 (304). Let's set the bit in mockKeyBitmask.
    // 304 / 8 = 38. So byte index 38 has bit 304 % 8 = 0 set.
    QByteArray xboxBitmask(KEY_MAX / 8 + 1, 0);
    xboxBitmask[38] = 0x01; // BTN_GAMEPAD (BTN_A)
    mockManager.mockKeyBitmask = xboxBitmask;

    mockManager.refresh();

    // Verify list size & types
    QVariantList controllers = mockManager.controllersAsVariant();
    
    // Should be exactly 2 controllers: 1 active Steam Controller (Slot 1) + 1 Xbox gamepad
    // The disconnected Steam Controller slots (2, 3, 4) should be hidden (classified as other)!
    QCOMPARE(controllers.size(), 2);

    int puckCount = 0;
    int xboxCount = 0;

    for (const QVariant &v : controllers) {
        QVariantMap dev = v.toMap();
        QString name = dev.value(KEY("name")).toString();
        if (name.contains(QStringLiteral("Steam Controller (Slot"))) {
            puckCount++;
            QCOMPARE(name, QStringLiteral("Steam Controller (Slot 1)"));
            QCOMPARE(dev.value(KEY("type")).toString(), QStringLiteral("controller"));
        } else if (name == QStringLiteral("Microsoft X-Box 360 pad")) {
            xboxCount++;
            QCOMPARE(dev.value(KEY("type")).toString(), QStringLiteral("controller"));
        }
    }

    QCOMPARE(puckCount, 1);
    QCOMPARE(xboxCount, 1);

    // Verify Sibling Assignment Propagation
    // Assign Puck Slot 1 Mouse (event2) to instance 0
    QVERIFY(mockManager.assignDevice(2, 0));

    // Verify event2 is assigned to 0
    QVariantMap mouseDev = mockManager.getDevice(2);
    QCOMPARE(mouseDev.value(KEY("assigned")).toBool(), true);
    QCOMPARE(mouseDev.value(KEY("assignedInstance")).toInt(), 0);

    // Verify event3 (Keyboard) was automatically assigned to 0 as well!
    QVariantMap kbdDev = mockManager.getDevice(3);
    QCOMPARE(kbdDev.value(KEY("assigned")).toBool(), true);
    QCOMPARE(kbdDev.value(KEY("assignedInstance")).toInt(), 0);

    // Verify event10 (Actual Gamepad) was automatically assigned to 0 as well!
    QVariantMap padDev = mockManager.getDevice(10);
    QCOMPARE(padDev.value(KEY("assigned")).toBool(), true);
    QCOMPARE(padDev.value(KEY("assignedInstance")).toInt(), 0);

    // Verify Slot 2 devices (event4, event5) remain unassigned!
    QVariantMap mouse2Dev = mockManager.getDevice(4);
    QCOMPARE(mouse2Dev.value(KEY("assigned")).toBool(), false);
    QVariantMap kbd2Dev = mockManager.getDevice(5);
    QCOMPARE(kbd2Dev.value(KEY("assigned")).toBool(), false);

    // Now assign Slot 2 Mouse (event4) to instance 1
    QVERIFY(mockManager.assignDevice(4, 1));

    // Verify Slot 2 devices are assigned to 1
    mouse2Dev = mockManager.getDevice(4);
    QCOMPARE(mouse2Dev.value(KEY("assigned")).toBool(), true);
    QCOMPARE(mouse2Dev.value(KEY("assignedInstance")).toInt(), 1);
    kbd2Dev = mockManager.getDevice(5);
    QCOMPARE(kbd2Dev.value(KEY("assigned")).toBool(), true);
    QCOMPARE(kbd2Dev.value(KEY("assignedInstance")).toInt(), 1);

    // Verify Slot 1 devices still assigned to 0
    mouseDev = mockManager.getDevice(2);
    QCOMPARE(mouseDev.value(KEY("assignedInstance")).toInt(), 0);
    kbdDev = mockManager.getDevice(3);
    QCOMPARE(kbdDev.value(KEY("assignedInstance")).toInt(), 0);
    padDev = mockManager.getDevice(10);
    QCOMPARE(padDev.value(KEY("assignedInstance")).toInt(), 0);

    // Unassign Puck Slot 1 Mouse (event2)
    QVERIFY(mockManager.assignDevice(2, -1));

    // Verify both are now unassigned
    mouseDev = mockManager.getDevice(2);
    QCOMPARE(mouseDev.value(KEY("assigned")).toBool(), false);
    QCOMPARE(mouseDev.value(KEY("assignedInstance")).toInt(), -1);

    kbdDev = mockManager.getDevice(3);
    QCOMPARE(kbdDev.value(KEY("assigned")).toBool(), false);
    QCOMPARE(kbdDev.value(KEY("assignedInstance")).toInt(), -1);

    // Verify event10 is now unassigned
    padDev = mockManager.getDevice(10);
    QCOMPARE(padDev.value(KEY("assigned")).toBool(), false);
    QCOMPARE(padDev.value(KEY("assignedInstance")).toInt(), -1);

    // Verify autoAssignControllers also assigns siblings
    mockManager.autoAssignControllers();

    // Verify event2 & event3 & event10 are assigned to same instance
    mouseDev = mockManager.getDevice(2);
    kbdDev = mockManager.getDevice(3);
    padDev = mockManager.getDevice(10);
    QCOMPARE(mouseDev.value(KEY("assigned")).toBool(), kbdDev.value(KEY("assigned")).toBool());
    QCOMPARE(mouseDev.value(KEY("assignedInstance")).toInt(), kbdDev.value(KEY("assignedInstance")).toInt());
    QCOMPARE(mouseDev.value(KEY("assigned")).toBool(), padDev.value(KEY("assigned")).toBool());
    QCOMPARE(mouseDev.value(KEY("assignedInstance")).toInt(), padDev.value(KEY("assignedInstance")).toInt());

    // Clean up env
    qunsetenv("COUCHPLAY_MOCK_DEVICES_FILE");
}

void TestDeviceManager::testPlayStationControllerGrouping()
{
    // Create a temporary file to mock /proc/bus/input/devices
    QTemporaryFile mockDevicesFile;
    QVERIFY(mockDevicesFile.open());

    QTextStream out(&mockDevicesFile);

    // DualSense Controller Sibling 1 (Gamepad)
    out << "I: Bus=0003 Vendor=054c Product=0ce6 Version=0111\n";
    out << "N: Name=\"Sony Interactive Entertainment Wireless Controller\"\n";
    out << "P: Phys=usb-0000:12:00.4-2/input0\n";
    out << "H: Handlers=event20 js0\n\n";

    // DualSense Controller Sibling 2 (Touchpad)
    out << "I: Bus=0003 Vendor=054c Product=0ce6 Version=0111\n";
    out << "N: Name=\"Sony Interactive Entertainment Wireless Controller Touchpad\"\n";
    out << "P: Phys=usb-0000:12:00.4-2/input1\n";
    out << "H: Handlers=event21 mouse1\n\n";

    // DualSense Controller Sibling 3 (Motion Sensors)
    out << "I: Bus=0003 Vendor=054c Product=0ce6 Version=0111\n";
    out << "N: Name=\"Sony Interactive Entertainment Wireless Controller Motion Sensors\"\n";
    out << "P: Phys=usb-0000:12:00.4-2/input2\n";
    out << "H: Handlers=event22 mouse2\n\n";

    out.flush();
    mockDevicesFile.close();

    qputenv("COUCHPLAY_MOCK_DEVICES_FILE", mockDevicesFile.fileName().toLocal8Bit());

    MockDeviceManager mockManager;
    
    // Mock ioctl success for event20 to return gamepad buttons
    mockManager.expectedOpenedPath = QStringLiteral("/dev/input/event20");
    mockManager.mockIoctlSuccess = true;
    QByteArray dsBitmask(KEY_MAX / 8 + 1, 0);
    dsBitmask[38] = 0x01; // BTN_GAMEPAD (BTN_A)
    mockManager.mockKeyBitmask = dsBitmask;

    mockManager.refresh();

    // Verify gamepad is detected as controller, touchpad & motion sensors as "other"
    QVariantList controllers = mockManager.controllersAsVariant();
    QCOMPARE(controllers.size(), 1);
    QCOMPARE(controllers.first().toMap().value(KEY("name")).toString(), QStringLiteral("Sony Interactive Entertainment Wireless Controller"));

    // Verify Sibling Assignment Propagation for PlayStation controller
    // Assign Sibling 1 (Gamepad, event20) to instance 1
    QVERIFY(mockManager.assignDevice(20, 1));

    // Verify event20 is assigned to 1
    QVariantMap padDev = mockManager.getDevice(20);
    QCOMPARE(padDev.value(KEY("assigned")).toBool(), true);
    QCOMPARE(padDev.value(KEY("assignedInstance")).toInt(), 1);

    // Verify event21 (Touchpad) was automatically assigned to 1 as well!
    QVariantMap touchDev = mockManager.getDevice(21);
    QCOMPARE(touchDev.value(KEY("assigned")).toBool(), true);
    QCOMPARE(touchDev.value(KEY("assignedInstance")).toInt(), 1);

    // Verify event22 (Motion Sensors) was automatically assigned to 1 as well!
    QVariantMap motionDev = mockManager.getDevice(22);
    QCOMPARE(motionDev.value(KEY("assigned")).toBool(), true);
    QCOMPARE(motionDev.value(KEY("assignedInstance")).toInt(), 1);

    // Unassign Gamepad (event20)
    QVERIFY(mockManager.assignDevice(20, -1));

    // Verify all are now unassigned
    padDev = mockManager.getDevice(20);
    touchDev = mockManager.getDevice(21);
    motionDev = mockManager.getDevice(22);
    QCOMPARE(padDev.value(KEY("assigned")).toBool(), false);
    QCOMPARE(touchDev.value(KEY("assigned")).toBool(), false);
    QCOMPARE(motionDev.value(KEY("assigned")).toBool(), false);

    // Clean up env
    qunsetenv("COUCHPLAY_MOCK_DEVICES_FILE");
}

void TestDeviceManager::testPlayStationBluetoothControllerGrouping()
{
    // Create a temporary file to mock /proc/bus/input/devices
    QTemporaryFile mockDevicesFile;
    QVERIFY(mockDevicesFile.open());

    QTextStream out(&mockDevicesFile);

    // DualSense Controller Sibling 1 (Gamepad) - Bluetooth (empty Phys path, MAC address in Uniq)
    out << "I: Bus=0005 Vendor=054c Product=0ce6 Version=0111\n";
    out << "N: Name=\"DualSense Wireless Controller\"\n";
    out << "P: Phys=\n";
    out << "U: Uniq=0c:27:56:57:e7:98\n";
    out << "H: Handlers=event20 js0\n\n";

    // DualSense Controller Sibling 2 (Touchpad) - Bluetooth
    out << "I: Bus=0005 Vendor=054c Product=0ce6 Version=0111\n";
    out << "N: Name=\"DualSense Wireless Controller Touchpad\"\n";
    out << "P: Phys=\n";
    out << "U: Uniq=0c:27:56:57:e7:98\n";
    out << "H: Handlers=event21 mouse1\n\n";

    // DualSense Controller Sibling 3 (Motion Sensors) - Bluetooth
    out << "I: Bus=0005 Vendor=054c Product=0ce6 Version=0111\n";
    out << "N: Name=\"DualSense Wireless Controller Motion Sensors\"\n";
    out << "P: Phys=\n";
    out << "U: Uniq=0c:27:56:57:e7:98\n";
    out << "H: Handlers=event22 mouse2\n\n";

    out.flush();
    mockDevicesFile.close();

    qputenv("COUCHPLAY_MOCK_DEVICES_FILE", mockDevicesFile.fileName().toLocal8Bit());

    MockDeviceManager mockManager;
    
    // Mock ioctl success for event20 to return gamepad buttons
    mockManager.expectedOpenedPath = QStringLiteral("/dev/input/event20");
    mockManager.mockIoctlSuccess = true;
    QByteArray dsBitmask(KEY_MAX / 8 + 1, 0);
    dsBitmask[38] = 0x01; // BTN_GAMEPAD (BTN_A)
    mockManager.mockKeyBitmask = dsBitmask;

    mockManager.refresh();

    // Verify gamepad is detected as controller, touchpad & motion sensors as "other"
    QVariantList controllers = mockManager.controllersAsVariant();
    QCOMPARE(controllers.size(), 1);
    QCOMPARE(controllers.first().toMap().value(KEY("name")).toString(), QStringLiteral("DualSense Wireless Controller"));

    // Verify Sibling Assignment Propagation for PlayStation controller
    // Assign Sibling 1 (Gamepad, event20) to instance 1
    QVERIFY(mockManager.assignDevice(20, 1));

    // Verify event20 is assigned to 1
    QVariantMap padDev = mockManager.getDevice(20);
    QCOMPARE(padDev.value(KEY("assigned")).toBool(), true);
    QCOMPARE(padDev.value(KEY("assignedInstance")).toInt(), 1);

    // Verify event21 (Touchpad) was automatically assigned to 1 as well via Uniq/MAC!
    QVariantMap touchDev = mockManager.getDevice(21);
    QCOMPARE(touchDev.value(KEY("assigned")).toBool(), true);
    QCOMPARE(touchDev.value(KEY("assignedInstance")).toInt(), 1);

    // Verify event22 (Motion Sensors) was automatically assigned to 1 as well!
    QVariantMap motionDev = mockManager.getDevice(22);
    QCOMPARE(motionDev.value(KEY("assigned")).toBool(), true);
    QCOMPARE(motionDev.value(KEY("assignedInstance")).toInt(), 1);

    // Unassign Gamepad (event20)
    QVERIFY(mockManager.assignDevice(20, -1));

    // Verify all are now unassigned
    padDev = mockManager.getDevice(20);
    touchDev = mockManager.getDevice(21);
    motionDev = mockManager.getDevice(22);
    QCOMPARE(padDev.value(KEY("assigned")).toBool(), false);
    QCOMPARE(touchDev.value(KEY("assigned")).toBool(), false);
    QCOMPARE(motionDev.value(KEY("assigned")).toBool(), false);

    // Clean up env
    qunsetenv("COUCHPLAY_MOCK_DEVICES_FILE");
}

QTEST_MAIN(TestDeviceManager)
#include "test_devicemanager.moc"
