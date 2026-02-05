// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <QObject>
#include <QTest>
#include <QDebug>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>

#include "../src/core/CommandVerifier.h"
#include "../src/core/Logging.h"

class TestCommandVerifier : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testFlatpakDetection();
    void testUserLocalDetection();
    void testAccessibleToOtherUsers();
    void testPathResolution();
    void testAbsoluteValidation();
    void testNonExistentCommand();
    void testFlatpakAppDetection();
};

void TestCommandVerifier::testFlatpakDetection()
{
    // Test flatpak command detection
    QVERIFY(CommandVerifier::isFlatpakCommand(QStringLiteral("flatpak run com.heroicgameslauncher.heroic")));
    QVERIFY(CommandVerifier::isFlatpakCommand(QStringLiteral("flatpak run com.valvesoftware.Steam")));
    
    // Test non-flatpak commands
    QVERIFY(!CommandVerifier::isFlatpakCommand(QStringLiteral("steam")));
    QVERIFY(!CommandVerifier::isFlatpakCommand(QStringLiteral("heroic")));
    QVERIFY(!CommandVerifier::isFlatpakCommand(QStringLiteral("/usr/bin/steam")));
}

void TestCommandVerifier::testUserLocalDetection()
{
    QVERIFY(CommandVerifier::isUserLocalCommand(QStringLiteral("/home/user/.local/bin/steam")));
    QVERIFY(CommandVerifier::isUserLocalCommand(QStringLiteral("/home/user/.local/share/applications/steam.desktop")));
    QVERIFY(CommandVerifier::isUserLocalCommand(QStringLiteral("/home/user/.config/heroic/config.json")));
    
    QVERIFY(!CommandVerifier::isUserLocalCommand(QStringLiteral("/usr/bin/steam")));
    QVERIFY(!CommandVerifier::isUserLocalCommand(QStringLiteral("/opt/steam/steam")));
    QVERIFY(!CommandVerifier::isUserLocalCommand(QStringLiteral("/home/user/games/steam")));
}

void TestCommandVerifier::testAccessibleToOtherUsers()
{
    QTemporaryDir tempDir(QDir::homePath() + QStringLiteral("/.couchplay_test_XXXXXX"));
    QVERIFY(tempDir.isValid());
    QString localPath = tempDir.path() + QStringLiteral("/local-exec");

    QFile localFile(localPath);
    QVERIFY(localFile.open(QIODevice::WriteOnly));
    localFile.write("#!/bin/sh\n");
    localFile.close();
    QVERIFY(QFile::setPermissions(localPath, QFileDevice::ReadUser | QFileDevice::WriteUser | QFileDevice::ExeUser));

    CommandVerificationResult localResult = CommandVerifier::verifyCommand(localPath);
    QVERIFY(localResult.isValid);
    QVERIFY(localResult.isAbsolutePath);
    QVERIFY(!localResult.isAccessibleToOtherUsers);

    QTemporaryDir systemDir(QDir::tempPath() + QStringLiteral("/couchplay_test_XXXXXX"));
    QVERIFY(systemDir.isValid());
    QString systemPath = systemDir.path() + QStringLiteral("/system-exec");

    QFile systemFile(systemPath);
    QVERIFY(systemFile.open(QIODevice::WriteOnly));
    systemFile.write("#!/bin/sh\n");
    systemFile.close();
    QVERIFY(QFile::setPermissions(systemPath, QFileDevice::ReadUser | QFileDevice::WriteUser | QFileDevice::ExeUser));

    CommandVerificationResult systemResult = CommandVerifier::verifyCommand(systemPath);
    QVERIFY(systemResult.isValid);
    QVERIFY(systemResult.isAbsolutePath);
    QVERIFY(!systemResult.isAccessibleToOtherUsers);

    QVERIFY(QFile::setPermissions(systemPath, QFileDevice::ReadUser | QFileDevice::WriteUser | QFileDevice::ExeUser
                                              | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                                              | QFileDevice::ReadOther | QFileDevice::ExeOther));

    CommandVerificationResult sharedResult = CommandVerifier::verifyCommand(systemPath);
    QVERIFY(sharedResult.isValid);
    QVERIFY(sharedResult.isAbsolutePath);
    QVERIFY(sharedResult.isAccessibleToOtherUsers);
}

void TestCommandVerifier::testPathResolution()
{
    // Test PATH-based command resolution
    QString resolved = CommandVerifier::resolveCommandPath(QStringLiteral("ls"));
    qDebug() << "Resolved 'ls' to:" << resolved;
    // Resolution result depends on test environment PATH
    
    resolved = CommandVerifier::resolveCommandPath(QStringLiteral("/usr/bin/ls"));
    QCOMPARE(resolved, QStringLiteral("/usr/bin/ls")); // Should return unchanged for absolute paths
}

void TestCommandVerifier::testAbsoluteValidation()
{
    // Test absolute path validation using the correct method name
    QVERIFY(CommandVerifier::isAbsolutePath(QStringLiteral("/usr/bin/steam")));
    QVERIFY(CommandVerifier::isAbsolutePath(QStringLiteral("/home/user/.config/heroic/config.json")));
    QVERIFY(CommandVerifier::isAbsolutePath(QStringLiteral("/opt/games/game")));
    
    // Test invalid paths
    QVERIFY(!CommandVerifier::isAbsolutePath(QStringLiteral("steam"))); // Relative path
    QVERIFY(!CommandVerifier::isAbsolutePath(QStringLiteral("~/.local/bin/steam"))); // Tilde expansion
    QVERIFY(!CommandVerifier::isAbsolutePath(QStringLiteral(""))); // Empty string
}

void TestCommandVerifier::testNonExistentCommand()
{
    // Test non-existent command detection using the correct method name
    QVERIFY(!CommandVerifier::isCommandExecutable(QStringLiteral("/non/existent/path")));
    QVERIFY(!CommandVerifier::commandExistsInPath(QStringLiteral("nonexistentcommand12345")));
}

void TestCommandVerifier::testFlatpakAppDetection()
{
    QVERIFY(!CommandVerifier::isFlatpakAppInstalled(QStringLiteral("invalid_app_id")));
    QVERIFY(!CommandVerifier::isFlatpakAppInstalled(QStringLiteral("com.heroicgameslauncher.heroic.")));
    QVERIFY(!CommandVerifier::isFlatpakAppInstalled(QStringLiteral(".")));
    QVERIFY(!CommandVerifier::isFlatpakAppInstalled(QStringLiteral("")));
    QVERIFY(!CommandVerifier::isFlatpakAppInstalled(QStringLiteral("com")));
    QVERIFY(!CommandVerifier::isFlatpakAppInstalled(QStringLiteral("com.example")));
}

QTEST_MAIN(TestCommandVerifier)
#include "test_commandverifier.moc"
