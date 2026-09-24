// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <cerrno>
#include <functional>
#include <utility>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QProcess>
#include <QSignalSpy>
#include <QVariantMap>
#include <QTest>

#define private public
#include "../helper/CouchPlayHelper.h"
#undef private
#include "../helper/SecureFs.h"
#include "../helper/SystemOps.h"
#include "../helper/MountSpec.h"

#include <fcntl.h>
#include <linux/fs.h>
#include <stdio.h>
#include <unistd.h>

class MockSystemOps : public SystemOps
{
public:
    MockSystemOps() = default;
    ~MockSystemOps() override = default;
    void setBeforeNextRenameAt(std::function<void()> callback)
    {
        m_beforeNextRenameAt = std::move(callback);
    }
    void setAfterFirstExchange(std::function<void()> callback)
    {
        m_afterFirstExchange = std::move(callback);
    }

    void setAuthResult(bool authorized)
    {
        m_authorized = authorized;
    }
    void setProcessExitCode(int exitCode)
    {
        m_processExitCode = exitCode;
    }
    void setMockProcessStart(bool mock)
    {
        m_mockProcessStart = mock;
    }
    void setStandardOutput(const QByteArray &output)
    {
        m_standardOutput = output;
    }
    void setStandardError(const QByteArray &output)
    {
        m_standardError = output;
    }
    void truncateOnNextRead(const QString &path)
    {
        m_truncateOnNextRead = path;
    }
    void replaceOnNextRead(const QString &path)
    {
        m_replaceOnNextRead = path;
    }
    void
    setUserExists(const QString &username, bool exists, uint uid = 0, gid_t gid = 0, const QString &home = QString())
    {
        if (exists) {
            struct passwd pw;
            QByteArray usernameBytes = username.toLocal8Bit();
            QByteArray homeBytes = home.toLocal8Bit();
            m_pwEntries[username] = {pw, usernameBytes, homeBytes, uid, gid, home};
        } else {
            m_pwEntries.remove(username);
        }
    }

    void setGroupExists(const QString &groupname, bool exists, gid_t gid = 0, const QStringList &members = {})
    {
        if (exists) {
            struct group gr;
            QByteArray groupBytes = groupname.toLocal8Bit();
            m_grEntries[groupname] = {gr, groupBytes, gid, members};
        } else {
            m_grEntries.remove(groupname);
        }
    }
    void setFileExists(const QString &path, bool exists)
    {
        m_files[path] = exists;
    }
    void setEntryList(const QString &path, const QStringList &entries)
    {
        m_entryLists[path] = entries;
    }
    void setDirectoryExists(const QString &path, bool exists)
    {
        m_directories[path] = exists;
    }
    void setSymlink(const QString &path, const QString &canonicalTarget)
    {
        m_files[path] = true;
        m_symlinks[path] = canonicalTarget;
    }
    void setCanonicalMapping(const QString &path, const QString &canonical)
    {
        m_canonical[path] = canonical;
    }
    void setChownResult(int result)
    {
        m_chownResult = result;
    }
    void setChmodResult(int result)
    {
        m_chmodResult = result;
    }

    void clear()
    {
        m_pwEntries.clear();
        m_grEntries.clear();
        m_files.clear();
        m_directories.clear();
        m_symlinks.clear();
        m_canonical.clear();
        m_entryLists.clear();
        m_authorized = true;
        m_processExitCode = 0;
        m_chownResult = 0;
        m_chmodResult = 0;
        m_processArgs.clear();
        m_processInvocations.clear();
        m_mockProcessStart = false;
        m_standardOutput.clear();
        m_standardError.clear();
        m_beforeNextRenameAt = {};
        m_afterFirstExchange = {};
        m_replaceOnNextRead.clear();

    }
    QStringList getLastProcessArgs() const
    {
        return m_processArgs;
    }
    QString getLastProcessCommand() const
    {
        return m_processCommand;
    }

    struct ProcessInvocation {
        QString command;
        QStringList args;
    };
    QList<ProcessInvocation> m_processInvocations;

    struct passwd *getpwnam(const char *name) override
    {
        QString username = QString::fromLocal8Bit(name);
        if (m_pwEntries.contains(username)) {
            auto &entry = m_pwEntries[username];
            entry.updatePointers();
            return &entry.pw;
        }
        return nullptr;
    }

    struct passwd *getpwuid(uid_t uid) override
    {
        for (auto &entry : m_pwEntries) {
            if (entry.uid == uid) {
                entry.updatePointers();
                return &entry.pw;
            }
        }
        return nullptr;
    }

    struct group *getgrnam(const char *name) override
    {
        QString groupname = QString::fromLocal8Bit(name);
        if (m_grEntries.contains(groupname)) {
            auto &entry = m_grEntries[groupname];
            entry.updatePointers();
            return &entry.gr;
        }
        return nullptr;
    }

    uid_t connectionUnixUser(const QString &busName) override
    {
        Q_UNUSED(busName)
        return 1000;
    }
    bool fileExists(const QString &path) override
    {
        return m_files.value(path, false);
    }

    bool isDirectory(const QString &path) override
    {
        return m_directories.value(path, false);
    }

    bool isSymLink(const QString &path) override
    {
        return m_symlinks.contains(path);
    }

    QString canonicalFilePath(const QString &path) override
    {
        return m_canonical.value(path, path);
    }

    bool mkpath(const QString &path) override
    {
        Q_UNUSED(path)
        return true;
    }

    bool copyFileSecure(const QString &source,
                        const QString &dest,
                        uid_t sourceOwner,
                        uid_t destOwner,
                        gid_t destGroup) override
    {
        Q_UNUSED(source)
        Q_UNUSED(dest)
        Q_UNUSED(sourceOwner)
        Q_UNUSED(destOwner)
        Q_UNUSED(destGroup)
        return true;
    }

    bool createDirectorySecure(const QString &path, uid_t owner, gid_t group) override
    {
        Q_UNUSED(path)
        Q_UNUSED(owner)
        Q_UNUSED(group)
        return true;
    }
    bool removeFile(const QString &path) override
    {
        Q_UNUSED(path)
        return true;
    }

    bool copyFile(const QString &source, const QString &dest) override
    {
        Q_UNUSED(source)
        Q_UNUSED(dest)
        return true;
    }

    bool writeFile(const QString &path, const QByteArray &content) override
    {
        Q_UNUSED(path)
        Q_UNUSED(content)
        return true;
    }

    bool statPath(const QString &path, struct stat *buf) override
    {
        buf->st_mode = S_IFREG | 0644;
        buf->st_uid = 1000;
        buf->st_gid = 1000;

        return true;
    }

    bool isCharDevice(mode_t mode) override
    {
        Q_UNUSED(mode)
        return true;
    }

    int chown(const QString &path, uid_t owner, gid_t group) override
    {
        Q_UNUSED(path)
        Q_UNUSED(owner)
        Q_UNUSED(group)
        return m_chownResult;
    }

    int chmod(const QString &path, mode_t mode) override
    {
        Q_UNUSED(path)
        Q_UNUSED(mode)
        return m_chmodResult;
    }

    QProcess *createProcess(QObject *parent = nullptr) override
    {
        return new QProcess(parent);
    }

    void startProcess(QProcess *process, const QString &program, const QStringList &arguments) override
    {
        m_processCommand = program;
        m_processArgs = arguments;
        m_processInvocations.append({program, arguments});
        if (!m_mockProcessStart) {
            process->start(program, arguments);
        }
    }

    bool waitForFinished(QProcess *process, int msecs) override
    {
        Q_UNUSED(process)
        Q_UNUSED(msecs)
        if (m_mockProcessStart) {
            return true;
        }
        return process->waitForFinished(msecs);
    }

    int processExitCode(QProcess *process) override
    {
        Q_UNUSED(process)
        return m_processExitCode;
    }

    QByteArray readStandardError(QProcess *process) override
    {
        Q_UNUSED(process)
        return m_standardError;
    }

    QByteArray readAllStandardOutput(QProcess *process) override
    {
        Q_UNUSED(process)
        return m_standardOutput;
    }

    ssize_t read(int fd, void *buffer, size_t count) override
    {
        if (!m_truncateOnNextRead.isEmpty()) {
            const QByteArray path = m_truncateOnNextRead.toLocal8Bit();
            m_truncateOnNextRead.clear();
            ::truncate(path.constData(), 1);
        }
        if (!m_replaceOnNextRead.isEmpty()) {
            const QString path = m_replaceOnNextRead;
            m_replaceOnNextRead.clear();
            const QByteArray pathBytes = path.toLocal8Bit();
            const QByteArray oldPathBytes = (path + QStringLiteral(".original")).toLocal8Bit();
            ::unlink(oldPathBytes.constData());
            if (::rename(pathBytes.constData(), oldPathBytes.constData()) != 0) {
                return -1;
            }
            QFile replacement(path);
            if (!replacement.open(QIODevice::WriteOnly)) {
                return -1;
            }
            if (replacement.write("pathname replacement") < 0) {
                replacement.close();
                return -1;
            }
            replacement.close();
        }
        return ::read(fd, buffer, count);
    }

    int renameAt(int oldDirFd, const char *oldPath, int newDirFd, const char *newPath, unsigned int flags) override
    {
        if (m_beforeNextRenameAt) {
            const auto callback = m_beforeNextRenameAt;
            m_beforeNextRenameAt = {};
            callback();
        }
        const int result = SystemOps::renameAt(oldDirFd, oldPath, newDirFd, newPath, flags);
        if (result == 0 && flags == RENAME_EXCHANGE && m_afterFirstExchange) {
            const auto callback = m_afterFirstExchange;
            m_afterFirstExchange = {};
            callback();
        }
        return result;
    }

    QStringList entryList(const QString &path, const QStringList &nameFilters, QDir::Filters filters) override
    {
        Q_UNUSED(nameFilters)
        Q_UNUSED(filters)
        return m_entryLists.value(path);
    }

    bool killProcess(pid_t pid, int signal) override
    {
        Q_UNUSED(pid)
        Q_UNUSED(signal)
        return true;
    }

    bool checkAuthorization(const QString &action, const QString &callerBusName) override
    {
        Q_UNUSED(action)
        Q_UNUSED(callerBusName)
        return m_authorized;
    }

private:
    struct PwEntry {
        struct passwd pw;
        QByteArray usernameBytes;
        QByteArray homeBytes;
        uint uid;
        gid_t gid;
        QString home;

        void updatePointers()
        {
            pw.pw_name = usernameBytes.data();
            pw.pw_dir = homeBytes.data();
            pw.pw_uid = uid;
            pw.pw_gid = gid;
            pw.pw_shell = const_cast<char *>("/bin/bash");
            pw.pw_passwd = const_cast<char *>("x");
        }
    };

    struct GrEntry {
        struct group gr;
        QByteArray groupBytes;
        gid_t gid;
        QStringList members;

        void updatePointers()
        {
            gr.gr_name = groupBytes.data();
            gr.gr_gid = gid;

            m_memberPtrs.clear();
            m_memberStrings.clear();
            for (const QString &member : members) {
                m_memberStrings.append(member.toLocal8Bit());
                m_memberPtrs.append(m_memberStrings.last().data());
            }
            m_memberPtrs.append(nullptr);
            gr.gr_mem = m_memberPtrs.data();
        }

        QList<QByteArray> m_memberStrings;
        QList<char *> m_memberPtrs;
    };

    QMap<QString, PwEntry> m_pwEntries;
    QMap<QString, GrEntry> m_grEntries;
    QMap<QString, bool> m_files;
    QMap<QString, bool> m_directories;
    QMap<QString, QString> m_symlinks;
    QMap<QString, QString> m_canonical;
    std::function<void()> m_beforeNextRenameAt;
    std::function<void()> m_afterFirstExchange;
    bool m_authorized = true;
    QMap<QString, QStringList> m_entryLists;
    int m_processExitCode = 0;
    int m_chownResult = 0;
    int m_chmodResult = 0;
    QString m_processCommand;
    QStringList m_processArgs;

    bool m_mockProcessStart = false;
    QByteArray m_standardOutput;
    QByteArray m_standardError;
    QString m_truncateOnNextRead;
    QString m_replaceOnNextRead;
};

