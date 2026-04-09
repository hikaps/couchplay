// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "CouchPlayHelper.h"
#include "SystemOps.h"

#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusArgument>
#include <QDBusVariant>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QThread>
#include <QDebug>

#include <unistd.h>
#include <sys/stat.h>
#include <signal.h>
#include <pwd.h>
#include <grp.h>

class UnitMonitor : public QObject
{
    Q_OBJECT
public:
    UnitMonitor(const QString &unitPath, const QString &serviceName,
                const QString &username, qint64 pid, CouchPlayHelper *helper)
        : QObject(helper), m_unitPath(unitPath), m_serviceName(serviceName),
          m_username(username), m_pid(pid), m_helper(helper)
    {
        QDBusConnection::systemBus().connect(
            QStringLiteral("org.freedesktop.systemd1"),
            m_unitPath,
            QStringLiteral("org.freedesktop.DBus.Properties"),
            QStringLiteral("PropertiesChanged"),
            this, SLOT(onPropertiesChanged(QString, QVariantMap, QStringList)));
    }

    ~UnitMonitor() override
    {
        QDBusConnection::systemBus().disconnect(
            QStringLiteral("org.freedesktop.systemd1"),
            m_unitPath,
            QStringLiteral("org.freedesktop.DBus.Properties"),
            QStringLiteral("PropertiesChanged"),
            this, SLOT(onPropertiesChanged(QString, QVariantMap, QStringList)));
    }

    QString serviceName() const { return m_serviceName; }

public Q_SLOTS:
    void onPropertiesChanged(const QString &interface, const QVariantMap &changed, const QStringList &)
    {
        if (interface != QStringLiteral("org.freedesktop.systemd1.Unit")) {
            return;
        }
        if (!changed.contains(QStringLiteral("ActiveState"))) {
            return;
        }

        QString activeState = changed.value(QStringLiteral("ActiveState")).toString();
        if (activeState != QStringLiteral("inactive") &&
            activeState != QStringLiteral("failed") &&
            activeState != QStringLiteral("dead")) {
            return;
        }

        if (m_helper->m_stoppingUnits.contains(m_serviceName)) {
            return;
        }

        QString result = queryResultProperty();
        QString reason = mapResultToReason(result);

        m_helper->m_usernameToUnitName.remove(m_username);
        m_helper->m_pidToUsername.remove(m_pid);

        m_helper->saveState();

        qInfo() << "Unit" << m_serviceName << "(user:" << m_username << ", PID:" << m_pid << ") stopped unexpectedly:" << reason;

        Q_EMIT m_helper->instanceStopped(m_username, m_pid, reason);

        deleteLater();
    }

private:
    QString queryResultProperty() const
    {
        QDBusMessage msg = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.systemd1"),
            m_unitPath,
            QStringLiteral("org.freedesktop.DBus.Properties"),
            QStringLiteral("Get")
        );
        msg << QStringLiteral("org.freedesktop.systemd1.Service")
            << QStringLiteral("Result");

        QDBusMessage reply = QDBusConnection::systemBus().call(msg);
        if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
            QVariant result = reply.arguments().at(0);
            if (result.canConvert<QDBusVariant>()) {
                return result.value<QDBusVariant>().variant().toString();
            }
        }
        return QStringLiteral("exit-code");
    }

    static QString mapResultToReason(const QString &result)
    {
        if (result == QStringLiteral("signal") || result == QStringLiteral("core-dump")) {
            return QStringLiteral("crashed");
        }
        if (result == QStringLiteral("exit-code")) {
            return QStringLiteral("exited");
        }
        return QStringLiteral("failed");
    }

    QString m_unitPath;
    QString m_serviceName;
    QString m_username;
    qint64 m_pid;
    CouchPlayHelper *m_helper;
};

// Username validation: lowercase, starts with letter, 1-32 chars, letters/digits/underscore/hyphen
static const QRegularExpression s_validUsername(QStringLiteral("^[a-z][a-z0-9_-]{0,31}$"));

// Version of the helper daemon
static const QString HELPER_VERSION = QStringLiteral("0.2.0");

// PolicyKit actions
static const QString ACTION_DEVICE_OWNER = QStringLiteral("io.github.hikaps.couchplay.change-device-owner");
static const QString ACTION_CREATE_USER = QStringLiteral("io.github.hikaps.couchplay.create-user");
static const QString ACTION_DELETE_USER = QStringLiteral("io.github.hikaps.couchplay.delete-user");
static const QString ACTION_ENABLE_LINGER = QStringLiteral("io.github.hikaps.couchplay.enable-linger");
static const QString ACTION_WAYLAND_ACCESS = QStringLiteral("io.github.hikaps.couchplay.setup-wayland-access");
static const QString ACTION_LAUNCH_INSTANCE = QStringLiteral("io.github.hikaps.couchplay.launch-instance");
static const QString ACTION_MANAGE_MOUNTS = QStringLiteral("io.github.hikaps.couchplay.manage-mounts");

// couchplay group name for managed users
static const QString COUCHPLAY_GROUP = QStringLiteral("couchplay");

CouchPlayHelper::CouchPlayHelper(SystemOps *ops, QObject *parent)
    : QObject(parent)
    , m_ops(ops ? ops : new RealSystemOps(this))
    , m_stateFilePath(QStringLiteral("/run/couchplay/state.json"))
{
    loadAndReconcileState();
}

CouchPlayHelper::~CouchPlayHelper()
{
    // Clean up: remove runtime access for all compositor UIDs
    for (uint uid : m_runtimeAccessSetForUid) {
        QString runtimeDir = QStringLiteral("/run/user/%1").arg(uid);
        static const QString group = QStringLiteral("couchplay");
        
        // Helper lambda for removing ACL (can't call RemoveRuntimeAccess as it needs auth)
        auto removeAcl = [&](const QString &path) {
            if (!m_ops->fileExists(path)) return;
            QProcess *proc = m_ops->createProcess();
            m_ops->startProcess(proc, QStringLiteral("setfacl"),
                {QStringLiteral("-x"), QStringLiteral("g:%1").arg(group), path});
            m_ops->waitForFinished(proc, 5000);
            delete proc;
        };
        
        removeAcl(runtimeDir + QStringLiteral("/pulse/native"));
        removeAcl(runtimeDir + QStringLiteral("/pulse"));
        removeAcl(runtimeDir + QStringLiteral("/pipewire-0-manager"));
        removeAcl(runtimeDir + QStringLiteral("/pipewire-0"));
        
        QDir dir(runtimeDir);
        for (const QString &xauthFile : dir.entryList({QStringLiteral("xauth_*")}, QDir::Files)) {
            removeAcl(runtimeDir + QStringLiteral("/") + xauthFile);
        }
        
        removeAcl(runtimeDir + QStringLiteral("/wayland-0"));
        removeAcl(runtimeDir);
        
        qDebug() << "Cleaned up runtime access for compositor UID" << uid;
    }
    m_runtimeAccessSetForUid.clear();

    // Clean up: unmount all shared directories
    if (!m_activeMounts.isEmpty()) {
        for (const QString &username : m_activeMounts.keys()) {
            for (const MountInfo &mount : m_activeMounts[username]) {
                QProcess *umountProc = m_ops->createProcess();
                m_ops->startProcess(umountProc, QStringLiteral("/usr/bin/umount"), {mount.target});
                m_ops->waitForFinished(umountProc, 5000);
                if (m_ops->processExitCode(umountProc) != 0) {
                    QProcess *lazyProc = m_ops->createProcess();
                    m_ops->startProcess(lazyProc, QStringLiteral("/usr/bin/umount"),
                        {QStringLiteral("-l"), mount.target});
                    m_ops->waitForFinished(lazyProc, 5000);
                    delete lazyProc;
                }
                delete umountProc;
            }
        }
        m_activeMounts.clear();
    }

    // Clean up: stop all launched transient units
    for (const QString &serviceName : m_usernameToUnitName) {
        m_stoppingUnits.insert(serviceName);
        stopServiceInstance(serviceName);
    }
    m_usernameToUnitName.clear();
    m_pidToUsername.clear();

    qDeleteAll(m_monitors);
    m_monitors.clear();

    // Clean up: reset all modified devices on shutdown
    if (!m_modifiedDevices.isEmpty()) {
        ResetAllDevices();
    }
}

