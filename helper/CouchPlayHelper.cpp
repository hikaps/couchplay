// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "CouchPlayHelper.h"
#include "SystemOps.h"

#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QThread>
#include <QDebug>

#include <cerrno>
#include <cstring>
#include <sys/mount.h>
#include <unistd.h>
#include <sys/stat.h>
#include <signal.h>
#include <pwd.h>
#include <grp.h>

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
{
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

    if (!m_pidToServiceName.isEmpty()) {
        for (auto it = m_pidToServiceName.constBegin(); it != m_pidToServiceName.constEnd(); ++it) {
            qInfo() << "Cleaning up service" << it.value() << "PID" << it.key();
            stopServiceInstance(it.value());
        }
        m_pidToServiceName.clear();
    }

    // Clean up: unmount all shared directories
    if (!m_activeMounts.isEmpty()) {
        for (const QString &username : m_activeMounts.keys()) {
            for (const MountInfo &mount : m_activeMounts[username]) {
                QProcess *umountProcess = m_ops->createProcess();
                m_ops->startProcess(umountProcess, QStringLiteral("umount"), {mount.target});
                m_ops->waitForFinished(umountProcess, 5000);
                if (m_ops->processExitCode(umountProcess) != 0) {
                    // Try lazy unmount
                    QProcess *lazyProcess = m_ops->createProcess();
                    m_ops->startProcess(lazyProcess, QStringLiteral("umount"), {QStringLiteral("-l"), mount.target});
                    m_ops->waitForFinished(lazyProcess, 5000);
                    delete lazyProcess;
                }
                delete umountProcess;
            }
        }
        m_activeMounts.clear();
    }

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
    
    return successCount;
}