class TestCouchPlayHelper : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // User management tests
    void testCreateUserSuccess();
    void testCreateUserInvalidUsername();
    void testCreateUserAlreadyExists();
    void testCreateUserAuthDenied();
    void testCreateUserProcessFailure();
    void testCreateUserLingerFailure();
    void testCreateUserWithoutInputGroup();
    void testCreateUserWithInputGroup();
    void testDeleteUserSuccess();
    void testDeleteUserInvalidUsername();
    void testDeleteUserNonexistent();
    void testDeleteUserAuthDenied();
    void testDeleteUserNotInCouchPlayGroup();
    void testDeleteUserProcessFailure();
    void testIsInCouchPlayGroupTrue();
    void testIsInCouchPlayGroupFalse();
    void testIsInCouchPlayGroupNonexistent();
    void testIsInCouchPlayGroupNonexistentGroup();
    void testEnableLingerSuccess();
    void testEnableLingerInvalidUsername();
    void testEnableLingerNonexistent();
    void testEnableLingerAuthDenied();
    void testEnableLingerProcessFailure();
    void testIsLingerEnabledTrue();
    void testIsLingerEnabledFalse();
    void testListCouchPlayUsers();
    void testListCouchPlayUsersFilters();
    void testGetUserInfo();
    void testGetUserInfoNotFound();
    void testIsSteamBootstrappedTrue();
    void testIsSteamBootstrappedFalse();
    void testGetUserSteamIdSelectsMostRecentAccount();
    void testGetUserSteamIdAmbiguousAccounts();
    void testGetUserSteamIdRejectsDuplicateMostRecentMarkers();
    void testGetUserSteamIdPrefersMostRecentWithoutUserdata();
    void testGetUserSteamIdDoesNotFallbackForUnsafeLoginUsers();
    void testReadSteamShortcutsForUser();
    void testWriteSteamShortcutsRejectsConcurrentEdit();
    void testReadSteamLibraryFoldersSecurely();
    void testReadSteamShortcutsMissingSteamDirectories();
    // Copy directory tests
    void testCopyDirectoryToUserAbsoluteTarget();
    void testCopyDirectoryToUserTraversalTarget();
    void testCopyDirectoryToUserSourceOutsideAllowedPrefixes();
    void testCopyDirectoryToUserSourceNotExists();
    void testCopyDirectoryToUserSuccessReplacesAndChowns();
    void testCopyDirectoryToUserPreservesTargetOnFailedCopy();
    void testCopyDirectoryToUserTargetSwapPreservesExternal();
    void testCopyDirectoryToUserDotDotNameAccepted();
    void testCopyDirectoryToUserSymlinkedTargetRejected();
    void testCopyDirectoryToUserSourceSymlinkResolvesOutside();
    void testSetupOverlayMountSymlinkedTargetRejected();
    void testMirrorDirectoryContentsSuccess();
    void testMirrorDirectoryContentsReplacesExistingSymlink();
    void testMirrorDirectoryContentsSymlinkVsNonEmptyDirFails();
    void testMirrorDirectoryContentsTargetNotExists();
    void testMirrorDirectoryContentsTraversalTarget();
    void testIsPathWithinAllowedPrefixMountRoots();
    void testComputeMountTargetDotDotNames();
    void testUnmountRetainsFailedMounts();
    void testMountSpecCodec();
    void testSetPathAclWithParentsSymlinkEscapeRejected();
    void testSetDirectoryAclSymlinkEscapeRejected();
    void testSecureWriteRejectsSymlinkLeaf();

    // Device ownership tests
    void testChangeDeviceOwnerInvalidPathNotUnderDevInput();
    void testChangeDeviceOwnerInvalidPathTraversal();
    void testChangeDeviceOwnerInvalidPathNotExists();
    void testChangeDeviceOwnerInvalidPathNotCharDevice();
    void testChangeDeviceOwnerSuccess();
    void testChangeDeviceOwnerAuthorizationDenied();
    void testChangeDeviceOwnerUserNotFound();
    void testChangeDeviceOwnerChownFails();
    void testChangeDeviceOwnerChmodFails();
    void testResetDeviceOwnerSuccess();
    void testResetDeviceOwnerInvalidPathNotUnderDevInput();
    void testResetDeviceOwnerInvalidPathTraversal();
    void testResetDeviceOwnerAuthorizationDenied();
    void testResetDeviceOwnerChownFails();
    void testResetDeviceOwnerChmodFails();
    void testResetAllDevicesEmpty();
    void testResetAllDevicesSuccess();
    void testResetAllDevicesPartialFailure();
    void testChangeDeviceOwnerBatchEmpty();
    void testChangeDeviceOwnerBatchAllSuccess();
    void testChangeDeviceOwnerBatchPartialFailure();
    void testChangeDeviceOwnerBatchAllFailure();

    // Runtime access tests (Polkit + D-Bus integration)
    void testSetupRuntimeAccessSuccess();
    void testSetupRuntimeAccessAuthorizationDenied();
    void testSetupRuntimeAccessUserNotFound();
    void testRemoveRuntimeAccessSuccess();
    void testRemoveRuntimeAccessAuthorizationDenied();
    void testRemoveRuntimeAccessUserNotFound();

    // Launch/Stop/Kill instance tests
    void testGenerateServiceName();
    void testLaunchInstance_basicLaunch();
    void testLaunchInstance_withBindPaths();
    void testLaunchInstance_validationEmptyUsername();
    void testLaunchInstance_validationNonexistentUser();
    void testStopInstance_serviceStop();
    void testKillInstance_serviceKill();
    void testStopInstance_fallbackToDirectKill();
    void testLaunchInstance_staleUnitRecovery();

private:
    CouchPlayHelper *m_helper = nullptr;
    MockSystemOps *m_ops = nullptr;
    QString m_serviceName;
    QString m_objectPath;
    QDBusInterface *m_dbusInterface = nullptr;
};

void TestCouchPlayHelper::initTestCase()
{
    m_serviceName = QStringLiteral("io.github.hikaps.CouchPlayHelper.Test");
    m_objectPath = QStringLiteral("/io/github/hikaps/CouchPlayHelper");

    m_ops = new MockSystemOps();
    m_ops->clear();

    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});

    m_helper = new CouchPlayHelper(m_ops);

    if (!QDBusConnection::sessionBus().registerObject(m_objectPath,
                                                      m_helper,
                                                      QDBusConnection::ExportAllSlots
                                                          | QDBusConnection::ExportAllSignals)) {
        qFatal("Failed to register CouchPlayHelper on session bus");
    }

    if (!QDBusConnection::sessionBus().registerService(m_serviceName)) {
        qFatal("Failed to register service name %s on session bus", qUtf8Printable(m_serviceName));
    }
}

void TestCouchPlayHelper::cleanupTestCase()
{
    QDBusConnection::sessionBus().unregisterObject(m_objectPath);
    QDBusConnection::sessionBus().unregisterService(m_serviceName);

    delete m_dbusInterface;
    delete m_helper;
    delete m_ops;
    m_dbusInterface = nullptr;
    m_helper = nullptr;
    m_ops = nullptr;
}

void TestCouchPlayHelper::init()
{
    m_dbusInterface = new QDBusInterface(m_serviceName,
                                         m_objectPath,
                                         QStringLiteral("io.github.hikaps.CouchPlayHelper"),
                                         QDBusConnection::sessionBus());

    if (!m_dbusInterface->isValid()) {
        qFatal("Failed to create QDBusInterface: %s", qUtf8Printable(m_dbusInterface->lastError().message()));
    }
}

void TestCouchPlayHelper::cleanup()
{
    delete m_dbusInterface;
    m_dbusInterface = nullptr;
    m_ops->clear();
}

void TestCouchPlayHelper::testCreateUserSuccess()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setUserExists(QStringLiteral("testuser1"), true, 1002, 1002);
    m_ops->setProcessExitCode(0);

    QDBusReply<uint> reply =
        m_dbusInterface->call(QStringLiteral("CreateUser"), QStringLiteral("testuser1"), QStringLiteral("Test User 1"));

    // User already exists, so should fail with "already exists"
    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::Failed);
    QVERIFY(reply.error().message().contains(QStringLiteral("already exists")));
}

void TestCouchPlayHelper::testCreateUserInvalidUsername()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});

    QDBusReply<uint> reply = m_dbusInterface->call(QStringLiteral("CreateUser"),
                                                   QStringLiteral("INVALID-USER"), // Invalid: uppercase and hyphen
                                                   QStringLiteral("Invalid User"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testCreateUserAlreadyExists()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setUserExists(QStringLiteral("existinguser"), true, 1002, 1002);

    QDBusReply<uint> reply = m_dbusInterface->call(QStringLiteral("CreateUser"),
                                                   QStringLiteral("existinguser"),
                                                   QStringLiteral("Existing User"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::Failed);
    QVERIFY(reply.error().message().contains(QStringLiteral("already exists")));
}

void TestCouchPlayHelper::testCreateUserAuthDenied()
{
    m_ops->clear();
    m_ops->setAuthResult(false);
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});

    QDBusReply<uint> reply =
        m_dbusInterface->call(QStringLiteral("CreateUser"), QStringLiteral("testuser"), QStringLiteral("Test User"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::AccessDenied);
}

void TestCouchPlayHelper::testCreateUserProcessFailure()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setProcessExitCode(1);

    QDBusReply<uint> reply =
        m_dbusInterface->call(QStringLiteral("CreateUser"), QStringLiteral("testuser"), QStringLiteral("Test User"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::Failed);
}

void TestCouchPlayHelper::testCreateUserLingerFailure()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setUserExists(QStringLiteral("testuser"), true, 1002, 1002);
    m_ops->setProcessExitCode(0);

    QDBusReply<uint> reply =
        m_dbusInterface->call(QStringLiteral("CreateUser"), QStringLiteral("testuser"), QStringLiteral("Test User"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::Failed);
}

void TestCouchPlayHelper::testCreateUserWithoutInputGroup()
{
    // "input" group absent (simulates Bazzite) — must not appear in -G
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setProcessExitCode(0);

    m_dbusInterface->call(QStringLiteral("CreateUser"), QStringLiteral("newuser"), QStringLiteral("New User"));

    QStringList useraddArgs;
    for (const auto &inv : m_ops->m_processInvocations) {
        if (inv.command == QStringLiteral("useradd")) {
            useraddArgs = inv.args;
            break;
        }
    }
    QVERIFY(!useraddArgs.isEmpty());

    int gIdx = useraddArgs.indexOf(QStringLiteral("-G"));
    QVERIFY(gIdx >= 0);
    QVERIFY(gIdx + 1 < useraddArgs.size());
    QCOMPARE(useraddArgs.at(gIdx + 1), QStringLiteral("couchplay"));
}

void TestCouchPlayHelper::testCreateUserWithInputGroup()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setProcessExitCode(0);

    m_dbusInterface->call(QStringLiteral("CreateUser"), QStringLiteral("newuser"), QStringLiteral("New User"));

    QStringList useraddArgs;
    for (const auto &inv : m_ops->m_processInvocations) {
        if (inv.command == QStringLiteral("useradd")) {
            useraddArgs = inv.args;
            break;
        }
    }
    QVERIFY(!useraddArgs.isEmpty());

    int gIdx = useraddArgs.indexOf(QStringLiteral("-G"));
    QVERIFY(gIdx >= 0);
    QVERIFY(gIdx + 1 < useraddArgs.size());
    QCOMPARE(useraddArgs.at(gIdx + 1), QStringLiteral("input,couchplay"));
}

void TestCouchPlayHelper::testDeleteUserSuccess()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {QStringLiteral("testuser")});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setUserExists(QStringLiteral("testuser"), true, 1002, 1001);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("DeleteUser"), QStringLiteral("testuser"), false);

    QVERIFY(reply.isValid());
    QVERIFY(reply.value());
}

void TestCouchPlayHelper::testDeleteUserInvalidUsername()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("DeleteUser"), QStringLiteral("INVALID-USER"), false);

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testDeleteUserNonexistent()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("DeleteUser"), QStringLiteral("nonexistent"), false);

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testDeleteUserAuthDenied()
{
    m_ops->clear();
    m_ops->setAuthResult(false);
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {QStringLiteral("testuser")});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setUserExists(QStringLiteral("testuser"), true, 1002, 1001);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("DeleteUser"), QStringLiteral("testuser"), false);

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::AccessDenied);
}

void TestCouchPlayHelper::testDeleteUserNotInCouchPlayGroup()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setUserExists(QStringLiteral("testuser"), true, 1002, 1002);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("DeleteUser"), QStringLiteral("testuser"), false);

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::AccessDenied);
}

void TestCouchPlayHelper::testDeleteUserProcessFailure()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {QStringLiteral("testuser")});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setUserExists(QStringLiteral("testuser"), true, 1002, 1001);
    m_ops->setProcessExitCode(1);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("DeleteUser"), QStringLiteral("testuser"), false);

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::Failed);
}

void TestCouchPlayHelper::testIsInCouchPlayGroupTrue()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {QStringLiteral("testuser")});

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("IsInCouchPlayGroup"), QStringLiteral("testuser"));

    QVERIFY(reply.isValid());
    QVERIFY(reply.value());
}

void TestCouchPlayHelper::testIsInCouchPlayGroupFalse()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("IsInCouchPlayGroup"), QStringLiteral("testuser"));

    QVERIFY(reply.isValid());
    QVERIFY(!reply.value());
}

void TestCouchPlayHelper::testIsInCouchPlayGroupNonexistent()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("IsInCouchPlayGroup"), QStringLiteral("nonexistent"));

    QVERIFY(reply.isValid());
    QVERIFY(!reply.value());
}

void TestCouchPlayHelper::testIsInCouchPlayGroupNonexistentGroup()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), false, 0, {});

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("IsInCouchPlayGroup"), QStringLiteral("testuser"));

    QVERIFY(reply.isValid());
    QVERIFY(!reply.value());
}

void TestCouchPlayHelper::testEnableLingerSuccess()
{
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("testuser"), true, 1002, 1002);
    m_ops->setProcessExitCode(0);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("EnableLinger"), QStringLiteral("testuser"));

    QVERIFY(reply.isValid());
    QVERIFY(reply.value());
}

void TestCouchPlayHelper::testEnableLingerInvalidUsername()
{
    m_ops->clear();

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("EnableLinger"), QStringLiteral("INVALID-USER"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testEnableLingerNonexistent()
{
    m_ops->clear();

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("EnableLinger"), QStringLiteral("nonexistent"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testEnableLingerAuthDenied()
{
    m_ops->clear();
    m_ops->setAuthResult(false);
    m_ops->setUserExists(QStringLiteral("testuser"), true, 1002, 1002);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("EnableLinger"), QStringLiteral("testuser"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::AccessDenied);
}

void TestCouchPlayHelper::testEnableLingerProcessFailure()
{
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("testuser"), true, 1002, 1002);
    m_ops->setProcessExitCode(1);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("EnableLinger"), QStringLiteral("testuser"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::Failed);
}

void TestCouchPlayHelper::testIsLingerEnabledTrue()
{
    m_ops->clear();
    m_ops->setFileExists(QStringLiteral("/var/lib/systemd/linger/testuser"), true);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("IsLingerEnabled"), QStringLiteral("testuser"));

    QVERIFY(reply.isValid());
    QVERIFY(reply.value());
}

void TestCouchPlayHelper::testIsLingerEnabledFalse()
{
    m_ops->clear();
    m_ops->setFileExists(QStringLiteral("/var/lib/systemd/linger/testuser"), false);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("IsLingerEnabled"), QStringLiteral("testuser"));

    QVERIFY(reply.isValid());
    QVERIFY(!reply.value());
}

void TestCouchPlayHelper::testIsSteamBootstrappedTrue()
{
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, QStringLiteral("/home/player1"));
    m_ops->setFileExists(QStringLiteral("/home/player1/.local/share/Steam/steam.sh"), true);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("IsSteamBootstrapped"), QStringLiteral("player1"));

    QVERIFY(reply.isValid());
    QVERIFY(reply.value());
}