bool CouchPlayHelper::ChangeDeviceOwner(const QString &devicePath, uint uid)
{
    // Validate input
    if (!isValidDevicePath(devicePath)) {
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("Invalid device path: %1").arg(devicePath));
        return false;
    }

    // Check authorization
    if (!checkAuthorization(ACTION_DEVICE_OWNER)) {
        sendErrorReply(QDBusError::AccessDenied, 
            QStringLiteral("Not authorized to change device ownership"));
        return false;
    }

    // Verify user exists
    struct passwd *pw = m_ops->getpwuid(uid);
    if (!pw) {
        sendErrorReply(QDBusError::InvalidArgs,
            QStringLiteral("User with UID %1 does not exist").arg(uid));
        return false;
    }

    // Change ownership
    if (m_ops->chown(devicePath, uid, pw->pw_gid) != 0) {
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("Failed to change ownership of %1: %2")
                .arg(devicePath, QString::fromLocal8Bit(strerror(errno))));
        return false;
    }

    // Set permissions to 0600 (owner read/write only) for input isolation
    // This ensures only the assigned user can read the device, not the group
    if (m_ops->chmod(devicePath, 0600) != 0) {
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("Failed to set permissions on %1: %2")
                .arg(devicePath, QString::fromLocal8Bit(strerror(errno))));
        return false;
    }

    // Track this device for cleanup
    if (!m_modifiedDevices.contains(devicePath)) {
        m_modifiedDevices.append(devicePath);
    }

    saveState();

    return true;
}

int CouchPlayHelper::ChangeDeviceOwnerBatch(const QStringList &devicePaths, uint uid)
{
    int successCount = 0;
    for (const QString &path : devicePaths) {
        if (ChangeDeviceOwner(path, uid)) {
            successCount++;
        }
    }
    return successCount;
}

bool CouchPlayHelper::ResetDeviceOwner(const QString &devicePath)
{
    if (!isValidDevicePath(devicePath)) {
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("Invalid device path: %1").arg(devicePath));
        return false;
    }

    if (!checkAuthorization(ACTION_DEVICE_OWNER)) {
        sendErrorReply(QDBusError::AccessDenied, 
            QStringLiteral("Not authorized to change device ownership"));
        return false;
    }

    // Get the 'input' group GID for proper reset
    struct group *inputGroup = m_ops->getgrnam("input");
    gid_t inputGid = inputGroup ? inputGroup->gr_gid : 0;

    // Reset to root:input (or root:root if input group doesn't exist)
    if (m_ops->chown(devicePath, 0, inputGid) != 0) {
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("Failed to reset ownership of %1").arg(devicePath));
        return false;
    }

    // Restore permissions to 0660 (owner and group read/write)
    if (m_ops->chmod(devicePath, 0660) != 0) {
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("Failed to reset permissions on %1").arg(devicePath));
        return false;
    }

    m_modifiedDevices.removeAll(devicePath);
    saveState();
    return true;
}

int CouchPlayHelper::ResetAllDevices()
{
    int successCount = 0;
    QStringList devices = m_modifiedDevices; // Copy since we modify during iteration
    
    // Get the 'input' group GID for proper reset
    struct group *inputGroup = m_ops->getgrnam("input");
    gid_t inputGid = inputGroup ? inputGroup->gr_gid : 0;

    for (const QString &path : devices) {
        // Direct reset without auth check for cleanup scenarios
        // Reset to root:input with 0660 permissions
        if (m_ops->chown(path, 0, inputGid) == 0 &&
            m_ops->chmod(path, 0660) == 0) {
            successCount++;
            m_modifiedDevices.removeAll(path);
        }
    }

    saveState();
    
    return successCount;
}

uint CouchPlayHelper::CreateUser(const QString &username, const QString &fullName)
{
    // Validate username (alphanumeric, lowercase, starts with letter)
    if (!s_validUsername.match(username).hasMatch()) {
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("Invalid username format"));
        return 0;
    }

    if (!checkAuthorization(ACTION_CREATE_USER)) {
        sendErrorReply(QDBusError::AccessDenied, 
            QStringLiteral("Not authorized to create users"));
        return 0;
    }

    // Check if user already exists
    if (userExists(username)) {
        sendErrorReply(QDBusError::Failed, 
            QStringLiteral("User '%1' already exists").arg(username));
        return 0;
    }

    // Ensure couchplay group exists (create if needed)
    QProcess *groupProcess = m_ops->createProcess();
    m_ops->startProcess(groupProcess, QStringLiteral("groupadd"), {QStringLiteral("-f"), COUCHPLAY_GROUP});
    m_ops->waitForFinished(groupProcess, 10000);
    delete groupProcess;
    // -f flag means no error if group exists, so we don't check exit code

    // Create user with useradd
    QProcess *process = m_ops->createProcess();
    QStringList args;
    args << QStringLiteral("-m")  // Create home directory
         << QStringLiteral("-c") << fullName
         << QStringLiteral("-s") << QStringLiteral("/bin/bash");

    // Add supplementary groups: input (for gamepad access) and couchplay (for management)
    args << QStringLiteral("-G") << QStringLiteral("input,") + COUCHPLAY_GROUP;

    args << username;

    m_ops->startProcess(process, QStringLiteral("useradd"), args);
    m_ops->waitForFinished(process, 30000);

    if (m_ops->processExitCode(process) != 0) {
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("Failed to create user: %1")
                .arg(QString::fromLocal8Bit(m_ops->readStandardError(process))));
        delete process;
        return 0;
    }
    delete process;

    // Get the new user's UID
    uint uid = getUserUid(username);
    if (uid == 0) {
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("User created but could not retrieve UID"));
        return 0;
    }

    // Enable linger for the new user so their systemd user session starts at boot
    // This is required for systemd-run transient units to access the user's D-Bus session
    QProcess *lingerProcess = m_ops->createProcess();
    m_ops->startProcess(lingerProcess, QStringLiteral("loginctl"),
        {QStringLiteral("enable-linger"), username});
    m_ops->waitForFinished(lingerProcess, 30000);

    if (m_ops->processExitCode(lingerProcess) != 0) {
        qWarning() << "Failed to enable linger for" << username
                   << ":" << QString::fromLocal8Bit(m_ops->readStandardError(lingerProcess));
        // Don't fail user creation, just warn - linger can be enabled later
    }
    delete lingerProcess;

    qDebug() << "Created user" << username << "with UID" << uid;
    return uid;
}

bool CouchPlayHelper::userExists(const QString &username)
{
    struct passwd *pw = m_ops->getpwnam(username.toLocal8Bit().constData());
    return pw != nullptr;
}

uint CouchPlayHelper::getUserUid(const QString &username)
{
    struct passwd *pw = m_ops->getpwnam(username.toLocal8Bit().constData());
    return pw ? pw->pw_uid : 0;
}

QString CouchPlayHelper::getUserHome(const QString &username)
{
    struct passwd *pw = m_ops->getpwnam(username.toLocal8Bit().constData());
    return pw ? QString::fromLocal8Bit(pw->pw_dir) : QString();
}

QString CouchPlayHelper::getUserHomeByUid(uint uid)
{
    struct passwd *pw = m_ops->getpwuid(uid);
    return pw ? QString::fromLocal8Bit(pw->pw_dir) : QString();
}

bool CouchPlayHelper::validateUserPath(const QString &path, const QString &username,
                                         const QString &callerName, QStringList &dirsToChown)
{
    dirsToChown.clear();

    QString userHome = getUserHome(username);
    if (!path.startsWith(userHome + QLatin1Char('/'))) {
        qWarning() << callerName << ": Path" << path
                   << "is not under user's home" << userHome;
        sendErrorReply(QDBusError::InvalidArgs,
            QStringLiteral("Path is not under user's home directory"));
        return false;
    }

    QStringList pathParts = path.mid(userHome.length()).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString checkPath = userHome;
    for (const QString &part : pathParts) {
        checkPath += QStringLiteral("/") + part;
        if (!m_ops->fileExists(checkPath)) {
            dirsToChown.append(checkPath);
        }
    }

    return true;
}

bool CouchPlayHelper::IsInCouchPlayGroup(const QString &username)
{
    // Get the couchplay group
    struct group *grp = m_ops->getgrnam(COUCHPLAY_GROUP.toLocal8Bit().constData());
    if (!grp) {
        return false;  // Group doesn't exist
    }

    // Check if username is in the group's member list
    for (char **member = grp->gr_mem; *member != nullptr; ++member) {
        if (username == QString::fromLocal8Bit(*member)) {
            return true;
        }
    }

    // Also check if couchplay is the user's primary group
    struct passwd *pw = m_ops->getpwnam(username.toLocal8Bit().constData());
    if (pw && pw->pw_gid == grp->gr_gid) {
        return true;
    }

    return false;
}

