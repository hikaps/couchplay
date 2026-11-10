// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include <QProcess>
#include <QSignalSpy>
#include <QTest>

#include "SettingsManager.h"
#include "UpdateChecker.h"

namespace
{
// Stands in for `flatpak-spawn --host flatpak update -y ...`
UpdateChecker::ProcessConfigurator shellRunner(const QString &script)
{
    return [script](QProcess &process) {
        process.setProgram(QStringLiteral("/bin/sh"));
        process.setArguments({QStringLiteral("-c"), script});
    };
}
}

class TestUpdateChecker : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testNormalizeVersion();
    void testCompareVersions();
    void testComparePrerelease();
    void testParseLatestTag();
    void testParseLatestTagRejectsDraftAndPrerelease();
    void testDetectInstallMethodMembership();
    void testInitialState();
    void testApplyLatestTagUpdateAvailable();
    void testApplyLatestTagUpToDate();
    void testApplyLatestTagTransitionsBack();
    void testAutoCheckSettingDefault();
    void testUpdateNowSuccess();
    void testUpdateNowNonZeroExit();
    void testUpdateNowFailedToStart();
    void testUpdateNowNotFlatpak();
    void testUpdateNowIgnoredWhileRunning();
};

void TestUpdateChecker::testNormalizeVersion()
{
    QCOMPARE(UpdateChecker::normalizeVersion(QStringLiteral("v0.4.1")), QStringLiteral("0.4.1"));
    QCOMPARE(UpdateChecker::normalizeVersion(QStringLiteral("V0.4.1")), QStringLiteral("0.4.1"));
    QCOMPARE(UpdateChecker::normalizeVersion(QStringLiteral("0.4.1")), QStringLiteral("0.4.1"));
    QCOMPARE(UpdateChecker::normalizeVersion(QStringLiteral("  v1.0.0\n")), QStringLiteral("1.0.0"));
}

void TestUpdateChecker::testCompareVersions()
{
    QCOMPARE(UpdateChecker::compareVersions(QStringLiteral("0.4.0"), QStringLiteral("0.4.0")), 0);
    QVERIFY(UpdateChecker::compareVersions(QStringLiteral("0.4.0"), QStringLiteral("0.4.1")) < 0);
    QVERIFY(UpdateChecker::compareVersions(QStringLiteral("v0.5.0"), QStringLiteral("0.4.9")) > 0);
    QCOMPARE(UpdateChecker::compareVersions(QStringLiteral("0.4"), QStringLiteral("0.4.0")), 0);
    QVERIFY(UpdateChecker::compareVersions(QStringLiteral("0.4"), QStringLiteral("0.4.1")) < 0);
    QVERIFY(UpdateChecker::compareVersions(QStringLiteral("1.0"), QStringLiteral("0.9")) > 0);
    // numeric, not lexicographic
    QVERIFY(UpdateChecker::compareVersions(QStringLiteral("0.10.0"), QStringLiteral("0.9.0")) > 0);
}

void TestUpdateChecker::testComparePrerelease()
{
    QVERIFY(UpdateChecker::compareVersions(QStringLiteral("0.7.0"), QStringLiteral("0.7.0-beta.1")) > 0);
    QVERIFY(UpdateChecker::compareVersions(QStringLiteral("0.7.0-beta.1"), QStringLiteral("0.7.0")) < 0);
    QVERIFY(UpdateChecker::compareVersions(QStringLiteral("0.7.1"), QStringLiteral("0.7.0-beta.1")) > 0);
    QCOMPARE(UpdateChecker::compareVersions(QStringLiteral("0.7.0-beta.1"), QStringLiteral("0.7.0-beta.1")), 0);
    QVERIFY(UpdateChecker::compareVersions(QStringLiteral("0.7.0-beta.1"), QStringLiteral("0.7.0-beta.2")) < 0);
}

void TestUpdateChecker::testParseLatestTag()
{
    QCOMPARE(UpdateChecker::parseLatestTag(QByteArrayLiteral("{\"tag_name\":\"v0.4.1\"}")), QStringLiteral("v0.4.1"));
    QCOMPARE(UpdateChecker::parseLatestTag(QByteArrayLiteral("{\"tag_name\":\"v0.4.1\",\"draft\":false,\"prerelease\":false}")),
             QStringLiteral("v0.4.1"));
    QVERIFY(UpdateChecker::parseLatestTag(QByteArrayLiteral("{}")).isEmpty());
    QVERIFY(UpdateChecker::parseLatestTag(QByteArrayLiteral("not json")).isEmpty());
    QVERIFY(UpdateChecker::parseLatestTag(QByteArrayLiteral("[]")).isEmpty());
    QVERIFY(UpdateChecker::parseLatestTag(QByteArrayLiteral("{\"tag_name\":3}")).isEmpty());
}

void TestUpdateChecker::testParseLatestTagRejectsDraftAndPrerelease()
{
    QVERIFY(UpdateChecker::parseLatestTag(QByteArrayLiteral("{\"tag_name\":\"v9.0.0\",\"prerelease\":true}")).isEmpty());
    QVERIFY(UpdateChecker::parseLatestTag(QByteArrayLiteral("{\"tag_name\":\"v9.0.0\",\"draft\":true}")).isEmpty());
}

