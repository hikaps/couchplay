// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "CouchPlayHelperClient.h"

#include <QDBusConnection>
#include <QSignalSpy>
#include <memory>
#include <QTest>

namespace
{
const QString kService = QStringLiteral("io.github.hikaps.CouchPlayHelper");
const QString kPath = QStringLiteral("/io/github/hikaps/CouchPlayHelper");

/// Helper exposing GetVersionInfo(), like a current helper.
class VersionedHelper : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.hikaps.CouchPlayHelper")
public Q_SLOTS:
    QVariantMap GetVersionInfo()
    {
        return {{QStringLiteral("version"), QStringLiteral("9.9.9-test")}, {QStringLiteral("apiLevel"), 3}};
    }
};

/// Helper predating GetVersionInfo(): the interface exists but the method does not.
class LegacyHelper : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.hikaps.CouchPlayHelper")
public Q_SLOTS:
    bool IsInCouchPlayGroup(const QString &)
    {
        return false;
    }
};

class RequiringClient : public CouchPlayHelperClient
{
public:
    RequiringClient(const QDBusConnection &bus, int required)
        : CouchPlayHelperClient(bus)
        , m_required(required)
    {
    }
    int requiredHelperApiLevel() const override
    {
        return m_required;
    }

private:
    int m_required;
};
} // namespace

class TestCouchPlayHelperClient : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init();
    void cleanup();
    void testCurrentHelperReportsVersionAndLevel();
    void testOutdatedOnlyBelowRequiredLevel();
    void testLegacyHelperMapsToUnknown();
    void testLegacyHelperOutdatedWhenLevelRequired();
    void testUnavailableHelperIsNotOutdated();
    void testRefreshOnCheckAvailabilityNotifies();
    void testDefaultRequiredLevelAcceptsAnyHelper();

private:
    template<typename Helper> void exportHelper(Helper &helper)
    {
        QDBusConnection bus = QDBusConnection::sessionBus();
        QVERIFY(bus.registerObject(kPath, &helper, QDBusConnection::ExportAllSlots));
    }

    bool m_serviceRegistered = false;
};

void TestCouchPlayHelperClient::init()
{
    QVERIFY(QDBusConnection::sessionBus().isConnected());
    m_serviceRegistered = QDBusConnection::sessionBus().registerService(kService);
    QVERIFY(m_serviceRegistered);
}

void TestCouchPlayHelperClient::cleanup()
{
    QDBusConnection::sessionBus().unregisterObject(kPath);
    if (m_serviceRegistered) {
        QDBusConnection::sessionBus().unregisterService(kService);
        m_serviceRegistered = false;
    }
}

void TestCouchPlayHelperClient::testCurrentHelperReportsVersionAndLevel()
{
    VersionedHelper helper;
    exportHelper(helper);

    CouchPlayHelperClient client(QDBusConnection::sessionBus());
    QVERIFY(client.isAvailable());
    QCOMPARE(client.helperVersion(), QStringLiteral("9.9.9-test"));
    QCOMPARE(client.helperApiLevel(), 3);
}

void TestCouchPlayHelperClient::testOutdatedOnlyBelowRequiredLevel()
{
    VersionedHelper helper;
    exportHelper(helper);

    RequiringClient equal(QDBusConnection::sessionBus(), 3);
    QCOMPARE(equal.requiredHelperApiLevel(), 3);
    QVERIFY(!equal.helperOutdated());

    RequiringClient above(QDBusConnection::sessionBus(), 4);
    QVERIFY(above.helperOutdated());
}

void TestCouchPlayHelperClient::testLegacyHelperMapsToUnknown()
{
    LegacyHelper helper;
    exportHelper(helper);

    CouchPlayHelperClient client(QDBusConnection::sessionBus());
    QVERIFY(client.isAvailable());
    QVERIFY(client.helperVersion().isEmpty());
    QCOMPARE(client.helperApiLevel(), 0);
    QVERIFY(!client.helperOutdated());
}

void TestCouchPlayHelperClient::testLegacyHelperOutdatedWhenLevelRequired()
{
    LegacyHelper helper;
    exportHelper(helper);

    RequiringClient client(QDBusConnection::sessionBus(), 1);
    QVERIFY(client.helperVersion().isEmpty());
    QCOMPARE(client.helperApiLevel(), 0);
    QVERIFY(client.helperOutdated());
}

void TestCouchPlayHelperClient::testUnavailableHelperIsNotOutdated()
{
    // No service on the bus at all: the interface is invalid.
    QVERIFY(QDBusConnection::sessionBus().unregisterService(kService));
    m_serviceRegistered = false;
    RequiringClient client(QDBusConnection::sessionBus(), 1);
    QVERIFY(!client.isAvailable());
    QVERIFY(client.helperVersion().isEmpty());
    QCOMPARE(client.helperApiLevel(), 0);
    QVERIFY(!client.helperOutdated());
}

void TestCouchPlayHelperClient::testRefreshOnCheckAvailabilityNotifies()
{
    auto versioned = std::make_unique<VersionedHelper>();
    exportHelper(*versioned);

    CouchPlayHelperClient client(QDBusConnection::sessionBus());
    QCOMPARE(client.helperApiLevel(), 3);

    // Helper restarted as an older build.
    QDBusConnection::sessionBus().unregisterObject(kPath);
    versioned.reset();
    LegacyHelper legacy;
    exportHelper(legacy);

    QSignalSpy spy(&client, &CouchPlayHelperClient::helperInfoChanged);
    client.checkAvailability();
    QCOMPARE(spy.count(), 1);
    QVERIFY(client.helperVersion().isEmpty());
    QCOMPARE(client.helperApiLevel(), 0);

    // Unchanged info does not re-notify.
    client.checkAvailability();
    QCOMPARE(spy.count(), 1);
}

void TestCouchPlayHelperClient::testDefaultRequiredLevelAcceptsAnyHelper()
{
    CouchPlayHelperClient client(QDBusConnection::sessionBus());
    QCOMPARE(client.requiredHelperApiLevel(), 0);
    QVERIFY(!client.helperOutdated());
}

QTEST_MAIN(TestCouchPlayHelperClient)
#include "test_couchplayhelperclient.moc"