void TestCouchPlayHelper::testIsSteamBootstrappedFalse()
{
    // Home present but no steam.sh (e.g. failed bootstrap left only userdata) — must report not bootstrapped.
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, QStringLiteral("/home/player1"));

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("IsSteamBootstrapped"), QStringLiteral("player1"));

    QVERIFY(reply.isValid());
    QVERIFY(!reply.value());
}
void TestCouchPlayHelper::testGetUserSteamIdSelectsMostRecentAccount()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());

    const QString username = QStringLiteral("player1");
    const QString steamRoot = home.path() + QStringLiteral("/.local/share/Steam");
    const QString userdataPath = steamRoot + QStringLiteral("/userdata");
    const QString oldSteamId = QStringLiteral("76561198000000001");
    const QString activeSteamId = QStringLiteral("76561198000000002");
    const QString loginUsersPath = steamRoot + QStringLiteral("/config/loginusers.vdf");
    QVERIFY(QDir().mkpath(steamRoot + QStringLiteral("/config")));
    QVERIFY(QDir().mkpath(userdataPath + QLatin1Char('/') + oldSteamId + QStringLiteral("/config")));
    QVERIFY(QDir().mkpath(userdataPath + QLatin1Char('/') + activeSteamId + QStringLiteral("/config")));

    QFile loginUsers(loginUsersPath);
    QVERIFY(loginUsers.open(QIODevice::WriteOnly));
    const QByteArray loginData = QByteArrayLiteral(
        "\"users\"\n{\n"
        "    \"76561198000000001\"\n    {\n        \"MostRecent\" \"0\"\n    }\n"
        "    \"76561198000000002\"\n    {\n        \"MostRecent\" \"1\"\n    }\n"
        "}\n");
    QCOMPARE(loginUsers.write(loginData), loginData.size());
    loginUsers.close();

    const QByteArray activeShortcuts = QByteArrayLiteral("active shortcuts");
    QFile shortcuts(userdataPath + QLatin1Char('/') + activeSteamId + QStringLiteral("/config/shortcuts.vdf"));
    QVERIFY(shortcuts.open(QIODevice::WriteOnly));
    QCOMPARE(shortcuts.write(activeShortcuts), activeShortcuts.size());
    shortcuts.close();

    m_ops->clear();
    m_ops->setUserExists(username, true, ::getuid(), ::getgid(), home.path());
    m_ops->setFileExists(userdataPath, true);
    m_ops->setEntryList(userdataPath, {oldSteamId, activeSteamId});

    QDBusReply<QString> steamId = m_dbusInterface->call(QStringLiteral("GetUserSteamId"), username);
    QVERIFY2(steamId.isValid(), qPrintable(steamId.error().message()));
    QCOMPARE(steamId.value(), activeSteamId);
    m_ops->replaceOnNextRead(loginUsersPath);
    QDBusReply<QString> pathnameReplaced = m_dbusInterface->call(QStringLiteral("GetUserSteamId"), username);
    QVERIFY(pathnameReplaced.isValid());
    QVERIFY(pathnameReplaced.value().isEmpty());
    QVERIFY(QFile::remove(loginUsersPath));
    QVERIFY(QFile::rename(loginUsersPath + QStringLiteral(".original"), loginUsersPath));
    QDBusReply<QByteArray> readShortcuts =
        m_dbusInterface->call(QStringLiteral("ReadSteamShortcutsForUser"), username, activeSteamId);
    QVERIFY2(readShortcuts.isValid(), qPrintable(readShortcuts.error().message()));
    QCOMPARE(readShortcuts.value(), activeShortcuts);
}

void TestCouchPlayHelper::testGetUserSteamIdAmbiguousAccounts()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());

    const QString username = QStringLiteral("player1");
    const QString steamRoot = home.path() + QStringLiteral("/.local/share/Steam");
    const QString userdataPath = steamRoot + QStringLiteral("/userdata");
    const QString firstSteamId = QStringLiteral("76561198000000001");
    const QString secondSteamId = QStringLiteral("76561198000000002");
    const QString loginUsersPath = steamRoot + QStringLiteral("/config/loginusers.vdf");
    QVERIFY(QDir().mkpath(steamRoot + QStringLiteral("/config")));

    QFile loginUsers(loginUsersPath);
    QVERIFY(loginUsers.open(QIODevice::WriteOnly));
    const QByteArray loginData = QByteArrayLiteral(
        "\"users\"\n{\n"
        "    \"76561198000000001\"\n    {\n        \"MostRecent\" \"0\"\n    }\n"
        "    \"76561198000000002\"\n    {\n        \"MostRecent\" \"0\"\n    }\n"
        "}\n");
    QCOMPARE(loginUsers.write(loginData), loginData.size());
    loginUsers.close();

    m_ops->clear();
    m_ops->setUserExists(username, true, ::getuid(), ::getgid(), home.path());
    m_ops->setFileExists(userdataPath, true);
    m_ops->setEntryList(userdataPath, {firstSteamId, secondSteamId});

    QDBusReply<QString> steamId = m_dbusInterface->call(QStringLiteral("GetUserSteamId"), username);
    QVERIFY2(steamId.isValid(), qPrintable(steamId.error().message()));
    QVERIFY(steamId.value().isEmpty());

    QDBusReply<QByteArray> readShortcuts =
        m_dbusInterface->call(QStringLiteral("ReadSteamShortcutsForUser"), username, firstSteamId);
    QVERIFY2(readShortcuts.isValid(), qPrintable(readShortcuts.error().message()));
    QVERIFY(readShortcuts.value().isEmpty());
}
void TestCouchPlayHelper::testGetUserSteamIdRejectsDuplicateMostRecentMarkers()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());

    const QString username = QStringLiteral("player1");
    const QString steamRoot = home.path() + QStringLiteral("/.local/share/Steam");
    const QString userdataPath = steamRoot + QStringLiteral("/userdata");
    const QString existingSteamId = QStringLiteral("76561198000000001");
    const QString secondSteamId = QStringLiteral("76561198000000002");
    const QString loginUsersPath = steamRoot + QStringLiteral("/config/loginusers.vdf");
    QVERIFY(QDir().mkpath(steamRoot + QStringLiteral("/config")));
    QVERIFY(QDir().mkpath(userdataPath + QLatin1Char('/') + existingSteamId + QStringLiteral("/config")));

    QFile loginUsers(loginUsersPath);
    QVERIFY(loginUsers.open(QIODevice::WriteOnly));
    const QByteArray loginData = QStringLiteral(
        "\"users\"\n{\n"
        "    \"%1\"\n    {\n        \"MostRecent\" \"1\"\n    }\n"
        "    \"%2\"\n    {\n        \"MostRecent\" \"1\"\n    }\n"
        "}\n")
                                    .arg(existingSteamId, secondSteamId)
                                    .toUtf8();
    QCOMPARE(loginUsers.write(loginData), loginData.size());
    loginUsers.close();

    m_ops->clear();
    m_ops->setUserExists(username, true, ::getuid(), ::getgid(), home.path());
    m_ops->setFileExists(userdataPath, true);
    m_ops->setEntryList(userdataPath, {existingSteamId});

    QDBusReply<QString> steamId = m_dbusInterface->call(QStringLiteral("GetUserSteamId"), username);
    QVERIFY2(steamId.isValid(), qPrintable(steamId.error().message()));
    QVERIFY(steamId.value().isEmpty());
}