void TestUpdateChecker::testDetectInstallMethodMembership()
{
    const QString method = UpdateChecker::detectInstallMethod();
    QVERIFY(method == QLatin1String("flatpak") || method == QLatin1String("steamos") || method == QLatin1String("native"));
}

void TestUpdateChecker::testInitialState()
{
    UpdateChecker checker(QStringLiteral("native"), shellRunner(QStringLiteral("exit 0")));

    QCOMPARE(checker.state(), UpdateChecker::Idle);
    QVERIFY(!checker.updateAvailable());
    QVERIFY(checker.latestVersion().isEmpty());
    QCOMPARE(checker.installMethod(), QStringLiteral("native"));
    QCOMPARE(checker.updateNowState(), UpdateChecker::UpdateNowIdle);
    QVERIFY(!checker.currentVersion().isEmpty());
}

void TestUpdateChecker::testApplyLatestTagUpdateAvailable()
{
    UpdateChecker checker;
    QSignalSpy stateSpy(&checker, &UpdateChecker::stateChanged);
    QSignalSpy availableSpy(&checker, &UpdateChecker::updateAvailableChanged);

    checker.applyLatestTag(QStringLiteral("v999.0.0"));

    QCOMPARE(checker.latestVersion(), QStringLiteral("999.0.0"));
    QVERIFY(checker.updateAvailable());
    QCOMPARE(checker.state(), UpdateChecker::UpdateAvailable);
    QCOMPARE(stateSpy.count(), 1);
    QCOMPARE(availableSpy.count(), 1);
}

void TestUpdateChecker::testApplyLatestTagUpToDate()
{
    UpdateChecker checker;

    checker.applyLatestTag(QStringLiteral("v0.0.0"));

    QCOMPARE(checker.state(), UpdateChecker::UpToDate);
    QVERIFY(!checker.updateAvailable());
}

void TestUpdateChecker::testApplyLatestTagTransitionsBack()
{
    UpdateChecker checker;
    checker.applyLatestTag(QStringLiteral("v999.0.0"));
    QSignalSpy availableSpy(&checker, &UpdateChecker::updateAvailableChanged);

    checker.applyLatestTag(checker.currentVersion());

    QCOMPARE(checker.state(), UpdateChecker::UpToDate);
    QVERIFY(!checker.updateAvailable());
    QCOMPARE(availableSpy.count(), 1);
}

void TestUpdateChecker::testAutoCheckSettingDefault()
{
    SettingsManager settings;
    QVERIFY(settings.checkForUpdatesAutomatically());
}

void TestUpdateChecker::testUpdateNowSuccess()
{
    UpdateChecker checker(QStringLiteral("flatpak"), shellRunner(QStringLiteral("echo 'Updating...'; echo Done; exit 0")));
    QSignalSpy stateSpy(&checker, &UpdateChecker::updateNowStateChanged);

    checker.updateNow();
    QCOMPARE(checker.updateNowState(), UpdateChecker::UpdateNowRunning);

    QTRY_COMPARE(checker.updateNowState(), UpdateChecker::UpdateNowSucceeded);
    QCOMPARE(checker.updateNowMessage(), QStringLiteral("Done"));
    QCOMPARE(stateSpy.count(), 2); // Running, Succeeded
}

void TestUpdateChecker::testUpdateNowNonZeroExit()
{
    UpdateChecker checker(QStringLiteral("flatpak"), shellRunner(QStringLiteral("echo 'error: no remote'; exit 3")));

    checker.updateNow();

    QTRY_COMPARE(checker.updateNowState(), UpdateChecker::UpdateNowFailed);
    QCOMPARE(checker.updateNowMessage(), QStringLiteral("error: no remote"));
}

void TestUpdateChecker::testUpdateNowFailedToStart()
{
    UpdateChecker checker(QStringLiteral("flatpak"), [](QProcess &process) {
        process.setProgram(QStringLiteral("/nonexistent/couchplay-test-binary"));
    });

    checker.updateNow();

    QTRY_COMPARE(checker.updateNowState(), UpdateChecker::UpdateNowFailed);
    QVERIFY(!checker.updateNowMessage().isEmpty());
}

void TestUpdateChecker::testUpdateNowNotFlatpak()
{
    bool runnerCalled = false;
    for (const QString &method : {QStringLiteral("native"), QStringLiteral("steamos")}) {
        UpdateChecker checker(method, [&runnerCalled](QProcess &) {
            runnerCalled = true;
        });

        checker.updateNow();

        QCOMPARE(checker.updateNowState(), UpdateChecker::UpdateNowFailed);
    }
    QVERIFY(!runnerCalled);
}

void TestUpdateChecker::testUpdateNowIgnoredWhileRunning()
{
    int starts = 0;
    UpdateChecker checker(QStringLiteral("flatpak"), [&starts](QProcess &process) {
        ++starts;
        process.setProgram(QStringLiteral("/bin/sh"));
        process.setArguments({QStringLiteral("-c"), QStringLiteral("sleep 0.2")});
    });

    checker.updateNow();
    checker.updateNow();

    QCOMPARE(starts, 1);
    QTRY_COMPARE(checker.updateNowState(), UpdateChecker::UpdateNowSucceeded);
}

QTEST_MAIN(TestUpdateChecker)
#include "test_updatechecker.moc"
