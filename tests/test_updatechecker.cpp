// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "SettingsManager.h"
#include "UpdateChecker.h"

namespace
{
// Stands in for `flatpak-spawn --host /bin/sh -c <script> sh <tag>`
UpdateChecker::ProcessConfigurator shellRunner(const QString &script)
{
    return [script](QProcess &process, const QString &) {
        process.setProgram(QStringLiteral("/bin/sh"));
        process.setArguments({QStringLiteral("-c"), script});
    };
}

// Runs the real update script against stub curl/sha256sum/flatpak executables that log their arguments.
class ScriptEnv
{
public:
    ScriptEnv()
    {
        if (!m_root.isValid()) {
            return;
        }
        QDir root(m_root.path());
        m_valid = root.mkpath(QStringLiteral("bin")) && root.mkpath(QStringLiteral("tmp"));
        m_bin = root.filePath(QStringLiteral("bin"));
        m_tmp = root.filePath(QStringLiteral("tmp"));
        m_log = root.filePath(QStringLiteral("stub.log"));

        writeStub(QStringLiteral("curl"),
                  "echo \"curl $*\" >> \"$STUB_LOG\"\n"
                  "out=\n"
                  "while [ $# -gt 0 ]; do\n"
                  "    if [ \"$1\" = -o ]; then out=$2; shift; fi\n"
                  "    shift\n"
                  "done\n"
                  "printf data > \"$out\"\n");
        writeStub(QStringLiteral("sha256sum"),
                  "echo \"sha256sum $*\" >> \"$STUB_LOG\"\n"
                  "[ \"$STUB_SHA_OK\" = 1 ]\n");
        writeStub(QStringLiteral("flatpak"),
                  "if [ \"$1\" = list ]; then\n"
                  "    printf 'io.github.hikaps.couchplay\\t%s\\n' \"$STUB_INST\"\n"
                  "    exit 0\n"
                  "fi\n"
                  "echo \"flatpak $*\" >> \"$STUB_LOG\"\n"
                  "[ \"$STUB_INSTALL_FAIL\" != 1 ]\n");
    }

    bool isValid() const
    {
        return m_valid;
    }

    UpdateChecker::ProcessConfigurator runner(bool checksumOk, const QString &installation, bool installFails = false) const
    {
        const QString bin = m_bin;
        const QString tmp = m_tmp;
        const QString log = m_log;
        return [=](QProcess &process, const QString &tag) {
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert(QStringLiteral("PATH"), bin + QStringLiteral(":/usr/bin:/bin"));
            env.insert(QStringLiteral("TMPDIR"), tmp);
            env.insert(QStringLiteral("STUB_LOG"), log);
            env.insert(QStringLiteral("STUB_SHA_OK"), checksumOk ? QStringLiteral("1") : QStringLiteral("0"));
            env.insert(QStringLiteral("STUB_INST"), installation);
            env.insert(QStringLiteral("STUB_INSTALL_FAIL"), installFails ? QStringLiteral("1") : QStringLiteral("0"));
            process.setProcessEnvironment(env);
            process.setProgram(QStringLiteral("/bin/sh"));
            process.setArguments({QStringLiteral("-c"), UpdateChecker::updateScript(), QStringLiteral("sh"), tag});
        };
    }

    QString log() const
    {
        QFile file(m_log);
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
    }

    bool tmpIsEmpty() const
    {
        return QDir(m_tmp).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty();
    }

private:
    void writeStub(const QString &name, const char *body)
    {
        QFile file(QDir(m_bin).filePath(name));
        if (!file.open(QIODevice::WriteOnly)) {
            m_valid = false;
            return;
        }
        file.write("#!/bin/sh\n");
        file.write(body);
        file.close();
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }

    QTemporaryDir m_root;
    QString m_bin;
    QString m_tmp;
    QString m_log;
    bool m_valid = false;
};
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
    void testUpdateNowPassesTag();
    void testUpdateNowNonZeroExit();
    void testUpdateNowFailedToStart();
    void testUpdateNowNotFlatpak();
    void testUpdateNowRefusedWithoutValidUpdate();
    void testUpdateNowIgnoredWhileRunning();
    void testIsValidReleaseTag();
    void testUpdateScriptIsFixedText();
    void testUpdateScriptInstallsBundle();
    void testUpdateScriptUsesSystemInstallation();
    void testUpdateScriptChecksumFailure();
    void testUpdateScriptInstallFailure();
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
    UpdateChecker checker(QStringLiteral("flatpak"), shellRunner(QStringLiteral("echo 'Installing...'; echo Done; exit 0")));
    checker.applyLatestTag(QStringLiteral("v999.0.0"));
    QSignalSpy stateSpy(&checker, &UpdateChecker::updateNowStateChanged);

