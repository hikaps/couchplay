// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <QTest>
#include <QSignalSpy>

#include "UserManager.h"

class TestUserManager : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Username validation tests
    void testIsValidUsernameEmpty();
    void testIsValidUsernameTooLong();
    void testIsValidUsernameStartsWithNumber();
    void testIsValidUsernameStartsWithUpper();
    void testIsValidUsernameValidSimple();
    void testIsValidUsernameValidWithNumbers();
    void testIsValidUsernameValidWithUnderscore();
    void testIsValidUsernameValidWithDash();
    void testIsValidUsernameInvalidSpecialChars();
    void testIsValidUsernameInvalidSpaces();
    void testIsValidUsernameInvalidUppercase();

    // Current user tests
    void testCurrentUserNotEmpty();
    void testCurrentUserNotInList();  // New: current user should be excluded
    
    // Users list tests
    void testUsersListFormat();
    void testUsersListOnlyCouchPlayGroup();  // New: only couchplay group members
    
    // User exists tests
    void testUserExistsRoot();
    void testUserExistsNonexistent();
    void testUserExistsCurrent();
    
    // isInCouchPlayGroup tests
    void testIsInCouchPlayGroupNonexistent();
    void testIsInCouchPlayGroupRoot();
    
    // Create user tests
    void testCreateUserInvalidUsername();
    void testCreateUserAlreadyExists();
    void testCreateUserRequiresHelper();
    
    // Delete user tests
    void testDeleteUserInvalidUsername();
    void testDeleteUserNonexistent();
    void testDeleteUserCurrentUser();
    void testDeleteUserRequiresHelper();
    
    // Refresh tests
    void testRefreshEmitsSignal();

private:
    UserManager *m_manager = nullptr;
};

void TestUserManager::initTestCase()
{
}

void TestUserManager::cleanupTestCase()
{
}

void TestUserManager::init()
{
    m_manager = new UserManager();
}

void TestUserManager::cleanup()
{
    delete m_manager;
    m_manager = nullptr;
}

// ============ Username Validation Tests ============

void TestUserManager::testIsValidUsernameEmpty()
{
    QVERIFY(!m_manager->isValidUsername(QString()));
    QVERIFY(!m_manager->isValidUsername(QStringLiteral("")));
}

void TestUserManager::testIsValidUsernameTooLong()
{
    // 33 characters - too long
    QString longName = QStringLiteral("abcdefghijklmnopqrstuvwxyz1234567");
    QVERIFY(longName.length() > 32);
    QVERIFY(!m_manager->isValidUsername(longName));
}

void TestUserManager::testIsValidUsernameStartsWithNumber()
{
    QVERIFY(!m_manager->isValidUsername(QStringLiteral("1user")));
    QVERIFY(!m_manager->isValidUsername(QStringLiteral("2test")));
    QVERIFY(!m_manager->isValidUsername(QStringLiteral("0abc")));
}

void TestUserManager::testIsValidUsernameStartsWithUpper()
{
    QVERIFY(!m_manager->isValidUsername(QStringLiteral("User")));
    QVERIFY(!m_manager->isValidUsername(QStringLiteral("TEST")));
    QVERIFY(!m_manager->isValidUsername(QStringLiteral("Admin")));
}

void TestUserManager::testIsValidUsernameValidSimple()
{
    QVERIFY(m_manager->isValidUsername(QStringLiteral("user")));
    QVERIFY(m_manager->isValidUsername(QStringLiteral("player")));
    QVERIFY(m_manager->isValidUsername(QStringLiteral("gamer")));
    QVERIFY(m_manager->isValidUsername(QStringLiteral("a")));
}

void TestUserManager::testIsValidUsernameValidWithNumbers()
{
    QVERIFY(m_manager->isValidUsername(QStringLiteral("player1")));
    QVERIFY(m_manager->isValidUsername(QStringLiteral("user123")));
    QVERIFY(m_manager->isValidUsername(QStringLiteral("gamer2024")));
}