void TestCouchPlayHelper::testGetUserSteamIdPrefersMostRecentWithoutUserdata()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());

    const QString username = QStringLiteral("player1");
    const QString steamRoot = home.path() + QStringLiteral("/.local/share/Steam");
    const QString userdataPath = steamRoot + QStringLiteral("/userdata");
    const QString staleSteamId = QStringLiteral("76561198000000001");
    const QString activeSteamId = QStringLiteral("76561198000000002");
    const QString loginUsersPath = steamRoot + QStringLiteral("/config/loginusers.vdf");
    QVERIFY(QDir().mkpath(steamRoot + QStringLiteral("/config")));
    QVERIFY(QDir().mkpath(userdataPath + QLatin1Char('/') + staleSteamId + QStringLiteral("/config")));

    QFile loginUsers(loginUsersPath);
    QVERIFY(loginUsers.open(QIODevice::WriteOnly));
    const QByteArray loginData = QByteArrayLiteral(
        "\"users\"\n{\n"
        "    \"76561198000000002\"\n    {\n        \"MostRecent\" \"1\"\n    }\n"
        "}\n");
    QCOMPARE(loginUsers.write(loginData), loginData.size());
    loginUsers.close();

    m_ops->clear();
    m_ops->setUserExists(username, true, ::getuid(), ::getgid(), home.path());
    m_ops->setFileExists(userdataPath, true);
    m_ops->setEntryList(userdataPath, {staleSteamId});

    QDBusReply<QString> steamId = m_dbusInterface->call(QStringLiteral("GetUserSteamId"), username);
    QVERIFY2(steamId.isValid(), qPrintable(steamId.error().message()));
    QCOMPARE(steamId.value(), activeSteamId);

    QDBusReply<QByteArray> readShortcuts =
        m_dbusInterface->call(QStringLiteral("ReadSteamShortcutsForUser"), username, activeSteamId);
    QVERIFY2(readShortcuts.isValid(), qPrintable(readShortcuts.error().message()));
    QVERIFY(readShortcuts.value().isEmpty());
}
void TestCouchPlayHelper::testGetUserSteamIdDoesNotFallbackForUnsafeLoginUsers()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());

    const QString username = QStringLiteral("player1");
    const QString steamRoot = home.path() + QStringLiteral("/.local/share/Steam");
    const QString userdataPath = steamRoot + QStringLiteral("/userdata");
    const QString staleSteamId = QStringLiteral("76561198000000001");
    const QString loginUsersPath = steamRoot + QStringLiteral("/config/loginusers.vdf");
    QVERIFY(QDir().mkpath(steamRoot + QStringLiteral("/config")));
    QVERIFY(QDir().mkpath(userdataPath + QLatin1Char('/') + staleSteamId + QStringLiteral("/config")));

    const QString outsidePath = home.path() + QStringLiteral("/loginusers-outside.vdf");
    QFile outsideFile(outsidePath);
    QVERIFY(outsideFile.open(QIODevice::WriteOnly));
    const QByteArray outsideData = QByteArrayLiteral(
        "\"users\"\n{\n"
        "    \"76561198000000001\"\n    {\n        \"MostRecent\" \"1\"\n    }\n"
        "}\n");
    QCOMPARE(outsideFile.write(outsideData), outsideData.size());
    outsideFile.close();
    const QByteArray outsideBytes = outsidePath.toLocal8Bit();
    const QByteArray loginUsersBytes = loginUsersPath.toLocal8Bit();
    QVERIFY(::symlink(outsideBytes.constData(), loginUsersBytes.constData()) == 0);

    m_ops->clear();
    m_ops->setUserExists(username, true, ::getuid(), ::getgid(), home.path());
    m_ops->setFileExists(userdataPath, true);
    m_ops->setEntryList(userdataPath, {staleSteamId});

    QDBusReply<QString> steamId = m_dbusInterface->call(QStringLiteral("GetUserSteamId"), username);
    QVERIFY2(steamId.isValid(), qPrintable(steamId.error().message()));
    QVERIFY(steamId.value().isEmpty());
}
void TestCouchPlayHelper::testReadSteamShortcutsForUser()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());

    const QString username = QStringLiteral("player1");
    const QString steamRoot = home.path() + QStringLiteral("/.local/share/Steam");
    const QString userdataPath = steamRoot + QStringLiteral("/userdata");
    const QString steamId = QStringLiteral("76561198000000000");
    const QString shortcutsPath = userdataPath + QLatin1Char('/') + steamId
        + QStringLiteral("/config/shortcuts.vdf");
    QVERIFY(QDir().mkpath(userdataPath + QLatin1Char('/') + steamId + QStringLiteral("/config")));

    const uid_t owner = ::getuid();
    const gid_t group = ::getgid();
    m_ops->clear();
    m_ops->setUserExists(username, true, owner, group, home.path());
    m_ops->setFileExists(userdataPath, true);
    m_ops->setEntryList(userdataPath, {steamId});

    const QByteArray expected("shortcuts fixture");
    QFile file(shortcutsPath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(expected), expected.size());
    file.close();
    m_ops->replaceOnNextRead(shortcutsPath);
    QDBusReply<QByteArray> pathnameReplaced =
        m_dbusInterface->call(QStringLiteral("ReadSteamShortcutsForUser"), username, steamId);
    QVERIFY(!pathnameReplaced.isValid());
    QVERIFY(QFile::remove(shortcutsPath));
    QVERIFY(QFile::rename(shortcutsPath + QStringLiteral(".original"), shortcutsPath));
    QDBusReply<QByteArray> reply =
        m_dbusInterface->call(QStringLiteral("ReadSteamShortcutsForUser"), username, steamId);
    QVERIFY2(reply.isValid(), qPrintable(reply.error().message()));
    QCOMPARE(reply.value(), expected);
    m_ops->truncateOnNextRead(shortcutsPath);
    QDBusReply<QByteArray> truncated =
        m_dbusInterface->call(QStringLiteral("ReadSteamShortcutsForUser"), username, steamId);
    QVERIFY(!truncated.isValid());
    m_ops->setAuthResult(false);
    QDBusReply<QByteArray> denied =
        m_dbusInterface->call(QStringLiteral("ReadSteamShortcutsForUser"), username, steamId);
    QVERIFY(!denied.isValid());
    m_ops->setAuthResult(true);

    QVERIFY(QFile::remove(shortcutsPath));
    QDBusReply<QByteArray> missing =
        m_dbusInterface->call(QStringLiteral("ReadSteamShortcutsForUser"), username, steamId);
    QVERIFY(missing.isValid());
    QVERIFY(missing.value().isEmpty());

    QFile emptyFile(shortcutsPath);
    QVERIFY(emptyFile.open(QIODevice::WriteOnly));
    emptyFile.close();
    QDBusReply<QByteArray> empty =
        m_dbusInterface->call(QStringLiteral("ReadSteamShortcutsForUser"), username, steamId);
    QVERIFY(!empty.isValid());

    QVERIFY(QFile::remove(shortcutsPath));
    const QString outsidePath = home.path() + QStringLiteral("/outside.vdf");
    QFile outsideFile(outsidePath);
    QVERIFY(outsideFile.open(QIODevice::WriteOnly));
    QCOMPARE(outsideFile.write(expected), expected.size());
    outsideFile.close();
    QVERIFY(::symlink(outsidePath.toLocal8Bit().constData(), shortcutsPath.toLocal8Bit().constData()) == 0);
    QDBusReply<QByteArray> symlinked =
        m_dbusInterface->call(QStringLiteral("ReadSteamShortcutsForUser"), username, steamId);
    QVERIFY(!symlinked.isValid());
}
void TestCouchPlayHelper::testWriteSteamShortcutsRejectsConcurrentEdit()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());

    const QString username = QStringLiteral("player1");
    const QString steamRoot = home.path() + QStringLiteral("/.local/share/Steam");
    const QString userdataPath = steamRoot + QStringLiteral("/userdata");
    const QString steamId = QStringLiteral("76561198000000000");
    const QString shortcutsPath = userdataPath + QLatin1Char('/') + steamId
        + QStringLiteral("/config/shortcuts.vdf");
    QVERIFY(QDir().mkpath(userdataPath + QLatin1Char('/') + steamId + QStringLiteral("/config")));

    m_ops->clear();
    m_ops->setUserExists(username, true, ::getuid(), ::getgid(), home.path());
    m_ops->setFileExists(userdataPath, true);
    m_ops->setMockProcessStart(true);
    m_ops->setProcessExitCode(1);
    m_ops->setEntryList(userdataPath, {steamId});
    const QByteArray initial = QByteArrayLiteral("initial shortcuts");
    QFile file(shortcutsPath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(initial), initial.size());
    file.close();

    QDBusReply<QByteArray> snapshot =
        m_dbusInterface->call(QStringLiteral("ReadSteamShortcutsForUser"), username, steamId);
    QVERIFY2(snapshot.isValid(), qPrintable(snapshot.error().message()));
    QCOMPARE(snapshot.value(), initial);

    const QByteArray concurrentEdit = QByteArrayLiteral("Steam's concurrent edit");
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(concurrentEdit), concurrentEdit.size());
    file.close();

    const QByteArray expectedDigest = QCryptographicHash::hash(snapshot.value(), QCryptographicHash::Sha256).toHex();
    const QByteArray replacement = QByteArrayLiteral("CouchPlay replacement");
    QDBusReply<bool> rejected = m_dbusInterface->call(QStringLiteral("WriteSteamShortcutsForUser"),
                                                     username,
                                                     steamId,
                                                     expectedDigest,
                                                     replacement);
    QVERIFY(!rejected.isValid());

    QFile unchanged(shortcutsPath);
    QVERIFY(unchanged.open(QIODevice::ReadOnly));
    QCOMPARE(unchanged.readAll(), concurrentEdit);
    unchanged.close();

    const QByteArray currentDigest = QCryptographicHash::hash(concurrentEdit, QCryptographicHash::Sha256).toHex();
    const QByteArray renameRaceEdit = QByteArrayLiteral("Edit between compare and exchange");
    const QByteArray externalReplacement = QByteArrayLiteral("External atomic replacement");
    const QString externalTempPath = QFileInfo(shortcutsPath).absolutePath() + QStringLiteral("/external.vdf");
    m_ops->setBeforeNextRenameAt([&] {
        QFile racedFile(shortcutsPath);
        if (!racedFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
        (void)racedFile.write(renameRaceEdit);
        racedFile.close();
    });
    m_ops->setAfterFirstExchange([&] {
        QFile racedFile(externalTempPath);
        if (!racedFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
        (void)racedFile.write(externalReplacement);
        racedFile.close();
        QVERIFY(::rename(externalTempPath.toLocal8Bit().constData(), shortcutsPath.toLocal8Bit().constData()) == 0);
    });
    QDBusReply<bool> raceRejected = m_dbusInterface->call(QStringLiteral("WriteSteamShortcutsForUser"),
                                                         username,
                                                         steamId,
                                                         currentDigest,
                                                         replacement);
    QVERIFY(!raceRejected.isValid());
    QVERIFY(unchanged.open(QIODevice::ReadOnly));
    QCOMPARE(unchanged.readAll(), externalReplacement);
    unchanged.close();
    const QStringList retainedNames = QDir(QFileInfo(shortcutsPath).absolutePath())
        .entryList({QStringLiteral(".shortcuts.vdf.couchplay-*.tmp")}, QDir::Files | QDir::Hidden);
    QCOMPARE(retainedNames.size(), 1);
    QFile retainedSnapshot(QFileInfo(shortcutsPath).absolutePath() + QLatin1Char('/') + retainedNames.first());
    QVERIFY(retainedSnapshot.open(QIODevice::ReadOnly));
    QCOMPARE(retainedSnapshot.readAll(), renameRaceEdit);
    retainedSnapshot.close();

    const QByteArray externalDigest = QCryptographicHash::hash(externalReplacement, QCryptographicHash::Sha256).toHex();
    m_ops->setProcessExitCode(0);
    QDBusReply<bool> runningRejected = m_dbusInterface->call(QStringLiteral("WriteSteamShortcutsForUser"),
                                                            username, steamId, externalDigest, replacement);
    QVERIFY(!runningRejected.isValid());
    QVERIFY(unchanged.open(QIODevice::ReadOnly));
    QCOMPARE(unchanged.readAll(), externalReplacement);
    unchanged.close();
    m_ops->setProcessExitCode(2);
    QDBusReply<bool> unknownRejected = m_dbusInterface->call(QStringLiteral("WriteSteamShortcutsForUser"),
                                                            username, steamId, externalDigest, replacement);
    QVERIFY(!unknownRejected.isValid());
    m_ops->setProcessExitCode(1);

    const QByteArray stableDigest = externalDigest;
    QDBusReply<bool> committed = m_dbusInterface->call(QStringLiteral("WriteSteamShortcutsForUser"),
                                                      username,
                                                      steamId,
                                                      stableDigest,
                                                      replacement);
    QVERIFY2(committed.isValid(), qPrintable(committed.error().message()));
    QVERIFY(committed.value());
    QVERIFY(unchanged.open(QIODevice::ReadOnly));
    QCOMPARE(unchanged.readAll(), replacement);
    unchanged.close();

    QVERIFY(QFile::remove(shortcutsPath));
    const QByteArray missingRaceEdit = QByteArrayLiteral("Steam created before no-replace");
    m_ops->setBeforeNextRenameAt([&] {
        QFile racedFile(shortcutsPath);
        if (!racedFile.open(QIODevice::WriteOnly)) {
            return;
        }
        (void)racedFile.write(missingRaceEdit);
        racedFile.close();
    });
    QDBusReply<bool> missingRaceRejected = m_dbusInterface->call(QStringLiteral("WriteSteamShortcutsForUser"),
                                                                username,
                                                                steamId,
                                                                QByteArrayLiteral("missing"),
                                                                concurrentEdit);
    QVERIFY(!missingRaceRejected.isValid());
    QFile missingWinner(shortcutsPath);
    QVERIFY(missingWinner.open(QIODevice::ReadOnly));
    QCOMPARE(missingWinner.readAll(), missingRaceEdit);
    missingWinner.close();

    QVERIFY(QFile::remove(shortcutsPath));
    QDBusReply<bool> created = m_dbusInterface->call(QStringLiteral("WriteSteamShortcutsForUser"),
                                                     username,
                                                     steamId,
                                                     QByteArrayLiteral("missing"),
                                                     concurrentEdit);
    QVERIFY2(created.isValid(), qPrintable(created.error().message()));
    QVERIFY(created.value());
    QFile createdFile(shortcutsPath);
    QVERIFY(createdFile.open(QIODevice::ReadOnly));
    QCOMPARE(createdFile.readAll(), concurrentEdit);
}
void TestCouchPlayHelper::testReadSteamLibraryFoldersSecurely()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());
    const QString username = QStringLiteral("player1");
    const QString steamRoot = home.path() + QStringLiteral("/.local/share/Steam");
    const QString configDir = steamRoot + QStringLiteral("/config");
    const QString userdata = steamRoot + QStringLiteral("/userdata");
    QVERIFY(QDir().mkpath(configDir));
    QVERIFY(QDir().mkpath(userdata));
    const QString path = configDir + QStringLiteral("/libraryfolders.vdf");
    const QByteArray original = QByteArrayLiteral("libraryfolders original bytes");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(original), original.size());
    file.close();

    m_ops->clear();
    m_ops->setUserExists(username, true, ::getuid(), ::getgid(), home.path());
    m_ops->setFileExists(userdata, true);
    QDBusReply<QVariantMap> snapshot =
        m_dbusInterface->call(QStringLiteral("ReadSteamLibraryFoldersForUser"), username);
    QVERIFY2(snapshot.isValid(), qPrintable(snapshot.error().message()));
    QCOMPARE(snapshot.value().value(QStringLiteral("exists")).toBool(), true);
    QCOMPARE(snapshot.value().value(QStringLiteral("content")).toByteArray(), original);
    m_ops->replaceOnNextRead(path);
    QDBusReply<QVariantMap> replaced =
        m_dbusInterface->call(QStringLiteral("ReadSteamLibraryFoldersForUser"), username);
    QVERIFY(!replaced.isValid());
    const QByteArray sessionVdf = QByteArrayLiteral("temporary CouchPlay library entries");
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(sessionVdf), sessionVdf.size());
    file.close();
    QDBusReply<bool> restored = m_dbusInterface->call(QStringLiteral("RestoreSteamLibraryFoldersForUser"),
                                                      username, true, original);
    QVERIFY2(restored.isValid(), qPrintable(restored.error().message()));
    QVERIFY(restored.value());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), original);
    file.close();
    const QString hardlink = home.path() + QStringLiteral("/hardlink.vdf");
    QVERIFY(::link(path.toLocal8Bit().constData(), hardlink.toLocal8Bit().constData()) == 0);
    QDBusReply<bool> hardlinkRejected =
        m_dbusInterface->call(QStringLiteral("RestoreSteamLibraryFoldersForUser"),
                               username,
                               true,
                               QByteArrayLiteral("must not truncate hardlinks"));
    QVERIFY(!hardlinkRejected.isValid());
    QFile hardlinkFile(hardlink);
    QVERIFY(hardlinkFile.open(QIODevice::ReadOnly));
    QCOMPARE(hardlinkFile.readAll(), original);
    hardlinkFile.close();
    QVERIFY(::unlink(hardlink.toLocal8Bit().constData()) == 0);
    const QByteArray concurrentReplacement = QByteArrayLiteral("concurrent replacement");
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(original), original.size());
    file.close();
    m_ops->setBeforeNextRenameAt([&] {
        const QString displaced = path + QStringLiteral(".race-original");
        QFile::remove(displaced);
        if (!QFile::rename(path, displaced)) return;
        QFile replacement(path);
        if (!replacement.open(QIODevice::WriteOnly)) return;
        (void)replacement.write(concurrentReplacement);
        replacement.close();
    });
    QDBusReply<bool> unlinkRace = m_dbusInterface->call(QStringLiteral("RestoreSteamLibraryFoldersForUser"),
                                                        username,
                                                        false,
                                                        QByteArray());
    QVERIFY(!unlinkRace.isValid());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), concurrentReplacement);
    file.close();
    QFile::remove(path + QStringLiteral(".race-original"));
    const QString outside = home.path() + QStringLiteral("/outside.vdf");
    QFile outsideFile(outside);
    QVERIFY(outsideFile.open(QIODevice::WriteOnly));
    QCOMPARE(outsideFile.write(original), original.size());
    outsideFile.close();
    QVERIFY(::symlink(outside.toLocal8Bit().constData(), path.toLocal8Bit().constData()) == 0);
    QDBusReply<QVariantMap> unsafe =
        m_dbusInterface->call(QStringLiteral("ReadSteamLibraryFoldersForUser"), username);
    QVERIFY(!unsafe.isValid());
}

void TestCouchPlayHelper::testReadSteamShortcutsMissingSteamDirectories()
{
    QTemporaryDir home;
    QVERIFY(home.isValid());

    const QString username = QStringLiteral("player1");
    const uid_t owner = ::getuid();
    const gid_t group = ::getgid();
    m_ops->clear();
    m_ops->setUserExists(username, true, owner, group, home.path());

    QDBusReply<QByteArray> missingRoot =
        m_dbusInterface->call(QStringLiteral("ReadSteamShortcutsForUser"), username, QStringLiteral("76561198000000000"));
    QVERIFY(missingRoot.isValid());
    QVERIFY(missingRoot.value().isEmpty());

    const QString steamRoot = home.path() + QStringLiteral("/.local/share/Steam");
    QVERIFY(QDir().mkpath(steamRoot));
    m_ops->setFileExists(steamRoot, true);

    QDBusReply<QByteArray> missingUserdata =
        m_dbusInterface->call(QStringLiteral("ReadSteamShortcutsForUser"), username, QStringLiteral("76561198000000000"));
    QVERIFY(missingUserdata.isValid());
    QVERIFY(missingUserdata.value().isEmpty());
}

void TestCouchPlayHelper::testCopyDirectoryToUserAbsoluteTarget()
{
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, QStringLiteral("/home/player1"));

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("CopyDirectoryToUser"),
                                                   QStringLiteral("player1"),
                                                   QStringLiteral("/home/compositor/games"),
                                                   QStringLiteral("/etc/passwd")); // absolute target

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testCopyDirectoryToUserTraversalTarget()
{
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, QStringLiteral("/home/player1"));

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("CopyDirectoryToUser"),
                                                   QStringLiteral("player1"),
                                                   QStringLiteral("/home/compositor/games"),
                                                   QStringLiteral("../escape")); // traversal

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testCopyDirectoryToUserSourceOutsideAllowedPrefixes()
{
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, QStringLiteral("/home/player1"));
    m_ops->setFileExists(QStringLiteral("/etc"), true); // exists and is a directory, but out of prefixes
    m_ops->setDirectoryExists(QStringLiteral("/etc"), true);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("CopyDirectoryToUser"),
                                                   QStringLiteral("player1"),
                                                   QStringLiteral("/etc"),
                                                   QStringLiteral("games"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
    QCOMPARE(m_ops->m_processInvocations.size(), 0); // nothing ran
}

