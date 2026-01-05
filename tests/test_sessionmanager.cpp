// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QDir>

#include "SessionManager.h"

// Helper macro for QVariantMap key access with proper QString conversion
#define KEY(x) QStringLiteral(x)

class TestSessionManager : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Basic functionality tests
    void testInitialization();
    void testNewSession();
    void testInstanceCount();
    void testCurrentLayout();
    
    // Instance configuration tests
    void testGetInstanceConfig();
    void testSetInstanceConfig();
    void testSetInstanceResolution();
    void testSetInstanceUser();
    
    // Profile management tests
    void testSaveProfile();
    void testLoadProfile();
    void testDeleteProfile();
    void testSavedProfiles();
    void testRefreshProfiles();
    
    // Signal tests
    void testInstanceCountChangedSignal();
    void testCurrentLayoutChangedSignal();
    void testProfilesChangedSignal();
    
    // User assignment tests
    void testGetAssignedUsers();

private:
    SessionManager *m_sessionManager = nullptr;
    QTemporaryDir *m_tempDir = nullptr;
};

void TestSessionManager::initTestCase()
{
    // Create a temporary directory for test profiles
    m_tempDir = new QTemporaryDir();
    QVERIFY(m_tempDir->isValid());
}

void TestSessionManager::cleanupTestCase()
{
    delete m_tempDir;
    m_tempDir = nullptr;
}

void TestSessionManager::init()
{
    m_sessionManager = new SessionManager(this);
}

void TestSessionManager::cleanup()
{
    delete m_sessionManager;
    m_sessionManager = nullptr;
}

void TestSessionManager::testInitialization()
{
    QVERIFY(m_sessionManager != nullptr);
    QCOMPARE(m_sessionManager->instanceCount(), 2);
    QCOMPARE(m_sessionManager->currentLayout(), QStringLiteral("horizontal"));
    QVERIFY(m_sessionManager->currentProfileName().isEmpty());
}

void TestSessionManager::testNewSession()
{
    // First, set some values
    m_sessionManager->setInstanceCount(3);
    m_sessionManager->setCurrentLayout(QStringLiteral("vertical"));
    
    // Now start a new session
    m_sessionManager->newSession();
    
    // Should reset to defaults
    QCOMPARE(m_sessionManager->instanceCount(), 2);
    QCOMPARE(m_sessionManager->currentLayout(), QStringLiteral("horizontal"));
    QVERIFY(m_sessionManager->currentProfileName().isEmpty());
}

void TestSessionManager::testInstanceCount()
{
    QSignalSpy spy(m_sessionManager, &SessionManager::instanceCountChanged);
    
    m_sessionManager->setInstanceCount(3);
    QCOMPARE(m_sessionManager->instanceCount(), 3);
    QCOMPARE(spy.count(), 1);
    
    // Test bounds
    m_sessionManager->setInstanceCount(4);
    QCOMPARE(m_sessionManager->instanceCount(), 4);
    
    m_sessionManager->setInstanceCount(1);
    QVERIFY(m_sessionManager->instanceCount() >= 2); // Minimum should be 2
}

void TestSessionManager::testCurrentLayout()
{
    QSignalSpy spy(m_sessionManager, &SessionManager::currentLayoutChanged);
    
    m_sessionManager->setCurrentLayout(QStringLiteral("vertical"));
    QCOMPARE(m_sessionManager->currentLayout(), QStringLiteral("vertical"));
    QCOMPARE(spy.count(), 1);
    
    m_sessionManager->setCurrentLayout(QStringLiteral("grid"));
    QCOMPARE(m_sessionManager->currentLayout(), QStringLiteral("grid"));
    
    m_sessionManager->setCurrentLayout(QStringLiteral("multi-monitor"));
    QCOMPARE(m_sessionManager->currentLayout(), QStringLiteral("multi-monitor"));
}

void TestSessionManager::testGetInstanceConfig()
{
    // Get config for instance 0
    QVariantMap config = m_sessionManager->getInstanceConfig(0);
    
    // Should have default values
    QVERIFY(config.contains(KEY("internalWidth")));
    QVERIFY(config.contains(KEY("internalHeight")));
    QVERIFY(config.contains(KEY("refreshRate")));
    
    // Invalid instance should return empty config
    QVariantMap invalidConfig = m_sessionManager->getInstanceConfig(-1);
    QVERIFY(invalidConfig.isEmpty());
    
    QVariantMap outOfBoundsConfig = m_sessionManager->getInstanceConfig(10);
    QVERIFY(outOfBoundsConfig.isEmpty());
}