void TestUserManager::testIsValidUsernameValidWithUnderscore()
{
    QVERIFY(m_manager->isValidUsername(QStringLiteral("player_one")));
    QVERIFY(m_manager->isValidUsername(QStringLiteral("user_name")));
    QVERIFY(m_manager->isValidUsername(QStringLiteral("game_user_1")));
}

void TestUserManager::testIsValidUsernameValidWithDash()
{
    QVERIFY(m_manager->isValidUsername(QStringLiteral("player-one")));
    QVERIFY(m_manager->isValidUsername(QStringLiteral("user-name")));
    QVERIFY(m_manager->isValidUsername(QStringLiteral("game-user-1")));
}

void TestUserManager::testIsValidUsernameInvalidSpecialChars()
{
    QVERIFY(!m_manager->isValidUsername(QStringLiteral("user@name")));
    QVERIFY(!m_manager->isValidUsername(QStringLiteral("user.name")));
    QVERIFY(!m_manager->isValidUsername(QStringLiteral("user#1")));
    QVERIFY(!m_manager->isValidUsername(QStringLiteral("user$test")));
    QVERIFY(!m_manager->isValidUsername(QStringLiteral("user%name")));
}

void TestUserManager::testIsValidUsernameInvalidSpaces()
{
    QVERIFY(!m_manager->isValidUsername(QStringLiteral("user name")));
    QVERIFY(!m_manager->isValidUsername(QStringLiteral(" user")));
    QVERIFY(!m_manager->isValidUsername(QStringLiteral("user ")));
}

void TestUserManager::testIsValidUsernameInvalidUppercase()
{
    // Uppercase in middle of name
    QVERIFY(!m_manager->isValidUsername(QStringLiteral("userName")));
    QVERIFY(!m_manager->isValidUsername(QStringLiteral("userNAME")));
}

// ============ Current User Tests ============

void TestUserManager::testCurrentUserNotEmpty()
{
    QString currentUser = m_manager->currentUser();
    QVERIFY(!currentUser.isEmpty());
}

void TestUserManager::testCurrentUserNotInList()
{
    // Current user should be excluded from the users list
    QString currentUser = m_manager->currentUser();
    QVariantList users = m_manager->usersAsVariant();
    
    for (const QVariant &v : users) {
        QVariantMap user = v.toMap();
        QString username = user[QStringLiteral("username")].toString();
        QVERIFY2(username != currentUser, 
                 qPrintable(QStringLiteral("Current user '%1' should not be in users list").arg(currentUser)));
    }
}

// ============ Users List Tests ============

void TestUserManager::testUsersListFormat()
{
    QVariantList users = m_manager->usersAsVariant();
    
    // List may be empty if no couchplay users exist, that's ok
    if (users.isEmpty()) {
        QSKIP("No couchplay users on this system");
    }
    
    // Check first user has expected fields
    QVariantMap user = users.first().toMap();
    QVERIFY(user.contains(QStringLiteral("username")));
    QVERIFY(user.contains(QStringLiteral("uid")));
    QVERIFY(user.contains(QStringLiteral("homeDir")));
    QVERIFY(user.contains(QStringLiteral("isCurrent")));
    
    // UID should be >= 1000 (regular users only)
    int uid = user[QStringLiteral("uid")].toInt();
    QVERIFY(uid >= 1000);
    QVERIFY(uid < 65534);
    
    // isCurrent should always be false (current user is excluded)
    QVERIFY(!user[QStringLiteral("isCurrent")].toBool());
}

void TestUserManager::testUsersListOnlyCouchPlayGroup()
{
    QVariantList users = m_manager->usersAsVariant();
    
    // All users in the list should be in couchplay group
    for (const QVariant &v : users) {
        QVariantMap user = v.toMap();
        QString username = user[QStringLiteral("username")].toString();
        QVERIFY2(m_manager->isInCouchPlayGroup(username),
                 qPrintable(QStringLiteral("User '%1' is in list but not in couchplay group").arg(username)));
    }
}

// ============ User Exists Tests ============

void TestUserManager::testUserExistsRoot()
{
    // Root should exist on any Linux system
    QVERIFY(m_manager->userExists(QStringLiteral("root")));
}

