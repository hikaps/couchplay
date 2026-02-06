// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <QTest>
#include <QSignalSpy>
#include <QTemporaryFile>
#include <QTemporaryDir>

#include "DeviceManager.h"
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
    // ... other slots ...
    
private:
    // Helper to create a fresh DeviceManager with mock data
    void createManager() {
        if (m_deviceManager) delete m_deviceManager;
        m_deviceManager = new DeviceManager(this);
        m_deviceManager->setInputPaths(m_mockInputDir.path(), m_mockDevicesFile);
    }
    
    DeviceManager *m_deviceManager = nullptr;
    QTemporaryDir m_mockInputDir;
    QString m_mockDevicesFile;
};

void TestDeviceManager::initTestCase()
{
    // Create mock devices file
    m_mockDevicesFile = QDir::currentPath() + "/mock_devices";
    QFile mockData(QFINDTESTDATA("data/mock_devices"));
    if (mockData.open(QIODevice::ReadOnly)) {
        QFile target(m_mockDevicesFile);
        if (target.open(QIODevice::WriteOnly)) {
            target.write(mockData.readAll());
            target.close();
        }
        mockData.close();
    } else {
        // Fallback if test data not found (e.g. running from wrong dir)
        // Write minimal mock data
        QFile target(m_mockDevicesFile);
        if (target.open(QIODevice::WriteOnly)) {
            target.write("I: Bus=0003 Vendor=045e Product=028e Version=0114\n");
            target.write("N: Name=\"Microsoft X-Box 360 pad\"\n");
            target.write("H: Handlers=event5 js0\n");
            target.write("\n");
            target.close();
        }
    }
}

void TestDeviceManager::cleanupTestCase()
{
    QFile::remove(m_mockDevicesFile);
}

void TestDeviceManager::testInitialization()
{
    m_deviceManager = new DeviceManager(this);
    
    // Create dummy event files in temp dir
    QFile(m_mockInputDir.filePath("event5")).open(QIODevice::WriteOnly);
    QFile(m_mockInputDir.filePath("event6")).open(QIODevice::WriteOnly);
    QFile(m_mockInputDir.filePath("event2")).open(QIODevice::WriteOnly);
    
    // Inject mock paths
    m_deviceManager->setInputPaths(m_mockInputDir.path(), m_mockDevicesFile);
    
    // DeviceManager should initialize with sensible defaults
    QVERIFY(m_deviceManager != nullptr);
    QCOMPARE(m_deviceManager->instanceCount(), 2);
    // ... rest of test
    
    delete m_deviceManager;
    m_deviceManager = nullptr;
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
    QCOMPARE(instanceDevices.size(), assignedCount);
    
    // Get devices for unassigned instance
    QList<int> emptyList = m_deviceManager->getDevicesForInstance(1);
    QCOMPARE(emptyList.size(), 0);
}

void TestDeviceManager::testGetDevicePathsForInstance()
{
    m_deviceManager->unassignAll();
    
    // Assign some devices
    QVariantList devices = m_deviceManager->devicesAsVariant();
    for (int i = 0; i < qMin(devices.size(), 2); ++i) {
        QVariantMap device = devices.at(i).toMap();
        int eventNumber = device.value(KEY("eventNumber")).toInt();
        m_deviceManager->assignDevice(eventNumber, 0);
    }
    
    // Get paths for instance 0
    QStringList paths = m_deviceManager->getDevicePathsForInstance(0);
    
    // Each path should start with /dev/input/event
    for (const QString &path : paths) {
        QVERIFY(path.startsWith(QStringLiteral("/dev/input/event")));
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
    QString stableId = DeviceManager::generateStableId(
        QStringLiteral("045e"), 
        QStringLiteral("028e"), 
        QStringLiteral("usb-0000:00:14.0-2.4/input0")
    );
    QCOMPARE(stableId, QStringLiteral("045e:028e:usb-0000:00:14.0-2.4/input0"));
    
    // Test with empty components - should return empty string
    QString emptyId = DeviceManager::generateStableId(QString(), QString(), QString());
    QVERIFY(emptyId.isEmpty());
    
    // Test with partial components - should still generate ID
    QString partialId = DeviceManager::generateStableId(
        QStringLiteral("045e"), 
        QString(), 
        QString()
    );
    QVERIFY(!partialId.isEmpty());
    QVERIFY(partialId.startsWith(QStringLiteral("045e:")));
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
    QCOMPARE(stableIds.size(), expectedStableIds.size());
    
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
        QCOMPARE(device.value(KEY("stableId")).toString(), 
                 retrieved.value(KEY("stableId")).toString());
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

QTEST_MAIN(TestDeviceManager)
#include "test_devicemanager.moc"