uint CouchPlayHelper::CreateUser(const QString &username, const QString &fullName)
{
    // Validate username (alphanumeric, lowercase, starts with letter)
    static QRegularExpression validUsername(QStringLiteral("^[a-z][a-z0-9_-]{0,31}$"));
    if (!validUsername.match(username).hasMatch()) {
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
    // This is required for machinectl shell to work properly
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
    static QRegularExpression validUsername(QStringLiteral("^[a-z][a-z0-9_-]{0,31}$"));
    if (!validUsername.match(username).hasMatch()) {
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
    static QRegularExpression validUsername(QStringLiteral("^[a-z][a-z0-9_-]{0,31}$"));
    if (!validUsername.match(username).hasMatch()) {
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
        bool success = (m_ops->processExitCode(proc) == 0);
        if (!success) {
            qWarning() << "Failed to set ACL on" << path << ":"
                       << QString::fromLocal8Bit(m_ops->readStandardError(proc));
        }
        delete proc;
        return success;
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

QString CouchPlayHelper::hashGamePath(const QString &gamePath)
{
    return QString::fromLatin1(QCryptographicHash::hash(
        QDir::cleanPath(gamePath).toUtf8(), QCryptographicHash::Md5).toHex().left(16));
}

qint64 CouchPlayHelper::LaunchInstance(const QString &username, uint compositorUid,
                                        const QStringList &gamescopeArgs,
                                        const QString &gameCommand,
                                        const QStringList &environment,
                                        const QString &gamePath)
{
    // Validate username
    static QRegularExpression validUsername(QStringLiteral("^[a-z][a-z0-9_-]{0,31}$"));
    if (!validUsername.match(username).hasMatch()) {
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("Invalid username format"));
        return 0;
    }

    if (!checkAuthorization(ACTION_LAUNCH_INSTANCE)) {
        sendErrorReply(QDBusError::AccessDenied, 
            QStringLiteral("Not authorized to launch instances"));
        return 0;
    }

    // Check if user exists
    if (!userExists(username)) {
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("User '%1' does not exist").arg(username));
        return 0;
    }

    // Verify compositor user exists
    struct passwd *pw = m_ops->getpwuid(compositorUid);
    if (!pw) {
        sendErrorReply(QDBusError::InvalidArgs,
            QStringLiteral("Compositor user with UID %1 does not exist").arg(compositorUid));
        return 0;
    }

    // Set up runtime access for couchplay group (once per compositor)
    // This grants access to Wayland, PipeWire, and PulseAudio sockets
    if (!m_runtimeAccessSetForUid.contains(compositorUid)) {
        if (!SetupRuntimeAccess(compositorUid)) {
            qWarning() << "Failed to set up runtime access for compositor" << compositorUid;
            // Continue anyway - may already be set from previous session
        }
    }

    QStringList bindPaths;
    if (!gamePath.isEmpty()) {
        QString gameId = hashGamePath(gamePath);
        QString mountPoint = QStringLiteral("/run/couchplay/mounts/%1/%2").arg(username, gameId);
        if (m_ops->isDirectory(mountPoint)) {
            bindPaths << QStringLiteral("%1:%2").arg(mountPoint, gamePath);
        }
    }

    QString serviceName = generateServiceName(username);
    qint64 pid = startTransientUnit(username, compositorUid, gamescopeArgs,
                                    gameCommand, environment, bindPaths);
    if (pid <= 0) {
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("Failed to start transient unit for user '%1'").arg(username));
        return 0;
    }

    m_pidToServiceName.insert(pid, serviceName);
    qInfo() << "LaunchInstance: started service" << serviceName
            << "PID" << pid << "user" << username
            << "bind paths:" << bindPaths.size();
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
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("Invalid PID"));
        return false;
    }

    if (m_pidToServiceName.contains(pid)) {
        QString serviceName = m_pidToServiceName.value(pid);
        qInfo() << "StopInstance: stopping service" << serviceName << "PID" << pid;
        stopServiceInstance(serviceName);
        QProcess *resetProc = m_ops->createProcess();
        m_ops->startProcess(resetProc, QStringLiteral("systemctl"), {QStringLiteral("reset-failed"), serviceName});
        m_ops->waitForFinished(resetProc, 5000);
        delete resetProc;
        m_pidToServiceName.remove(pid);
        return true;
    }

    if (m_ops->killProcess(static_cast<pid_t>(pid), SIGTERM)) {
        return true;
    }

    sendErrorReply(QDBusError::Failed, 
        QStringLiteral("Failed to stop process %1: %2")
            .arg(pid).arg(QString::fromLocal8Bit(strerror(errno))));
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
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("Invalid PID"));
        return false;
    }

    if (m_pidToServiceName.contains(pid)) {
        QString serviceName = m_pidToServiceName.value(pid);
        qInfo() << "KillInstance: killing service" << serviceName << "PID" << pid;
        stopServiceInstance(serviceName);
        QProcess *resetProc = m_ops->createProcess();
        m_ops->startProcess(resetProc, QStringLiteral("systemctl"), {QStringLiteral("reset-failed"), serviceName});
        m_ops->waitForFinished(resetProc, 5000);
        delete resetProc;
        m_pidToServiceName.remove(pid);
        return true;
    }

    if (pid > 0 && m_ops->killProcess(static_cast<pid_t>(pid), SIGKILL)) {
        return true;
    }

    sendErrorReply(QDBusError::Failed, 
        QStringLiteral("Failed to kill process %1: %2")
            .arg(pid).arg(QString::fromLocal8Bit(strerror(errno))));
    return false;
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
    QString userUid = pwd ? QString::number(pwd->pw_uid) : QStringLiteral("1000");
    QString compositorRuntimeDir = QStringLiteral("/run/user/%1").arg(compositorUid);

    QStringList systemdRunArgs;
    systemdRunArgs << QStringLiteral("--unit") << serviceName;
    systemdRunArgs << QStringLiteral("--uid") << username;
    systemdRunArgs << QStringLiteral("--property=Type=simple");
    systemdRunArgs << QStringLiteral("--property=Delegate=yes");
    systemdRunArgs << QStringLiteral("--property=MemoryDenyWriteExecute=false");
    systemdRunArgs << QStringLiteral("--property=PrivateMounts=no");

    QStringList envParts;
    envParts << QStringLiteral("DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/%1/bus").arg(userUid);
    envParts << QStringLiteral("WAYLAND_DISPLAY=%1/wayland-0").arg(compositorRuntimeDir);
    envParts << QStringLiteral("XDG_RUNTIME_DIR=/run/user/%1").arg(userUid);
    envParts << QStringLiteral("PIPEWIRE_RUNTIME_DIR=/run/user/%1").arg(userUid);
    envParts << QStringLiteral("PULSE_SERVER=unix:%1/pulse/native").arg(compositorRuntimeDir);
    for (const QString &var : environment) {
        envParts << var;
    }

    systemdRunArgs << QStringLiteral("--property=BindReadOnlyPaths=%1").arg(compositorRuntimeDir);

    for (const QString &bindPath : bindPaths) {
        int colonPos = bindPath.indexOf(QLatin1Char(':'));
        if (colonPos < 0) continue;
        QString escaped = bindPath;
        escaped.replace(QLatin1Char(' '), QStringLiteral("\\ "));
        QString arg = QStringLiteral("--property=BindPaths=%1").arg(escaped);
        systemdRunArgs << arg;
    }

    QStringList cmdArgs;
    cmdArgs << QStringLiteral("/usr/bin/gamescope") << gamescopeArgs;

    QStringList gameParts = QProcess::splitCommand(gameCommand);
    cmdArgs << QStringLiteral("--") << gameParts;

    systemdRunArgs << QStringLiteral("--property=Environment=%1").arg(envParts.join(QLatin1Char(' ')));

    systemdRunArgs << QStringLiteral("--") << cmdArgs;

    QProcess *proc = m_ops->createProcess();
    m_ops->startProcess(proc, QStringLiteral("systemd-run"), systemdRunArgs);
    m_ops->waitForFinished(proc, 10000);
    int exitCode = m_ops->processExitCode(proc);
    if (exitCode != 0) {
        QByteArray errOutput = m_ops->readStandardError(proc);
        delete proc;

        if (errOutput.contains("already loaded")) {
            qInfo() << "Stale unit" << serviceName << "found — stopping and retrying";
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

    qint64 mainPid = 0;
    for (int attempt = 0; attempt < 30; ++attempt) {
        QThread::msleep(200);
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
    }

    return mainPid;
}

bool CouchPlayHelper::stopServiceInstance(const QString &serviceName)
{
    QProcess *stopProcess = m_ops->createProcess();
    m_ops->startProcess(stopProcess, QStringLiteral("systemctl"), {QStringLiteral("stop"), serviceName});
    m_ops->waitForFinished(stopProcess, 15000);
    int exitCode = m_ops->processExitCode(stopProcess);
    delete stopProcess;

    if (exitCode != 0) {
        qWarning() << "systemctl stop failed for" << serviceName << "exit code:" << exitCode;
        return false;
    }
    return true;
}

int CouchPlayHelper::MountSharedDirectories(const QString &username, uint compositorUid,
                                             const QStringList &directories)
{
    // Validate username
    static QRegularExpression validUsername(QStringLiteral("^[a-z][a-z0-9_-]{0,31}$"));
    if (!validUsername.match(username).hasMatch()) {
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
        m_ops->startProcess(mountProcess, QStringLiteral("mount"),
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

    return successCount;
}

int CouchPlayHelper::UnmountSharedDirectories(const QString &username)
{
    // Validate username
    static QRegularExpression validUsername(QStringLiteral("^[a-z][a-z0-9_-]{0,31}$"));
    if (!validUsername.match(username).hasMatch()) {
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

    // Unmount in reverse order (in case of nested mounts)
    for (int i = mounts.size() - 1; i >= 0; --i) {
        const MountInfo &mount = mounts.at(i);

        QProcess *umountProcess = m_ops->createProcess();
        m_ops->startProcess(umountProcess, QStringLiteral("umount"), {mount.target});
        m_ops->waitForFinished(umountProcess, 10000);

        if (m_ops->processExitCode(umountProcess) == 0) {
            successCount++;
            delete umountProcess;
        } else {
            // Try lazy unmount as fallback
            QProcess *lazyUmount = m_ops->createProcess();
            m_ops->startProcess(lazyUmount, QStringLiteral("umount"), {QStringLiteral("-l"), mount.target});
            m_ops->waitForFinished(lazyUmount, 10000);

            if (m_ops->processExitCode(lazyUmount) == 0) {
                successCount++;
            } else {
                qWarning() << "UnmountSharedDirectories: Failed to unmount" << mount.target
                           << ":" << QString::fromLocal8Bit(m_ops->readStandardError(umountProcess));
            }
            delete umountProcess;
            delete lazyUmount;
        }
    }

    m_activeMounts.remove(username);
    return successCount;
}

int CouchPlayHelper::UnmountAllSharedDirectories()
{
    if (!checkAuthorization(ACTION_MANAGE_MOUNTS)) {
        sendErrorReply(QDBusError::AccessDenied, 
            QStringLiteral("Not authorized to manage mounts"));
        return 0;
    }

    int totalCount = 0;
    QStringList users = m_activeMounts.keys();

    for (const QString &username : users) {
        // Call without auth check since we already checked above
        QList<MountInfo> mounts = m_activeMounts[username];
        
        for (int i = mounts.size() - 1; i >= 0; --i) {
            const MountInfo &mount = mounts.at(i);

            QProcess *umountProcess = m_ops->createProcess();
            m_ops->startProcess(umountProcess, QStringLiteral("umount"), {mount.target});
            m_ops->waitForFinished(umountProcess, 10000);

            if (m_ops->processExitCode(umountProcess) == 0) {
                totalCount++;
                delete umountProcess;
            } else {
                // Try lazy unmount as fallback
                QProcess *lazyUmount = m_ops->createProcess();
                m_ops->startProcess(lazyUmount, QStringLiteral("umount"), {QStringLiteral("-l"), mount.target});
                m_ops->waitForFinished(lazyUmount, 10000);

                if (m_ops->processExitCode(lazyUmount) == 0) {
                    totalCount++;
                } else {
                    qWarning() << "UnmountAllSharedDirectories: Failed to unmount" << mount.target;
                }
                delete umountProcess;
                delete lazyUmount;
            }
        }
        
        m_activeMounts.remove(username);
    }

    return totalCount;
}

bool CouchPlayHelper::CopyFileToUser(const QString &sourcePath, const QString &targetPath,
                                      const QString &username)
{
    // Validate username
    static QRegularExpression validUsername(QStringLiteral("^[a-z][a-z0-9_-]{0,31}$"));
    if (!validUsername.match(username).hasMatch()) {
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
    static QRegularExpression validUsername(QStringLiteral("^[a-z][a-z0-9_-]{0,31}$"));
    if (!validUsername.match(username).hasMatch()) {
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
    static QRegularExpression validUsername(QStringLiteral("^[a-z][a-z0-9_-]{0,31}$"));
    if (!validUsername.match(username).hasMatch()) {
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
    static QRegularExpression validUsername(QStringLiteral("^[a-z][a-z0-9_-]{0,31}$"));
    if (!validUsername.match(username).hasMatch()) {
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
    static QRegularExpression validUsername(QStringLiteral("^[a-z][a-z0-9_-]{0,31}$"));
    if (!validUsername.match(username).hasMatch()) {
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
    static QRegularExpression validUsername(QStringLiteral("^[a-z][a-z0-9_-]{0,31}$"));
    if (!validUsername.match(username).hasMatch()) {
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

bool CouchPlayHelper::SetupOverlayMount(const QString &username, const QString &gamePath,
                                          const QString &gameId, const QStringList &overrideFiles,
                                          uint compositorUid)
{
    Q_UNUSED(compositorUid)
    qInfo() << "SetupOverlayMount: user" << username
             << "gamePath:" << gamePath
             << "gameId:" << gameId
             << "overrideFiles:" << overrideFiles.size();

    static QRegularExpression validUsername(QStringLiteral("^[a-z][a-z0-9_-]{0,31}$"));
    if (!validUsername.match(username).hasMatch()) {
        qWarning() << "Invalid username format for user" << username;
        sendErrorReply(QDBusError::InvalidArgs,
            QStringLiteral("Invalid username format"));
        return false;
    }

    if (!checkAuthorization(ACTION_MANAGE_MOUNTS)) {
        qWarning() << "User" << username << "not authorized to manage mounts";
        sendErrorReply(QDBusError::AccessDenied,
            QStringLiteral("Not authorized to manage mounts"));
        return false;
    }

    if (!userExists(username)) {
        qWarning() << "User" << username << "does not exist";
        sendErrorReply(QDBusError::InvalidArgs,
            QStringLiteral("User '%1' does not exist").arg(username));
        return false;
    }

    if (!IsInCouchPlayGroup(username)) {
        qWarning() << "User" << username << "is not a CouchPlay user";
        sendErrorReply(QDBusError::AccessDenied,
            QStringLiteral("User '%1' is not a CouchPlay user").arg(username));
        return false;
    }

    if (!gamePath.startsWith(QLatin1Char('/'))) {
        qWarning() << "gamePath must be an absolute path for user" << username;
        sendErrorReply(QDBusError::InvalidArgs,
            QStringLiteral("gamePath must be an absolute path"));
        return false;
    }

    if (!m_ops->fileExists(gamePath)) {
        qWarning() << "gamePath does not exist:" << gamePath << "for user" << username;
        sendErrorReply(QDBusError::InvalidArgs,
            QStringLiteral("gamePath does not exist: %1").arg(gamePath));
        return false;
    }

    if (!m_ops->isDirectory(gamePath)) {
        qWarning() << "gamePath is not a directory:" << gamePath << "for user" << username;
        sendErrorReply(QDBusError::InvalidArgs,
            QStringLiteral("gamePath is not a directory: %1").arg(gamePath));
        return false;
    }

    if (gameId.isEmpty()) {
        qWarning() << "gameId cannot be empty for user" << username;
        sendErrorReply(QDBusError::InvalidArgs,
            QStringLiteral("gameId cannot be empty"));
        return false;
    }

    QString userHome = getUserHome(username);
    if (userHome.isEmpty()) {
        qWarning() << "Could not determine home directory for user" << username;
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("Could not determine home directory for user '%1'").arg(username));
        return false;
    }

    uint userUid = getUserUid(username);
    struct passwd *pw = m_ops->getpwuid(userUid);
    if (!pw) {
        qWarning() << "Could not get user info for" << username;
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("Could not get user info for '%1'").arg(username));
        return false;
    }

    QString overlayBase = userHome + QStringLiteral("/.couchplay/overlays/") + gameId;
    QString upperDir = overlayBase + QStringLiteral("/upper");
    QString workDir = overlayBase + QStringLiteral("/work");
    QString mountPoint = QStringLiteral("/run/couchplay/mounts/%1/%2").arg(username, gameId);

    // Check if mount point is already active (from a crashed previous session)
    QFile mountInfoFile(QStringLiteral("/proc/mounts"));
    if (mountInfoFile.open(QIODevice::ReadOnly)) {
        QByteArray mounts = mountInfoFile.readAll();
        mountInfoFile.close();
        QByteArray needle = (mountPoint + QLatin1Char(' ')).toUtf8();
        if (mounts.contains(needle)) {
            qInfo() << "Mount point already active, tearing down existing overlay:" << mountPoint;
            TeardownOverlayMount(username, gameId);
        }
    }

    // Clean stale workdir (overlayfs requires empty workdir)
    QDir(workDir).removeRecursively();

    if (!m_ops->mkpath(upperDir)) {
        qWarning() << "Failed to create upper directory:" << upperDir << "for user" << username;
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("Failed to create upper directory: %1").arg(upperDir));
        return false;
    }

    if (!m_ops->mkpath(workDir)) {
        qWarning() << "Failed to create work directory:" << workDir << "for user" << username;
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("Failed to create work directory: %1").arg(workDir));
        return false;
    }

    for (const QString &part : {QStringLiteral(".couchplay"), QStringLiteral("overlays"), gameId, QStringLiteral("upper"), QStringLiteral("work")}) {
        QString dirPath = userHome + QStringLiteral("/") + part;
        m_ops->chown(dirPath, userUid, pw->pw_gid);
    }

    for (const QString &relPath : overrideFiles) {
        if (relPath.contains(QStringLiteral("..")) || relPath.startsWith(QLatin1Char('/'))) {
            continue;
        }

        QString srcFile = gamePath + QStringLiteral("/") + relPath;
        QString dstFile = upperDir + QStringLiteral("/") + relPath;

        if (!m_ops->fileExists(srcFile)) {
            continue;
        }

        int lastSlash = dstFile.lastIndexOf(QLatin1Char('/'));
        if (lastSlash > 0) {
            QString dstDir = dstFile.left(lastSlash);
            if (!m_ops->mkpath(dstDir)) {
                continue;
            }
            QString dirWalker = upperDir;
            for (const QString &part : dstDir.mid(upperDir.length()).split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
                dirWalker += QStringLiteral("/") + part;
                m_ops->chown(dirWalker, userUid, pw->pw_gid);
            }
        }

        if (!m_ops->fileExists(dstFile)) {
            if (m_ops->copyFile(srcFile, dstFile)) {
                m_ops->chown(dstFile, userUid, pw->pw_gid);
            }
        }
    }

    if (!m_ops->mkpath(mountPoint)) {
        qWarning() << "Failed to create mount point:" << mountPoint << "for user" << username;
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("Failed to create mount point: %1").arg(mountPoint));
        return false;
    }

    QString options = QStringLiteral("lowerdir=%1,upperdir=%2,workdir=%3,metacopy=on")
                          .arg(gamePath, upperDir, workDir);

    int mountResult = m_ops->mount(QStringLiteral("none"), mountPoint,
                                   QStringLiteral("overlay"), 0, options);
    if (mountResult != 0) {
        qWarning() << "overlayfs mount failed for user" << username
                   << "gameId" << gameId
                   << "mount point:" << mountPoint
                   << "options:" << options;
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("overlayfs mount failed for user '%1' game '%2' (check helper logs)")
                .arg(username, gameId));
         return false;
    }

    qInfo() << "Overlay mounted for user" << username
            << "gameId" << gameId
            << "at" << mountPoint;
    return true;
}

bool CouchPlayHelper::TeardownOverlayMount(const QString &username, const QString &gameId)
{
    static QRegularExpression validUsername(QStringLiteral("^[a-z][a-z0-9_-]{0,31}$"));
    if (!validUsername.match(username).hasMatch()) {
        sendErrorReply(QDBusError::InvalidArgs,
            QStringLiteral("Invalid username format"));
        return false;
    }

    if (!checkAuthorization(ACTION_MANAGE_MOUNTS)) {
        sendErrorReply(QDBusError::AccessDenied,
            QStringLiteral("Not authorized to manage mounts"));
        return false;
    }

    if (gameId.isEmpty()) {
        sendErrorReply(QDBusError::InvalidArgs,
            QStringLiteral("gameId cannot be empty"));
        return false;
    }

    QString mountPoint = QStringLiteral("/run/couchplay/mounts/%1/%2").arg(username, gameId);

    if (!m_ops->isDirectory(mountPoint)) {
        qInfo() << "Overlay mount point does not exist:" << mountPoint;
        return true;
    }

    int result = m_ops->umount(mountPoint);
    if (result != 0) {
        qWarning() << "umount failed for" << mountPoint
                   << "- trying lazy unmount";
        result = m_ops->umount2(mountPoint, MNT_DETACH);
        if (result != 0) {
            qWarning() << "lazy umount also failed for" << mountPoint;
            sendErrorReply(QDBusError::Failed,
                QStringLiteral("Failed to unmount overlay at %1").arg(mountPoint));
            return false;
        }
    }

    qInfo() << "Overlay unmounted for user" << username << "gameId" << gameId;
    return true;
}

bool CouchPlayHelper::TeardownAllUserOverlays(const QString &username)
{
    static QRegularExpression validUsername(QStringLiteral("^[a-z][a-z0-9_-]{0,31}$"));
    if (!validUsername.match(username).hasMatch()) {
        sendErrorReply(QDBusError::InvalidArgs,
            QStringLiteral("Invalid username format"));
        return false;
    }

    if (!checkAuthorization(ACTION_MANAGE_MOUNTS)) {
        sendErrorReply(QDBusError::AccessDenied,
            QStringLiteral("Not authorized to manage mounts"));
        return false;
    }

    QString userMountBase = QStringLiteral("/run/couchplay/mounts/%1").arg(username);

    if (!m_ops->isDirectory(userMountBase)) {
        return true;
    }

    QStringList subdirs = m_ops->entryList(userMountBase, QStringList(), QDir::Dirs | QDir::NoDotAndDotDot);
    bool allSucceeded = true;

    for (const QString &subdir : subdirs) {
        QString mountPoint = userMountBase + QStringLiteral("/") + subdir;

        int result = m_ops->umount(mountPoint);
        if (result != 0) {
            qWarning() << "umount failed for" << mountPoint << "- trying lazy unmount";
            result = m_ops->umount2(mountPoint, MNT_DETACH);
            if (result != 0) {
                qWarning() << "lazy umount also failed for" << mountPoint;
                allSucceeded = false;
                continue;
            }
        }

        qInfo() << "Overlay unmounted for user" << username << "gameId" << subdir;
    }

    return allSucceeded;
}

bool CouchPlayHelper::WriteOverrideFile(const QString &username, const QString &gameId,
                                         const QString &relativePath, const QByteArray &content)
{
    static QRegularExpression validUsername(QStringLiteral("^[a-z][a-z0-9_-]{0,31}$"));
    if (!validUsername.match(username).hasMatch()) {
        sendErrorReply(QDBusError::InvalidArgs,
            QStringLiteral("Invalid username format"));
        return false;
    }

    if (!checkAuthorization(ACTION_MANAGE_MOUNTS)) {
        sendErrorReply(QDBusError::AccessDenied,
            QStringLiteral("Not authorized to manage mounts"));
        return false;
    }

    if (!userExists(username)) {
        sendErrorReply(QDBusError::InvalidArgs,
            QStringLiteral("User '%1' does not exist").arg(username));
        return false;
    }

    if (!IsInCouchPlayGroup(username)) {
        sendErrorReply(QDBusError::AccessDenied,
            QStringLiteral("User '%1' is not a CouchPlay user").arg(username));
        return false;
    }

    if (relativePath.contains(QStringLiteral("..")) || relativePath.startsWith(QLatin1Char('/'))) {
        sendErrorReply(QDBusError::InvalidArgs,
            QStringLiteral("Invalid relativePath: path traversal not allowed"));
        return false;
    }

    QString userHome = getUserHome(username);
    QString upperDir = userHome + QStringLiteral("/.couchplay/overlays/") + gameId + QStringLiteral("/upper");
    if (!m_ops->isDirectory(upperDir)) {
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("No overlay found for user '%1' and gameId '%2'").arg(username, gameId));
        return false;
    }

    uint userUid = getUserUid(username);
    struct passwd *pw = m_ops->getpwuid(userUid);
    if (!pw) {
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("Could not get user info for '%1'").arg(username));
        return false;
    }

    QString targetPath = upperDir + QStringLiteral("/") + relativePath;

    int lastSlash = targetPath.lastIndexOf(QLatin1Char('/'));
    if (lastSlash > 0) {
        QString targetDir = targetPath.left(lastSlash);
        if (!m_ops->mkpath(targetDir)) {
            sendErrorReply(QDBusError::Failed,
                QStringLiteral("Failed to create directory: %1").arg(targetDir));
            return false;
        }
        QString dirWalker = upperDir;
        for (const QString &part : targetDir.mid(upperDir.length()).split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
            dirWalker += QStringLiteral("/") + part;
            m_ops->chown(dirWalker, userUid, pw->pw_gid);
        }
    }

    if (!m_ops->writeFile(targetPath, content)) {
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("Failed to write file: %1").arg(targetPath));
        return false;
    }

    m_ops->chown(targetPath, userUid, pw->pw_gid);
    m_ops->chmod(targetPath, 0644);

    qDebug() << "WriteOverrideFile: Wrote" << targetPath << "for" << username;
    return true;
}

QString CouchPlayHelper::GetOverlayMountPoint(const QString &username, const QString &gameId)
{
    static QRegularExpression validUsername(QStringLiteral("^[a-z][a-z0-9_-]{0,31}$"));
    if (!validUsername.match(username).hasMatch()) {
        return QString();
    }

    QString mountPoint = QStringLiteral("/run/couchplay/mounts/%1/%2").arg(username, gameId);
    if (m_ops->isDirectory(mountPoint)) {
        return mountPoint;
    }

    return QString();
}
