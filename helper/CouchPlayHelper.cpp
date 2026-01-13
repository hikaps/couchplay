// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "CouchPlayHelper.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QDebug>

#include <unistd.h>
#include <sys/stat.h>
#include <signal.h>
#include <pwd.h>
#include <grp.h>

// Version of the helper daemon
static const QString HELPER_VERSION = QStringLiteral("0.1.0");

// PolicyKit actions
static const QString ACTION_DEVICE_OWNER = QStringLiteral("io.github.hikaps.couchplay.change-device-owner");
static const QString ACTION_CREATE_USER = QStringLiteral("io.github.hikaps.couchplay.create-user");
static const QString ACTION_ENABLE_LINGER = QStringLiteral("io.github.hikaps.couchplay.enable-linger");
static const QString ACTION_WAYLAND_ACCESS = QStringLiteral("io.github.hikaps.couchplay.setup-wayland-access");
static const QString ACTION_LAUNCH_INSTANCE = QStringLiteral("io.github.hikaps.couchplay.launch-instance");
static const QString ACTION_MANAGE_MOUNTS = QStringLiteral("io.github.hikaps.couchplay.manage-mounts");

CouchPlayHelper::CouchPlayHelper(QObject *parent)
    : QObject(parent)
{
}

CouchPlayHelper::~CouchPlayHelper()
{
    // Clean up: unmount all shared directories
    if (!m_activeMounts.isEmpty()) {
        for (const QString &username : m_activeMounts.keys()) {
            for (const MountInfo &mount : m_activeMounts[username]) {
                QProcess umountProcess;
                umountProcess.start(QStringLiteral("umount"), {mount.target});
                umountProcess.waitForFinished(5000);
                if (umountProcess.exitCode() != 0) {
                    // Try lazy unmount
                    QProcess::execute(QStringLiteral("umount"), {QStringLiteral("-l"), mount.target});
                }
            }
        }
        m_activeMounts.clear();
    }

    // Clean up: stop all launched processes
    for (auto it = m_launchedProcesses.begin(); it != m_launchedProcesses.end(); ++it) {
        QProcess *process = it.value();
        if (process && process->state() != QProcess::NotRunning) {
            process->terminate();
            process->waitForFinished(3000);
            if (process->state() != QProcess::NotRunning) {
                process->kill();
            }
        }
        delete process;
    }
    m_launchedProcesses.clear();

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
    struct passwd *pw = getpwuid(uid);
    if (!pw) {
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("User with UID %1 does not exist").arg(uid));
        return false;
    }

    // Change ownership
    if (chown(devicePath.toLocal8Bit().constData(), uid, pw->pw_gid) != 0) {
        sendErrorReply(QDBusError::Failed, 
            QStringLiteral("Failed to change ownership of %1: %2")
                .arg(devicePath, QString::fromLocal8Bit(strerror(errno))));
        return false;
    }

    // Set permissions to 0600 (owner read/write only) for input isolation
    // This ensures only the assigned user can read the device, not the group
    if (chmod(devicePath.toLocal8Bit().constData(), 0600) != 0) {
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
    struct group *inputGroup = getgrnam("input");
    gid_t inputGid = inputGroup ? inputGroup->gr_gid : 0;

    // Reset to root:input (or root:root if input group doesn't exist)
    if (chown(devicePath.toLocal8Bit().constData(), 0, inputGid) != 0) {
        sendErrorReply(QDBusError::Failed, 
            QStringLiteral("Failed to reset ownership of %1").arg(devicePath));
        return false;
    }

    // Restore permissions to 0660 (owner and group read/write)
    if (chmod(devicePath.toLocal8Bit().constData(), 0660) != 0) {
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
    struct group *inputGroup = getgrnam("input");
    gid_t inputGid = inputGroup ? inputGroup->gr_gid : 0;
    
    for (const QString &path : devices) {
        // Direct reset without auth check for cleanup scenarios
        // Reset to root:input with 0660 permissions
        if (chown(path.toLocal8Bit().constData(), 0, inputGid) == 0 &&
            chmod(path.toLocal8Bit().constData(), 0660) == 0) {
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

    // Create user with useradd
    QProcess process;
    QStringList args;
    args << QStringLiteral("-m")  // Create home directory
         << QStringLiteral("-c") << fullName
         << QStringLiteral("-s") << QStringLiteral("/bin/bash");
    
    // Add supplementary groups that exist on the system
    // On immutable distros like Bazzite, only 'input' group typically exists
    // We use a simple approach: try to add input group only, which is required for gamepad access
    args << QStringLiteral("-G") << QStringLiteral("input");
    
    args << username;

    process.start(QStringLiteral("useradd"), args);
    process.waitForFinished(30000);

    if (process.exitCode() != 0) {
        sendErrorReply(QDBusError::Failed, 
            QStringLiteral("Failed to create user: %1")
                .arg(QString::fromLocal8Bit(process.readAllStandardError())));
        return 0;
    }

    // Get the new user's UID
    uint uid = getUserUid(username);
    if (uid == 0) {
        sendErrorReply(QDBusError::Failed, 
            QStringLiteral("User created but could not retrieve UID"));
        return 0;
    }

    // Enable linger for the new user so their systemd user session starts at boot
    // This is required for machinectl shell to work properly
    QProcess lingerProcess;
    lingerProcess.start(QStringLiteral("loginctl"), 
        {QStringLiteral("enable-linger"), username});
    lingerProcess.waitForFinished(30000);

    if (lingerProcess.exitCode() != 0) {
        qWarning() << "Failed to enable linger for" << username 
                   << ":" << QString::fromLocal8Bit(lingerProcess.readAllStandardError());
        // Don't fail user creation, just warn - linger can be enabled later
    } else {
        qDebug() << "Enabled linger for new user" << username;
    }

    return uid;
}

bool CouchPlayHelper::userExists(const QString &username)
{
    struct passwd *pw = getpwnam(username.toLocal8Bit().constData());
    return pw != nullptr;
}

uint CouchPlayHelper::getUserUid(const QString &username)
{
    struct passwd *pw = getpwnam(username.toLocal8Bit().constData());
    return pw ? pw->pw_uid : 0;
}

QString CouchPlayHelper::getUserHome(const QString &username)
{
    struct passwd *pw = getpwnam(username.toLocal8Bit().constData());
    return pw ? QString::fromLocal8Bit(pw->pw_dir) : QString();
}

QString CouchPlayHelper::getUserHomeByUid(uint uid)
{
    struct passwd *pw = getpwuid(uid);
    return pw ? QString::fromLocal8Bit(pw->pw_dir) : QString();
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
    QProcess process;
    process.start(QStringLiteral("loginctl"), 
        {QStringLiteral("enable-linger"), username});
    process.waitForFinished(30000);

    if (process.exitCode() != 0) {
        sendErrorReply(QDBusError::Failed, 
            QStringLiteral("Failed to enable linger: %1")
                .arg(QString::fromLocal8Bit(process.readAllStandardError())));
        return false;
    }

    qDebug() << "Enabled linger for user" << username;
    return true;
}

bool CouchPlayHelper::IsLingerEnabled(const QString &username)
{
    // Check if linger file exists in /var/lib/systemd/linger/
    QString lingerFile = QStringLiteral("/var/lib/systemd/linger/%1").arg(username);
    return QFile::exists(lingerFile);
}

bool CouchPlayHelper::SetupWaylandAccess(const QString &username, uint compositorUid)
{
    // Validate username
    static QRegularExpression validUsername(QStringLiteral("^[a-z][a-z0-9_-]{0,31}$"));
    if (!validUsername.match(username).hasMatch()) {
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("Invalid username format"));
        return false;
    }

    if (!checkAuthorization(ACTION_WAYLAND_ACCESS)) {
        sendErrorReply(QDBusError::AccessDenied, 
            QStringLiteral("Not authorized to set up Wayland access"));
        return false;
    }

    // Check if secondary user exists
    if (!userExists(username)) {
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("User '%1' does not exist").arg(username));
        return false;
    }

    // Verify compositor user exists
    struct passwd *pw = getpwuid(compositorUid);
    if (!pw) {
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("Compositor user with UID %1 does not exist").arg(compositorUid));
        return false;
    }

    // Build paths
    QString runtimeDir = QStringLiteral("/run/user/%1").arg(compositorUid);
    QString waylandSocket = runtimeDir + QStringLiteral("/wayland-0");

    // Check if runtime dir exists
    if (!QFile::exists(runtimeDir)) {
        sendErrorReply(QDBusError::Failed, 
            QStringLiteral("Runtime directory %1 does not exist").arg(runtimeDir));
        return false;
    }

    // Check if Wayland socket exists
    if (!QFile::exists(waylandSocket)) {
        sendErrorReply(QDBusError::Failed, 
            QStringLiteral("Wayland socket %1 does not exist").arg(waylandSocket));
        return false;
    }

    // Grant execute (traverse) permission on XDG_RUNTIME_DIR
    QProcess setfaclDir;
    setfaclDir.start(QStringLiteral("setfacl"), 
        {QStringLiteral("-m"), QStringLiteral("u:%1:x").arg(username), runtimeDir});
    setfaclDir.waitForFinished(5000);

    if (setfaclDir.exitCode() != 0) {
        sendErrorReply(QDBusError::Failed, 
            QStringLiteral("Failed to set ACL on runtime dir: %1")
                .arg(QString::fromLocal8Bit(setfaclDir.readAllStandardError())));
        return false;
    }

    // Grant read/write permission on the Wayland socket
    QProcess setfaclSocket;
    setfaclSocket.start(QStringLiteral("setfacl"), 
        {QStringLiteral("-m"), QStringLiteral("u:%1:rw").arg(username), waylandSocket});
    setfaclSocket.waitForFinished(5000);

    if (setfaclSocket.exitCode() != 0) {
        // Clean up directory ACL on failure
        QProcess::execute(QStringLiteral("setfacl"), 
            {QStringLiteral("-x"), QStringLiteral("u:%1").arg(username), runtimeDir});
        sendErrorReply(QDBusError::Failed, 
            QStringLiteral("Failed to set ACL on Wayland socket: %1")
                .arg(QString::fromLocal8Bit(setfaclSocket.readAllStandardError())));
        return false;
    }

    // Grant read permission on any xauth files in the runtime directory
    // These are needed for X11/XWayland authentication
    QDir dir(runtimeDir);
    QStringList xauthFiles = dir.entryList({QStringLiteral("xauth_*")}, QDir::Files);
    for (const QString &xauthFile : xauthFiles) {
        QString xauthPath = runtimeDir + QStringLiteral("/") + xauthFile;
        QProcess setfaclXauth;
        setfaclXauth.start(QStringLiteral("setfacl"), 
            {QStringLiteral("-m"), QStringLiteral("u:%1:r").arg(username), xauthPath});
        setfaclXauth.waitForFinished(5000);
        if (setfaclXauth.exitCode() == 0) {
            qDebug() << "Set ACL on xauth file" << xauthPath << "for" << username;
        } else {
            qWarning() << "Failed to set ACL on xauth file" << xauthPath;
        }
    }

    // Also grant read on PipeWire socket for audio
    QString pipewireSocket = runtimeDir + QStringLiteral("/pipewire-0");
    if (QFile::exists(pipewireSocket)) {
        QProcess setfaclPipewire;
        setfaclPipewire.start(QStringLiteral("setfacl"), 
            {QStringLiteral("-m"), QStringLiteral("u:%1:rw").arg(username), pipewireSocket});
        setfaclPipewire.waitForFinished(5000);
        if (setfaclPipewire.exitCode() == 0) {
            qDebug() << "Set ACL on PipeWire socket for" << username;
        }
    }

    qDebug() << "Set up Wayland access for" << username << "to" << waylandSocket;
    return true;
}