bool CouchPlayHelper::DeleteUser(const QString &username, bool removeHome)
{
    // Validate username
    if (!s_validUsername.match(username).hasMatch()) {
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("Invalid username format"));
        return false;
    }

    if (!checkAuthorization(ACTION_DELETE_USER)) {
        sendErrorReply(QDBusError::AccessDenied, 
            QStringLiteral("Not authorized to delete users"));
        return false;
    }

    // Check if user exists
    if (!userExists(username)) {
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("User '%1' does not exist").arg(username));
        return false;
    }

    // CRITICAL: Only allow deleting users in the couchplay group
    if (!IsInCouchPlayGroup(username)) {
        sendErrorReply(QDBusError::AccessDenied, 
            QStringLiteral("User '%1' is not a CouchPlay user (not in couchplay group)").arg(username));
        return false;
    }

    // Get the user's UID before deletion (needed for IPC cleanup)
    struct passwd *pw = m_ops->getpwnam(username.toLocal8Bit().constData());
    uid_t userUid = pw ? pw->pw_uid : 0;

    // Disable linger first
    QProcess *lingerProcess = m_ops->createProcess();
    m_ops->startProcess(lingerProcess, QStringLiteral("loginctl"),
        {QStringLiteral("disable-linger"), username});
    m_ops->waitForFinished(lingerProcess, 10000);
    delete lingerProcess;
    // Don't fail if this doesn't work

    // Kill any running processes for the user
    QProcess *pkillProcess = m_ops->createProcess();
    m_ops->startProcess(pkillProcess, QStringLiteral("pkill"), {QStringLiteral("-u"), username});
    m_ops->waitForFinished(pkillProcess, 10000);
    delete pkillProcess;
    // Don't fail if there are no processes to kill

    // Wait a moment for processes to terminate
    QThread::msleep(500);

    // Clean up IPC resources (semaphores, shared memory, message queues) owned by the user
    // This prevents "Permission denied" errors if a new user is created with the same name
    // but a different UID, and tries to access stale IPC resources from the old user.
    if (userUid > 0) {
        // Remove semaphores owned by the user
        QProcess *ipcrm = m_ops->createProcess();
        m_ops->startProcess(ipcrm, QStringLiteral("/bin/bash"),
            {QStringLiteral("-c"),
             QStringLiteral("ipcs -s | awk '$3 == %1 {print $2}' | xargs -r ipcrm -s").arg(userUid)});
        m_ops->waitForFinished(ipcrm, 10000);
        delete ipcrm;

        // Remove shared memory segments owned by the user
        QProcess *shmrm = m_ops->createProcess();
        m_ops->startProcess(shmrm, QStringLiteral("/bin/bash"),
            {QStringLiteral("-c"),
             QStringLiteral("ipcs -m | awk '$3 == %1 {print $2}' | xargs -r ipcrm -m").arg(userUid)});
        m_ops->waitForFinished(shmrm, 10000);
        delete shmrm;

        // Remove message queues owned by the user
        QProcess *msgrm = m_ops->createProcess();
        m_ops->startProcess(msgrm, QStringLiteral("/bin/bash"),
            {QStringLiteral("-c"),
             QStringLiteral("ipcs -q | awk '$3 == %1 {print $2}' | xargs -r ipcrm -q").arg(userUid)});
        m_ops->waitForFinished(msgrm, 10000);
        delete msgrm;

        // Clean up /tmp files owned by the user (Steam dumps, etc.)
        QProcess *tmprm = m_ops->createProcess();
        m_ops->startProcess(tmprm, QStringLiteral("find"),
            {QStringLiteral("/tmp"), QStringLiteral("-user"), QString::number(userUid),
             QStringLiteral("-delete")});
        m_ops->waitForFinished(tmprm, 30000);
        delete tmprm;

        // Clean up /dev/shm files owned by the user
        QProcess *shmfilerm = m_ops->createProcess();
        m_ops->startProcess(shmfilerm, QStringLiteral("find"),
            {QStringLiteral("/dev/shm"), QStringLiteral("-user"), QString::number(userUid),
             QStringLiteral("-delete")});
        m_ops->waitForFinished(shmfilerm, 10000);
        delete shmfilerm;
    }

    // Delete user with userdel
    QProcess *process = m_ops->createProcess();
    QStringList args;
    if (removeHome) {
        args << QStringLiteral("-r");  // Remove home directory
    }
    args << username;

    m_ops->startProcess(process, QStringLiteral("userdel"), args);
    m_ops->waitForFinished(process, 30000);

    if (m_ops->processExitCode(process) != 0) {
        QString errorMsg = QString::fromLocal8Bit(m_ops->readStandardError(process));
        qWarning() << "DeleteUser failed:" << errorMsg;
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("Failed to delete user: %1").arg(errorMsg));
        delete process;
        return false;
    }
    delete process;

    qDebug() << "Deleted user" << username;
    return true;
}

bool CouchPlayHelper::EnableLinger(const QString &username)
{
    // Validate username
    if (!s_validUsername.match(username).hasMatch()) {
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("Invalid username format"));
        return false;
    }

    if (!checkAuthorization(ACTION_ENABLE_LINGER)) {
        sendErrorReply(QDBusError::AccessDenied, 
            QStringLiteral("Not authorized to enable linger"));
        return false;
    }

    // Check if user exists
    if (!userExists(username)) {
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("User '%1' does not exist").arg(username));
        return false;
    }

    // Enable linger via loginctl
    QProcess *process = m_ops->createProcess();
    m_ops->startProcess(process, QStringLiteral("loginctl"),
        {QStringLiteral("enable-linger"), username});
    m_ops->waitForFinished(process, 30000);

    if (m_ops->processExitCode(process) != 0) {
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("Failed to enable linger: %1")
                .arg(QString::fromLocal8Bit(m_ops->readStandardError(process))));
        delete process;
        return false;
    }
    delete process;

    return true;
}

bool CouchPlayHelper::IsLingerEnabled(const QString &username)
{
    // Check if linger file exists in /var/lib/systemd/linger/
    QString lingerFile = QStringLiteral("/var/lib/systemd/linger/%1").arg(username);
    return m_ops->fileExists(lingerFile);
}

bool CouchPlayHelper::SetupRuntimeAccess(uint compositorUid)
{
    if (!checkAuthorization(ACTION_WAYLAND_ACCESS)) {
        sendErrorReply(QDBusError::AccessDenied, 
            QStringLiteral("Not authorized to set up runtime access"));
        return false;
    }

    // Verify compositor user exists
    struct passwd *pw = m_ops->getpwuid(compositorUid);
    if (!pw) {
        sendErrorReply(QDBusError::InvalidArgs,
            QStringLiteral("Compositor user with UID %1 does not exist").arg(compositorUid));
        return false;
    }

    QString runtimeDir = QStringLiteral("/run/user/%1").arg(compositorUid);

    if (!m_ops->fileExists(runtimeDir)) {
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("Runtime directory %1 does not exist").arg(runtimeDir));
        return false;
    }

    static const QString group = QStringLiteral("couchplay");
    bool success = true;

    // Helper lambda for setfacl
    auto setAcl = [&](const QString &path, const QString &perm) -> bool {
        if (!m_ops->fileExists(path)) {
            return true;  // Not an error - optional paths
        }
        QProcess *proc = m_ops->createProcess();
        m_ops->startProcess(proc, QStringLiteral("setfacl"),
            {QStringLiteral("-m"), QStringLiteral("g:%1:%2").arg(group, perm), path});
        m_ops->waitForFinished(proc, 5000);
        bool aclOk = (m_ops->processExitCode(proc) == 0);
        if (!aclOk) {
            qWarning() << "Failed to set ACL on" << path << ":"
                       << QString::fromLocal8Bit(m_ops->readStandardError(proc));
        }
        delete proc;
        return aclOk;
    };

    // Runtime directory - traverse permission
    if (!setAcl(runtimeDir, QStringLiteral("x"))) {
        sendErrorReply(QDBusError::Failed, 
            QStringLiteral("Failed to set ACL on runtime directory"));
        return false;
    }

    // Wayland socket
    QString waylandSocket = runtimeDir + QStringLiteral("/wayland-0");
    if (!setAcl(waylandSocket, QStringLiteral("rw"))) {
        sendErrorReply(QDBusError::Failed, 
            QStringLiteral("Failed to set ACL on Wayland socket"));
        return false;
    }

    // X authentication files
    for (const QString &xauthFile : m_ops->entryList(runtimeDir, {QStringLiteral("xauth_*")}, QDir::Files)) {
        setAcl(runtimeDir + QStringLiteral("/") + xauthFile, QStringLiteral("r"));
    }

    // PipeWire sockets
    success &= setAcl(runtimeDir + QStringLiteral("/pipewire-0"), QStringLiteral("rw"));
    success &= setAcl(runtimeDir + QStringLiteral("/pipewire-0-manager"), QStringLiteral("rw"));

    // PulseAudio compatibility
    // The pulse directory typically has mode 0700, which means the ACL mask is ---
    // We need to set both the group ACL and update the mask for it to be effective
    QString pulseDir = runtimeDir + QStringLiteral("/pulse");
    if (m_ops->fileExists(pulseDir)) {
        QProcess *proc = m_ops->createProcess();
        m_ops->startProcess(proc, QStringLiteral("setfacl"),
            {QStringLiteral("-m"), QStringLiteral("g:%1:x,m::x").arg(group), pulseDir});
        m_ops->waitForFinished(proc, 5000);
        if (m_ops->processExitCode(proc) != 0) {
            qWarning() << "Failed to set ACL on" << pulseDir << ":"
                       << QString::fromLocal8Bit(m_ops->readStandardError(proc));
            success = false;
        }
        delete proc;
    }
    success &= setAcl(pulseDir + QStringLiteral("/native"), QStringLiteral("rw"));

    if (success) {
        m_runtimeAccessSetForUid.insert(compositorUid);
        saveState();
    }

    return success;
}

