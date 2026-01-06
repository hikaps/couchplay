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
#include <pwd.h>
#include <grp.h>

// Version of the helper daemon
static const QString HELPER_VERSION = QStringLiteral("0.1.0");

// PolicyKit actions
static const QString ACTION_DEVICE_OWNER = QStringLiteral("io.github.hikaps.couchplay.change-device-owner");
static const QString ACTION_CREATE_USER = QStringLiteral("io.github.hikaps.couchplay.create-user");
static const QString ACTION_ENABLE_LINGER = QStringLiteral("io.github.hikaps.couchplay.enable-linger");
static const QString ACTION_WAYLAND_ACCESS = QStringLiteral("io.github.hikaps.couchplay.setup-wayland-access");

CouchPlayHelper::CouchPlayHelper(QObject *parent)
    : QObject(parent)
{
}

CouchPlayHelper::~CouchPlayHelper()
{
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

    // Reset to root:root
    if (chown(devicePath.toLocal8Bit().constData(), 0, 0) != 0) {
        sendErrorReply(QDBusError::Failed, 
            QStringLiteral("Failed to reset ownership of %1").arg(devicePath));
        return false;
    }

    m_modifiedDevices.removeAll(devicePath);
    return true;
}

int CouchPlayHelper::ResetAllDevices()
{
    int successCount = 0;
    QStringList devices = m_modifiedDevices; // Copy since we modify during iteration
    
    for (const QString &path : devices) {
        // Direct reset without auth check for cleanup scenarios
        if (chown(path.toLocal8Bit().constData(), 0, 0) == 0) {
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