bool CouchPlayHelper::RemoveWaylandAccess(const QString &username, uint compositorUid)
{
    // Validate username
    static QRegularExpression validUsername(QStringLiteral("^[a-z][a-z0-9_-]{0,31}$"));
    if (!validUsername.match(username).hasMatch()) {
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("Invalid username format"));
        return false;
    }

    if (!checkAuthorization(ACTION_WAYLAND_ACCESS)) {
        sendErrorReply(QDBusError::AccessDenied, 
            QStringLiteral("Not authorized to remove Wayland access"));
        return false;
    }

    // Build paths
    QString runtimeDir = QStringLiteral("/run/user/%1").arg(compositorUid);
    QString waylandSocket = runtimeDir + QStringLiteral("/wayland-0");

    bool success = true;

    // Remove ACL from Wayland socket (if it exists)
    if (QFile::exists(waylandSocket)) {
        QProcess removeFaclSocket;
        removeFaclSocket.start(QStringLiteral("setfacl"), 
            {QStringLiteral("-x"), QStringLiteral("u:%1").arg(username), waylandSocket});
        removeFaclSocket.waitForFinished(5000);
        if (removeFaclSocket.exitCode() != 0) {
            qWarning() << "Failed to remove ACL from Wayland socket:" 
                       << QString::fromLocal8Bit(removeFaclSocket.readAllStandardError());
            success = false;
        }
    }

    // Remove ACL from xauth files (if any exist)
    QDir dir(runtimeDir);
    QStringList xauthFiles = dir.entryList({QStringLiteral("xauth_*")}, QDir::Files);
    for (const QString &xauthFile : xauthFiles) {
        QString xauthPath = runtimeDir + QStringLiteral("/") + xauthFile;
        QProcess removeFaclXauth;
        removeFaclXauth.start(QStringLiteral("setfacl"), 
            {QStringLiteral("-x"), QStringLiteral("u:%1").arg(username), xauthPath});
        removeFaclXauth.waitForFinished(5000);
        // Don't fail overall if xauth cleanup fails - file may have been removed
    }

    // Remove ACL from PipeWire socket (if it exists)
    QString pipewireSocket = runtimeDir + QStringLiteral("/pipewire-0");
    if (QFile::exists(pipewireSocket)) {
        QProcess removeFaclPipewire;
        removeFaclPipewire.start(QStringLiteral("setfacl"), 
            {QStringLiteral("-x"), QStringLiteral("u:%1").arg(username), pipewireSocket});
        removeFaclPipewire.waitForFinished(5000);
        // Don't fail overall if pipewire cleanup fails
    }

    // Remove ACL from runtime directory (if it exists)
    if (QFile::exists(runtimeDir)) {
        QProcess removeFaclDir;
        removeFaclDir.start(QStringLiteral("setfacl"), 
            {QStringLiteral("-x"), QStringLiteral("u:%1").arg(username), runtimeDir});
        removeFaclDir.waitForFinished(5000);
        if (removeFaclDir.exitCode() != 0) {
            qWarning() << "Failed to remove ACL from runtime dir:" 
                       << QString::fromLocal8Bit(removeFaclDir.readAllStandardError());
            success = false;
        }
    }

    if (success) {
        qDebug() << "Removed Wayland access for" << username << "from" << waylandSocket;
    }
    return success;
}

