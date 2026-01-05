// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <QTest>
#include <QSignalSpy>
#include <QGuiApplication>
#include <QScreen>

#include "MonitorManager.h"

class TestMonitorManager : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Basic tests
    void testCreation();
    void testMonitorCountMatchesQt();
    void testMonitorsAsVariantFormat();
    void testPrimaryMonitorFlag();
    void testRefreshEmitsSignal();
    void testDisplayStringFormat();

private:
    MonitorManager *m_manager = nullptr;
    bool m_hasDisplay = false;
};

void TestMonitorManager::initTestCase()
{
    // Check if we have a display available
    m_hasDisplay = QGuiApplication::screens().size() > 0;
}

void TestMonitorManager::cleanupTestCase()
{
}

void TestMonitorManager::init()
{
    m_manager = new MonitorManager();
}

void TestMonitorManager::cleanup()
{
    delete m_manager;
    m_manager = nullptr;
}

void TestMonitorManager::testCreation()
{
    // Should be able to create manager even without display
    QVERIFY(m_manager != nullptr);
}

void TestMonitorManager::testMonitorCountMatchesQt()
{
    if (!m_hasDisplay) {
        QSKIP("No display available for this test");
    }
    
    int qtScreenCount = QGuiApplication::screens().size();
    int managerCount = m_manager->monitorCount();
    
    QCOMPARE(managerCount, qtScreenCount);
}

void TestMonitorManager::testMonitorsAsVariantFormat()
{
    if (!m_hasDisplay) {
        QSKIP("No display available for this test");
    }
    
    QVariantList monitors = m_manager->monitorsAsVariant();
    
    if (monitors.isEmpty()) {
        QSKIP("No monitors detected");
    }
    
    // Check first monitor has all expected fields
    QVariantMap monitor = monitors.first().toMap();
    
    QVERIFY(monitor.contains(QStringLiteral("index")));
    QVERIFY(monitor.contains(QStringLiteral("name")));
    QVERIFY(monitor.contains(QStringLiteral("connector")));
    QVERIFY(monitor.contains(QStringLiteral("width")));
    QVERIFY(monitor.contains(QStringLiteral("height")));
    QVERIFY(monitor.contains(QStringLiteral("refreshRate")));
    QVERIFY(monitor.contains(QStringLiteral("primary")));
    QVERIFY(monitor.contains(QStringLiteral("displayString")));
    
    // Check values are reasonable
    int width = monitor[QStringLiteral("width")].toInt();
    int height = monitor[QStringLiteral("height")].toInt();
    int refreshRate = monitor[QStringLiteral("refreshRate")].toInt();
    
    QVERIFY(width > 0);
    QVERIFY(height > 0);
    QVERIFY(refreshRate > 0);
}

void TestMonitorManager::testPrimaryMonitorFlag()
{
    if (!m_hasDisplay) {
        QSKIP("No display available for this test");
    }
    
    QVariantList monitors = m_manager->monitorsAsVariant();
    
    if (monitors.isEmpty()) {
        QSKIP("No monitors detected");
    }
    
    // Exactly one monitor should be marked as primary
    int primaryCount = 0;
    for (const QVariant &v : monitors) {
        QVariantMap monitor = v.toMap();
        if (monitor[QStringLiteral("primary")].toBool()) {
            primaryCount++;
        }
    }
    
    QCOMPARE(primaryCount, 1);
}

void TestMonitorManager::testRefreshEmitsSignal()
{
    QSignalSpy spy(m_manager, &MonitorManager::monitorsChanged);
    
    m_manager->refresh();
    
    QCOMPARE(spy.count(), 1);
}

void TestMonitorManager::testDisplayStringFormat()
{
    if (!m_hasDisplay) {
        QSKIP("No display available for this test");
    }
    
    QVariantList monitors = m_manager->monitorsAsVariant();
    
    if (monitors.isEmpty()) {
        QSKIP("No monitors detected");
    }
    
    QVariantMap monitor = monitors.first().toMap();
    QString displayString = monitor[QStringLiteral("displayString")].toString();
    
    // Should contain resolution and refresh rate
    QVERIFY(displayString.contains(QStringLiteral("x"))); // e.g., 1920x1080
    QVERIFY(displayString.contains(QStringLiteral("Hz"))); // e.g., 60Hz
    
    // Format should be "Name (WxH @ RHz)"
    QVERIFY(displayString.contains(QStringLiteral("(")));
    QVERIFY(displayString.contains(QStringLiteral(")")));
    QVERIFY(displayString.contains(QStringLiteral("@")));
}

QTEST_MAIN(TestMonitorManager)
#include "test_monitormanager.moc"
