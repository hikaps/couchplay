// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include <QSignalSpy>
#include <QTest>

#include "SettingsManager.h"
#include "UpdateChecker.h"

class TestUpdateChecker : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testNormalizeVersion();
    void testCompareVersions();
    void testParseLatestTag();
    void testDetectInstallMethodMembership();
    void testApplyLatestTagUpdateAvailable();
    void testApplyLatestTagUpToDate();
    void testAutoCheckSettingDefault();
};

void TestUpdateChecker::testNormalizeVersion()
{
    QCOMPARE(UpdateChecker::normalizeVersion(QStringLiteral("v0.4.1")), QStringLiteral("0.4.1"));
    QCOMPARE(UpdateChecker::normalizeVersion(QStringLiteral("V0.4.1")), QStringLiteral("0.4.1"));
    QCOMPARE(UpdateChecker::normalizeVersion(QStringLiteral("0.4.1")), QStringLiteral("0.4.1"));
}

void TestUpdateChecker::testCompareVersions()
{
    QCOMPARE(UpdateChecker::compareVersions(QStringLiteral("0.4.0"), QStringLiteral("0.4.0")), 0);
    QVERIFY(UpdateChecker::compareVersions(QStringLiteral("0.4.0"), QStringLiteral("0.4.1")) < 0);
    QVERIFY(UpdateChecker::compareVersions(QStringLiteral("v0.5.0"), QStringLiteral("0.4.9")) > 0);
    QCOMPARE(UpdateChecker::compareVersions(QStringLiteral("0.4"), QStringLiteral("0.4.0")), 0);
    QVERIFY(UpdateChecker::compareVersions(QStringLiteral("0.4"), QStringLiteral("0.4.1")) < 0);
    QVERIFY(UpdateChecker::compareVersions(QStringLiteral("1.0"), QStringLiteral("0.9")) > 0);
}

void TestUpdateChecker::testParseLatestTag()
{
    QCOMPARE(UpdateChecker::parseLatestTag(QByteArrayLiteral("{\"tag_name\":\"v0.4.1\"}")), QStringLiteral("v0.4.1"));
    QVERIFY(UpdateChecker::parseLatestTag(QByteArrayLiteral("{}")).isEmpty());
    QVERIFY(UpdateChecker::parseLatestTag(QByteArrayLiteral("not json")).isEmpty());
    QVERIFY(UpdateChecker::parseLatestTag(QByteArrayLiteral("{\"tag_name\":3}")).isEmpty());
}

void TestUpdateChecker::testDetectInstallMethodMembership()
{
    const QString method = UpdateChecker::detectInstallMethod();
    QVERIFY(method == QLatin1String("flatpak") || method == QLatin1String("steamos") || method == QLatin1String("native"));
}

void TestUpdateChecker::testApplyLatestTagUpdateAvailable()
{
    UpdateChecker checker;
    QSignalSpy stateSpy(&checker, &UpdateChecker::stateChanged);

    checker.applyLatestTag(QStringLiteral("v999.0.0"));

    QCOMPARE(checker.latestVersion(), QStringLiteral("999.0.0"));
    QVERIFY(checker.updateAvailable());
    QCOMPARE(checker.state(), UpdateChecker::UpdateAvailable);
    QVERIFY(stateSpy.count() >= 1);
}

void TestUpdateChecker::testApplyLatestTagUpToDate()
{
    UpdateChecker checker;

    checker.applyLatestTag(QStringLiteral("v0.0.0"));

    QCOMPARE(checker.state(), UpdateChecker::UpToDate);
    QVERIFY(!checker.updateAvailable());
}

void TestUpdateChecker::testAutoCheckSettingDefault()
{
    SettingsManager settings;
    QVERIFY(settings.checkForUpdatesAutomatically());
}

QTEST_MAIN(TestUpdateChecker)
#include "test_updatechecker.moc"