void TestCouchPlayHelper::testCopyDirectoryToUserSourceNotExists()
{
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, QStringLiteral("/home/player1"));

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("CopyDirectoryToUser"),
                                                   QStringLiteral("player1"),
                                                   QStringLiteral("/home/compositor/games"), // not mocked
                                                   QStringLiteral("games"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testCopyDirectoryToUserSuccessReplacesAndChowns()
{
    // Real filesystem layout: mutations are FD-anchored in-process, not
    // subprocess calls, so this verifies end behavior rather than argv
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, homeDir.path());

    QDir sourceDir(homeDir.path() + QStringLiteral("/source-game"));
    QVERIFY(sourceDir.mkpath(QStringLiteral(".")));
    m_ops->setFileExists(sourceDir.path(), true); // validation layer consults the mock
    m_ops->setDirectoryExists(sourceDir.path(), true);
    {
        QFile f(sourceDir.filePath(QStringLiteral("config.ini")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("player=1\n");
    }
    QVERIFY(sourceDir.mkpath(QStringLiteral("subdir")));
    {
        QFile f(sourceDir.filePath(QStringLiteral("subdir/data.bin")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("data");
    }

    // Stale target from a previous session: must be replaced, not nested into
    QDir targetDir(homeDir.path() + QStringLiteral("/games"));
    QVERIFY(targetDir.mkpath(QStringLiteral(".")));
    {
        QFile stale(targetDir.filePath(QStringLiteral("stale.txt")));
        QVERIFY(stale.open(QIODevice::WriteOnly));
        stale.write("old");
    }

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("CopyDirectoryToUser"),
                                                   QStringLiteral("player1"),
                                                   sourceDir.path(),
                                                   QStringLiteral("games"));

    QVERIFY(reply.isValid());
    QVERIFY(reply.value());

    QFileInfo replaced(targetDir.filePath(QStringLiteral("config.ini")));
    QVERIFY(replaced.exists());
    QFile replacedFile(replaced.absoluteFilePath());
    QVERIFY(replacedFile.open(QIODevice::ReadOnly));
    QCOMPARE(replacedFile.readAll(), QByteArray("player=1\n"));
    QVERIFY(QFileInfo(targetDir.filePath(QStringLiteral("subdir/data.bin"))).exists());
    QVERIFY(!QFileInfo(targetDir.filePath(QStringLiteral("stale.txt"))).exists());

    if (geteuid() == 0) { // ownership transfer requires root (CI runs as root)
        struct stat st;
        QCOMPARE(::stat(replaced.absoluteFilePath().toLocal8Bit().constData(), &st), 0);
        QCOMPARE(st.st_uid, static_cast<uid_t>(1001));
        QCOMPARE(st.st_gid, static_cast<gid_t>(1001));
    }

    // The temp-sibling swap must not leave hidden siblings behind
    const QStringList parentEntries =
        QDir(homeDir.path()).entryList(QDir::Dirs | QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
    QCOMPARE(parentEntries.size(), 2); // source-game + games
}

void TestCouchPlayHelper::testCopyDirectoryToUserTargetSwapPreservesExternal()
{
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, homeDir.path());

    QDir sourceDir(homeDir.path() + QStringLiteral("/source-game"));
    QVERIFY(sourceDir.mkpath(QStringLiteral(".")));
    m_ops->setFileExists(sourceDir.path(), true);
    m_ops->setDirectoryExists(sourceDir.path(), true);
    QFile sourceFile(sourceDir.filePath(QStringLiteral("new.dat")));
    QVERIFY(sourceFile.open(QIODevice::WriteOnly));
    QVERIFY(sourceFile.write("new") == 3);
    sourceFile.close();

    QDir targetDir(homeDir.path() + QStringLiteral("/games"));
    QVERIFY(targetDir.mkpath(QStringLiteral(".")));
    QFile oldFile(targetDir.filePath(QStringLiteral("old.dat")));
    QVERIFY(oldFile.open(QIODevice::WriteOnly));
    QVERIFY(oldFile.write("old") == 3);
    oldFile.close();

    QDir externalDir(homeDir.path() + QStringLiteral("/external-target"));
    QVERIFY(externalDir.mkpath(QStringLiteral(".")));
    QFile externalFile(externalDir.filePath(QStringLiteral("external.dat")));
    QVERIFY(externalFile.open(QIODevice::WriteOnly));
    QVERIFY(externalFile.write("external") == 8);
    externalFile.close();

    m_ops->setBeforeNextRenameAt([&] {
        QDir parent(homeDir.path());
        if (!parent.rename(QStringLiteral("games"), QStringLiteral("moved-target"))) return;
        (void)parent.rename(QStringLiteral("external-target"), QStringLiteral("games"));
    });
    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("CopyDirectoryToUser"),
                                                   QStringLiteral("player1"),
                                                   sourceDir.path(),
                                                   QStringLiteral("games"));
    QVERIFY(!reply.isValid());
    QFile preserved(homeDir.filePath(QStringLiteral("games/external.dat")));
    QVERIFY(preserved.open(QIODevice::ReadOnly));
    QCOMPARE(preserved.readAll(), QByteArrayLiteral("external"));
    preserved.close();
}

void TestCouchPlayHelper::testCopyDirectoryToUserPreservesTargetOnFailedCopy()
{
    // A replacement copy that fails midway (special file in the source) must
    // leave the player's previous data intact — never delete-then-copy
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, homeDir.path());

    QDir sourceDir(homeDir.path() + QStringLiteral("/source-game"));
    QVERIFY(sourceDir.mkpath(QStringLiteral(".")));
    m_ops->setFileExists(sourceDir.path(), true); // validation layer consults the mock
    m_ops->setDirectoryExists(sourceDir.path(), true);
    {
        QFile f(sourceDir.filePath(QStringLiteral("config.ini")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("new");
    }
    // The copier refuses special files -> the copy fails partway through
    QCOMPARE(::mkfifo(sourceDir.filePath(QStringLiteral("pipe")).toLocal8Bit().constData(), 0666), 0);

    QDir targetDir(homeDir.path() + QStringLiteral("/games"));
    QVERIFY(targetDir.mkpath(QStringLiteral(".")));
    {
        QFile previous(targetDir.filePath(QStringLiteral("save.dat")));
        QVERIFY(previous.open(QIODevice::WriteOnly));
        previous.write("previous");
    }

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("CopyDirectoryToUser"),
                                                   QStringLiteral("player1"),
                                                   sourceDir.path(),
                                                   QStringLiteral("games"));

    QVERIFY(!reply.isValid()); // failed copy -> D-Bus error

    // The player's previous data survived the failed replacement
    QFile survived(targetDir.filePath(QStringLiteral("save.dat")));
    QVERIFY(survived.open(QIODevice::ReadOnly));
    QCOMPARE(survived.readAll(), QByteArray("previous"));
    QVERIFY(!QFileInfo(targetDir.filePath(QStringLiteral("config.ini"))).exists());

    // No temporary/backup siblings left next to the target
    const QStringList parentEntries =
        QDir(homeDir.path()).entryList(QDir::Dirs | QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
    QVERIFY(parentEntries.contains(QStringLiteral("source-game")));
    QVERIFY(parentEntries.contains(QStringLiteral("games")));
    QCOMPARE(parentEntries.size(), 2);
}

void TestCouchPlayHelper::testCopyDirectoryToUserDotDotNameAccepted()
{
    // ".." inside a name is valid; only whole ".." components climb the tree
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, homeDir.path());

    QDir sourceDir(homeDir.path() + QStringLiteral("/source-game"));
    QVERIFY(sourceDir.mkpath(QStringLiteral(".")));
    m_ops->setFileExists(sourceDir.path(), true);
    m_ops->setDirectoryExists(sourceDir.path(), true);
    {
        QFile f(sourceDir.filePath(QStringLiteral("config.ini")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
    }

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("CopyDirectoryToUser"),
                                                   QStringLiteral("player1"),
                                                   sourceDir.path(),
                                                   QStringLiteral("Saves/Foo..Bar"));

    QVERIFY(reply.isValid());
    QVERIFY(reply.value());
    QVERIFY(QFileInfo(homeDir.filePath(QStringLiteral("Saves/Foo..Bar/config.ini"))).exists());
}

void TestCouchPlayHelper::testCopyDirectoryToUserSymlinkedTargetRejected()
{
    m_ops->clear();
    m_ops->setMockProcessStart(true);
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, QStringLiteral("/home/player1"));
    m_ops->setFileExists(QStringLiteral("/home/compositor/games"), true);
    m_ops->setDirectoryExists(QStringLiteral("/home/compositor/games"), true);
    // Player replaced ~/.config with a symlink: root-run rm/cp/chown must not follow it
    m_ops->setSymlink(QStringLiteral("/home/player1/.config"), QStringLiteral("/etc"));

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("CopyDirectoryToUser"),
                                                   QStringLiteral("player1"),
                                                   QStringLiteral("/home/compositor/games"),
                                                   QStringLiteral(".config/games"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
    QCOMPARE(m_ops->m_processInvocations.size(), 0);
}

void TestCouchPlayHelper::testCopyDirectoryToUserSourceSymlinkResolvesOutside()
{
    m_ops->clear();
    m_ops->setMockProcessStart(true);
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, QStringLiteral("/home/player1"));
    // An allowed-prefix source that is really a symlink to /etc
    m_ops->setFileExists(QStringLiteral("/home/compositor/link"), true);
    m_ops->setDirectoryExists(QStringLiteral("/home/compositor/link"), true);
    m_ops->setCanonicalMapping(QStringLiteral("/home/compositor/link"), QStringLiteral("/etc"));

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("CopyDirectoryToUser"),
                                                   QStringLiteral("player1"),
                                                   QStringLiteral("/home/compositor/link"),
                                                   QStringLiteral("games"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
    QCOMPARE(m_ops->m_processInvocations.size(), 0);
}

void TestCouchPlayHelper::testSetupOverlayMountSymlinkedTargetRejected()
{
    m_ops->clear();
    m_ops->setMockProcessStart(true);
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, QStringLiteral("/home/player1"));
    struct passwd *self = getpwuid(getuid());
    QVERIFY(self);
    m_ops->setUserExists(QString::fromLocal8Bit(self->pw_name), true, getuid(), self->pw_gid,
                         QStringLiteral("/home/compositor"));
    m_ops->setFileExists(QStringLiteral("/home/compositor/games"), true);
    m_ops->setDirectoryExists(QStringLiteral("/home/compositor/games"), true);
    // Symlinked ancestor on the mount target path
    m_ops->setSymlink(QStringLiteral("/home/player1/shares"), QStringLiteral("/etc"));

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("SetupOverlayMount"),
                                                   QStringLiteral("player1"),

                                                   QStringLiteral("/home/compositor/games"),
                                                   QStringLiteral("shares/games"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::Failed);
    QCOMPARE(m_ops->m_processInvocations.size(), 0);
}

void TestCouchPlayHelper::testMirrorDirectoryContentsSuccess()
{
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, homeDir.path());

    QDir stagingDir(homeDir.path() + QStringLiteral("/staging"));
    QVERIFY(stagingDir.mkpath(QStringLiteral(".")));
    m_ops->setFileExists(stagingDir.path(), true); // validation layer consults the mock
    m_ops->setDirectoryExists(stagingDir.path(), true);
    {
        QFile f(stagingDir.filePath(QStringLiteral("config.ini")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("player=1\n");
    }

    // Existing merge target (e.g. an overlay mount point) with its own file
    QDir targetDir(homeDir.path() + QStringLiteral("/Games/MyGame"));
    QVERIFY(targetDir.mkpath(QStringLiteral(".")));
    m_ops->setFileExists(targetDir.path(), true);
    m_ops->setDirectoryExists(targetDir.path(), true);
    {
        QFile existing(targetDir.filePath(QStringLiteral("keep.txt")));
        QVERIFY(existing.open(QIODevice::WriteOnly));
        existing.write("keep");
    }

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("MirrorDirectoryContents"),
                                                   QStringLiteral("player1"),
                                                   stagingDir.path(),
                                                   QStringLiteral("Games/MyGame"));

    QVERIFY(reply.isValid());
    QVERIFY(reply.value());

    // Merge: staged file copied in, pre-existing file untouched
    QFile copied(targetDir.filePath(QStringLiteral("config.ini")));
    QVERIFY(copied.open(QIODevice::ReadOnly));
    QCOMPARE(copied.readAll(), QByteArray("player=1\n"));
    QFile kept(targetDir.filePath(QStringLiteral("keep.txt")));
    QVERIFY(kept.open(QIODevice::ReadOnly));
    QCOMPARE(kept.readAll(), QByteArray("keep"));

    if (geteuid() == 0) {
        struct stat st;
        QCOMPARE(::stat(targetDir.filePath(QStringLiteral("config.ini")).toLocal8Bit().constData(), &st), 0);
        QCOMPARE(st.st_uid, static_cast<uid_t>(1001));
    }
}

void TestCouchPlayHelper::testMirrorDirectoryContentsReplacesExistingSymlink()
{
    // Merge-overwrite semantics: a staged symlink replaces a stale symlink
    // (and a regular file) of the same name instead of being silently dropped
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, homeDir.path());

    QDir stagingDir(homeDir.path() + QStringLiteral("/staging"));
    QVERIFY(stagingDir.mkpath(QStringLiteral(".")));
    m_ops->setFileExists(stagingDir.path(), true); // validation layer consults the mock
    m_ops->setDirectoryExists(stagingDir.path(), true);
    QCOMPARE(::symlink("new-target", stagingDir.filePath(QStringLiteral("link")).toLocal8Bit().constData()), 0);
    QCOMPARE(::symlink("entry-link", stagingDir.filePath(QStringLiteral("entry")).toLocal8Bit().constData()), 0);

    QDir targetDir(homeDir.path() + QStringLiteral("/Games/MyGame"));
    QVERIFY(targetDir.mkpath(QStringLiteral(".")));
    m_ops->setFileExists(targetDir.path(), true);
    m_ops->setDirectoryExists(targetDir.path(), true);
    QCOMPARE(::symlink("old-target", targetDir.filePath(QStringLiteral("link")).toLocal8Bit().constData()), 0);
    {
        QFile stale(targetDir.filePath(QStringLiteral("entry"))); // regular file replaced by a link
        QVERIFY(stale.open(QIODevice::WriteOnly));
        stale.write("data");
    }

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("MirrorDirectoryContents"),
                                                   QStringLiteral("player1"),
                                                   stagingDir.path(),
                                                   QStringLiteral("Games/MyGame"));

    QVERIFY(reply.isValid());
    QVERIFY(reply.value());

    // symLinkTarget() resolves the raw link string against the link's directory
    QCOMPARE(QFileInfo(targetDir.filePath(QStringLiteral("link"))).symLinkTarget(),
             targetDir.filePath(QStringLiteral("new-target")));
    const QFileInfo replacedEntry(targetDir.filePath(QStringLiteral("entry")));
    QVERIFY(replacedEntry.isSymLink());
    QCOMPARE(replacedEntry.symLinkTarget(), targetDir.filePath(QStringLiteral("entry-link")));
}