QString CouchPlayHelper::Version()
{
    return HELPER_VERSION;
}

bool CouchPlayHelper::checkAuthorization(const QString &action)
{
    // In a full implementation, this would check PolicyKit
    // For now, we trust the D-Bus system bus ACL
    // TODO: Implement proper PolicyKit authorization check
    
    Q_UNUSED(action)
    
    // Check if caller is root or in appropriate group
    // The system D-Bus policy should restrict who can call us
    return true;
}

bool CouchPlayHelper::isValidDevicePath(const QString &path)
{
    // Must be under /dev/input/
    if (!path.startsWith(QStringLiteral("/dev/input/"))) {
        return false;
    }

    // Check for path traversal attempts
    if (path.contains(QStringLiteral(".."))) {
        return false;
    }

    // Must be a character device
    QFileInfo info(path);
    if (!info.exists()) {
        return false;
    }

    struct stat st;
    if (stat(path.toLocal8Bit().constData(), &st) != 0) {
        return false;
    }

    // Must be a character device (input devices are char devices)
    return S_ISCHR(st.st_mode);
}

qint64 CouchPlayHelper::LaunchInstance(const QString &username, uint compositorUid,
                                        const QStringList &gamescopeArgs,
                                        const QString &gameCommand,
                                        const QStringList &environment)
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
    struct passwd *pw = getpwuid(compositorUid);
    if (!pw) {
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("Compositor user with UID %1 does not exist").arg(compositorUid));
        return 0;
    }

    // Set up Wayland access for the user (if different from compositor user)
    // Call our own method - it will set up ACLs on the Wayland socket
    uint userUid = getUserUid(username);
    if (userUid != compositorUid) {
        if (!SetupWaylandAccess(username, compositorUid)) {
            // SetupWaylandAccess already sends error reply
            qWarning() << "Failed to set up Wayland access for" << username;
            // Continue anyway - might work if ACLs were already set
        }
    } else {
        qDebug() << "User" << username << "is compositor user, skipping Wayland ACL setup";
    }

    // Build the command to execute
    QString command = buildInstanceCommand(username, compositorUid, gamescopeArgs, 
                                            gameCommand, environment);
    
    qDebug() << "LaunchInstance: Spawning for user" << username;
    qDebug() << "LaunchInstance: Command:" << command.left(200) << "...";

    // Create and start the process
    QProcess *process = new QProcess(this);
    
    // Connect to finished signal to clean up
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, process](int exitCode, QProcess::ExitStatus exitStatus) {
        qint64 pid = process->processId();
        qDebug() << "LaunchInstance: Process" << pid << "finished with code" << exitCode
                 << (exitStatus == QProcess::CrashExit ? "(crashed)" : "");
        
        // Clean up from our tracking map
        m_launchedProcesses.remove(pid);
        process->deleteLater();
    });

    // Start the process
    process->start(QStringLiteral("/bin/bash"), {QStringLiteral("-c"), command});
    
    if (!process->waitForStarted(5000)) {
        sendErrorReply(QDBusError::Failed, 
            QStringLiteral("Failed to start process: %1").arg(process->errorString()));
        delete process;
        return 0;
    }

    qint64 pid = process->processId();
    m_launchedProcesses.insert(pid, process);
    
    qDebug() << "LaunchInstance: Started process with PID" << pid << "for user" << username;
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

    // Check if we have this process
    if (m_launchedProcesses.contains(pid)) {
        QProcess *process = m_launchedProcesses.value(pid);
        if (process && process->state() != QProcess::NotRunning) {
            process->terminate();
            qDebug() << "StopInstance: Sent SIGTERM to PID" << pid;
            return true;
        }
    }

    // Process not in our map - try to signal it directly
    // This allows stopping processes that might have been launched before a restart
    if (::kill(static_cast<pid_t>(pid), SIGTERM) == 0) {
        qDebug() << "StopInstance: Sent SIGTERM to external PID" << pid;
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

    // Check if we have this process
    if (m_launchedProcesses.contains(pid)) {
        QProcess *process = m_launchedProcesses.value(pid);
        if (process && process->state() != QProcess::NotRunning) {
            process->kill();
            qDebug() << "KillInstance: Sent SIGKILL to PID" << pid;
            return true;
        }
    }

    // Process not in our map - try to signal it directly
    if (::kill(static_cast<pid_t>(pid), SIGKILL) == 0) {
        qDebug() << "KillInstance: Sent SIGKILL to external PID" << pid;
        return true;
    }

    sendErrorReply(QDBusError::Failed, 
        QStringLiteral("Failed to kill process %1: %2")
            .arg(pid).arg(QString::fromLocal8Bit(strerror(errno))));
    return false;
}