bool CouchPlayHelper::RemoveRuntimeAccess(uint compositorUid)
{
    if (!checkAuthorization(ACTION_WAYLAND_ACCESS)) {
        sendErrorReply(QDBusError::AccessDenied, 
            QStringLiteral("Not authorized to remove runtime access"));
        return false;
    }

    QString runtimeDir = QStringLiteral("/run/user/%1").arg(compositorUid);
    static const QString group = QStringLiteral("couchplay");
    bool success = true;

    // Helper lambda for removing ACL
    auto removeAcl = [&](const QString &path) -> bool {
        if (!m_ops->fileExists(path)) {
            return true;
        }
        QProcess *proc = m_ops->createProcess();
        m_ops->startProcess(proc, QStringLiteral("setfacl"),
            {QStringLiteral("-x"), QStringLiteral("g:%1").arg(group), path});
        m_ops->waitForFinished(proc, 5000);
        bool result = (m_ops->processExitCode(proc) == 0);
        delete proc;
        // Don't fail on removal errors - file may have been deleted
        return result;
    };

    // Remove all ACLs we set (reverse order)
    removeAcl(runtimeDir + QStringLiteral("/pulse/native"));
    removeAcl(runtimeDir + QStringLiteral("/pulse"));
    removeAcl(runtimeDir + QStringLiteral("/pipewire-0-manager"));
    removeAcl(runtimeDir + QStringLiteral("/pipewire-0"));
    
    QDir dir(runtimeDir);
    for (const QString &xauthFile : dir.entryList({QStringLiteral("xauth_*")}, QDir::Files)) {
        removeAcl(runtimeDir + QStringLiteral("/") + xauthFile);
    }
    
    removeAcl(runtimeDir + QStringLiteral("/wayland-0"));
    success &= removeAcl(runtimeDir);

    m_runtimeAccessSetForUid.remove(compositorUid);
    saveState();

    return success;
}

QString CouchPlayHelper::Version()
{
    return HELPER_VERSION;
}

bool CouchPlayHelper::checkAuthorization(const QString &action)
{
    return m_ops->checkAuthorization(action);
}

bool CouchPlayHelper::isValidDevicePath(const QString &path)
{
    // Check for path traversal attempts
    if (path.contains(QStringLiteral(".."))) {
        return false;
    }

    // Must be under /dev/input/ OR /dev/hidraw*
    bool isInputDevice = path.startsWith(QStringLiteral("/dev/input/"));
    bool isHidrawDevice = path.startsWith(QStringLiteral("/dev/hidraw"));

    if (!isInputDevice && !isHidrawDevice) {
        return false;
    }

    // Validate hidraw path format strictly
    if (isHidrawDevice) {
        static QRegularExpression hidrawRegex(QStringLiteral("^/dev/hidraw\\d+$"));
        if (!hidrawRegex.match(path).hasMatch()) {
            return false;
        }
    }

    // Must be a character device
    if (!m_ops->fileExists(path)) {
        return false;
    }

    struct stat st;
    if (!m_ops->statPath(path, &st)) {
        return false;
    }

    // Must be a character device (input devices are char devices)
    return m_ops->isCharDevice(st.st_mode);
}

qint64 CouchPlayHelper::LaunchInstance(const QString &username, uint compositorUid,
                                        const QStringList &gamescopeArgs,
                                        const QString &gameCommand,
                                        const QStringList &environment,
                                        const QStringList &bindPaths)
{
    if (!s_validUsername.match(username).hasMatch()) {
        sendErrorReply(QDBusError::InvalidArgs,
            QStringLiteral("Invalid username format"));
        return 0;
    }

    if (!checkAuthorization(ACTION_LAUNCH_INSTANCE)) {
        sendErrorReply(QDBusError::AccessDenied,
            QStringLiteral("Not authorized to launch instances"));
        return 0;
    }

    if (!userExists(username)) {
        sendErrorReply(QDBusError::InvalidArgs,
            QStringLiteral("User '%1' does not exist").arg(username));
        return 0;
    }

    struct passwd *pw = m_ops->getpwuid(compositorUid);
    if (!pw) {
        sendErrorReply(QDBusError::InvalidArgs,
            QStringLiteral("Compositor user with UID %1 does not exist").arg(compositorUid));
        return 0;
    }

    if (!m_runtimeAccessSetForUid.contains(compositorUid)) {
        if (!SetupRuntimeAccess(compositorUid)) {
            qWarning() << "Failed to set up runtime access for compositor" << compositorUid;
        }
    }

    qint64 pid = startTransientUnit(username, compositorUid, gamescopeArgs,
                                     gameCommand, environment, bindPaths);
    if (pid <= 0) {
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("Failed to launch instance for user '%1'").arg(username));
        return 0;
    }
    return pid;
}

bool CouchPlayHelper::StopInstance(qint64 pid)
{
    if (!checkAuthorization(ACTION_LAUNCH_INSTANCE)) {
        sendErrorReply(QDBusError::AccessDenied,
            QStringLiteral("Not authorized to stop instances"));
        return false;
    }

    if (pid <= 0) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Invalid PID"));
        return false;
    }

    if (m_pidToUsername.contains(pid)) {
        QString username = m_pidToUsername.value(pid);
        QString serviceName = m_usernameToUnitName.value(username);
        if (!serviceName.isEmpty()) {
            m_stoppingUnits.insert(serviceName);
            delete m_monitors.take(serviceName);
            stopServiceInstance(serviceName);
            m_usernameToUnitName.remove(username);
            m_pidToUsername.remove(pid);
            m_stoppingUnits.remove(serviceName);
            saveState();
            return true;
        }
    }

    if (m_ops->killProcess(static_cast<pid_t>(pid), SIGTERM)) {
        return true;
    }

    sendErrorReply(QDBusError::Failed,
        QStringLiteral("Failed to stop process %1").arg(pid));
    return false;
}

bool CouchPlayHelper::KillInstance(qint64 pid)
{
    if (!checkAuthorization(ACTION_LAUNCH_INSTANCE)) {
        sendErrorReply(QDBusError::AccessDenied,
            QStringLiteral("Not authorized to kill instances"));
        return false;
    }

    if (pid <= 0) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Invalid PID"));
        return false;
    }

    if (m_pidToUsername.contains(pid)) {
        QString username = m_pidToUsername.value(pid);
        QString serviceName = m_usernameToUnitName.value(username);
        if (!serviceName.isEmpty()) {
            m_stoppingUnits.insert(serviceName);
            delete m_monitors.take(serviceName);
            QProcess *killProc = m_ops->createProcess();
            m_ops->startProcess(killProc, QStringLiteral("systemctl"),
                {QStringLiteral("kill"), serviceName, QStringLiteral("--signal=SIGKILL")});
            m_ops->waitForFinished(killProc, 10000);
            delete killProc;

            stopServiceInstance(serviceName);
            m_usernameToUnitName.remove(username);
            m_pidToUsername.remove(pid);
            m_stoppingUnits.remove(serviceName);
            saveState();
            return true;
        }
    }

    if (m_ops->killProcess(static_cast<pid_t>(pid), SIGKILL)) {
        return true;
    }

    sendErrorReply(QDBusError::Failed,
        QStringLiteral("Failed to kill process %1").arg(pid));
    return false;
}

QString CouchPlayHelper::generateServiceName(const QString &username)
{
    return QStringLiteral("couchplay-%1.service").arg(username);
}