    checker.updateNow();
    QCOMPARE(checker.updateNowState(), UpdateChecker::UpdateNowRunning);

    QTRY_COMPARE(checker.updateNowState(), UpdateChecker::UpdateNowSucceeded);
    QCOMPARE(checker.updateNowMessage(), QStringLiteral("Done"));
    QCOMPARE(stateSpy.count(), 2); // Running, Succeeded
}

void TestUpdateChecker::testUpdateNowPassesTag()
{
    QString seenTag;
    UpdateChecker checker(QStringLiteral("flatpak"), [&seenTag](QProcess &process, const QString &tag) {
        seenTag = tag;
        process.setProgram(QStringLiteral("/bin/sh"));
        process.setArguments({QStringLiteral("-c"), QStringLiteral("exit 0")});
    });
    checker.applyLatestTag(QStringLiteral("v999.0.1"));

    checker.updateNow();

    QCOMPARE(seenTag, QStringLiteral("v999.0.1"));
    QTRY_COMPARE(checker.updateNowState(), UpdateChecker::UpdateNowSucceeded);
}

void TestUpdateChecker::testUpdateNowNonZeroExit()
{
    UpdateChecker checker(QStringLiteral("flatpak"), shellRunner(QStringLiteral("echo 'error: no remote'; exit 3")));
    checker.applyLatestTag(QStringLiteral("v999.0.0"));

    checker.updateNow();

    QTRY_COMPARE(checker.updateNowState(), UpdateChecker::UpdateNowFailed);
    QCOMPARE(checker.updateNowMessage(), QStringLiteral("error: no remote"));
}

void TestUpdateChecker::testUpdateNowFailedToStart()
{
    UpdateChecker checker(QStringLiteral("flatpak"), [](QProcess &process, const QString &) {
        process.setProgram(QStringLiteral("/nonexistent/couchplay-test-binary"));
    });
    checker.applyLatestTag(QStringLiteral("v999.0.0"));

    checker.updateNow();

    QTRY_COMPARE(checker.updateNowState(), UpdateChecker::UpdateNowFailed);
    QVERIFY(!checker.updateNowMessage().isEmpty());
}

void TestUpdateChecker::testUpdateNowNotFlatpak()
{
    bool runnerCalled = false;
    for (const QString &method : {QStringLiteral("native"), QStringLiteral("steamos")}) {
        UpdateChecker checker(method, [&runnerCalled](QProcess &, const QString &) {
            runnerCalled = true;
        });
        checker.applyLatestTag(QStringLiteral("v999.0.0"));

        checker.updateNow();

        QCOMPARE(checker.updateNowState(), UpdateChecker::UpdateNowFailed);
    }
    QVERIFY(!runnerCalled);
}

void TestUpdateChecker::testUpdateNowRefusedWithoutValidUpdate()
{
    bool runnerCalled = false;
    UpdateChecker checker(QStringLiteral("flatpak"), [&runnerCalled](QProcess &, const QString &) {
        runnerCalled = true;
    });

    // no update known yet
    checker.updateNow();
    QCOMPARE(checker.updateNowState(), UpdateChecker::UpdateNowFailed);
    QVERIFY(!checker.updateNowMessage().isEmpty());

    // up to date
    checker.applyLatestTag(QStringLiteral("v0.0.0"));
    checker.updateNow();
    QCOMPARE(checker.updateNowState(), UpdateChecker::UpdateNowFailed);

    // hostile tag that is not a plain version
    checker.applyLatestTag(QStringLiteral("v999.0.0; touch /tmp/couchplay-pwned"));
    checker.updateNow();
    QCOMPARE(checker.updateNowState(), UpdateChecker::UpdateNowFailed);

    QVERIFY(!runnerCalled);
}

void TestUpdateChecker::testUpdateNowIgnoredWhileRunning()
{
    int starts = 0;
    UpdateChecker checker(QStringLiteral("flatpak"), [&starts](QProcess &process, const QString &) {
        ++starts;
        process.setProgram(QStringLiteral("/bin/sh"));
        process.setArguments({QStringLiteral("-c"), QStringLiteral("sleep 0.2")});
    });
    checker.applyLatestTag(QStringLiteral("v999.0.0"));

    checker.updateNow();
    checker.updateNow();

    QCOMPARE(starts, 1);
    QTRY_COMPARE(checker.updateNowState(), UpdateChecker::UpdateNowSucceeded);
}