QString CouchPlayHelper::buildInstanceCommand(const QString &username, uint compositorUid,
                                               const QStringList &gamescopeArgs,
                                               const QString &gameCommand,
                                               const QStringList &environment)
{
    // Build environment exports for the user
    // Key insight: Let the user use their OWN XDG_RUNTIME_DIR
    // (so gamescope can create lockfiles there), but point WAYLAND_DISPLAY
    // to the compositor user's Wayland socket as an absolute path.
    
    QStringList exports;
    
    // Compositor user's runtime directory for Wayland socket
    QString compositorRuntimeDir = QStringLiteral("/run/user/%1").arg(compositorUid);
    QString compositorWaylandSocket = compositorRuntimeDir + QStringLiteral("/wayland-0");
    
    // Set WAYLAND_DISPLAY to the absolute path of the compositor user's Wayland socket
    // The user has ACL access to this socket (set up by SetupWaylandAccess)
    exports << QStringLiteral("export WAYLAND_DISPLAY=%1").arg(compositorWaylandSocket);
    
    // For audio, point to the compositor user's PipeWire socket
    // PipeWire uses PIPEWIRE_RUNTIME_DIR if set, otherwise XDG_RUNTIME_DIR
    exports << QStringLiteral("export PIPEWIRE_RUNTIME_DIR=%1").arg(compositorRuntimeDir);
    
    // Add any additional environment variables from the caller
    for (const QString &var : environment) {
        exports << QStringLiteral("export %1").arg(var);
    }

    // Build the gamescope command with logging
    QString logFile = QStringLiteral("/tmp/couchplay-%1.log").arg(username);
    
    // Escape the game command for embedding in bash -c
    QString gameCommandForBash = gameCommand;
    gameCommandForBash.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    gameCommandForBash.replace(QLatin1Char('$'), QStringLiteral("\\$"));
    gameCommandForBash.replace(QLatin1Char('`'), QStringLiteral("\\`"));
    
    QString gamescopeCmd = QStringLiteral("/usr/bin/gamescope %1 -- /bin/bash -c \"%2\" 2>&1 | tee %3")
                               .arg(gamescopeArgs.join(QLatin1Char(' ')), gameCommandForBash, logFile);

    // Escape the entire gamescopeCmd for embedding in single quotes
    QString escapedGamescopeCmd = gamescopeCmd;
    escapedGamescopeCmd.replace(QLatin1Char('\''), QStringLiteral("'\\''"));

    // Join exports with semicolons
    QString exportStr = exports.join(QStringLiteral("; "));

    // Use machinectl shell to run in the user's systemd session
    // This requires linger to be enabled for the user (done by CreateUser)
    QString command = QStringLiteral("machinectl shell %1@ /bin/bash -c '%2; %3'")
                          .arg(username, exportStr, escapedGamescopeCmd);

    return command;
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
        if (!QFile::exists(source)) {
            qWarning() << "MountSharedDirectories: Source path does not exist:" << source;
            continue;
        }

        // Validate source is a directory
        QFileInfo sourceInfo(source);
        if (!sourceInfo.isDir()) {
            qWarning() << "MountSharedDirectories: Source is not a directory:" << source;
            continue;
        }

        // Compute target path
        QString target = computeMountTarget(source, alias, userHome, compositorHome);

        // Create target directory if it doesn't exist
        QDir targetDir(target);
        if (!targetDir.exists()) {
            if (!QDir().mkpath(target)) {
                qWarning() << "MountSharedDirectories: Failed to create target directory:" << target;
                continue;
            }
            // Set ownership of created directory to the target user
            uint userUid = getUserUid(username);
            struct passwd *pw = getpwuid(userUid);
            if (pw) {
                chown(target.toLocal8Bit().constData(), userUid, pw->pw_gid);
            }
        }

        // Perform bind mount
        QProcess mountProcess;
        mountProcess.start(QStringLiteral("mount"), 
            {QStringLiteral("--bind"), source, target});
        mountProcess.waitForFinished(10000);

        if (mountProcess.exitCode() != 0) {
            qWarning() << "MountSharedDirectories: Failed to mount" << source << "to" << target
                       << ":" << QString::fromLocal8Bit(mountProcess.readAllStandardError());
            continue;
        }

        // Track the mount for cleanup
        MountInfo info;
        info.source = source;
        info.target = target;
        m_activeMounts[username].append(info);

        qDebug() << "MountSharedDirectories: Mounted" << source << "to" << target << "for" << username;
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

        QProcess umountProcess;
        umountProcess.start(QStringLiteral("umount"), {mount.target});
        umountProcess.waitForFinished(10000);

        if (umountProcess.exitCode() == 0) {
            qDebug() << "UnmountSharedDirectories: Unmounted" << mount.target << "for" << username;
            successCount++;
        } else {
            // Try lazy unmount as fallback
            QProcess lazyUmount;
            lazyUmount.start(QStringLiteral("umount"), {QStringLiteral("-l"), mount.target});
            lazyUmount.waitForFinished(10000);

            if (lazyUmount.exitCode() == 0) {
                qDebug() << "UnmountSharedDirectories: Lazy unmounted" << mount.target << "for" << username;
                successCount++;
            } else {
                qWarning() << "UnmountSharedDirectories: Failed to unmount" << mount.target
                           << ":" << QString::fromLocal8Bit(umountProcess.readAllStandardError());
            }
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

            QProcess umountProcess;
            umountProcess.start(QStringLiteral("umount"), {mount.target});
            umountProcess.waitForFinished(10000);

            if (umountProcess.exitCode() == 0) {
                qDebug() << "UnmountAllSharedDirectories: Unmounted" << mount.target;
                totalCount++;
            } else {
                // Try lazy unmount as fallback
                QProcess lazyUmount;
                lazyUmount.start(QStringLiteral("umount"), {QStringLiteral("-l"), mount.target});
                lazyUmount.waitForFinished(10000);

                if (lazyUmount.exitCode() == 0) {
                    qDebug() << "UnmountAllSharedDirectories: Lazy unmounted" << mount.target;
                    totalCount++;
                } else {
                    qWarning() << "UnmountAllSharedDirectories: Failed to unmount" << mount.target;
                }
            }
        }
        
        m_activeMounts.remove(username);
    }

    return totalCount;
}