qint64 CouchPlayHelper::startTransientUnit(const QString &username, uint compositorUid,
                                           const QStringList &gamescopeArgs,
                                           const QString &gameCommand,
                                           const QStringList &environment,
                                           const QStringList &bindPaths)
{
    QString serviceName = generateServiceName(username);

    struct passwd *pwd = m_ops->getpwnam(username.toLocal8Bit().constData());
    if (!pwd) {
        qWarning() << "startTransientUnit: failed to resolve UID for user" << username;
        return 0;
    }
    QString userUid = QString::number(pwd->pw_uid);
    QString compositorRuntimeDir = QStringLiteral("/run/user/%1").arg(compositorUid);

    QStringList systemdRunArgs;
    systemdRunArgs << QStringLiteral("--unit") << serviceName;
    systemdRunArgs << QStringLiteral("--uid") << username;
    systemdRunArgs << QStringLiteral("--property=Type=simple");
    systemdRunArgs << QStringLiteral("--property=Delegate=yes");
    systemdRunArgs << QStringLiteral("--property=MemoryDenyWriteExecute=false");

    // Use -E flag for each environment variable (cleaner escaping than --property=Environment=)
    auto addEnv = [&](const QString &assignment) {
        systemdRunArgs << QStringLiteral("-E") << assignment;
    };
    addEnv(QStringLiteral("DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/%1/bus").arg(userUid));
    addEnv(QStringLiteral("WAYLAND_DISPLAY=%1/wayland-0").arg(compositorRuntimeDir));
    addEnv(QStringLiteral("XDG_RUNTIME_DIR=/run/user/%1").arg(userUid));
    // For audio, point to the compositor user's PipeWire and PulseAudio sockets
    addEnv(QStringLiteral("PIPEWIRE_RUNTIME_DIR=%1").arg(compositorRuntimeDir));
    // PulseAudio clients (including games via SDL) need PULSE_SERVER to find the socket
    addEnv(QStringLiteral("PULSE_SERVER=unix:%1/pulse/native").arg(compositorRuntimeDir));
    for (const QString &var : environment) {
        int eqPos = var.indexOf(QLatin1Char('='));
        if (eqPos > 0) {
            addEnv(var);
        }
    }

    systemdRunArgs << QStringLiteral("--property=BindReadOnlyPaths=%1").arg(compositorRuntimeDir);

    for (const QString &bp : bindPaths) {
        systemdRunArgs << QStringLiteral("--property=BindPaths=%1").arg(bp);
    }

    QStringList cmdArgs;
    cmdArgs << QStringLiteral("/usr/bin/gamescope") << gamescopeArgs;
    cmdArgs << QStringLiteral("--") << QStringLiteral("/bin/bash")
            << QStringLiteral("-c") << gameCommand;
    systemdRunArgs << QStringLiteral("--") << cmdArgs;

    QProcess *proc = m_ops->createProcess();
    m_ops->startProcess(proc, QStringLiteral("systemd-run"), systemdRunArgs);
    m_ops->waitForFinished(proc, 10000);
    int exitCode = m_ops->processExitCode(proc);
    if (exitCode != 0) {
        QByteArray errOutput = m_ops->readStandardError(proc);
        delete proc;

        if (errOutput.contains("already loaded")) {
            qInfo() << "Stale unit" << serviceName << "found - stopping and retrying";
            QProcess *stopProc = m_ops->createProcess();
            m_ops->startProcess(stopProc, QStringLiteral("systemctl"), {QStringLiteral("stop"), serviceName});
            m_ops->waitForFinished(stopProc, 5000);
            delete stopProc;

            QProcess *resetProc = m_ops->createProcess();
            m_ops->startProcess(resetProc, QStringLiteral("systemctl"), {QStringLiteral("reset-failed"), serviceName});
            m_ops->waitForFinished(resetProc, 5000);
            delete resetProc;

            QThread::msleep(200);

            proc = m_ops->createProcess();
            m_ops->startProcess(proc, QStringLiteral("systemd-run"), systemdRunArgs);
            m_ops->waitForFinished(proc, 10000);
            exitCode = m_ops->processExitCode(proc);
            if (exitCode != 0) {
                errOutput = m_ops->readStandardError(proc);
                qWarning() << "systemd-run retry failed for" << serviceName
                            << "exit code:" << exitCode << errOutput;
                delete proc;
                return 0;
            }
            delete proc;
        } else {
            qWarning() << "systemd-run failed for" << serviceName
                        << "exit code:" << exitCode << errOutput;
            return 0;
        }
    } else {
        delete proc;
    }

    // Wait briefly for systemd to register the unit, then poll for MainPID (max 3 attempts)
    qint64 mainPid = 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        QThread::msleep(500);
        QProcess *showProc = m_ops->createProcess();
        m_ops->startProcess(showProc, QStringLiteral("systemctl"),
            {QStringLiteral("show"), serviceName, QStringLiteral("-p"), QStringLiteral("MainPID"), QStringLiteral("--value")});
        m_ops->waitForFinished(showProc, 5000);
        QByteArray output = m_ops->readAllStandardOutput(showProc).trimmed();
        delete showProc;

        if (!output.isEmpty()) {
            bool ok = false;
            mainPid = output.toLongLong(&ok);
            if (ok && mainPid > 0) break;
        }
    }

    if (mainPid <= 0) {
        qWarning() << "Could not get MainPID for transient unit" << serviceName;
        return 0;
    }

    m_usernameToUnitName[username] = serviceName;
    m_pidToUsername[mainPid] = username;

    saveState();

    monitorUnitState(serviceName, username, mainPid);

    qInfo() << "Started transient unit" << serviceName << "with PID" << mainPid;
    return mainPid;
}

void CouchPlayHelper::stopServiceInstance(const QString &serviceName)
{
    QProcess *stopProc = m_ops->createProcess();
    m_ops->startProcess(stopProc, QStringLiteral("systemctl"), {QStringLiteral("stop"), serviceName});
    m_ops->waitForFinished(stopProc, 10000);
    delete stopProc;

    QProcess *resetProc = m_ops->createProcess();
    m_ops->startProcess(resetProc, QStringLiteral("systemctl"), {QStringLiteral("reset-failed"), serviceName});
    m_ops->waitForFinished(resetProc, 5000);
    delete resetProc;
}

void CouchPlayHelper::monitorUnitState(const QString &serviceName, const QString &username, qint64 mainPid)
{
    QDBusMessage getUnitMsg = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.systemd1"),
        QStringLiteral("/org/freedesktop/systemd1"),
        QStringLiteral("org.freedesktop.systemd1.Manager"),
        QStringLiteral("GetUnit")
    );
    getUnitMsg << serviceName;

    QDBusMessage reply = QDBusConnection::systemBus().call(getUnitMsg);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        qWarning() << "monitorUnitState: GetUnit failed for" << serviceName << ":" << reply.errorMessage();
        return;
    }

    QDBusObjectPath unitPath;
    const QDBusArgument &arg = reply.arguments().at(0).value<QDBusArgument>();
    arg >> unitPath;

    QString path = unitPath.path();
    if (path.isEmpty()) {
        qWarning() << "monitorUnitState: empty unit path for" << serviceName;
        return;
    }

    auto *monitor = new UnitMonitor(path, serviceName, username, mainPid, this);
    m_monitors.insert(serviceName, monitor);
}

QString CouchPlayHelper::computeMountTarget(const QString &source, const QString &alias,
                                             const QString &userHome, const QString &compositorHome)
{
    // Determine where to mount the source directory in the user's home
    
    if (source.startsWith(compositorHome) && alias.isEmpty()) {
        // Home-relative: mount at same relative path in user's home
        QString relativePath = source.mid(compositorHome.length());
        return userHome + relativePath;
    } else if (!alias.isEmpty()) {
        // Has alias: mount at specified location relative to user's home
        if (alias.startsWith(QLatin1Char('/'))) {
            // Absolute alias - just append to home (remove leading slash)
            return userHome + alias;
        }
        return userHome + QStringLiteral("/") + alias;
    } else {
        // Non-home path, no alias: mount under .couchplay/mounts/
        return userHome + QStringLiteral("/.couchplay/mounts") + source;
    }
}