void TestCouchPlayHelper::testMirrorDirectoryContentsSymlinkVsNonEmptyDirFails()
{
    // A staged symlink cannot replace a non-empty directory — fail loudly
    // instead of silently keeping the stale entry
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, homeDir.path());

    QDir stagingDir(homeDir.path() + QStringLiteral("/staging"));
    QVERIFY(stagingDir.mkpath(QStringLiteral(".")));
    m_ops->setFileExists(stagingDir.path(), true);
    m_ops->setDirectoryExists(stagingDir.path(), true);
    QCOMPARE(::symlink("target", stagingDir.filePath(QStringLiteral("entry")).toLocal8Bit().constData()), 0);

    QDir targetDir(homeDir.path() + QStringLiteral("/Games/MyGame"));
    QVERIFY(targetDir.mkpath(QStringLiteral(".")));
    m_ops->setFileExists(targetDir.path(), true);
    m_ops->setDirectoryExists(targetDir.path(), true);
    QVERIFY(targetDir.mkpath(QStringLiteral("entry")));
    {
        QFile f(targetDir.filePath(QStringLiteral("entry/keep.txt")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("keep");
    }

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("MirrorDirectoryContents"),
                                                   QStringLiteral("player1"),
                                                   stagingDir.path(),
                                                   QStringLiteral("Games/MyGame"));

    QVERIFY(!reply.isValid());
    // The conflicting directory is untouched
    QVERIFY(QFileInfo(targetDir.filePath(QStringLiteral("entry/keep.txt"))).exists());
}

void TestCouchPlayHelper::testMirrorDirectoryContentsTargetNotExists()
{
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, QStringLiteral("/home/player1"));
    m_ops->setFileExists(QStringLiteral("/home/compositor/staging"), true);
    m_ops->setDirectoryExists(QStringLiteral("/home/compositor/staging"), true);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("MirrorDirectoryContents"),
                                                   QStringLiteral("player1"),
                                                   QStringLiteral("/home/compositor/staging"),
                                                   QStringLiteral("Games/MyGame")); // not mocked -> missing

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
    QCOMPARE(m_ops->m_processInvocations.size(), 0);
}

void TestCouchPlayHelper::testMirrorDirectoryContentsTraversalTarget()
{
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, QStringLiteral("/home/player1"));
    m_ops->setFileExists(QStringLiteral("/home/compositor/staging"), true);
    m_ops->setDirectoryExists(QStringLiteral("/home/compositor/staging"), true);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("MirrorDirectoryContents"),
                                                   QStringLiteral("player1"),
                                                   QStringLiteral("/home/compositor/staging"),
                                                   QStringLiteral("../escape"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
    QCOMPARE(m_ops->m_processInvocations.size(), 0);
}

void TestCouchPlayHelper::testIsPathWithinAllowedPrefixMountRoots()
{
    // Must accept the same mount roots SetPathAclWithParents treats as
    // traversal stop boundaries, so external libraries keep working
    QVERIFY(m_helper->isPathWithinAllowedPrefix(QStringLiteral("/home/player1/games")));
    QVERIFY(m_helper->isPathWithinAllowedPrefix(QStringLiteral("/var/home/player1/games")));
    QVERIFY(m_helper->isPathWithinAllowedPrefix(QStringLiteral("/run/media/user/disk/steamlibrary")));
    QVERIFY(m_helper->isPathWithinAllowedPrefix(QStringLiteral("/media/steamlibrary")));
    QVERIFY(m_helper->isPathWithinAllowedPrefix(QStringLiteral("/mnt/steamlibrary")));
    QVERIFY(m_helper->isPathWithinAllowedPrefix(QStringLiteral("/tmp/couchplay/share")));

    QVERIFY(!m_helper->isPathWithinAllowedPrefix(QStringLiteral("/etc")));
    QVERIFY(!m_helper->isPathWithinAllowedPrefix(QStringLiteral("/usr/share/games")));
    QVERIFY(!m_helper->isPathWithinAllowedPrefix(QStringLiteral("/home"))); // root itself, not a subdir
    QVERIFY(!m_helper->isPathWithinAllowedPrefix(QStringLiteral("/home/a/../b")));
    QVERIFY(!m_helper->isPathWithinAllowedPrefix(QString()));

    // ".." inside a name is fine; a whole ".." component is traversal
    QVERIFY(m_helper->isPathWithinAllowedPrefix(QStringLiteral("/home/deck/Games/Foo..Bar")));
    QVERIFY(m_helper->isPathWithinAllowedPrefix(QStringLiteral("/mnt/library..2/saves")));
    QVERIFY(!m_helper->isPathWithinAllowedPrefix(QStringLiteral("/home/deck/Games/..")));
    QVERIFY(!m_helper->isPathWithinAllowedPrefix(QStringLiteral("/tmp/a/../../etc")));
}

void TestCouchPlayHelper::testComputeMountTargetDotDotNames()
{
    // Source validation accepts names containing ".."; mount-target
    // computation must agree instead of rejecting them (whole ".."
    // components are still traversal)
    const QString userHome = QStringLiteral("/home/player1");
    const QString compositorHome = QStringLiteral("/home/deck");

    QCOMPARE(m_helper->computeMountTarget(QStringLiteral("/home/deck/Games/Foo..Bar"), QString(), userHome, compositorHome),
             QStringLiteral("/home/player1/Games/Foo..Bar"));
    QCOMPARE(m_helper->computeMountTarget(QStringLiteral("/mnt/lib..2"),
                                          QStringLiteral("shares/Foo..Bar"),
                                          userHome,
                                          compositorHome),
             QStringLiteral("/home/player1/shares/Foo..Bar"));
    QCOMPARE(m_helper->computeMountTarget(QStringLiteral("/mnt/lib..2/save"), QString(), userHome, compositorHome),
             QStringLiteral("/home/player1/.couchplay/mounts/mnt/lib..2/save"));

    QVERIFY(m_helper->computeMountTarget(QStringLiteral("/home/deck/../etc"), QString(), userHome, compositorHome).isEmpty());
    QVERIFY(
        m_helper->computeMountTarget(QStringLiteral("/home/deck/games"), QStringLiteral("../escape"), userHome, compositorHome)
            .isEmpty());

    // An alias or source that normalizes to the home itself would attach the
    // shared source over the player's entire home
    QVERIFY(m_helper->computeMountTarget(QStringLiteral("/home/deck/games"), QStringLiteral("."), userHome, compositorHome)
                .isEmpty());
    QVERIFY(
        m_helper->computeMountTarget(QStringLiteral("/home/deck/games"), QStringLiteral("./"), userHome, compositorHome)
            .isEmpty());
    QVERIFY(m_helper->computeMountTarget(QStringLiteral("/home/deck/games"), QStringLiteral("/"), userHome, compositorHome)
                .isEmpty());
    QVERIFY(m_helper->computeMountTarget(QStringLiteral("/home/deck/."), QString(), userHome, compositorHome).isEmpty());
    // A trailing "/." deeper down is a valid descendant once normalized
    QCOMPARE(m_helper->computeMountTarget(QStringLiteral("/home/deck/games"), QStringLiteral("shares/."),
                                          userHome, compositorHome),
             QStringLiteral("/home/player1/shares"));
}

void TestCouchPlayHelper::testSetPathAclWithParentsSymlinkEscapeRejected()
{
    // setfacl follows symlinks: an allowed-prefix path resolving outside the
    // allowed roots must be refused before any ACL is applied
    m_ops->clear();
    m_ops->setMockProcessStart(true);
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, QStringLiteral("/home/player1"));
    m_ops->setFileExists(QStringLiteral("/home/deck/shared-link"), true);
    m_ops->setDirectoryExists(QStringLiteral("/home/deck/shared-link"), true);
    m_ops->setCanonicalMapping(QStringLiteral("/home/deck/shared-link"), QStringLiteral("/root/private"));

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("SetPathAclWithParents"),
                                                   QStringLiteral("/home/deck/shared-link"),
                                                   QStringLiteral("player1"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
    QCOMPARE(m_ops->m_processInvocations.size(), 0);
}

void TestCouchPlayHelper::testSetDirectoryAclSymlinkEscapeRejected()
{
    m_ops->clear();
    m_ops->setMockProcessStart(true);
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, QStringLiteral("/home/player1"));
    m_ops->setFileExists(QStringLiteral("/home/deck/shared-link"), true);
    m_ops->setDirectoryExists(QStringLiteral("/home/deck/shared-link"), true);
    m_ops->setCanonicalMapping(QStringLiteral("/home/deck/shared-link"), QStringLiteral("/root/private"));

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("SetDirectoryAcl"),
                                                   QStringLiteral("/home/deck/shared-link"),
                                                   QStringLiteral("player1"),
                                                   false);

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
    QCOMPARE(m_ops->m_processInvocations.size(), 0);
}
void TestCouchPlayHelper::testSecureWriteRejectsSymlinkLeaf()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString protectedFile = tempDir.path() + QStringLiteral("/protected");
    QFile file(protectedFile);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write("original") == 8);
    file.close();
    QVERIFY(QFile::link(protectedFile, tempDir.path() + QStringLiteral("/target")));

    const int parentFd = SecureFs::openBaseDir(tempDir.path());
    QVERIFY(parentFd >= 0);
    QCOMPARE(SecureFs::writeFileAt(parentFd,
                                   QStringLiteral("target"),
                                   QByteArrayLiteral("replacement"),
                                   getuid(),
                                   getgid()),
             -ELOOP);
    ::close(parentFd);

    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArrayLiteral("original"));
}

void TestCouchPlayHelper::testMountSpecCodec()
{
    // Round-trips: pipes, backslashes and aliases must survive the wire
    struct Case {
        QString source;
        QString alias;
    };
    const QList<Case> cases = {
        {QStringLiteral("/home/compositor/.config/game"), QString()},
        {QStringLiteral("/mnt/Game|Saves"), QString()},
        {QStringLiteral("/mnt/Game|Saves"), QStringLiteral("shares/game|x")},
        {QStringLiteral("/data\\set"), QStringLiteral("alias")},
        {QStringLiteral("/both|and\\mix"), QStringLiteral("a|b")},
    };
    for (const Case &c : cases) {
        QString source;
        QString alias;
        QVERIFY2(decodeMountSpec(encodeMountSpec(c.source, c.alias), source, alias),
                 qPrintable(encodeMountSpec(c.source, c.alias)));
        QCOMPARE(source, c.source);
        QCOMPARE(alias, c.alias);
    }

    // Legacy unescaped specs decode unchanged
    QString source;
    QString alias;
    QVERIFY(decodeMountSpec(QStringLiteral("/plain/path|"), source, alias));
    QCOMPARE(source, QStringLiteral("/plain/path"));
    QCOMPARE(alias, QString());
    QVERIFY(decodeMountSpec(QStringLiteral("/src|shares/game"), source, alias));
    QCOMPARE(source, QStringLiteral("/src"));
    QCOMPARE(alias, QStringLiteral("shares/game"));
    QVERIFY(decodeMountSpec(QStringLiteral("/lone\\backslash|x"), source, alias));
    QCOMPARE(source, QStringLiteral("/lone\\backslash"));
    QCOMPARE(alias, QStringLiteral("x"));

    // More than one unescaped separator is malformed
    QVERIFY(!decodeMountSpec(QStringLiteral("/mnt/Game|Saves|extra"), source, alias));
}

void TestCouchPlayHelper::testUnmountRetainsFailedMounts()
{
    // A failed unmount (persistent EBUSY, post-restart stale path…) must keep
    // the MountInfo AND its pinned FD, so later teardown or destruction can
    // retry instead of orphaning a root mount nobody can clean up anymore
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("target")));

    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, QStringLiteral("/home/player1"));

    // FD-pinned entry whose target is not actually a mount point: the FD
    // umount fails (EINVAL) and the entry must survive with its pinned FD
    const int fd = ::open(QFile::encodeName(dir.path()).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(fd >= 0);

    CouchPlayHelper::MountInfo info;
    info.source = QStringLiteral("/home/compositor/games");
    info.target = dir.filePath(QStringLiteral("target"));
    info.mountType = QStringLiteral("bind");
    info.targetParentFd = fd;
    info.targetLeaf = QStringLiteral("target");
    m_helper->m_activeMounts[QStringLiteral("player1")].append(info);

    QDBusReply<int> reply = m_dbusInterface->call(QStringLiteral("UnmountAllSharedDirectories"));
    QVERIFY(reply.isValid());
    QCOMPARE(reply.value(), 0);
    QVERIFY(m_helper->m_activeMounts.contains(QStringLiteral("player1")));
    QCOMPARE(m_helper->m_activeMounts[QStringLiteral("player1")].size(), 1);
    const int pinnedFd = m_helper->m_activeMounts[QStringLiteral("player1")].first().targetParentFd;
    QVERIFY(pinnedFd >= 0);
    QCOMPARE(::fcntl(pinnedFd, F_GETFD) != -1, true); // FD stays open for a later retry

    // Path-based entry (post-restart state) with a failing umount: retained too
    m_ops->setMockProcessStart(true);
    m_ops->setProcessExitCode(1);
    CouchPlayHelper::MountInfo pathInfo;
    pathInfo.source = QStringLiteral("/home/compositor/other");
    pathInfo.target = dir.filePath(QStringLiteral("other"));
    pathInfo.mountType = QStringLiteral("bind");
    m_helper->m_activeMounts[QStringLiteral("player1")].append(pathInfo);

    QDBusReply<int> userReply = m_dbusInterface->call(QStringLiteral("UnmountSharedDirectories"),
                                                      QStringLiteral("player1"));
    QVERIFY(userReply.isValid());
    QCOMPARE(userReply.value(), 0);
    QCOMPARE(m_helper->m_activeMounts[QStringLiteral("player1")].size(), 2);

    // Cleanup so the helper destructor doesn't retry these fake mounts
    ::close(m_helper->m_activeMounts[QStringLiteral("player1")].first().targetParentFd);
    m_helper->m_activeMounts.clear();
}

void TestCouchPlayHelper::testChangeDeviceOwnerInvalidPathNotUnderDevInput()
{
    m_ops->clear();

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("ChangeDeviceOwner"),
                                                   QStringLiteral("/dev/sda"), // Not under /dev/input/
                                                   1000u);

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testChangeDeviceOwnerInvalidPathTraversal()
{
    m_ops->clear();

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("ChangeDeviceOwner"),
                                                   QStringLiteral("/dev/input/../sda"), // Path traversal
                                                   1000u);

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testChangeDeviceOwnerInvalidPathNotExists()
{
    m_ops->clear();

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("ChangeDeviceOwner"),
                                                   QStringLiteral("/dev/input/event0"), // Doesn't exist
                                                   1000u);

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testChangeDeviceOwnerInvalidPathNotCharDevice()
{
    m_ops->clear();
    m_ops->setFileExists(QStringLiteral("/dev/input/event0"), true);
    m_ops->setChownResult(0);
    m_ops->setChmodResult(0);

    QDBusReply<bool> reply =
        m_dbusInterface->call(QStringLiteral("ChangeDeviceOwner"), QStringLiteral("/dev/input/event0"), 1000u);

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testChangeDeviceOwnerSuccess()
{
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("testuser"), true, 1000, 1000);
    m_ops->setFileExists(QStringLiteral("/dev/input/event0"), true);
    m_ops->setChownResult(0);
    m_ops->setChmodResult(0);

    QDBusReply<bool> reply =
        m_dbusInterface->call(QStringLiteral("ChangeDeviceOwner"), QStringLiteral("/dev/input/event0"), 1000u);

    QVERIFY(reply.isValid());
    QVERIFY(reply.value());
}