void TestUserManager::testUserExistsNonexistent()
{
    QVERIFY(!m_manager->userExists(QStringLiteral("nonexistent_user_xyz123")));
}

void TestUserManager::testUserExistsCurrent()
{
    QString currentUser = m_manager->currentUser();
    QVERIFY(m_manager->userExists(currentUser));
}

// ============ isInCouchPlayGroup Tests ============

void TestUserManager::testIsInCouchPlayGroupNonexistent()
{
    QVERIFY(!m_manager->isInCouchPlayGroup(QStringLiteral("nonexistent_user_xyz123")));
}

void TestUserManager::testIsInCouchPlayGroupRoot()
{
    // Root should not be in couchplay group
    QVERIFY(!m_manager->isInCouchPlayGroup(QStringLiteral("root")));
}

// ============ Create User Tests ============

void TestUserManager::testCreateUserInvalidUsername()
{
    QSignalSpy errorSpy(m_manager, &UserManager::errorOccurred);
    
    bool result = m_manager->createUser(QStringLiteral("Invalid User"));
    
    QVERIFY(!result);
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.first().first().toString(), QStringLiteral("Invalid username"));
}

void TestUserManager::testCreateUserAlreadyExists()
{
    QSignalSpy errorSpy(m_manager, &UserManager::errorOccurred);
    
    // Try to create a user that already exists (root)
    bool result = m_manager->createUser(QStringLiteral("root"));
    
    QVERIFY(!result);
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.first().first().toString(), QStringLiteral("User already exists"));
}

void TestUserManager::testCreateUserRequiresHelper()
{
    QSignalSpy errorSpy(m_manager, &UserManager::errorOccurred);
    
    // Valid username that doesn't exist (no helper client set)
    bool result = m_manager->createUser(QStringLiteral("newcouchplayuser"));
    
    QVERIFY(!result);
    QCOMPARE(errorSpy.count(), 1);
    // Should indicate helper is required
    QVERIFY(errorSpy.first().first().toString().contains(QStringLiteral("Helper")));
}

// ============ Delete User Tests ============

void TestUserManager::testDeleteUserInvalidUsername()
{
    QSignalSpy errorSpy(m_manager, &UserManager::errorOccurred);
    
    bool result = m_manager->deleteUser(QStringLiteral("Invalid User"), false);
    
    QVERIFY(!result);
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.first().first().toString(), QStringLiteral("Invalid username"));
}

void TestUserManager::testDeleteUserNonexistent()
{
    QSignalSpy errorSpy(m_manager, &UserManager::errorOccurred);
    
    bool result = m_manager->deleteUser(QStringLiteral("nonexistent_user_xyz123"), false);
    
    QVERIFY(!result);
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.first().first().toString(), QStringLiteral("User does not exist"));
}

void TestUserManager::testDeleteUserCurrentUser()
{
    QSignalSpy errorSpy(m_manager, &UserManager::errorOccurred);
    
    QString currentUser = m_manager->currentUser();
    bool result = m_manager->deleteUser(currentUser, false);
    
    QVERIFY(!result);
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.first().first().toString(), QStringLiteral("Cannot delete the current user"));
}

void TestUserManager::testDeleteUserRequiresHelper()
{
    QSignalSpy errorSpy(m_manager, &UserManager::errorOccurred);
    
    // Try to delete root (exists, not current, no helper)
    // Note: This will fail with "helper not available" because root is not in couchplay group
    // but we check helper first, then the helper checks the group
    bool result = m_manager->deleteUser(QStringLiteral("root"), false);
    
    QVERIFY(!result);
    QCOMPARE(errorSpy.count(), 1);
    // Should indicate helper is required
    QVERIFY(errorSpy.first().first().toString().contains(QStringLiteral("Helper")));
}

// ============ Refresh Tests ============

void TestUserManager::testRefreshEmitsSignal()
{
    QSignalSpy spy(m_manager, &UserManager::usersChanged);
    
    m_manager->refresh();
    
    QCOMPARE(spy.count(), 1);
}

QTEST_MAIN(TestUserManager)
#include "test_usermanager.moc"