int CouchPlayHelper::MountSharedDirectories(const QString &username, uint compositorUid,
                                             const QStringList &directories)
{
    // Validate username
    if (!s_validUsername.match(username).hasMatch()) {
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("Invalid username format"));
        return 0;
    }

    if (!checkAuthorization(ACTION_MANAGE_MOUNTS)) {
        sendErrorReply(QDBusError::AccessDenied, 
            QStringLiteral("Not authorized to manage mounts"));
        return 0;
    }

    // Check if user exists
    if (!userExists(username)) {
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("User '%1' does not exist").arg(username));
        return 0;
    }

    QString userHome = getUserHome(username);
    if (userHome.isEmpty()) {
        sendErrorReply(QDBusError::Failed, 
            QStringLiteral("Could not determine home directory for user '%1'").arg(username));
        return 0;
    }

    QString compositorHome = getUserHomeByUid(compositorUid);
    if (compositorHome.isEmpty()) {
        sendErrorReply(QDBusError::Failed, 
            QStringLiteral("Could not determine home directory for compositor user"));
        return 0;
    }

    int successCount = 0;

    for (const QString &dirSpec : directories) {
        // Parse "source|alias" format
        QStringList parts = dirSpec.split(QLatin1Char('|'));
        if (parts.isEmpty()) {
            continue;
        }

        QString source = parts.at(0);
        QString alias = parts.size() > 1 ? parts.at(1) : QString();

        // Validate source path exists
        if (!m_ops->fileExists(source)) {
            qWarning() << "MountSharedDirectories: Source path does not exist:" << source;
            continue;
        }

        // Validate source is a directory
        if (!m_ops->isDirectory(source)) {
            qWarning() << "MountSharedDirectories: Source is not a directory:" << source;
            continue;
        }

        // Compute target path
        QString target = computeMountTarget(source, alias, userHome, compositorHome);

        // Create target directory if it doesn't exist
        if (!m_ops->fileExists(target)) {
            if (!m_ops->mkpath(target)) {
                qWarning() << "MountSharedDirectories: Failed to create target directory:" << target;
                continue;
            }
            // Set ownership of created directory to the target user
            uint userUid = getUserUid(username);
            struct passwd *pw = m_ops->getpwuid(userUid);
            if (pw) {
                m_ops->chown(target, userUid, pw->pw_gid);
            }
        }

        // Perform bind mount
        QProcess *mountProcess = m_ops->createProcess();
        m_ops->startProcess(mountProcess, QStringLiteral("/usr/bin/mount"),
            {QStringLiteral("--bind"), source, target});
        m_ops->waitForFinished(mountProcess, 10000);

        if (m_ops->processExitCode(mountProcess) != 0) {
            qWarning() << "MountSharedDirectories: Failed to mount" << source << "to" << target
                       << ":" << QString::fromLocal8Bit(m_ops->readStandardError(mountProcess));
            delete mountProcess;
            continue;
        }
        delete mountProcess;

        // Track the mount for cleanup
        MountInfo info;
        info.source = source;
        info.target = target;
        m_activeMounts[username].append(info);

        successCount++;
    }

    saveState();

    return successCount;
}

int CouchPlayHelper::UnmountSharedDirectories(const QString &username)
{
    // Validate username
    if (!s_validUsername.match(username).hasMatch()) {
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("Invalid username format"));
        return 0;
    }

    if (!checkAuthorization(ACTION_MANAGE_MOUNTS)) {
        sendErrorReply(QDBusError::AccessDenied, 
            QStringLiteral("Not authorized to manage mounts"));
        return 0;
    }

    if (!m_activeMounts.contains(username)) {
        return 0;  // No mounts to remove
    }

    int successCount = 0;
    QList<MountInfo> mounts = m_activeMounts[username];

    for (int i = mounts.size() - 1; i >= 0; --i) {
        const MountInfo &mount = mounts.at(i);

        QProcess *umountProc = m_ops->createProcess();
        m_ops->startProcess(umountProc, QStringLiteral("/usr/bin/umount"), {mount.target});
        m_ops->waitForFinished(umountProc, 10000);

        if (m_ops->processExitCode(umountProc) != 0) {
            qWarning() << "UnmountSharedDirectories: umount failed for" << mount.target
                       << "- trying lazy unmount";
            delete umountProc;

            QProcess *lazyProc = m_ops->createProcess();
            m_ops->startProcess(lazyProc, QStringLiteral("/usr/bin/umount"),
                {QStringLiteral("-l"), mount.target});
            m_ops->waitForFinished(lazyProc, 10000);
            if (m_ops->processExitCode(lazyProc) == 0) {
                successCount++;
            }
            delete lazyProc;
        } else {
            successCount++;
            delete umountProc;
        }
    }

    m_activeMounts.remove(username);
    saveState();
    return successCount;
}

int CouchPlayHelper::UnmountAllSharedDirectories()
{
    if (!checkAuthorization(ACTION_MANAGE_MOUNTS)) {
        sendErrorReply(QDBusError::AccessDenied, 
            QStringLiteral("Not authorized to manage mounts"));
        return 0;
    }

    int successCount = 0;
    QStringList users = m_activeMounts.keys();

    for (const QString &username : users) {
        QList<MountInfo> mounts = m_activeMounts[username];
        
        for (int i = mounts.size() - 1; i >= 0; --i) {
            const MountInfo &mount = mounts.at(i);

            QProcess *umountProc = m_ops->createProcess();
            m_ops->startProcess(umountProc, QStringLiteral("/usr/bin/umount"), {mount.target});
            m_ops->waitForFinished(umountProc, 10000);

            bool unmounted = (m_ops->processExitCode(umountProc) == 0);
            if (!unmounted) {
                QProcess *lazyProc = m_ops->createProcess();
                m_ops->startProcess(lazyProc, QStringLiteral("/usr/bin/umount"),
                    {QStringLiteral("-l"), mount.target});
                m_ops->waitForFinished(lazyProc, 10000);
                unmounted = (m_ops->processExitCode(lazyProc) == 0);
                delete lazyProc;
            }
            delete umountProc;
            if (unmounted) {
                successCount++;
            }
        }
        
        m_activeMounts.remove(username);
    }

    saveState();

    return successCount;
}

bool CouchPlayHelper::CopyFileToUser(const QString &sourcePath, const QString &targetPath,
                                      const QString &username)
{
    // Validate username
    if (!s_validUsername.match(username).hasMatch()) {
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("Invalid username format"));
        return false;
    }

    if (!checkAuthorization(ACTION_MANAGE_MOUNTS)) {
        sendErrorReply(QDBusError::AccessDenied, 
            QStringLiteral("Not authorized to copy files"));
        return false;
    }

    // Check if user exists
    if (!userExists(username)) {
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("User '%1' does not exist").arg(username));
        return false;
    }

    // Validate source file exists
    if (!m_ops->fileExists(sourcePath)) {
        qWarning() << "CopyFileToUser: Source file does not exist:" << sourcePath;
        sendErrorReply(QDBusError::InvalidArgs,
            QStringLiteral("Source file does not exist: %1").arg(sourcePath));
        return false;
    }

    // Get user info for ownership
    uint userUid = getUserUid(username);
    struct passwd *pw = m_ops->getpwuid(userUid);
    if (!pw) {
        qWarning() << "CopyFileToUser: Could not get user info for" << username;
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("Could not get user info for '%1'").arg(username));
        return false;
    }

    // Create parent directories if needed
    int lastSlash = targetPath.lastIndexOf(QLatin1Char('/'));
    QString targetDir = (lastSlash >= 0) ? targetPath.left(lastSlash) : QStringLiteral(".");

    QStringList dirsToChown;
    if (!validateUserPath(targetDir, username, QStringLiteral("CopyFileToUser"), dirsToChown)) {
        return false;
    }

    if (!m_ops->mkpath(targetDir)) {
        qWarning() << "CopyFileToUser: Failed to create directory:" << targetDir;
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("Failed to create directory: %1").arg(targetDir));
        return false;
    }

    for (const QString &dir : dirsToChown) {
        m_ops->chown(dir, userUid, pw->pw_gid);
    }

    // Copy the file
    // Remove target first if it exists
    if (m_ops->fileExists(targetPath)) {
        m_ops->removeFile(targetPath);
    }

    if (!m_ops->copyFile(sourcePath, targetPath)) {
        qWarning() << "CopyFileToUser: Failed to copy" << sourcePath << "to" << targetPath;
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("Failed to copy file from %1 to %2").arg(sourcePath, targetPath));
        return false;
    }

    // Set ownership on the copied file
    if (m_ops->chown(targetPath, userUid, pw->pw_gid) != 0) {
        qWarning() << "CopyFileToUser: Failed to set ownership on" << targetPath;
        // Don't fail - file was copied successfully
    }

    // Set permissions to 0644 (owner read/write, group/other read)
    if (m_ops->chmod(targetPath, 0644) != 0) {
        qWarning() << "CopyFileToUser: Failed to set permissions on" << targetPath;
    }

    return true;
}