void TestSessionManager::testSetInstanceConfig()
{
    QVariantMap config;
    config.insert(KEY("internalWidth"), 1280);
    config.insert(KEY("internalHeight"), 720);
    config.insert(KEY("refreshRate"), 120);
    
    m_sessionManager->setInstanceConfig(0, config);
    
    QVariantMap retrieved = m_sessionManager->getInstanceConfig(0);
    QCOMPARE(retrieved.value(KEY("internalWidth")).toInt(), 1280);
    QCOMPARE(retrieved.value(KEY("internalHeight")).toInt(), 720);
    QCOMPARE(retrieved.value(KEY("refreshRate")).toInt(), 120);
}

void TestSessionManager::testSetInstanceResolution()
{
    m_sessionManager->setInstanceResolution(0, 2560, 1440, 1920, 1080);
    
    QVariantMap config = m_sessionManager->getInstanceConfig(0);
    QCOMPARE(config.value(KEY("internalWidth")).toInt(), 2560);
    QCOMPARE(config.value(KEY("internalHeight")).toInt(), 1440);
    QCOMPARE(config.value(KEY("outputWidth")).toInt(), 1920);
    QCOMPARE(config.value(KEY("outputHeight")).toInt(), 1080);
}

void TestSessionManager::testSetInstanceUser()
{
    m_sessionManager->setInstanceUser(1, QStringLiteral("player2"));
    
    QVariantMap config = m_sessionManager->getInstanceConfig(1);
    QCOMPARE(config.value(KEY("username")).toString(), QStringLiteral("player2"));
    
    // Empty username should clear the user
    m_sessionManager->setInstanceUser(1, QString());
    config = m_sessionManager->getInstanceConfig(1);
    QVERIFY(config.value(KEY("username")).toString().isEmpty());
}

void TestSessionManager::testSaveProfile()
{
    QSignalSpy spy(m_sessionManager, &SessionManager::savedProfilesChanged);
    
    // Configure session
    m_sessionManager->setInstanceCount(3);
    m_sessionManager->setCurrentLayout(QStringLiteral("grid"));
    
    // Save profile
    bool result = m_sessionManager->saveProfile(QStringLiteral("TestProfile"));
    QCOMPARE(result, true);
    QCOMPARE(spy.count(), 1);
    
    // Current profile name should be updated
    QCOMPARE(m_sessionManager->currentProfileName(), QStringLiteral("TestProfile"));
}

void TestSessionManager::testLoadProfile()
{
    // First save a profile
    m_sessionManager->setInstanceCount(4);
    m_sessionManager->setCurrentLayout(QStringLiteral("vertical"));
    m_sessionManager->saveProfile(QStringLiteral("LoadTestProfile"));
    
    // Reset session
    m_sessionManager->newSession();
    QCOMPARE(m_sessionManager->instanceCount(), 2);
    
    // Load the profile
    bool result = m_sessionManager->loadProfile(QStringLiteral("LoadTestProfile"));
    QCOMPARE(result, true);
    QCOMPARE(m_sessionManager->instanceCount(), 4);
    QCOMPARE(m_sessionManager->currentLayout(), QStringLiteral("vertical"));
    QCOMPARE(m_sessionManager->currentProfileName(), QStringLiteral("LoadTestProfile"));
    
    // Loading non-existent profile should fail
    result = m_sessionManager->loadProfile(QStringLiteral("NonExistentProfile"));
    QCOMPARE(result, false);
}

void TestSessionManager::testDeleteProfile()
{
    // Save a profile first
    m_sessionManager->saveProfile(QStringLiteral("DeleteTestProfile"));
    
    QSignalSpy spy(m_sessionManager, &SessionManager::savedProfilesChanged);
    
    // Delete it
    bool result = m_sessionManager->deleteProfile(QStringLiteral("DeleteTestProfile"));
    QCOMPARE(result, true);
    QCOMPARE(spy.count(), 1);
    
    // Current profile should be cleared if it was the deleted one
    QVERIFY(m_sessionManager->currentProfileName().isEmpty());
    
    // Deleting non-existent profile should fail
    result = m_sessionManager->deleteProfile(QStringLiteral("NonExistentProfile"));
    QCOMPARE(result, false);
}

