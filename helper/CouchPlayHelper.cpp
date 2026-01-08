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

CouchPlayHelper::CouchPlayHelper(QObject *parent)
    : QObject(parent)
{
}

CouchPlayHelper::~CouchPlayHelper()
{
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

bool CouchPlayHelper::SetupWaylandAccess(const QString &username, uint primaryUid)
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

    // Verify primary user exists
    struct passwd *pw = getpwuid(primaryUid);
    if (!pw) {
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("Primary user with UID %1 does not exist").arg(primaryUid));
        return false;
    }

    // Build paths
    QString runtimeDir = QStringLiteral("/run/user/%1").arg(primaryUid);
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

bool CouchPlayHelper::RemoveWaylandAccess(const QString &username, uint primaryUid)
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
    QString runtimeDir = QStringLiteral("/run/user/%1").arg(primaryUid);
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

qint64 CouchPlayHelper::LaunchInstance(const QString &username, uint primaryUid,
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

    // Verify primary user exists
    struct passwd *pw = getpwuid(primaryUid);
    if (!pw) {
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("Primary user with UID %1 does not exist").arg(primaryUid));
        return 0;
    }

    // Set up Wayland access for the secondary user first
    // Call our own method - it will set up ACLs on the Wayland socket
    if (!SetupWaylandAccess(username, primaryUid)) {
        // SetupWaylandAccess already sends error reply
        qWarning() << "Failed to set up Wayland access for" << username;
        // Continue anyway - might work if ACLs were already set
    }

    // Build the command to execute
    QString command = buildInstanceCommand(username, primaryUid, gamescopeArgs, 
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

QString CouchPlayHelper::buildInstanceCommand(const QString &username, uint primaryUid,
                                               const QStringList &gamescopeArgs,
                                               const QString &gameCommand,
                                               const QStringList &environment)
{
    // Build environment exports for the secondary user
    // Key insight: Let the secondary user use their OWN XDG_RUNTIME_DIR
    // (so gamescope can create lockfiles there), but point WAYLAND_DISPLAY
    // to the primary user's Wayland socket as an absolute path.
    
    QStringList exports;
    
    // Primary user's runtime directory for Wayland socket
    QString primaryRuntimeDir = QStringLiteral("/run/user/%1").arg(primaryUid);
    QString primaryWaylandSocket = primaryRuntimeDir + QStringLiteral("/wayland-0");
    
    // Set WAYLAND_DISPLAY to the absolute path of the primary user's Wayland socket
    // The secondary user has ACL access to this socket (set up by SetupWaylandAccess)
    exports << QStringLiteral("export WAYLAND_DISPLAY=%1").arg(primaryWaylandSocket);
    
    // For audio, point to the primary user's PipeWire socket
    // PipeWire uses PIPEWIRE_RUNTIME_DIR if set, otherwise XDG_RUNTIME_DIR
    exports << QStringLiteral("export PIPEWIRE_RUNTIME_DIR=%1").arg(primaryRuntimeDir);
    
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