bool CouchPlayHelper::WriteFileToUser(const QByteArray &content, const QString &targetPath,
                                       const QString &username)
{
    // Validate username
    if (!s_validUsername.match(username).hasMatch()) {
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("Invalid username format"));
        return false;
    }

    if (!checkAuthorization(ACTION_MANAGE_MOUNTS)) {
        sendErrorReply(QDBusError::AccessDenied, 
            QStringLiteral("Not authorized to write files"));
        return false;
    }

    // Check if user exists
    if (!userExists(username)) {
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("User '%1' does not exist").arg(username));
        return false;
    }

    // Get user info for ownership
    uint userUid = getUserUid(username);
    struct passwd *pw = m_ops->getpwuid(userUid);
    if (!pw) {
        qWarning() << "WriteFileToUser: Could not get user info for" << username;
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("Could not get user info for '%1'").arg(username));
        return false;
    }

    // Create parent directories if needed
    int lastSlash = targetPath.lastIndexOf(QLatin1Char('/'));
    QString targetDir = (lastSlash >= 0) ? targetPath.left(lastSlash) : QStringLiteral(".");

    QStringList dirsToChown;
    if (!validateUserPath(targetDir, username, QStringLiteral("WriteFileToUser"), dirsToChown)) {
        return false;
    }

    if (!m_ops->mkpath(targetDir)) {
        qWarning() << "WriteFileToUser: Failed to create directory:" << targetDir;
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("Failed to create directory: %1").arg(targetDir));
        return false;
    }

    for (const QString &dir : dirsToChown) {
        m_ops->chown(dir, userUid, pw->pw_gid);
    }

    // Write the file
    if (!m_ops->writeFile(targetPath, content)) {
        qWarning() << "WriteFileToUser: Failed to write to" << targetPath;
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("Failed to write to file"));
        return false;
    }

    // Set ownership on the file
    if (m_ops->chown(targetPath, userUid, pw->pw_gid) != 0) {
        qWarning() << "WriteFileToUser: Failed to set ownership on" << targetPath;
        // Don't fail - file was written successfully
    }

    // Set permissions to 0644
    if (m_ops->chmod(targetPath, 0644) != 0) {
        qWarning() << "WriteFileToUser: Failed to set permissions on" << targetPath;
    }

    return true;
}

bool CouchPlayHelper::CreateUserDirectory(const QString &path, const QString &username)
{
    // Validate username
    if (!s_validUsername.match(username).hasMatch()) {
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("Invalid username format"));
        return false;
    }

    if (!checkAuthorization(ACTION_MANAGE_MOUNTS)) {
        sendErrorReply(QDBusError::AccessDenied, 
            QStringLiteral("Not authorized to create directories"));
        return false;
    }

    // Check if user exists
    if (!userExists(username)) {
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("User '%1' does not exist").arg(username));
        return false;
    }

    // Get user info for ownership
    uint userUid = getUserUid(username);
    struct passwd *pw = m_ops->getpwuid(userUid);
    if (!pw) {
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("Could not get user info for '%1'").arg(username));
        return false;
    }

    QStringList dirsToChown;
    if (!validateUserPath(path, username, QStringLiteral("CreateUserDirectory"), dirsToChown)) {
        return false;
    }

    // Create directories
    if (!m_ops->mkpath(path)) {
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("Failed to create directory: %1").arg(path));
        return false;
    }

    for (const QString &dir : dirsToChown) {
        m_ops->chown(dir, userUid, pw->pw_gid);
    }

    return true;
}

bool CouchPlayHelper::SetDirectoryAcl(const QString &path, const QString &username, bool recursive)
{
    // Validate username
    if (!s_validUsername.match(username).hasMatch()) {
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("Invalid username format"));
        return false;
    }

    if (!checkAuthorization(ACTION_MANAGE_MOUNTS)) {
        sendErrorReply(QDBusError::AccessDenied, 
            QStringLiteral("Not authorized to set directory ACLs"));
        return false;
    }

    // Check if user exists
    if (!userExists(username)) {
        sendErrorReply(QDBusError::InvalidArgs,
            QStringLiteral("User '%1' does not exist").arg(username));
        return false;
    }

    // Check if path exists
    if (!m_ops->fileExists(path)) {
        sendErrorReply(QDBusError::InvalidArgs,
            QStringLiteral("Path does not exist: %1").arg(path));
        return false;
    }

    // Build setfacl command
    // setfacl -m u:username:rx /path (non-recursive)
    // setfacl -R -m u:username:rx /path (recursive)
    QStringList args;
    if (recursive) {
        args << QStringLiteral("-R");
    }
    args << QStringLiteral("-m");
    args << QStringLiteral("u:%1:rx").arg(username);
    args << path;

    QProcess *setfacl = m_ops->createProcess();
    m_ops->startProcess(setfacl, QStringLiteral("setfacl"), args);

    if (!m_ops->waitForFinished(setfacl, 60000)) {  // 60 second timeout for recursive operations
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("setfacl timed out for path: %1").arg(path));
        delete setfacl;
        return false;
    }

    if (m_ops->processExitCode(setfacl) != 0) {
        QString errorOutput = QString::fromUtf8(m_ops->readStandardError(setfacl));
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("setfacl failed for path %1: %2").arg(path, errorOutput));
        delete setfacl;
        return false;
    }
    delete setfacl;

    return true;
}

bool CouchPlayHelper::SetPathAclWithParents(const QString &path, const QString &username)
{
    // Validate username
    if (!s_validUsername.match(username).hasMatch()) {
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("Invalid username format"));
        return false;
    }

    if (!checkAuthorization(ACTION_MANAGE_MOUNTS)) {
        sendErrorReply(QDBusError::AccessDenied, 
            QStringLiteral("Not authorized to set directory ACLs"));
        return false;
    }

    // Check if user exists
    if (!userExists(username)) {
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("User '%1' does not exist").arg(username));
        return false;
    }

    // Check if path exists
    if (!m_ops->fileExists(path)) {
        sendErrorReply(QDBusError::InvalidArgs,
            QStringLiteral("Path does not exist: %1").arg(path));
        return false;
    }

    // Safe boundaries where we stop traversing upward
    // These are directories that either already have proper permissions
    // or we shouldn't modify
    static const QStringList stopBoundaries = {
        QStringLiteral("/run/media"),
        QStringLiteral("/media"),
        QStringLiteral("/mnt"),
        QStringLiteral("/home"),
        QStringLiteral("/var/home"),  // Bazzite/Fedora Silverblue
        QStringLiteral("/"),
    };

    // Collect all parent directories that need ACLs
    QStringList pathsToSet;
    QString current = path;
    
    // Normalize path (remove trailing slash)
    while (current.endsWith(QLatin1Char('/')) && current.length() > 1) {
        current.chop(1);
    }
    
    // Add the target path first
    pathsToSet.prepend(current);
    
    // Walk up the directory tree
    while (true) {
        int lastSlash = current.lastIndexOf(QLatin1Char('/'));
        if (lastSlash <= 0) {
            break;  // Reached root
        }
        
        current = current.left(lastSlash);
        if (current.isEmpty()) {
            current = QStringLiteral("/");
        }
        
        // Check if we've reached a stop boundary
        bool atBoundary = false;
        for (const QString &boundary : stopBoundaries) {
            if (current == boundary || current.length() < boundary.length()) {
                atBoundary = true;
                break;
            }
        }
        
        if (atBoundary) {
            break;
        }
        
        // Add this parent to the list (prepend so we set from top down)
        pathsToSet.prepend(current);
    }

    // Set ACL on each path (non-recursive, just rx for traversal)
    bool allSucceeded = true;
    for (const QString &p : pathsToSet) {
        if (!m_ops->fileExists(p)) {
            qWarning() << "SetPathAclWithParents: Path does not exist, skipping:" << p;
            continue;
        }

        QStringList args;
        args << QStringLiteral("-m");
        args << QStringLiteral("u:%1:rx").arg(username);
        args << p;

        QProcess *setfacl = m_ops->createProcess();
        m_ops->startProcess(setfacl, QStringLiteral("setfacl"), args);

        if (!m_ops->waitForFinished(setfacl, 5000)) {
            qWarning() << "SetPathAclWithParents: setfacl timed out for:" << p;
            allSucceeded = false;
            delete setfacl;
            continue;
        }

        if (m_ops->processExitCode(setfacl) != 0) {
            QString errorOutput = QString::fromUtf8(m_ops->readStandardError(setfacl));
            qWarning() << "SetPathAclWithParents: setfacl failed for" << p << ":" << errorOutput;
            // Continue anyway - some paths might not support ACLs (e.g., NTFS)
            // but mount point does
        }
        delete setfacl;
    }

    return allSucceeded;
}