void TestUpdateChecker::testIsValidReleaseTag()
{
    QVERIFY(UpdateChecker::isValidReleaseTag(QStringLiteral("v0.7.0")));
    QVERIFY(UpdateChecker::isValidReleaseTag(QStringLiteral("0.7")));
    QVERIFY(UpdateChecker::isValidReleaseTag(QStringLiteral("v12")));
    QVERIFY(!UpdateChecker::isValidReleaseTag(QString()));
    QVERIFY(!UpdateChecker::isValidReleaseTag(QStringLiteral("v")));
    QVERIFY(!UpdateChecker::isValidReleaseTag(QStringLiteral("v0.7.0-beta.1")));
    QVERIFY(!UpdateChecker::isValidReleaseTag(QStringLiteral("v0.7.0\n")));
    QVERIFY(!UpdateChecker::isValidReleaseTag(QStringLiteral("v1.0; rm -rf /")));
    QVERIFY(!UpdateChecker::isValidReleaseTag(QStringLiteral("../v1.0")));
}

void TestUpdateChecker::testUpdateScriptIsFixedText()
{
    const QString script = UpdateChecker::updateScript();
    QVERIFY(script.contains(QStringLiteral("releases/download/$tag")));
    QVERIFY(!script.contains(QStringLiteral("999")));
    QVERIFY(script.contains(QStringLiteral("--retry 3")));
    QVERIFY(script.contains(QStringLiteral("sha256sum -c")));
    QVERIFY(script.contains(QStringLiteral("flatpak install -y --noninteractive")));
}

void TestUpdateChecker::testUpdateScriptInstallsBundle()
{
    ScriptEnv env;
    QVERIFY(env.isValid());
    UpdateChecker checker(QStringLiteral("flatpak"), env.runner(true, QStringLiteral("user")));
    checker.applyLatestTag(QStringLiteral("v999.0.0"));

    checker.updateNow();

    QTRY_COMPARE(checker.updateNowState(), UpdateChecker::UpdateNowSucceeded);
    QCOMPARE(checker.updateNowMessage(), QStringLiteral("Update installed."));
    const QString log = env.log();
    QVERIFY(log.contains(QStringLiteral("https://github.com/hikaps/couchplay/releases/download/v999.0.0/couchplay.flatpak\n")));
    QVERIFY(log.contains(QStringLiteral("releases/download/v999.0.0/couchplay.flatpak.sha256")));
    QVERIFY(log.contains(QStringLiteral("--retry 3")));
    QVERIFY(log.contains(QStringLiteral("sha256sum -c couchplay.flatpak.sha256")));
    QVERIFY(log.contains(QStringLiteral("flatpak install -y --noninteractive --user --bundle couchplay.flatpak")));
    QVERIFY(env.tmpIsEmpty());
}

void TestUpdateChecker::testUpdateScriptUsesSystemInstallation()
{
    ScriptEnv env;
    QVERIFY(env.isValid());
    UpdateChecker checker(QStringLiteral("flatpak"), env.runner(true, QStringLiteral("system")));
    checker.applyLatestTag(QStringLiteral("v999.0.0"));

    checker.updateNow();

    QTRY_COMPARE(checker.updateNowState(), UpdateChecker::UpdateNowSucceeded);
    QVERIFY(env.log().contains(QStringLiteral("flatpak install -y --noninteractive --system --bundle couchplay.flatpak")));
    QVERIFY(env.tmpIsEmpty());
}

void TestUpdateChecker::testUpdateScriptChecksumFailure()
{
    ScriptEnv env;
    QVERIFY(env.isValid());
    UpdateChecker checker(QStringLiteral("flatpak"), env.runner(false, QStringLiteral("user")));
    checker.applyLatestTag(QStringLiteral("v999.0.0"));

    checker.updateNow();

    QTRY_COMPARE(checker.updateNowState(), UpdateChecker::UpdateNowFailed);
    QVERIFY(checker.updateNowMessage().startsWith(QStringLiteral("Checksum verification failed")));
    QVERIFY(!env.log().contains(QStringLiteral("flatpak install")));
    QVERIFY(env.tmpIsEmpty());
}

void TestUpdateChecker::testUpdateScriptInstallFailure()
{
    ScriptEnv env;
    QVERIFY(env.isValid());
    UpdateChecker checker(QStringLiteral("flatpak"), env.runner(true, QStringLiteral("user"), true));
    checker.applyLatestTag(QStringLiteral("v999.0.0"));

    checker.updateNow();

    QTRY_COMPARE(checker.updateNowState(), UpdateChecker::UpdateNowFailed);
    QVERIFY(checker.updateNowMessage().startsWith(QStringLiteral("Installing failed")));
    QVERIFY(env.tmpIsEmpty());
}

QTEST_MAIN(TestUpdateChecker)
#include "test_updatechecker.moc"