void TestSessionManager::testSavedProfiles()
{
    // Clear existing profiles first
    QList<SessionProfile> profiles = m_sessionManager->savedProfiles();
    for (const SessionProfile &profile : profiles) {
        m_sessionManager->deleteProfile(profile.name);
    }
    
    // Initially should be empty
    profiles = m_sessionManager->savedProfiles();
    int initialCount = profiles.size();
    
    // Save some profiles
    m_sessionManager->saveProfile(QStringLiteral("Profile1"));
    m_sessionManager->saveProfile(QStringLiteral("Profile2"));
    
    profiles = m_sessionManager->savedProfiles();
    QCOMPARE(profiles.size(), initialCount + 2);
    
    // Profiles should contain expected data
    bool foundProfile1 = false;
    bool foundProfile2 = false;
    for (const SessionProfile &profile : profiles) {
        if (profile.name == QStringLiteral("Profile1")) {
            foundProfile1 = true;
        }
        if (profile.name == QStringLiteral("Profile2")) {
            foundProfile2 = true;
        }
    }
    QVERIFY(foundProfile1);
    QVERIFY(foundProfile2);
}

void TestSessionManager::testRefreshProfiles()
{
    QSignalSpy spy(m_sessionManager, &SessionManager::savedProfilesChanged);
    
    m_sessionManager->refreshProfiles();
    
    // Should emit savedProfilesChanged
    QCOMPARE(spy.count(), 1);
}

void TestSessionManager::testInstanceCountChangedSignal()
{
    QSignalSpy spy(m_sessionManager, &SessionManager::instanceCountChanged);
    
    m_sessionManager->setInstanceCount(3);
    QCOMPARE(spy.count(), 1);
    
    // Setting same value shouldn't emit
    m_sessionManager->setInstanceCount(3);
    QCOMPARE(spy.count(), 1);
}

void TestSessionManager::testCurrentLayoutChangedSignal()
{
    QSignalSpy spy(m_sessionManager, &SessionManager::currentLayoutChanged);
    
    m_sessionManager->setCurrentLayout(QStringLiteral("grid"));
    QCOMPARE(spy.count(), 1);
    
    // Setting same value shouldn't emit
    m_sessionManager->setCurrentLayout(QStringLiteral("grid"));
    QCOMPARE(spy.count(), 1);
}

void TestSessionManager::testProfilesChangedSignal()
{
    QSignalSpy spy(m_sessionManager, &SessionManager::savedProfilesChanged);
    
    m_sessionManager->saveProfile(QStringLiteral("SignalTestProfile"));
    QCOMPARE(spy.count(), 1);
    
    m_sessionManager->deleteProfile(QStringLiteral("SignalTestProfile"));
    QCOMPARE(spy.count(), 2);
}

void TestSessionManager::testGetAssignedUsers()
{
    m_sessionManager->newSession();
    m_sessionManager->setInstanceCount(3);
    
    // Set users for instances
    m_sessionManager->setInstanceUser(0, QString());  // Primary user (empty string)
    m_sessionManager->setInstanceUser(1, QStringLiteral("player2"));
    m_sessionManager->setInstanceUser(2, QStringLiteral("player3"));
    
    // Get assigned users excluding index 0 - should return player2 and player3
    QStringList assigned = m_sessionManager->getAssignedUsers(0);
    QCOMPARE(assigned.size(), 2);
    QVERIFY(assigned.contains(QStringLiteral("player2")));
    QVERIFY(assigned.contains(QStringLiteral("player3")));
    
    // Get assigned users excluding index 1 - should return only player3
    assigned = m_sessionManager->getAssignedUsers(1);
    QCOMPARE(assigned.size(), 1);
    QVERIFY(assigned.contains(QStringLiteral("player3")));
    
    // Get assigned users excluding index 2 - should return only player2
    assigned = m_sessionManager->getAssignedUsers(2);
    QCOMPARE(assigned.size(), 1);
    QVERIFY(assigned.contains(QStringLiteral("player2")));
    
    // Empty usernames should not be included
    m_sessionManager->setInstanceUser(1, QString());
    assigned = m_sessionManager->getAssignedUsers(0);
    QCOMPARE(assigned.size(), 1);
    QVERIFY(assigned.contains(QStringLiteral("player3")));
}

QTEST_MAIN(TestSessionManager)
#include "test_sessionmanager.moc"
