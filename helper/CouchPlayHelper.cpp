// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "CouchPlayHelper.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QDebug>

#include <unistd.h>
#include <sys/stat.h>
#include <pwd.h>

// Version of the helper daemon
static const QString HELPER_VERSION = QStringLiteral("0.1.0");

// PolicyKit actions
static const QString ACTION_DEVICE_OWNER = QStringLiteral("io.github.hikaps.couchplay.change-device-owner");
static const QString ACTION_CREATE_USER = QStringLiteral("io.github.hikaps.couchplay.create-user");
static const QString ACTION_CONFIGURE_AUDIO = QStringLiteral("io.github.hikaps.couchplay.configure-audio");

CouchPlayHelper::CouchPlayHelper(QObject *parent)
    : QObject(parent)
{
    qDebug() << "CouchPlayHelper daemon started, version" << HELPER_VERSION;
}

CouchPlayHelper::~CouchPlayHelper()
{
    // Clean up: reset all modified devices on shutdown
    if (!m_modifiedDevices.isEmpty()) {
        qDebug() << "Cleaning up" << m_modifiedDevices.size() << "modified devices";
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

    qDebug() << "Changed ownership of" << devicePath << "to UID" << uid;
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
    qDebug() << "Reset ownership of" << devicePath << "to root";
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
    
    qDebug() << "Reset" << successCount << "devices to root ownership";
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
    if (UserExists(username)) {
        sendErrorReply(QDBusError::Failed, 
            QStringLiteral("User '%1' already exists").arg(username));
        return 0;
    }

    // Create user with useradd
    QProcess process;
    QStringList args;
    args << QStringLiteral("-m")  // Create home directory
         << QStringLiteral("-c") << fullName
         << QStringLiteral("-s") << QStringLiteral("/bin/bash")
         << QStringLiteral("-G") << QStringLiteral("audio,video,input,games")
         << username;

    process.start(QStringLiteral("useradd"), args);
    process.waitForFinished(30000);

    if (process.exitCode() != 0) {
        sendErrorReply(QDBusError::Failed, 
            QStringLiteral("Failed to create user: %1")
                .arg(QString::fromLocal8Bit(process.readAllStandardError())));
        return 0;
    }

    // Get the new user's UID
    uint uid = GetUserUid(username);
    if (uid == 0) {
        sendErrorReply(QDBusError::Failed, 
            QStringLiteral("User created but could not retrieve UID"));
        return 0;
    }

    qDebug() << "Created user" << username << "with UID" << uid;
    return uid;
}

bool CouchPlayHelper::UserExists(const QString &username)
{
    struct passwd *pw = getpwnam(username.toLocal8Bit().constData());
    return pw != nullptr;
}

uint CouchPlayHelper::GetUserUid(const QString &username)
{
    struct passwd *pw = getpwnam(username.toLocal8Bit().constData());
    return pw ? pw->pw_uid : 0;
}

bool CouchPlayHelper::ConfigurePipeWireForUser(uint uid)
{
    if (!checkAuthorization(ACTION_CONFIGURE_AUDIO)) {
        sendErrorReply(QDBusError::AccessDenied, 
            QStringLiteral("Not authorized to configure audio"));
        return false;
    }

    struct passwd *pw = getpwuid(uid);
    if (!pw) {
        sendErrorReply(QDBusError::InvalidArgs, 
            QStringLiteral("User with UID %1 does not exist").arg(uid));
        return false;
    }

    QString homeDir = QString::fromLocal8Bit(pw->pw_dir);
    QString pipewireConfigDir = homeDir + QStringLiteral("/.config/pipewire/pipewire.conf.d");

    // Create config directory
    QProcess::execute(QStringLiteral("mkdir"), {QStringLiteral("-p"), pipewireConfigDir});
    
    // Create a basic PipeWire config for multi-user audio
    // This is a placeholder - actual config would be more sophisticated
    QString configPath = pipewireConfigDir + QStringLiteral("/10-couchplay.conf");
    QFile configFile(configPath);
    
    if (configFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        // Basic config allowing audio playback
        configFile.write("# CouchPlay PipeWire configuration\n");
        configFile.write("# Allows audio output for split-screen gaming\n");
        configFile.write("context.properties = {\n");
        configFile.write("    default.clock.allowed-rates = [ 44100 48000 ]\n");
        configFile.write("}\n");
        configFile.close();
        
        // Fix ownership
        chown(configPath.toLocal8Bit().constData(), uid, pw->pw_gid);
        
        qDebug() << "Configured PipeWire for user" << pw->pw_name;
        return true;
    }

    sendErrorReply(QDBusError::Failed, 
        QStringLiteral("Failed to write PipeWire configuration"));
    return false;
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