void TestCouchPlayHelper::testChangeDeviceOwnerAuthorizationDenied()
{
    m_ops->clear();
    m_ops->setAuthResult(false);
    m_ops->setFileExists(QStringLiteral("/dev/input/event0"), true);
    m_ops->setChownResult(0);
    m_ops->setChmodResult(0);

    QDBusReply<bool> reply =
        m_dbusInterface->call(QStringLiteral("ChangeDeviceOwner"), QStringLiteral("/dev/input/event0"), 1000u);

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::AccessDenied);
}

void TestCouchPlayHelper::testChangeDeviceOwnerUserNotFound()
{
    m_ops->clear();
    m_ops->setFileExists(QStringLiteral("/dev/input/event0"), true);
    m_ops->setChownResult(0);
    m_ops->setChmodResult(0);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("ChangeDeviceOwner"),
                                                   QStringLiteral("/dev/input/event0"),
                                                   9999u // Nonexistent UID
    );

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testChangeDeviceOwnerChownFails()
{
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("testuser"), true, 1000, 1000);
    m_ops->setFileExists(QStringLiteral("/dev/input/event0"), true);
    m_ops->setChownResult(-1);

    QDBusReply<bool> reply =
        m_dbusInterface->call(QStringLiteral("ChangeDeviceOwner"), QStringLiteral("/dev/input/event0"), 1000u);

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::Failed);
}

void TestCouchPlayHelper::testChangeDeviceOwnerChmodFails()
{
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("testuser"), true, 1000, 1000);
    m_ops->setFileExists(QStringLiteral("/dev/input/event0"), true);
    m_ops->setChownResult(0);
    m_ops->setChmodResult(-1);

    QDBusReply<bool> reply =
        m_dbusInterface->call(QStringLiteral("ChangeDeviceOwner"), QStringLiteral("/dev/input/event0"), 1000u);

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::Failed);
}

void TestCouchPlayHelper::testResetDeviceOwnerSuccess()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setFileExists(QStringLiteral("/dev/input/event0"), true);
    m_ops->setChownResult(0);
    m_ops->setChmodResult(0);

    QDBusReply<bool> reply =
        m_dbusInterface->call(QStringLiteral("ResetDeviceOwner"), QStringLiteral("/dev/input/event0"));

    QVERIFY(reply.isValid());
    QVERIFY(reply.value());
}

void TestCouchPlayHelper::testResetDeviceOwnerInvalidPathNotUnderDevInput()
{
    m_ops->clear();

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("ResetDeviceOwner"), QStringLiteral("/dev/sda"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testResetDeviceOwnerInvalidPathTraversal()
{
    m_ops->clear();

    QDBusReply<bool> reply =
        m_dbusInterface->call(QStringLiteral("ResetDeviceOwner"), QStringLiteral("/dev/input/../sda"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testResetDeviceOwnerAuthorizationDenied()
{
    m_ops->clear();
    m_ops->setAuthResult(false);
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setFileExists(QStringLiteral("/dev/input/event0"), true);
    m_ops->setChownResult(0);
    m_ops->setChmodResult(0);

    QDBusReply<bool> reply =
        m_dbusInterface->call(QStringLiteral("ResetDeviceOwner"), QStringLiteral("/dev/input/event0"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::AccessDenied);
}

void TestCouchPlayHelper::testResetDeviceOwnerChownFails()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setFileExists(QStringLiteral("/dev/input/event0"), true);
    m_ops->setChownResult(-1);

    QDBusReply<bool> reply =
        m_dbusInterface->call(QStringLiteral("ResetDeviceOwner"), QStringLiteral("/dev/input/event0"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::Failed);
}

void TestCouchPlayHelper::testResetDeviceOwnerChmodFails()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setFileExists(QStringLiteral("/dev/input/event0"), true);
    m_ops->setChownResult(0);
    m_ops->setChmodResult(-1);

    QDBusReply<bool> reply =
        m_dbusInterface->call(QStringLiteral("ResetDeviceOwner"), QStringLiteral("/dev/input/event0"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::Failed);
}

void TestCouchPlayHelper::testResetAllDevicesEmpty()
{
    m_ops->clear();

    QDBusReply<int> reply = m_dbusInterface->call(QStringLiteral("ResetAllDevices"));

    QVERIFY(reply.isValid());
    QCOMPARE(reply.value(), 0);
}

void TestCouchPlayHelper::testResetAllDevicesSuccess()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setUserExists(QStringLiteral("testuser1"), true, 1000, 1000);
    m_ops->setUserExists(QStringLiteral("testuser2"), true, 1001, 1001);
    m_ops->setFileExists(QStringLiteral("/dev/input/event0"), true);
    m_ops->setFileExists(QStringLiteral("/dev/input/event1"), true);
    m_ops->setChownResult(0);
    m_ops->setChmodResult(0);

    QDBusReply<bool> reply1 =
        m_dbusInterface->call(QStringLiteral("ChangeDeviceOwner"), QStringLiteral("/dev/input/event0"), 1000u);
    QDBusReply<bool> reply2 =
        m_dbusInterface->call(QStringLiteral("ChangeDeviceOwner"), QStringLiteral("/dev/input/event1"), 1001u);

    QVERIFY(reply1.isValid() && reply2.isValid());

    QDBusReply<int> resetReply = m_dbusInterface->call(QStringLiteral("ResetAllDevices"));

    QVERIFY(resetReply.isValid());
    QCOMPARE(resetReply.value(), 2);
}

void TestCouchPlayHelper::testResetAllDevicesPartialFailure()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setUserExists(QStringLiteral("testuser1"), true, 1000, 1000);
    m_ops->setUserExists(QStringLiteral("testuser2"), true, 1001, 1001);
    m_ops->setFileExists(QStringLiteral("/dev/input/event0"), true);
    m_ops->setFileExists(QStringLiteral("/dev/input/event1"), true);
    m_ops->setChownResult(0);
    m_ops->setChmodResult(0);

    QDBusReply<bool> reply1 =
        m_dbusInterface->call(QStringLiteral("ChangeDeviceOwner"), QStringLiteral("/dev/input/event0"), 1000u);
    QDBusReply<bool> reply2 =
        m_dbusInterface->call(QStringLiteral("ChangeDeviceOwner"), QStringLiteral("/dev/input/event1"), 1001u);

    QVERIFY(reply1.isValid() && reply2.isValid());

    m_ops->setChownResult(-1); // Make next chown fail

    QDBusReply<int> resetReply = m_dbusInterface->call(QStringLiteral("ResetAllDevices"));

    QVERIFY(!resetReply.isValid());
    QCOMPARE(resetReply.error().type(), QDBusError::Failed);
}

void TestCouchPlayHelper::testChangeDeviceOwnerBatchEmpty()
{
    m_ops->clear();

    QDBusReply<int> reply = m_dbusInterface->call(QStringLiteral("ChangeDeviceOwnerBatch"), QStringList(), 1000u);

    QVERIFY(reply.isValid());
    QCOMPARE(reply.value(), 0);
}

void TestCouchPlayHelper::testChangeDeviceOwnerBatchAllSuccess()
{
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("testuser"), true, 1000, 1000);
    m_ops->setFileExists(QStringLiteral("/dev/input/event0"), true);
    m_ops->setFileExists(QStringLiteral("/dev/input/event1"), true);
    m_ops->setFileExists(QStringLiteral("/dev/input/event2"), true);
    m_ops->setChownResult(0);
    m_ops->setChmodResult(0);

    QStringList paths = {QStringLiteral("/dev/input/event0"),
                         QStringLiteral("/dev/input/event1"),
                         QStringLiteral("/dev/input/event2")};

    QDBusReply<int> reply = m_dbusInterface->call(QStringLiteral("ChangeDeviceOwnerBatch"), paths, 1000u);

    QVERIFY(reply.isValid());
    QCOMPARE(reply.value(), 3);
}

void TestCouchPlayHelper::testChangeDeviceOwnerBatchPartialFailure()
{
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("testuser"), true, 1000, 1000);
    m_ops->setFileExists(QStringLiteral("/dev/input/event0"), true);
    m_ops->setChownResult(0);
    m_ops->setChmodResult(0);

    // Clear any existing modified devices before this test
    QDBusReply<int> clearReply = m_dbusInterface->call(QStringLiteral("ResetAllDevices"));

    QStringList paths = {QStringLiteral("/dev/input/event0"), QStringLiteral("/dev/sda")};

    QDBusReply<int> reply = m_dbusInterface->call(QStringLiteral("ChangeDeviceOwnerBatch"), paths, 1000u);

    QVERIFY(!reply.isValid());
}

void TestCouchPlayHelper::testChangeDeviceOwnerBatchAllFailure()
{
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("testuser"), true, 1000, 1000);

    QStringList paths = {QStringLiteral("/dev/sda"), QStringLiteral("/dev/sdb")};

    QDBusReply<int> reply = m_dbusInterface->call(QStringLiteral("ChangeDeviceOwnerBatch"), paths, 1000u);

    QVERIFY(!reply.isValid());
}

void TestCouchPlayHelper::testSetupRuntimeAccessSuccess()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setUserExists(QStringLiteral("compositor"), true, 1000, 1000, QStringLiteral("/home/compositor"));
    m_ops->setFileExists(QStringLiteral("/run/user/1000"), true);
    m_ops->setFileExists(QStringLiteral("/run/user/1000/wayland-0"), true);
    m_ops->setFileExists(QStringLiteral("/run/user/1000/bus"), true);
    m_ops->setProcessExitCode(0);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("SetupRuntimeAccess"));

    QVERIFY(reply.isValid());
    QVERIFY(reply.value());
}

void TestCouchPlayHelper::testSetupRuntimeAccessAuthorizationDenied()
{
    m_ops->clear();
    m_ops->setAuthResult(false);
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setUserExists(QStringLiteral("compositor"), true, 1000, 1000, QStringLiteral("/home/compositor"));
    m_ops->setDirectoryExists(QStringLiteral("/run/user/1000"), true);
    m_ops->setChownResult(0);
    m_ops->setChmodResult(0);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("SetupRuntimeAccess"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::AccessDenied);
}

void TestCouchPlayHelper::testSetupRuntimeAccessUserNotFound()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("SetupRuntimeAccess"));



    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);

}

void TestCouchPlayHelper::testRemoveRuntimeAccessSuccess()
{

    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setUserExists(QStringLiteral("compositor"), true, 1000, 1000, QStringLiteral("/home/compositor"));
    m_ops->setFileExists(QStringLiteral("/run/user/1000"), true);
    m_ops->setFileExists(QStringLiteral("/run/user/1000/wayland-0"), true);
    m_ops->setFileExists(QStringLiteral("/run/user/1000/bus"), true);
    m_ops->setProcessExitCode(0);

    QDBusReply<bool> setupReply = m_dbusInterface->call(QStringLiteral("SetupRuntimeAccess"));
    QVERIFY(setupReply.isValid());

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("RemoveRuntimeAccess"));

    QVERIFY(reply.isValid());
    QVERIFY(reply.value());
}

void TestCouchPlayHelper::testRemoveRuntimeAccessAuthorizationDenied()
{
    m_ops->clear();
    m_ops->setAuthResult(false);
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setUserExists(QStringLiteral("compositor"), true, 1000, 1000, QStringLiteral("/home/compositor"));
    m_ops->setDirectoryExists(QStringLiteral("/run/user/1000"), true);
    m_ops->setChownResult(0);
    m_ops->setChmodResult(0);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("RemoveRuntimeAccess"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::AccessDenied);
}

void TestCouchPlayHelper::testRemoveRuntimeAccessUserNotFound()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setProcessExitCode(0);
    // Compositor user does not exist, but RemoveRuntimeAccess doesn't check
    // It just runs setfacl commands and returns success if all paths don't exist

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("RemoveRuntimeAccess"));



    // Method returns success even if user doesn't exist
    // (it just runs setfacl on non-existent paths which is a no-op)
    QVERIFY(reply.isValid());
    QVERIFY(reply.value());
}

void TestCouchPlayHelper::testGenerateServiceName()
{
    QCOMPARE(m_helper->generateServiceName(QStringLiteral("player1")), QStringLiteral("couchplay-player1.service"));
    QCOMPARE(m_helper->generateServiceName(QStringLiteral("test_user")), QStringLiteral("couchplay-test_user.service"));
    QCOMPARE(m_helper->generateServiceName(QStringLiteral("a")), QStringLiteral("couchplay-a.service"));
}