bool CouchPlayHelper::CopyFileToUser(const QString &sourcePath, const QString &targetPath,
                                      const QString &username)
{
    qDebug() << "CopyFileToUser:" << sourcePath << "->" << targetPath << "for" << username;
    
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
    if (!QFile::exists(sourcePath)) {
        qWarning() << "CopyFileToUser: Source file does not exist:" << sourcePath;
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("Source file does not exist: %1").arg(sourcePath));
        return false;
    }

    // Get user info for ownership
    uint userUid = getUserUid(username);
    struct passwd *pw = getpwuid(userUid);
    if (!pw) {
        qWarning() << "CopyFileToUser: Could not get user info for" << username;
        sendErrorReply(QDBusError::Failed, 
            QStringLiteral("Could not get user info for '%1'").arg(username));
        return false;
    }

    // Create parent directories if needed
    QFileInfo targetInfo(targetPath);
    QString targetDir = targetInfo.absolutePath();
    
    if (!QDir().mkpath(targetDir)) {
        qWarning() << "CopyFileToUser: Failed to create directory:" << targetDir;
        sendErrorReply(QDBusError::Failed, 
            QStringLiteral("Failed to create directory: %1").arg(targetDir));
        return false;
    }

    // Set ownership on created directories
    // Walk up from target directory, setting ownership on each new directory
    QString userHome = getUserHome(username);
    QStringList pathParts = targetDir.mid(userHome.length()).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString currentPath = userHome;
    for (const QString &part : pathParts) {
        currentPath += QStringLiteral("/") + part;
        // Only chown if it exists (mkpath created it)
        if (QDir(currentPath).exists()) {
            chown(currentPath.toLocal8Bit().constData(), userUid, pw->pw_gid);
        }
    }

    // Copy the file
    // Remove target first if it exists
    if (QFile::exists(targetPath)) {
        QFile::remove(targetPath);
    }

    if (!QFile::copy(sourcePath, targetPath)) {
        qWarning() << "CopyFileToUser: Failed to copy" << sourcePath << "to" << targetPath;
        sendErrorReply(QDBusError::Failed, 
            QStringLiteral("Failed to copy file from %1 to %2").arg(sourcePath, targetPath));
        return false;
    }

    // Set ownership on the copied file
    if (chown(targetPath.toLocal8Bit().constData(), userUid, pw->pw_gid) != 0) {
        qWarning() << "CopyFileToUser: Failed to set ownership on" << targetPath;
        // Don't fail - file was copied successfully
    }

    // Set permissions to 0644 (owner read/write, group/other read)
    if (chmod(targetPath.toLocal8Bit().constData(), 0644) != 0) {
        qWarning() << "CopyFileToUser: Failed to set permissions on" << targetPath;
    }

    qDebug() << "CopyFileToUser: Copied" << sourcePath << "to" << targetPath << "for" << username;
    return true;
}