QString CouchPlayHelper::GetUserSteamId(const QString &username)
{
    // Validate username
    if (!s_validUsername.match(username).hasMatch()) {
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("Invalid username format"));
        return QString();
    }

    // Check if user exists
    if (!userExists(username)) {
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("User '%1' does not exist").arg(username));
        return QString();
    }

    QString userHome = getUserHome(username);
    if (userHome.isEmpty()) {
        return QString();
    }

    // Check for Steam userdata in common locations
    QStringList possibleRoots = {
        userHome + QStringLiteral("/.steam/steam/userdata"),
        userHome + QStringLiteral("/.local/share/Steam/userdata"),
    };

    for (const QString &userDataBase : possibleRoots) {
        if (!m_ops->fileExists(userDataBase)) {
            continue;
        }

        // Find first numeric directory (Steam user ID)
        QStringList entries = m_ops->entryList(userDataBase, QStringList(), QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &entry : entries) {
            bool ok;
            entry.toULongLong(&ok);
            if (ok) {
                return entry;
            }
        }
    }

    return QString();
}

void CouchPlayHelper::saveState()
{
    QJsonObject root;
    root[QStringLiteral("version")] = 1;

    QJsonArray devicesArray;
    for (const QString &device : m_modifiedDevices) {
        devicesArray.append(device);
    }
    root[QStringLiteral("modifiedDevices")] = devicesArray;

    QJsonObject mountsObject;
    for (auto it = m_activeMounts.constBegin(); it != m_activeMounts.constEnd(); ++it) {
        QJsonArray mountsArray;
        for (const MountInfo &info : it.value()) {
            QJsonObject mountObj;
            mountObj[QStringLiteral("source")] = info.source;
            mountObj[QStringLiteral("target")] = info.target;
            mountsArray.append(mountObj);
        }
        mountsObject[it.key()] = mountsArray;
    }
    root[QStringLiteral("activeMounts")] = mountsObject;

    QJsonObject unitsObject;
    for (auto it = m_usernameToUnitName.constBegin(); it != m_usernameToUnitName.constEnd(); ++it) {
        unitsObject[it.key()] = it.value();
    }
    root[QStringLiteral("activeUnits")] = unitsObject;

    QJsonObject pidObject;
    for (auto it = m_pidToUsername.constBegin(); it != m_pidToUsername.constEnd(); ++it) {
        pidObject[QString::number(it.key())] = it.value();
    }
    root[QStringLiteral("pidToUsername")] = pidObject;

    QJsonArray runtimeUids;
    for (uint uid : m_runtimeAccessSetForUid) {
        runtimeUids.append(static_cast<qint64>(uid));
    }
    root[QStringLiteral("runtimeAccessUids")] = runtimeUids;

    QJsonDocument doc(root);
    QByteArray data = doc.toJson(QJsonDocument::Compact);

    QDir().mkpath(QFileInfo(m_stateFilePath).absolutePath());

    QSaveFile file(m_stateFilePath);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "saveState: Failed to open" << m_stateFilePath << ":" << file.errorString();
        return;
    }
    file.write(data);
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    if (!file.commit()) {
        qWarning() << "saveState: Failed to commit" << m_stateFilePath << ":" << file.errorString();
    }
}

void CouchPlayHelper::loadAndReconcileState()
{
    QDir().mkpath(QStringLiteral("/run/couchplay"));

    QFile file(m_stateFilePath);
    if (!file.exists()) {
        qDebug() << "loadAndReconcileState: No state file found at" << m_stateFilePath << "- starting fresh";
        return;
    }

    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "loadAndReconcileState: Failed to open" << m_stateFilePath << "- starting fresh";
        return;
    }

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "loadAndReconcileState: Failed to parse state file:" << parseError.errorString() << "- starting fresh";
        return;
    }

    QJsonObject root = doc.object();
    int version = root.value(QStringLiteral("version")).toInt(0);
    if (version != 1) {
        qWarning() << "loadAndReconcileState: Unknown state version" << version << "- starting fresh";
        return;
    }

    bool changed = false;

    QSet<QString> activeSystemdUnits;
    {
        QProcess proc;
        proc.start(QStringLiteral("systemctl"), {QStringLiteral("list-units"), QStringLiteral("couchplay-*.service"), QStringLiteral("--no-legend"), QStringLiteral("--no-pager")});
        proc.waitForFinished(5000);
        QByteArray output = proc.readAllStandardOutput();
        for (const QByteArray &line : output.split('\n')) {
            QByteArray trimmed = line.trimmed();
            if (trimmed.isEmpty()) continue;
            int spacePos = trimmed.indexOf(' ');
            if (spacePos > 0) {
                activeSystemdUnits.insert(QString::fromUtf8(trimmed.left(spacePos)));
            }
        }
    }

    QJsonObject unitsObject = root.value(QStringLiteral("activeUnits")).toObject();
    QMap<QString, QString> loadedUsernameToUnit;
    for (auto it = unitsObject.constBegin(); it != unitsObject.constEnd(); ++it) {
        QString unitName = it.value().toString();
        if (activeSystemdUnits.contains(unitName)) {
            loadedUsernameToUnit[it.key()] = unitName;
        } else {
            qDebug() << "loadAndReconcileState: Removing stale unit" << unitName << "for user" << it.key();
            changed = true;
        }
    }
    m_usernameToUnitName = loadedUsernameToUnit;

    QJsonObject pidObject = root.value(QStringLiteral("pidToUsername")).toObject();
    QMap<qint64, QString> loadedPidToUsername;
    QSet<QString> activeUsernames;
    for (const QString &unitName : m_usernameToUnitName) {
        activeUsernames.insert(m_usernameToUnitName.key(unitName));
    }
    for (auto it = pidObject.constBegin(); it != pidObject.constEnd(); ++it) {
        bool ok = false;
        qint64 pid = it.key().toLongLong(&ok);
        if (!ok) { changed = true; continue; }
        QString username = it.value().toString();
        if (activeUsernames.contains(username)) {
            loadedPidToUsername[pid] = username;
        } else {
            qDebug() << "loadAndReconcileState: Removing stale PID" << pid << "for user" << username;
            changed = true;
        }
    }
    m_pidToUsername = loadedPidToUsername;

    QJsonArray devicesArray = root.value(QStringLiteral("modifiedDevices")).toArray();
    QStringList loadedDevices;
    for (const QJsonValue &val : devicesArray) {
        QString device = val.toString();
        if (m_ops->fileExists(device)) {
            loadedDevices.append(device);
        } else {
            qDebug() << "loadAndReconcileState: Removing gone device" << device;
            changed = true;
        }
    }
    m_modifiedDevices = loadedDevices;

    QJsonObject mountsObject = root.value(QStringLiteral("activeMounts")).toObject();
    QMap<QString, QList<MountInfo>> loadedMounts;
    for (auto it = mountsObject.constBegin(); it != mountsObject.constEnd(); ++it) {
        QString username = it.key();
        QJsonArray mountsArray = it.value().toArray();
        QList<MountInfo> userMounts;
        for (const QJsonValue &mountVal : mountsArray) {
            QJsonObject mountObj = mountVal.toObject();
            QString target = mountObj.value(QStringLiteral("target")).toString();

            QFile mountsFile(QStringLiteral("/proc/mounts"));
            bool isMounted = false;
            if (mountsFile.open(QIODevice::ReadOnly)) {
                QByteArray mountsData = mountsFile.readAll();
                mountsFile.close();
                isMounted = mountsData.contains(target.toUtf8());
            }

            if (isMounted) {
                MountInfo info;
                info.source = mountObj.value(QStringLiteral("source")).toString();
                info.target = target;
                userMounts.append(info);
            } else {
                qDebug() << "loadAndReconcileState: Removing inactive mount" << target;
                changed = true;
            }
        }
        if (!userMounts.isEmpty()) {
            loadedMounts[username] = userMounts;
        }
    }
    m_activeMounts = loadedMounts;

    QJsonArray runtimeUids = root.value(QStringLiteral("runtimeAccessUids")).toArray();
    QSet<uint> loadedRuntimeUids;
    for (const QJsonValue &val : runtimeUids) {
        loadedRuntimeUids.insert(static_cast<uint>(val.toInteger()));
    }
    m_runtimeAccessSetForUid = loadedRuntimeUids;

    if (changed) {
        saveState();
    }

    qDebug() << "loadAndReconcileState: Restored" << m_usernameToUnitName.size() << "units,"
             << m_modifiedDevices.size() << "devices," << m_activeMounts.size() << "mount users,"
             << m_runtimeAccessSetForUid.size() << "runtime UIDs";
}

#include "CouchPlayHelper.moc"