void TestCouchPlayHelper::testLaunchInstance_basicLaunch()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {QStringLiteral("player1")});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001);
    m_ops->setFileExists(QStringLiteral("/run/user/1001"), true);
    m_ops->setFileExists(QStringLiteral("/run/user/1001/bus"), true);
    m_ops->setUserExists(QStringLiteral("compositor"), true, 1000, 1000, QStringLiteral("/home/compositor"));
    m_ops->setFileExists(QStringLiteral("/run/user/1000"), true);
    m_ops->setFileExists(QStringLiteral("/run/user/1000/wayland-0"), true);
    m_ops->setFileExists(QStringLiteral("/run/user/1000/bus"), true);
    m_ops->setFileExists(QStringLiteral("/usr/bin/gamescope"), true);
    m_ops->setProcessExitCode(0);
    m_ops->setMockProcessStart(true);
    m_ops->setStandardOutput(QByteArray("12345\n"));

    QDBusReply<qint64> reply = m_dbusInterface->call(QStringLiteral("LaunchInstance"),
                                                     QStringLiteral("player1"),
                                                     QString(),
                                                     QStringList{QStringLiteral("-W"), QStringLiteral("960")},
                                                     QStringList{QStringLiteral("steam"), QStringLiteral("-bigpicture")},
                                                     QString(),
                                                     QStringList(),
                                                     QStringList{QStringLiteral("ENABLE_GAMESCOPE_WSI=1")},
                                                     QStringList());

    QVERIFY(reply.isValid());
    QCOMPARE(reply.value(), qint64(12345));

    bool foundSystemdRun = false;
    for (const auto &inv : m_ops->m_processInvocations) {
        if (inv.command == QStringLiteral("systemd-run")) {
            foundSystemdRun = true;
            QVERIFY(inv.args.contains(QStringLiteral("--unit")));
            QVERIFY(inv.args.contains(QStringLiteral("couchplay-player1.service")));
            QVERIFY(inv.args.contains(QStringLiteral("--uid")));
            QVERIFY(inv.args.contains(QStringLiteral("player1")));
            QVERIFY(!inv.args.contains(QStringLiteral("--property=Environment=")));
            QVERIFY(inv.args.contains(QStringLiteral("-E")));
            QVERIFY(inv.args.contains(QStringLiteral("/usr/bin/gamescope")));
            QVERIFY(inv.args.contains(QStringLiteral("-W")));
            QVERIFY(inv.args.contains(QStringLiteral("960")));
            QVERIFY(inv.args.contains(QStringLiteral("steam")));
            QVERIFY(inv.args.contains(QStringLiteral("-bigpicture")));
            QVERIFY(!inv.args.contains(QStringLiteral("-c")));
            QVERIFY(!inv.args.contains(QStringLiteral("/bin/bash")));
            break;
        }
    }
    QVERIFY2(foundSystemdRun, "systemd-run should be called");

    bool foundSystemctlShow = false;
    for (const auto &inv : m_ops->m_processInvocations) {
        if (inv.command == QStringLiteral("systemctl") && inv.args.contains(QStringLiteral("show"))
            && inv.args.contains(QStringLiteral("--value"))) {
            foundSystemctlShow = true;
            QVERIFY(inv.args.contains(QStringLiteral("couchplay-player1.service")));
            QVERIFY(inv.args.contains(QStringLiteral("MainPID")));
            break;
        }
    }
    QVERIFY2(foundSystemctlShow, "systemctl show should be called to get MainPID");
}

void TestCouchPlayHelper::testLaunchInstance_withBindPaths()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {QStringLiteral("player1")});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001);
    m_ops->setFileExists(QStringLiteral("/run/user/1001"), true);
    m_ops->setFileExists(QStringLiteral("/run/user/1001/bus"), true);
    m_ops->setUserExists(QStringLiteral("compositor"), true, 1000, 1000, QStringLiteral("/home/compositor"));
    m_ops->setFileExists(QStringLiteral("/run/user/1000"), true);
    m_ops->setFileExists(QStringLiteral("/run/user/1000/wayland-0"), true);
    m_ops->setProcessExitCode(0);
    m_ops->setMockProcessStart(true);
    m_ops->setStandardOutput(QByteArray("54321\n"));

    QStringList bindPaths = {QStringLiteral("/tmp/overrides/config.ini:/home/player1/games/config.ini")};

    QDBusReply<qint64> reply = m_dbusInterface->call(QStringLiteral("LaunchInstance"),
                                                     QStringLiteral("player1"),
                                                     QString(),
                                                     QStringList{},
                                                     QStringList{QStringLiteral("steam")},
                                                     QString(),
                                                     QStringList{QStringLiteral("/home/player1/games")},
                                                     QStringList{},
                                                     bindPaths);

    QVERIFY(reply.isValid());
    QCOMPARE(reply.value(), qint64(54321));

    bool foundBindPaths = false;
    for (const auto &inv : m_ops->m_processInvocations) {
        if (inv.command == QStringLiteral("systemd-run")) {
            for (const QString &arg : inv.args) {
                if (arg.contains(QStringLiteral("BindReadOnlyPaths=/tmp/overrides/config.ini:/home/player1/games/config.ini"))) {
                    foundBindPaths = true;
                    break;
                }
            }
        }
    }
    QVERIFY2(foundBindPaths, "BindPaths property should be set in systemd-run args");
}

void TestCouchPlayHelper::testLaunchInstance_validationEmptyUsername()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});

    QDBusReply<qint64> reply = m_dbusInterface->call(QStringLiteral("LaunchInstance"),
                                                     QString(),
                                                     QString(),
                                                     QStringList{},
                                                     QStringList{QStringLiteral("steam")},
                                                     QString(),
                                                     QStringList(),
                                                     QStringList{},
                                                     QStringList());

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testLaunchInstance_validationNonexistentUser()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});

    QDBusReply<qint64> reply = m_dbusInterface->call(QStringLiteral("LaunchInstance"),
                                                     QStringLiteral("nonexistent"),
                                                     QString(),
                                                     QStringList{},
                                                     QStringList{QStringLiteral("steam")},
                                                     QString(),
                                                     QStringList(),
                                                     QStringList{},
                                                     QStringList{});

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testStopInstance_serviceStop()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {QStringLiteral("player1")});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001);
    m_ops->setFileExists(QStringLiteral("/run/user/1001"), true);
    m_ops->setFileExists(QStringLiteral("/run/user/1001/bus"), true);
    m_ops->setUserExists(QStringLiteral("compositor"), true, 1000, 1000, QStringLiteral("/home/compositor"));
    m_ops->setFileExists(QStringLiteral("/run/user/1000"), true);
    m_ops->setFileExists(QStringLiteral("/run/user/1000/wayland-0"), true);
    m_ops->setFileExists(QStringLiteral("/run/user/1000/bus"), true);
    m_ops->setProcessExitCode(0);
    m_ops->setMockProcessStart(true);
    m_ops->setStandardOutput(QByteArray("99999\n"));

    QDBusReply<qint64> launchReply = m_dbusInterface->call(QStringLiteral("LaunchInstance"),
                                                           QStringLiteral("player1"),
                                                           QString(),
                                                           QStringList{},
                                                           QStringList{QStringLiteral("steam")},
                                                           QString(),
                                                           QStringList(),
                                                           QStringList{},
                                                           QStringList{});
    QVERIFY(launchReply.isValid());
    qint64 pid = launchReply.value();
    QVERIFY(pid > 0);

    m_ops->m_processInvocations.clear();

    QDBusReply<bool> stopReply = m_dbusInterface->call(QStringLiteral("StopInstance"), pid);

    QVERIFY(stopReply.isValid());
    QVERIFY(stopReply.value());

    bool foundStop = false;
    bool foundResetFailed = false;
    for (const auto &inv : m_ops->m_processInvocations) {
        if (inv.command == QStringLiteral("systemctl") && inv.args.contains(QStringLiteral("stop"))
            && inv.args.contains(QStringLiteral("couchplay-player1.service"))) {
            foundStop = true;
        }
        if (inv.command == QStringLiteral("systemctl") && inv.args.contains(QStringLiteral("reset-failed"))
            && inv.args.contains(QStringLiteral("couchplay-player1.service"))) {
            foundResetFailed = true;
        }
    }
    QVERIFY2(foundStop, "systemctl stop should be called for the service");
    QVERIFY2(foundResetFailed, "systemctl reset-failed should be called after stop");
}

void TestCouchPlayHelper::testKillInstance_serviceKill()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {QStringLiteral("player1")});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001);
    m_ops->setFileExists(QStringLiteral("/run/user/1001"), true);
    m_ops->setFileExists(QStringLiteral("/run/user/1001/bus"), true);
    m_ops->setUserExists(QStringLiteral("compositor"), true, 1000, 1000, QStringLiteral("/home/compositor"));
    m_ops->setFileExists(QStringLiteral("/run/user/1000"), true);
    m_ops->setFileExists(QStringLiteral("/run/user/1000/wayland-0"), true);
    m_ops->setFileExists(QStringLiteral("/run/user/1000/bus"), true);
    m_ops->setProcessExitCode(0);
    m_ops->setMockProcessStart(true);
    m_ops->setStandardOutput(QByteArray("77777\n"));

    QDBusReply<qint64> launchReply = m_dbusInterface->call(QStringLiteral("LaunchInstance"),
                                                           QStringLiteral("player1"),
                                                           QString(),
                                                           QStringList{},
                                                           QStringList{QStringLiteral("steam")},
                                                           QString(),
                                                           QStringList(),
                                                           QStringList{},
                                                           QStringList{});
    QVERIFY(launchReply.isValid());
    qint64 pid = launchReply.value();
    QVERIFY(pid > 0);

    m_ops->m_processInvocations.clear();

    QDBusReply<bool> killReply = m_dbusInterface->call(QStringLiteral("KillInstance"), pid);

    QVERIFY(killReply.isValid());
    QVERIFY(killReply.value());

    bool foundSigkill = false;
    for (const auto &inv : m_ops->m_processInvocations) {
        if (inv.command == QStringLiteral("systemctl") && inv.args.contains(QStringLiteral("kill"))
            && inv.args.contains(QStringLiteral("--signal=SIGKILL"))
            && inv.args.contains(QStringLiteral("couchplay-player1.service"))) {
            foundSigkill = true;
        }
    }
    QVERIFY2(foundSigkill, "systemctl kill --signal=SIGKILL should be called");
}

void TestCouchPlayHelper::testStopInstance_fallbackToDirectKill()
{
    m_ops->clear();
    m_helper->m_pidToUsername.clear();
    m_helper->m_pidOwnerUid.clear();
    m_helper->m_usernameToUnitName.clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setProcessExitCode(0);
    m_ops->setMockProcessStart(true);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("StopInstance"), qint64(99999));
    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);

}

void TestCouchPlayHelper::testLaunchInstance_staleUnitRecovery()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {QStringLiteral("player1")});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001);
    m_ops->setFileExists(QStringLiteral("/run/user/1001"), true);
    m_ops->setFileExists(QStringLiteral("/run/user/1001/bus"), true);
    m_ops->setUserExists(QStringLiteral("compositor"), true, 1000, 1000, QStringLiteral("/home/compositor"));
    m_ops->setFileExists(QStringLiteral("/run/user/1000"), true);
    m_ops->setFileExists(QStringLiteral("/run/user/1000/wayland-0"), true);
    m_ops->setFileExists(QStringLiteral("/run/user/1000/bus"), true);

    m_ops->setProcessExitCode(1);
    m_ops->setMockProcessStart(true);
    m_ops->setStandardError(QByteArray("Unit couchplay-player1.service already loaded"));
    m_ops->setStandardOutput(QByteArray(""));

    QDBusReply<qint64> reply = m_dbusInterface->call(QStringLiteral("LaunchInstance"),
                                                     QStringLiteral("player1"),
                                                     QString(),
                                                     QStringList{},
                                                     QStringList{QStringLiteral("steam")},
                                                     QString(),
                                                     QStringList(),
                                                     QStringList{},
                                                     QStringList{});

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::Failed);

    int systemdRunCount = 0;
    bool foundEnvEFlag = false;
    bool foundStop = false;
    bool foundResetFailed = false;
    for (const auto &inv : m_ops->m_processInvocations) {
        if (inv.command == QStringLiteral("systemd-run")) {
            systemdRunCount++;
            if (inv.args.contains(QStringLiteral("-E"))) {
                foundEnvEFlag = true;
            }
        }
        if (inv.command == QStringLiteral("systemctl")) {
            if (inv.args.contains(QStringLiteral("stop"))) {
                foundStop = true;
            }
            if (inv.args.contains(QStringLiteral("reset-failed"))) {
                foundResetFailed = true;
            }
        }
    }
    QVERIFY2(foundStop, "systemctl stop should be called during stale unit recovery");
    QVERIFY2(foundResetFailed, "systemctl reset-failed should be called during stale unit recovery");
    QVERIFY2(systemdRunCount >= 2, "systemd-run should be called at least twice (initial + retry)");
    QVERIFY2(foundEnvEFlag, "-E flag should be used for environment variables");
}

void TestCouchPlayHelper::testListCouchPlayUsers()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001,
                          {QStringLiteral("player1"), QStringLiteral("player2")});
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, QStringLiteral("/home/player1"));
    m_ops->setUserExists(QStringLiteral("player2"), true, 1002, 1001, QStringLiteral("/home/player2"));
    m_ops->setFileExists(QStringLiteral("/home/player1"), true);
    m_ops->setFileExists(QStringLiteral("/home/player2"), true);

    QDBusReply<QStringList> reply = m_dbusInterface->call(QStringLiteral("ListCouchPlayUsers"));

    QVERIFY(reply.isValid());
    const QStringList users = reply.value();
    QCOMPARE(users.size(), 2);
    QVERIFY(users.contains(QStringLiteral("player1\t1001\t1001\t/home/player1\t/bin/bash")));
    QVERIFY(users.contains(QStringLiteral("player2\t1002\t1001\t/home/player2\t/bin/bash")));
}

void TestCouchPlayHelper::testListCouchPlayUsersFilters()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001,
                          {QStringLiteral("lowuid"), QStringLiteral("nohome")});
    m_ops->setUserExists(QStringLiteral("lowuid"), true, 999, 1001, QStringLiteral("/home/lowuid"));
    m_ops->setFileExists(QStringLiteral("/home/lowuid"), true);
    m_ops->setUserExists(QStringLiteral("nohome"), true, 1003, 1001, QStringLiteral("/home/nohome"));

    QDBusReply<QStringList> reply = m_dbusInterface->call(QStringLiteral("ListCouchPlayUsers"));

    QVERIFY(reply.isValid());
    QVERIFY(reply.value().isEmpty());
}

void TestCouchPlayHelper::testGetUserInfo()
{
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, QStringLiteral("/home/player1"));

    QDBusReply<QVariantMap> reply = m_dbusInterface->call(QStringLiteral("GetUserInfo"), QStringLiteral("player1"));

    QVERIFY(reply.isValid());
    const QVariantMap info = reply.value();
    QCOMPARE(info.value(QStringLiteral("uid")).toUInt(), 1001u);
    QCOMPARE(info.value(QStringLiteral("gid")).toUInt(), 1001u);
    QCOMPARE(info.value(QStringLiteral("home")).toString(), QStringLiteral("/home/player1"));
}

void TestCouchPlayHelper::testGetUserInfoNotFound()
{
    m_ops->clear();

    QDBusReply<QVariantMap> reply = m_dbusInterface->call(QStringLiteral("GetUserInfo"), QStringLiteral("ghost"));

    QVERIFY(reply.isValid());
    QVERIFY(reply.value().isEmpty());
}

QTEST_MAIN(TestCouchPlayHelper)
#include "test_couchplayhelper.moc"