bool CouchPlayHelper::WriteFileToUser(const QByteArray &content, const QString &targetPath,
                                       const QString &username)
{
    qDebug() << "WriteFileToUser:" << content.size() << "bytes to" << targetPath << "for" << username;
    
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
    struct passwd *pw = getpwuid(userUid);
    if (!pw) {
        qWarning() << "WriteFileToUser: Could not get user info for" << username;
        sendErrorReply(QDBusError::Failed, 
            QStringLiteral("Could not get user info for '%1'").arg(username));
        return false;
    }

    // Create parent directories if needed
    QFileInfo targetInfo(targetPath);
    QString targetDir = targetInfo.absolutePath();
    
    if (!QDir().mkpath(targetDir)) {
        qWarning() << "WriteFileToUser: Failed to create directory:" << targetDir;
        sendErrorReply(QDBusError::Failed, 
            QStringLiteral("Failed to create directory: %1").arg(targetDir));
        return false;
    }

    // Set ownership on created directories
    QString userHome = getUserHome(username);
    QStringList pathParts = targetDir.mid(userHome.length()).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString currentPath = userHome;
    for (const QString &part : pathParts) {
        currentPath += QStringLiteral("/") + part;
        if (QDir(currentPath).exists()) {
            chown(currentPath.toLocal8Bit().constData(), userUid, pw->pw_gid);
        }
    }

    // Write the file
    QFile file(targetPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "WriteFileToUser: Failed to open" << targetPath << "for writing:" << file.errorString();
        sendErrorReply(QDBusError::Failed, 
            QStringLiteral("Failed to open file for writing: %1").arg(file.errorString()));
        return false;
    }

    qint64 written = file.write(content);
    file.close();

    if (written != content.size()) {
        qWarning() << "WriteFileToUser: Only wrote" << written << "of" << content.size() << "bytes";
        sendErrorReply(QDBusError::Failed, 
            QStringLiteral("Failed to write all data to file"));
        return false;
    }

    // Set ownership on the file
    if (chown(targetPath.toLocal8Bit().constData(), userUid, pw->pw_gid) != 0) {
        qWarning() << "WriteFileToUser: Failed to set ownership on" << targetPath;
        // Don't fail - file was written successfully
    }

    // Set permissions to 0644
    if (chmod(targetPath.toLocal8Bit().constData(), 0644) != 0) {
        qWarning() << "WriteFileToUser: Failed to set permissions on" << targetPath;
    }

    qDebug() << "WriteFileToUser: Wrote" << content.size() << "bytes to" << targetPath << "for" << username;
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
    struct passwd *pw = getpwuid(userUid);
    if (!pw) {
        sendErrorReply(QDBusError::Failed, 
            QStringLiteral("Could not get user info for '%1'").arg(username));
        return false;
    }

    // Create directories
    if (!QDir().mkpath(path)) {
        sendErrorReply(QDBusError::Failed, 
            QStringLiteral("Failed to create directory: %1").arg(path));
        return false;
    }

    // Set ownership on the directory and all parent directories under user's home
    QString userHome = getUserHome(username);
    QStringList pathParts = path.mid(userHome.length()).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString currentPath = userHome;
    for (const QString &part : pathParts) {
        currentPath += QStringLiteral("/") + part;
        if (QDir(currentPath).exists()) {
            chown(currentPath.toLocal8Bit().constData(), userUid, pw->pw_gid);
        }
    }

    qDebug() << "CreateUserDirectory: Created" << path << "for" << username;
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
    if (!QFileInfo::exists(path)) {
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

    QProcess setfacl;
    setfacl.start(QStringLiteral("setfacl"), args);
    
    if (!setfacl.waitForFinished(60000)) {  // 60 second timeout for recursive operations
        sendErrorReply(QDBusError::Failed, 
            QStringLiteral("setfacl timed out for path: %1").arg(path));
        return false;
    }

    if (setfacl.exitCode() != 0) {
        QString errorOutput = QString::fromUtf8(setfacl.readAllStandardError());
        sendErrorReply(QDBusError::Failed, 
            QStringLiteral("setfacl failed for path %1: %2").arg(path, errorOutput));
        return false;
    }

    qDebug() << "SetDirectoryAcl:" << (recursive ? "Recursively set" : "Set") 
             << "ACL for user" << username << "on" << path;
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
    if (!QFileInfo::exists(path)) {
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

    qDebug() << "SetPathAclWithParents: Setting ACLs for user" << username 
             << "on" << pathsToSet.size() << "paths:" << pathsToSet;

    // Set ACL on each path (non-recursive, just rx for traversal)
    bool allSucceeded = true;
    for (const QString &p : pathsToSet) {
        if (!QFileInfo::exists(p)) {
            qWarning() << "SetPathAclWithParents: Path does not exist, skipping:" << p;
            continue;
        }

        QStringList args;
        args << QStringLiteral("-m");
        args << QStringLiteral("u:%1:rx").arg(username);
        args << p;

        QProcess setfacl;
        setfacl.start(QStringLiteral("setfacl"), args);
        
        if (!setfacl.waitForFinished(5000)) {
            qWarning() << "SetPathAclWithParents: setfacl timed out for:" << p;
            allSucceeded = false;
            continue;
        }

        if (setfacl.exitCode() != 0) {
            QString errorOutput = QString::fromUtf8(setfacl.readAllStandardError());
            qWarning() << "SetPathAclWithParents: setfacl failed for" << p << ":" << errorOutput;
            // Continue anyway - some paths might not support ACLs (e.g., NTFS)
            // but the mount point does
        }
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
        QDir userDataDir(userDataBase);
        if (!userDataDir.exists()) {
            continue;
        }

        // Find first numeric directory (Steam user ID)
        QStringList entries = userDataDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &entry : entries) {
            bool ok;
            entry.toULongLong(&ok);
            if (ok) {
                qDebug() << "GetUserSteamId: Found Steam ID" << entry << "for user" << username;
                return entry;
            }
        }
    }

    qDebug() << "GetUserSteamId: Steam userdata not found for" << username;
    return QString();
}
