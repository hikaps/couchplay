// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "CouchPlayHelper.h"
#include "MountSpec.h"
#include "PolkitActions.h"
#include "SecureFs.h"
#include "SystemOps.h"

#include <QCryptographicHash>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QThread>

#include <linux/fs.h>
#include <linux/input.h>
#include <fcntl.h>
#include <QSocketNotifier>
#include <QTimer>
#include <QFileSystemWatcher>

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <functional>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits>
#ifndef O_PATH
#define O_PATH 010000000
#endif

class UnitMonitor : public QObject
{
    Q_OBJECT
public:
    UnitMonitor(const QString &unitPath,
                const QString &serviceName,
                const QString &username,
                qint64 pid,
                CouchPlayHelper *helper)
        : QObject(helper)
        , m_unitPath(unitPath)
        , m_serviceName(serviceName)
        , m_username(username)
        , m_pid(pid)
        , m_helper(helper)
    {
        QDBusConnection::systemBus().connect(QStringLiteral("org.freedesktop.systemd1"),
                                             m_unitPath,
                                             QStringLiteral("org.freedesktop.DBus.Properties"),
                                             QStringLiteral("PropertiesChanged"),
                                             this,
                                             SLOT(onPropertiesChanged(QString, QVariantMap, QStringList)));
    }

    ~UnitMonitor() override
    {
        QDBusConnection::systemBus().disconnect(QStringLiteral("org.freedesktop.systemd1"),
                                                m_unitPath,
                                                QStringLiteral("org.freedesktop.DBus.Properties"),
                                                QStringLiteral("PropertiesChanged"),
                                                this,
                                                SLOT(onPropertiesChanged(QString, QVariantMap, QStringList)));
    }

    QString serviceName() const
    {
        return m_serviceName;
    }

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
        if (activeState != QStringLiteral("inactive") && activeState != QStringLiteral("failed")
            && activeState != QStringLiteral("dead")) {
            return;
        }

        if (m_helper->m_stoppingUnits.contains(m_serviceName)) {
            return;
        }

        QString result = queryResultProperty();
        QString reason = mapResultToReason(result);

        m_helper->m_usernameToUnitName.remove(m_username);
        m_helper->m_pidToUsername.remove(m_pid);
        m_helper->m_pidOwnerUid.remove(m_pid);
        m_helper->cleanupTcpListenerIfLast(m_username);

        m_helper->saveState();

        qInfo() << "Unit" << m_serviceName << "(user:" << m_username << ", PID:" << m_pid
                << ") stopped unexpectedly:" << reason;

        Q_EMIT m_helper->instanceStopped(m_username, m_pid, reason);

        deleteLater();
    }

private:
    QString queryResultProperty() const
    {
        QDBusMessage msg = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.systemd1"),
                                                          m_unitPath,
                                                          QStringLiteral("org.freedesktop.DBus.Properties"),
                                                          QStringLiteral("Get"));
        msg << QStringLiteral("org.freedesktop.systemd1.Service") << QStringLiteral("Result");

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

static const QRegularExpression s_validUsername(QStringLiteral("^[a-z][a-z0-9_-]{0,31}$"));

static const QString ACTION_DEVICE_OWNER = QStringLiteral("io.github.hikaps.couchplay.change-device-owner");
static const QString ACTION_CREATE_USER = QStringLiteral("io.github.hikaps.couchplay.create-user");
static const QString ACTION_DELETE_USER = QStringLiteral("io.github.hikaps.couchplay.delete-user");
static const QString ACTION_ENABLE_LINGER = QStringLiteral("io.github.hikaps.couchplay.enable-linger");
static const QString ACTION_WAYLAND_ACCESS = QStringLiteral("io.github.hikaps.couchplay.setup-wayland-access");
static const QString ACTION_LAUNCH_INSTANCE = QStringLiteral("io.github.hikaps.couchplay.launch-instance");
static const QString ACTION_MANAGE_MOUNTS = QStringLiteral("io.github.hikaps.couchplay.manage-mounts");
static const QString ACTION_MANAGE_VIRTUAL_DISPLAY = QStringLiteral("io.github.hikaps.couchplay.manage-virtual-display");
static const QString ACTION_MANAGE_AUDIO_SINK = QStringLiteral("io.github.hikaps.couchplay.manage-audio-sink");

static const QString COUCHPLAY_GROUP = QStringLiteral("couchplay");
static constexpr uint INVALID_CALLER_UID = std::numeric_limits<uint>::max();

CouchPlayHelper::CouchPlayHelper(SystemOps *ops, QObject *parent)
    : QObject(parent)
    , m_ops(ops ? ops : new RealSystemOps(this))
    , m_stateFilePath(QStringLiteral("/run/couchplay/state.json"))
{
    loadAndReconcileState();
    setupUinputAccess();

    // Populate initial known event numbers
    QDir dir(QStringLiteral("/dev/input"));
    QStringList eventFiles = dir.entryList({QStringLiteral("event*")}, QDir::Files | QDir::System);
    static const QRegularExpression eventRegex(QStringLiteral("event(\\d+)"));
    for (const QString &eventFile : eventFiles) {
        QRegularExpressionMatch match = eventRegex.match(eventFile);
        if (match.hasMatch()) {
            m_knownEventNumbers.insert(match.captured(1).toInt());
        }
    }

    // Set up directory watcher on /dev/input
    m_inputWatcher = new QFileSystemWatcher(this);
    m_inputWatcher->addPath(QStringLiteral("/dev/input"));
    connect(m_inputWatcher, &QFileSystemWatcher::directoryChanged, this, &CouchPlayHelper::onInputDirectoryChanged);

    m_inputDebounceTimer = new QTimer(this);
    m_inputDebounceTimer->setInterval(500);
    m_inputDebounceTimer->setSingleShot(true);
    connect(m_inputDebounceTimer, &QTimer::timeout, this, &CouchPlayHelper::onInputDebounceTimeout);

    // Watch any virtual controllers already present at startup
    for (const QString &eventFile : eventFiles) {
        QRegularExpressionMatch match = eventRegex.match(eventFile);
        if (match.hasMatch()) {
            int eventNumber = match.captured(1).toInt();
            if (isVirtualDevice(eventNumber)) {
                QString devicePath = QStringLiteral("/dev/input/%1").arg(eventFile);
                qDebug() << "CouchPlayHelper: Pre-existing virtual device found at startup, watching:" << devicePath;
                startWatchingDevice(devicePath);
            }
        }
    }
}

CouchPlayHelper::~CouchPlayHelper()
{
    for (uint uid : m_runtimeAccessSetForUid) {
        QString runtimeDir = QStringLiteral("/run/user/%1").arg(uid);
        removeRuntimeAcls(runtimeDir);
        removePulseTcpListener(uid);
        qDebug() << "Cleaned up runtime access for compositor UID" << uid;
    }
    m_runtimeAccessSetForUid.clear();

    if (!m_activeMounts.isEmpty()) {
        for (const QString &username : m_activeMounts.keys()) {
            for (MountInfo &mount : m_activeMounts[username]) {
                // Prefer the pinned FD: same race protections as explicit
                // unmount, so shutdown cleanup cannot be redirected by an
                // ancestor swap. Never re-resolve a path after a pinned-FD
                unmount fails; the path may have been swapped meanwhile.
                // Restored (post-restart) state has no FD and is handled by
                // the safely re-opened-parent cleanup below.
                if (mount.targetParentFd >= 0) {
                    int result = SecureFs::umountAtFd(mount.targetParentFd, mount.targetLeaf);
                    ::close(mount.targetParentFd);
                    mount.targetParentFd = -1;
                    if (result != 0) {
                        qWarning() << "CouchPlayHelper: FD umount failed during shutdown for" << mount.target
                                   << ":" << strerror(-result);
                    }
                    continue;
                }
                if (!unmountMountInfo(mount)) {
                    qWarning() << "CouchPlayHelper: restored mount cleanup failed during shutdown for"
                               << mount.target;
                }
            }
        }
        m_activeMounts.clear();
    }

    for (const VirtualDisplayInfo &info : m_virtualDisplays) {
        if (info.pid > 0) {
            m_ops->killProcess(static_cast<pid_t>(info.pid), SIGTERM);
        }
        if (!info.serviceName.isEmpty()) {
            stopServiceInstance(info.serviceName);
        }
    }
    m_virtualDisplays.clear();

    for (const QString &username : m_nullSinks.keys()) {
        for (const NullSinkInfo &sink : m_nullSinks[username]) {
            unloadNullSinkModule(username, sink.sinkName);
        }
    }
    m_nullSinks.clear();

    for (const QString &serviceName : m_usernameToUnitName) {
        m_stoppingUnits.insert(serviceName);
        stopServiceInstance(serviceName);
    }
    m_usernameToUnitName.clear();
    m_pidToUsername.clear();
    m_pidOwnerUid.clear();

    qDeleteAll(m_monitors);
    m_monitors.clear();

    stopWatchingAllDevices();

    if (!m_modifiedDevices.isEmpty() || !m_modifiedHidDevices.isEmpty()) {
        resetAllDevicesInternal();
    }
    removeUinputAccess();
}

bool CouchPlayHelper::ChangeDeviceOwner(const QString &devicePath, uint uid)
{
    if (!isValidDevicePath(devicePath)) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Invalid device path: %1").arg(devicePath));
        return false;
    }

    if (!checkAuthorization(ACTION_DEVICE_OWNER)) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Not authorized to change device ownership"));
        return false;
    }

    struct passwd *pw = m_ops->getpwuid(uid);
    if (!pw) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("User with UID %1 does not exist").arg(uid));
        return false;
    }

    // Try to resolve the device to a physical HID device
    HidDeviceInfo hid = findHidDevice(devicePath);
    bool isPhysical = !hid.deviceId.isEmpty();

    if (isPhysical) {
        QString devId = hid.deviceId;
        QString trackingPrefix = QStringLiteral("hid|%1|%2|").arg(hid.deviceId, hid.driverPath);
        QString trackingId = trackingPrefix + QString::fromLocal8Bit(pw->pw_name);

        // Check if this device is already modified for the same user
        bool alreadyAssigned = false;
        for (const QString &existing : m_modifiedHidDevices) {
            if (existing.startsWith(trackingPrefix)) {
                if (existing == trackingId) {
                    alreadyAssigned = true;
                } else {
                    // Different user: remove old tracking ID first
                    m_modifiedHidDevices.removeAll(existing);
                }
                break;
            }
        }

        if (alreadyAssigned) {
            QString rulePath = getTempUdevRulePath(devId);
            if (QFileInfo::exists(rulePath)) {
                if (!m_modifiedDevices.contains(devicePath)) {
                    m_modifiedDevices.append(devicePath);
                }
                // Clear ACLs on this node to ensure it remains clean
                runCommand(QStringLiteral("setfacl"), {QStringLiteral("-b"), devicePath});
                startWatchingDevice(devicePath);
                saveState();
                return true;
            } else {
                // Rule was cleared (e.g. helper restarted), we need to rewrite it and rebind
                alreadyAssigned = false;
            }
        }

        // 1. Write the temporary udev rule assigning this device/subsystem to the target user
        if (writeTempUdevRule(hid, QString::fromLocal8Bit(pw->pw_name))) {
            // 2. Reload udev rules so the rule takes effect on rebinding
            runCommand(QStringLiteral("udevadm"), {QStringLiteral("control"), QStringLiteral("--reload-rules")});

            // 3. Unbind the driver to force active openers (like Steam) to close their FDs
            QString driverPath = hid.driverPath;
            QString unbindPath = driverPath + QStringLiteral("/unbind");
            qDebug() << "CouchPlayHelper: Unbinding device" << devId << "from driver" << driverPath;
            if (!m_ops->writeFile(unbindPath, devId.toLocal8Bit())) {
                qWarning() << "CouchPlayHelper: Failed to write to driver unbind:" << unbindPath;
            }

            // 4. Rebind the driver so the device is recreated with our udev rules applied
            QString bindPath = driverPath + QStringLiteral("/bind");
            qDebug() << "CouchPlayHelper: Rebinding device" << devId << "to driver" << driverPath;
            if (!m_ops->writeFile(bindPath, devId.toLocal8Bit())) {
                qWarning() << "CouchPlayHelper: Failed to write to driver bind:" << bindPath;
            }

            // Wait for udev to finish processing the device creation and logind to apply ACLs
            runCommand(QStringLiteral("udevadm"), {QStringLiteral("settle")});

            // After rebind, the hidraw node may have received a new /dev/hidrawN number.
            // Re-resolve the actual current path from sysfs using the stable device ID.
            QString resolvedDevicePath = findHidrawPathForDeviceId(devId);
            if (resolvedDevicePath.isEmpty()) {
                qWarning() << "CouchPlayHelper: Could not resolve post-rebind hidraw path for" << devId
                           << "-- falling back to original path" << devicePath;
                resolvedDevicePath = devicePath;
            } else if (resolvedDevicePath != devicePath) {
                qDebug() << "CouchPlayHelper: Post-rebind hidraw path changed from" << devicePath
                         << "to" << resolvedDevicePath;
                // Update tracking so cleanup removes the right node
                m_modifiedDevices.removeAll(devicePath);
            }

            // Clear any systemd-logind ACLs (which grant read/write access to the host seat user 'deck')
            runCommand(QStringLiteral("setfacl"), {QStringLiteral("-b"), resolvedDevicePath});

            // Track the modified device for cleanup on exit
            if (!m_modifiedHidDevices.contains(trackingId)) {
                m_modifiedHidDevices.append(trackingId);
            }

            // Watch the resolved (current) hidraw path for the exit chord
            if (!m_modifiedDevices.contains(resolvedDevicePath)) {
                m_modifiedDevices.append(resolvedDevicePath);
            }
            startWatchingDevice(resolvedDevicePath);
            saveState();
            return true;
        }
    } else {
        // Fallback for virtual/software devices: change owner directly on the existing device node
        if (m_ops->chown(devicePath, uid, pw->pw_gid) != 0) {
            sendErrorReply(QDBusError::Failed,
                           QStringLiteral("Failed to change ownership of %1: %2")
                               .arg(devicePath, QString::fromLocal8Bit(strerror(errno))));
            return false;
        }

        // Only the assigned user can read the device, not the group
        if (m_ops->chmod(devicePath, 0600) != 0) {
            sendErrorReply(QDBusError::Failed,
                           QStringLiteral("Failed to set permissions on %1: %2")
                               .arg(devicePath, QString::fromLocal8Bit(strerror(errno))));
            return false;
        }

        // Clear any systemd-logind ACLs on the virtual device node
        runCommand(QStringLiteral("setfacl"), {QStringLiteral("-b"), devicePath});
    }

    if (!m_modifiedDevices.contains(devicePath)) {
        m_modifiedDevices.append(devicePath);
    }

    startWatchingDevice(devicePath);
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
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Invalid device path: %1").arg(devicePath));
        return false;
    }

    if (!checkAuthorization(ACTION_DEVICE_OWNER)) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Not authorized to change device ownership"));
        return false;
    }

    // Clean up any associated temporary udev rule for this physical device
    HidDeviceInfo hid = findHidDevice(devicePath);
    if (!hid.deviceId.isEmpty()) {
        QString devId = hid.deviceId;
        QString trackingPrefix = QStringLiteral("hid|%1|").arg(hid.deviceId);

        QString matchedTracking;
        for (const QString &existing : m_modifiedHidDevices) {
            if (existing.startsWith(trackingPrefix)) {
                matchedTracking = existing;
                break;
            }
        }

        if (!matchedTracking.isEmpty()) {
            m_modifiedHidDevices.removeAll(matchedTracking);
            removeTempUdevRule(devId);

            QStringList parts = matchedTracking.split(QLatin1Char('|'));
            if (parts.size() >= 3) {
                QString driverPath = parts.at(2);
                QString unbindPath = driverPath + QStringLiteral("/unbind");
                m_ops->writeFile(unbindPath, devId.toLocal8Bit());

                QString bindPath = driverPath + QStringLiteral("/bind");
                m_ops->writeFile(bindPath, devId.toLocal8Bit());
            }

            runCommand(QStringLiteral("udevadm"), {QStringLiteral("control"), QStringLiteral("--reload-rules")});
            runCommand(QStringLiteral("udevadm"),
                       {QStringLiteral("trigger"), QStringLiteral("--subsystem-match=input"),
                        QStringLiteral("--action=change")});
            runCommand(QStringLiteral("udevadm"),
                       {QStringLiteral("trigger"), QStringLiteral("--subsystem-match=hidraw"),
                        QStringLiteral("--action=change")});
        }
    }

    struct group *inputGroup = m_ops->getgrnam("input");
    gid_t inputGid = inputGroup ? inputGroup->gr_gid : 0;

    // Reset to root:input (or root:root if input group doesn't exist)
    if (m_ops->chown(devicePath, 0, inputGid) != 0) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Failed to reset ownership of %1").arg(devicePath));
        return false;
    }

    if (m_ops->chmod(devicePath, 0660) != 0) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Failed to reset permissions on %1").arg(devicePath));
        return false;
    }

    stopWatchingDevice(devicePath);
    m_modifiedDevices.removeAll(devicePath);
    saveState();
    return true;
}

bool CouchPlayHelper::WatchDevice(const QString &devicePath)
{
    if (!isValidDevicePath(devicePath)) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Invalid device path: %1").arg(devicePath));
        return false;
    }

    if (!checkAuthorization(ACTION_DEVICE_OWNER)) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Not authorized to watch device"));
        return false;
    }

    startWatchingDevice(devicePath);
    return true;
}



int CouchPlayHelper::ResetAllDevices()
{
    if (!checkAuthorization(ACTION_DEVICE_OWNER)) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Not authorized to reset devices"));
        return 0;
    }
    const int resetCount = resetAllDevicesInternal();
    if (!m_modifiedDevices.isEmpty() || !m_modifiedHidDevices.isEmpty()) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not reset all device ownership"));
        return 0;
    }
    return resetCount;
}

int CouchPlayHelper::resetAllDevicesInternal()
{
    stopWatchingAllDevices();

    int successCount = 0;
    QStringList devices = m_modifiedDevices;

    struct group *inputGroup = m_ops->getgrnam("input");
    gid_t inputGid = inputGroup ? inputGroup->gr_gid : 0;

    // Clean up temporary udev rules for physical HID devices and rebind to restore default ownership.
    // Keep entries whose cleanup is incomplete so a later ResetAllDevices call can retry them.
    QStringList hidDevices = m_modifiedHidDevices;
    QStringList remainingHidDevices;

    bool needUdevReload = false;

    for (const QString &trackingId : hidDevices) {
        QStringList parts = trackingId.split(QLatin1Char('|'));
        if (parts.size() < 3) {
            remainingHidDevices.append(trackingId);
            continue;
        }

        QString devId = parts.at(1);
        QString driverPath = parts.at(2);

        const bool ruleRemoved = removeTempUdevRule(devId);
        if (ruleRemoved) {
            needUdevReload = true;
        }

        QString unbindPath = driverPath + QStringLiteral("/unbind");
        qDebug() << "CouchPlayHelper: Reset - Unbinding device" << devId << "from driver" << driverPath;
        const bool unbound = m_ops->writeFile(unbindPath, devId.toLocal8Bit());

        QString bindPath = driverPath + QStringLiteral("/bind");
        qDebug() << "CouchPlayHelper: Reset - Rebinding device" << devId << "to driver" << driverPath;
        const bool rebound = m_ops->writeFile(bindPath, devId.toLocal8Bit());
        if (!ruleRemoved || !unbound || !rebound) {
            remainingHidDevices.append(trackingId);
        }
    }
    m_modifiedHidDevices = remainingHidDevices;

    bool udevCommandsSucceeded = true;
    if (needUdevReload) {
        // Reload udev rules after removing all temporary rules.
        udevCommandsSucceeded = runCommand(QStringLiteral("udevadm"),
                                            {QStringLiteral("control"), QStringLiteral("--reload-rules")})
            && udevCommandsSucceeded;

        // Trigger udev to re-apply default rules to input and hidraw subsystems.
        const bool inputTriggerSucceeded = runCommand(
            QStringLiteral("udevadm"),
            {QStringLiteral("trigger"), QStringLiteral("--subsystem-match=input"), QStringLiteral("--action=change")});
        udevCommandsSucceeded = inputTriggerSucceeded && udevCommandsSucceeded;
        const bool hidrawTriggerSucceeded = runCommand(
            QStringLiteral("udevadm"),
            {QStringLiteral("trigger"), QStringLiteral("--subsystem-match=hidraw"), QStringLiteral("--action=change")});
        udevCommandsSucceeded = hidrawTriggerSucceeded && udevCommandsSucceeded;
    }
    if (!udevCommandsSucceeded) {
        for (const QString &trackingId : hidDevices) {
            if (!m_modifiedHidDevices.contains(trackingId)) {
                m_modifiedHidDevices.append(trackingId);
            }
        }
    }

    // Reset virtual/software devices via direct chown/chmod
    for (const QString &path : devices) {
        if (m_ops->chown(path, 0, inputGid) == 0 && m_ops->chmod(path, 0660) == 0) {
            successCount++;
            m_modifiedDevices.removeAll(path);
        }
    }

    saveState();

    return successCount;
}

uint CouchPlayHelper::CreateUser(const QString &username, const QString &fullName)
{
    if (!s_validUsername.match(username).hasMatch()) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Invalid username format"));
        return 0;
    }

    if (!checkAuthorization(ACTION_CREATE_USER)) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Not authorized to create users"));
        return 0;
    }

    if (userExists(username)) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("User '%1' already exists").arg(username));
        return 0;
    }

    // -f flag means no error if group exists, so we don't check exit code
    runCommand(QStringLiteral("groupadd"), {QStringLiteral("-f"), COUCHPLAY_GROUP});

    QProcess *process = m_ops->createProcess();
    QStringList args;
    args << QStringLiteral("-m") << QStringLiteral("-c") << fullName << QStringLiteral("-s")
         << QStringLiteral("/bin/bash");

    // "input" group is optional (absent on Bazzite); omit if missing to avoid useradd failure (issue #23)
    QString supplementaryGroups = COUCHPLAY_GROUP;
    if (m_ops->getgrnam("input")) {
        supplementaryGroups = QStringLiteral("input,") + COUCHPLAY_GROUP;
    }
    args << QStringLiteral("-G") << supplementaryGroups;

    args << username;

    m_ops->startProcess(process, QStringLiteral("useradd"), args);
    m_ops->waitForFinished(process, 30000);

    if (m_ops->processExitCode(process) != 0) {
        sendErrorReply(
            QDBusError::Failed,
            QStringLiteral("Failed to create user: %1").arg(QString::fromLocal8Bit(m_ops->readStandardError(process))));
        delete process;
        return 0;
    }
    delete process;

    uint uid = getUserUid(username);
    if (uid == 0) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("User created but could not retrieve UID"));
        return 0;
    }

    // Enable linger so systemd user session starts at boot (required for systemd-run transient units)
    QProcess *lingerProcess = m_ops->createProcess();
    m_ops->startProcess(lingerProcess, QStringLiteral("loginctl"), {QStringLiteral("enable-linger"), username});
    m_ops->waitForFinished(lingerProcess, 30000);

    if (m_ops->processExitCode(lingerProcess) != 0) {
        qWarning() << "Failed to enable linger for" << username << ":"
                   << QString::fromLocal8Bit(m_ops->readStandardError(lingerProcess));
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

bool CouchPlayHelper::validateUserPath(const QString &path,
                                       const QString &username,
                                       const QString &callerName,
                                       QStringList &dirsToChown)
{
    dirsToChown.clear();

    QString userHome = getUserHome(username);
    if (!path.startsWith(userHome + QLatin1Char('/'))) {
        qWarning() << callerName << ": Path" << path << "is not under user's home" << userHome;
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Path is not under user's home directory"));
        return false;
    }

    if (pathHasSymlinkComponents(path, userHome)) {
        qWarning() << callerName << ": Path" << path << "contains a symlinked component below" << userHome;
        sendErrorReply(QDBusError::InvalidArgs,
                       QStringLiteral("Path contains a symlinked component inside the user's home"));
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
    struct group *grp = m_ops->getgrnam(COUCHPLAY_GROUP.toLocal8Bit().constData());
    if (!grp) {
        return false;
    }

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

QStringList CouchPlayHelper::ListCouchPlayUsers()
{
    QStringList result;

    struct group *grp = m_ops->getgrnam(COUCHPLAY_GROUP.toLocal8Bit().constData());
    if (!grp) {
        return result; // Group doesn't exist yet
    }

    for (char **member = grp->gr_mem; *member != nullptr; ++member) {
        const QString username = QString::fromLocal8Bit(*member);
        struct passwd *pw = m_ops->getpwnam(username.toLocal8Bit().constData());
        if (!pw) {
            continue;
        }

        // Copy fields immediately: passwd* is only valid until the next getpwnam call.
        const uint uid = pw->pw_uid;
        const uint gid = pw->pw_gid;
        const QString home = QString::fromLocal8Bit(pw->pw_dir);
        const QString shell = QString::fromLocal8Bit(pw->pw_shell);

        // Filters matching UserManager::parseUsers (src/core/UserManager.cpp)
        if (uid < 1000 || uid >= 65534) {
            continue;
        }
        if (shell.contains(QStringLiteral("nologin")) || shell.contains(QStringLiteral("false"))) {
            continue;
        }
        if (!m_ops->fileExists(home)) {
            continue;
        }

        result.append(QStringLiteral("%1\t%2\t%3\t%4\t%5")
                          .arg(username)
                          .arg(uid)
                          .arg(gid)
                          .arg(home, shell));
    }

    return result;
}

QVariantMap CouchPlayHelper::GetUserInfo(const QString &username)
{
    QVariantMap info;
    if (!s_validUsername.match(username).hasMatch()) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Invalid username format"));
        return info;
    }

    struct passwd *pw = m_ops->getpwnam(username.toLocal8Bit().constData());
    if (!pw) {
        return info;
    }

    info.insert(QStringLiteral("uid"), static_cast<uint>(pw->pw_uid));
    info.insert(QStringLiteral("gid"), static_cast<uint>(pw->pw_gid));
    info.insert(QStringLiteral("home"), QString::fromLocal8Bit(pw->pw_dir));
    return info;
}

QString CouchPlayHelper::GetUserHomeByUid(uint uid)
{
    // Informational, like GetUserInfo: lets the (possibly sandboxed) GUI
    // resolve the compositor's host-side home for home-relative target mapping
    struct passwd *pw = m_ops->getpwuid(uid);
    if (!pw) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("No user with uid %1").arg(uid));
        return QString();
    }
    return QString::fromLocal8Bit(pw->pw_dir);
}

QString CouchPlayHelper::GetUserSteamRoot(const QString &username)
{
    if (!s_validUsername.match(username).hasMatch()) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Invalid username format"));
        return {};
    }

    const QString userHome = getUserHome(username);
    if (userHome.isEmpty()) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("User '%1' does not exist").arg(username));
        return {};
    }

    const QString canonicalHome = m_ops->canonicalFilePath(userHome);
    const QString safeHome = canonicalHome.isEmpty() ? userHome : canonicalHome;
    const QStringList candidates = {
        userHome + QStringLiteral("/.local/share/Steam"),
        userHome + QStringLiteral("/.steam/steam"),
        userHome + QStringLiteral("/.var/app/com.valvesoftware.Steam/.local/share/Steam"),
        userHome + QStringLiteral("/.var/app/com.valvesoftware.Steam/.steam/steam"),
        userHome + QStringLiteral("/.var/app/com.valvesoftware.Steam/data/Steam"),
    };
    for (const QString &candidate : candidates) {
        const bool rootIsDirectory = m_ops->isDirectory(candidate);
        const bool hasSteamMarker = m_ops->fileExists(candidate + QStringLiteral("/steam.sh"))
            || m_ops->fileExists(candidate + QStringLiteral("/userdata"))
            || m_ops->fileExists(candidate + QStringLiteral("/config"));
        if (!rootIsDirectory || !hasSteamMarker) {
            continue;
        }

        const QString canonicalCandidate = m_ops->canonicalFilePath(candidate);
        if (canonicalCandidate.isEmpty()
            || (canonicalCandidate != safeHome && !canonicalCandidate.startsWith(safeHome + QLatin1Char('/')))) {
            qWarning() << "GetUserSteamRoot: Ignoring Steam path outside user home:" << candidate << "->"
                       << canonicalCandidate;
            continue;
        }

        // Keep the user's configured home spelling, but strip symlinked
        // components below it so callers can use no-follow FD walks.
        return userHome + canonicalCandidate.mid(safeHome.length());
    }

    return {};
}

bool CouchPlayHelper::DeleteUser(const QString &username, bool removeHome)
{
    if (!validateUserAndAuth(username, ACTION_DELETE_USER)) {
        return false;
    }

    // Only allow deleting users in the couchplay group
    if (!IsInCouchPlayGroup(username)) {
        sendErrorReply(QDBusError::AccessDenied,
                       QStringLiteral("User '%1' is not a CouchPlay user (not in couchplay group)").arg(username));
        return false;
    }

    // Get the user's UID before deletion (needed for IPC cleanup)
    struct passwd *pw = m_ops->getpwnam(username.toLocal8Bit().constData());
    uid_t userUid = pw ? pw->pw_uid : 0;

    // Don't fail if these don't work
    runCommand(QStringLiteral("loginctl"), {QStringLiteral("disable-linger"), username});
    runCommand(QStringLiteral("pkill"), {QStringLiteral("-u"), username});

    // Wait a moment for processes to terminate
    QThread::msleep(500);

    // Clean up IPC resources to prevent "Permission denied" errors if a new user
    // gets the same name with a different UID and tries to access stale resources
    if (userUid > 0) {
        runCommand(QStringLiteral("/bin/bash"),
                   {QStringLiteral("-c"),
                    QStringLiteral("ipcs -s | awk '$3 == %1 {print $2}' | xargs -r ipcrm -s").arg(userUid)});

        runCommand(QStringLiteral("/bin/bash"),
                   {QStringLiteral("-c"),
                    QStringLiteral("ipcs -m | awk '$3 == %1 {print $2}' | xargs -r ipcrm -m").arg(userUid)});

        runCommand(QStringLiteral("/bin/bash"),
                   {QStringLiteral("-c"),
                    QStringLiteral("ipcs -q | awk '$3 == %1 {print $2}' | xargs -r ipcrm -q").arg(userUid)});

        // Clean up /tmp files owned by the user (Steam dumps, etc.)
        runCommand(
            QStringLiteral("find"),
            {QStringLiteral("/tmp"), QStringLiteral("-user"), QString::number(userUid), QStringLiteral("-delete")},
            30000);

        // Clean up /dev/shm files owned by the user
        runCommand(
            QStringLiteral("find"),
            {QStringLiteral("/dev/shm"), QStringLiteral("-user"), QString::number(userUid), QStringLiteral("-delete")});
    }

    QProcess *process = m_ops->createProcess();
    QStringList args;
    if (removeHome) {
        args << QStringLiteral("-r");
    }
    args << username;

    m_ops->startProcess(process, QStringLiteral("userdel"), args);
    m_ops->waitForFinished(process, 30000);

    if (m_ops->processExitCode(process) != 0) {
        QString errorMsg = QString::fromLocal8Bit(m_ops->readStandardError(process));
        qWarning() << "DeleteUser failed:" << errorMsg;
        sendErrorReply(QDBusError::Failed, QStringLiteral("Failed to delete user: %1").arg(errorMsg));
        delete process;
        return false;
    }
    delete process;

    qDebug() << "Deleted user" << username;
    return true;
}

bool CouchPlayHelper::EnableLinger(const QString &username)
{
    if (!validateUserAndAuth(username, ACTION_ENABLE_LINGER)) {
        return false;
    }

    QProcess *process = m_ops->createProcess();
    m_ops->startProcess(process, QStringLiteral("loginctl"), {QStringLiteral("enable-linger"), username});
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
    // Linger state is stored as a file in /var/lib/systemd/linger/
    QString lingerFile = QStringLiteral("/var/lib/systemd/linger/%1").arg(username);
    return m_ops->fileExists(lingerFile);
}

bool CouchPlayHelper::SetupRuntimeAccess()
{
    if (!checkAuthorization(ACTION_WAYLAND_ACCESS)) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Not authorized to set up runtime access"));
        return false;
    }
    const uint compositorUid = callerUid();
    if (compositorUid == INVALID_CALLER_UID) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Could not resolve caller identity"));
        return false;
    }

    struct passwd *pw = m_ops->getpwuid(compositorUid);
    if (!pw) {
        sendErrorReply(QDBusError::InvalidArgs,
                       QStringLiteral("Compositor user with UID %1 does not exist").arg(compositorUid));
        return false;
    }

    QString runtimeDir = QStringLiteral("/run/user/%1").arg(compositorUid);

    if (!m_ops->fileExists(runtimeDir)) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Runtime directory %1 does not exist").arg(runtimeDir));
        return false;
    }

    bool success = true;

    // Remove stale entries first to avoid "Duplicate entries" errors from leftover GIDs
    auto setAcl = [&](const QString &path, const QString &perm) -> bool {
        if (!m_ops->fileExists(path)) {
            return true;
        }
        QProcess *removeProc = m_ops->createProcess();
        m_ops->startProcess(removeProc,
                            QStringLiteral("setfacl"),
                            {QStringLiteral("-x"), QStringLiteral("g:%1").arg(COUCHPLAY_GROUP), path});
        m_ops->waitForFinished(removeProc, 5000);
        delete removeProc;

        QProcess *proc = m_ops->createProcess();
        m_ops->startProcess(proc,
                            QStringLiteral("setfacl"),
                            {QStringLiteral("-m"), QStringLiteral("g:%1:%2").arg(COUCHPLAY_GROUP, perm), path});
        m_ops->waitForFinished(proc, 5000);
        bool aclOk = (m_ops->processExitCode(proc) == 0);
        if (!aclOk) {
            qWarning() << "Failed to set ACL on" << path << ":"
                       << QString::fromLocal8Bit(m_ops->readStandardError(proc));
        }
        delete proc;
        return aclOk;
    };

    if (!setAcl(runtimeDir, QStringLiteral("x"))) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Failed to set ACL on runtime directory"));
        return false;
    }

    QString waylandSocket = runtimeDir + QStringLiteral("/wayland-0");
    if (!setAcl(waylandSocket, QStringLiteral("rw"))) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Failed to set ACL on Wayland socket"));
        return false;
    }

    for (const QString &xauthFile : m_ops->entryList(runtimeDir, {QStringLiteral("xauth_*")}, QDir::Files)) {
        setAcl(runtimeDir + QStringLiteral("/") + xauthFile, QStringLiteral("r"));
    }

    success &= setAcl(runtimeDir + QStringLiteral("/pipewire-0"), QStringLiteral("rw"));
    success &= setAcl(runtimeDir + QStringLiteral("/pipewire-0-manager"), QStringLiteral("rw"));

    // PulseAudio: mode 0700 means ACL mask is ---, so we must set both group ACL and mask
    QString pulseDir = runtimeDir + QStringLiteral("/pulse");
    if (m_ops->fileExists(pulseDir)) {
        QProcess *proc = m_ops->createProcess();
        m_ops->startProcess(proc,
                            QStringLiteral("setfacl"),
                            {QStringLiteral("-m"), QStringLiteral("g:%1:x,m::x").arg(COUCHPLAY_GROUP), pulseDir});
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
        // Ensure PipeWire PulseAudio TCP listener is configured for cross-user audio
        if (!setupPulseTcpListener(compositorUid)) {
            qWarning() << "Failed to set up PipeWire TCP listener for compositor" << compositorUid;
            // Not fatal — unix socket ACLs are still set
        }
        m_runtimeAccessSetForUid.insert(compositorUid);
        saveState();
    }

    return success;
}

bool CouchPlayHelper::RemoveRuntimeAccess()
{
    if (!checkAuthorization(ACTION_WAYLAND_ACCESS)) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Not authorized to remove runtime access"));
        return false;
    }
    const uint compositorUid = callerUid();
    if (compositorUid == INVALID_CALLER_UID) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Could not resolve caller identity"));
        return false;
    }

    QString runtimeDir = QStringLiteral("/run/user/%1").arg(compositorUid);

    removeRuntimeAcls(runtimeDir);

    removePulseTcpListener(compositorUid);

    m_runtimeAccessSetForUid.remove(compositorUid);
    saveState();

    return true;
}

void CouchPlayHelper::removeRuntimeAcls(const QString &runtimeDir)
{
    auto removeAcl = [&](const QString &path) {
        if (!m_ops->fileExists(path))
            return;
        QProcess *proc = m_ops->createProcess();
        m_ops->startProcess(proc,
                            QStringLiteral("setfacl"),
                            {QStringLiteral("-x"), QStringLiteral("g:%1").arg(COUCHPLAY_GROUP), path});
        m_ops->waitForFinished(proc, 5000);
        if (m_ops->processExitCode(proc) != 0) {
            qWarning() << "Failed to remove ACL on" << path << ":"
                       << QString::fromLocal8Bit(m_ops->readStandardError(proc));
        }
        delete proc;
    };

    removeAcl(runtimeDir + QStringLiteral("/pulse/native"));

    QString pulseDir = runtimeDir + QStringLiteral("/pulse");
    if (m_ops->fileExists(pulseDir)) {
        removeAcl(pulseDir);
        QProcess *maskProc = m_ops->createProcess();
        m_ops->startProcess(maskProc,
                            QStringLiteral("setfacl"),
                            {QStringLiteral("-m"), QStringLiteral("m::---"), pulseDir});
        m_ops->waitForFinished(maskProc, 5000);
        if (m_ops->processExitCode(maskProc) != 0) {
            qWarning() << "Failed to reset ACL mask on" << pulseDir << ":"
                       << QString::fromLocal8Bit(m_ops->readStandardError(maskProc));
        }
        delete maskProc;
    }

    removeAcl(runtimeDir + QStringLiteral("/pipewire-0-manager"));
    removeAcl(runtimeDir + QStringLiteral("/pipewire-0"));

    for (const QString &xauthFile : m_ops->entryList(runtimeDir, {QStringLiteral("xauth_*")}, QDir::Files)) {
        removeAcl(runtimeDir + QStringLiteral("/") + xauthFile);
    }

    removeAcl(runtimeDir + QStringLiteral("/wayland-0"));
    removeAcl(runtimeDir);
}

bool CouchPlayHelper::setupPulseTcpListener(uint compositorUid)
{
    struct passwd *pw = m_ops->getpwuid(compositorUid);
    if (!pw) {
        qWarning() << "setupPulseTcpListener: compositor user not found for UID" << compositorUid;
        return false;
    }

    QString homeDir = QString::fromLocal8Bit(pw->pw_dir);
    QString confDir = homeDir + QStringLiteral("/.config/pipewire/pipewire-pulse.conf.d");
    QString confFile = confDir + QStringLiteral("/50-couchplay.conf");

    // Config already exists — assume it's correct
    if (m_ops->fileExists(confFile)) {
        return true;
    }

    qInfo() << "Deploying PipeWire PulseAudio TCP listener config for UID" << compositorUid;

    // Create config directory
    if (!m_ops->mkpath(confDir)) {
        qWarning() << "Failed to create PipeWire config directory" << confDir;
        return false;
    }

    // Write the drop-in config
    QByteArray config =
        "# SPDX-License-Identifier: GPL-3.0-or-later\n"
        "# SPDX-FileCopyrightText: 2025 CouchPlay Contributors\n"
        "#\n"
        "# PipeWire PulseAudio TCP listener for cross-user audio routing.\n"
        "# Installed by CouchPlay helper. Do not edit.\n"
        "\n"
        "pulse.properties = {\n"
        "    server.address = [\n"
        "        \"unix:native\"\n"
        "        \"tcp:127.0.0.1:4713\"\n"
        "    ]\n"
        "}\n";

    QFile file(confFile);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "Failed to write PipeWire config" << confFile << ":" << file.errorString();
        return false;
    }
    file.write(config);
    file.close();

    // Set ownership to compositor user
    m_ops->chown(confFile, pw->pw_uid, pw->pw_gid);

    // Restart pipewire-pulse to pick up the new config
    restartUserPipeWirePulse(compositorUid);

    return true;
}

void CouchPlayHelper::removePulseTcpListener(uint compositorUid)
{
    struct passwd *pw = m_ops->getpwuid(compositorUid);
    if (!pw) {
        return;
    }

    QString homeDir = QString::fromLocal8Bit(pw->pw_dir);
    QString confFile = homeDir + QStringLiteral("/.config/pipewire/pipewire-pulse.conf.d/50-couchplay.conf");

    if (m_ops->fileExists(confFile)) {
        qInfo() << "Removing PipeWire TCP listener config for UID" << compositorUid;
        QFile::remove(confFile);

        // Restart pipewire-pulse to revert to defaults
        restartUserPipeWirePulse(compositorUid);
    }
}

void CouchPlayHelper::restartUserPipeWirePulse(uint compositorUid)
{
    // Restart pipewire-pulse as the compositor user via machinectl
    // This is the correct way to run a command in a user's systemd session
    struct passwd *pw = m_ops->getpwuid(compositorUid);
    if (!pw) {
        return;
    }

    QString username = QString::fromLocal8Bit(pw->pw_name);

    // Use machinectl shell to restart pipewire-pulse in the user's session
    // This works because linger is enabled for all users
    QProcess *proc = m_ops->createProcess();
    m_ops->startProcess(proc,
                        QStringLiteral("machinectl"),
                        {QStringLiteral("shell"),
                         username + QStringLiteral("@"),
                         QStringLiteral("/bin/bash"),
                         QStringLiteral("-c"),
                         QStringLiteral("systemctl --user restart pipewire-pulse")});
    m_ops->waitForFinished(proc, 10000);
    if (m_ops->processExitCode(proc) != 0) {
        qWarning() << "Failed to restart pipewire-pulse for" << username << ":"
                   << QString::fromLocal8Bit(m_ops->readStandardError(proc));
    } else {
        qInfo() << "Restarted pipewire-pulse for" << username;
        // Give pipewire-pulse a moment to start listening on TCP
        QThread::msleep(500);
    }
    delete proc;
}

uint CouchPlayHelper::callerUid() const
{
    const uid_t uid = m_ops->connectionUnixUser(message().service());
    if (uid == static_cast<uid_t>(-1)) {
        return INVALID_CALLER_UID;
    }
    return static_cast<uint>(uid);
}
bool CouchPlayHelper::checkAuthorization(const QString &action)
{
    return m_ops->checkAuthorization(action, message().service());
}
bool CouchPlayHelper::validateUserAndAuth(const QString &username, const QString &action)
{
    if (!s_validUsername.match(username).hasMatch()) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Invalid username format"));
        return false;
    }
    if (!checkAuthorization(action)) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Not authorized"));
        return false;
    }
    if (!userExists(username)) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("User '%1' does not exist").arg(username));
        return false;
    }
    const uint targetUid = getUserUid(username);
    const uint requestUid = callerUid();
    if (requestUid == INVALID_CALLER_UID) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Could not resolve caller identity"));
        return false;
    }
    if ((action == ACTION_LAUNCH_INSTANCE || action == ACTION_MANAGE_VIRTUAL_DISPLAY)
        && targetUid != requestUid && !IsInCouchPlayGroup(username)) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Target user is not a CouchPlay-managed user"));
        return false;
    }
    return true;
}

bool CouchPlayHelper::runCommand(const QString &program, const QStringList &args, int timeoutMs)
{
    QProcess *proc = m_ops->createProcess();
    m_ops->startProcess(proc, program, args);
    m_ops->waitForFinished(proc, timeoutMs);
    bool ok = (m_ops->processExitCode(proc) == 0);
    delete proc;
    return ok;
}

bool CouchPlayHelper::isValidDevicePath(const QString &path)
{
    if (path.contains(QStringLiteral(".."))) {
        return false;
    }

    bool isInputDevice = path.startsWith(QStringLiteral("/dev/input/"));
    bool isHidrawDevice = path.startsWith(QStringLiteral("/dev/hidraw"));

    if (!isInputDevice && !isHidrawDevice) {
        return false;
    }

    if (isHidrawDevice) {
        static QRegularExpression hidrawRegex(QStringLiteral("^/dev/hidraw\\d+$"));
        if (!hidrawRegex.match(path).hasMatch()) {
            return false;
        }
    }

    if (!m_ops->fileExists(path)) {
        return false;
    }

    struct stat st;
    if (!m_ops->statPath(path, &st)) {
        return false;
    }

    return m_ops->isCharDevice(st.st_mode);
}

qint64 CouchPlayHelper::LaunchInstance(const QString &username,
                                       const QString &displayContext,
                                       const QStringList &gamescopeArgs,
                                       const QStringList &gameCommand,
                                       const QString &workingDirectory,
                                       const QStringList &sharedRoots,
                                       const QStringList &environment,
                                       const QStringList &bindPaths)
{
    if (!validateUserAndAuth(username, ACTION_LAUNCH_INSTANCE)) {
        return 0;
    }
    if (gameCommand.isEmpty() || gameCommand.constFirst().isEmpty()) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Child launch command cannot be empty"));
        return 0;
    }
    if (!workingDirectory.isEmpty()
        && (!QDir::isAbsolutePath(workingDirectory) || !m_ops->fileExists(workingDirectory))) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Working directory must be an existing absolute path"));
        return 0;
    }
    if (!workingDirectory.isEmpty()) {
        const QString targetHome = getUserHome(username);
        bool allowed = !targetHome.isEmpty()
            && (workingDirectory == targetHome || workingDirectory.startsWith(targetHome + QLatin1Char('/')));
        for (const QString &root : sharedRoots) {
            if (workingDirectory == root || workingDirectory.startsWith(root + QLatin1Char('/'))) {
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Invalid launch working directory"));
            return 0;
        }
    }








    const uint compositorUid = callerUid();
    if (compositorUid == INVALID_CALLER_UID) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Could not resolve caller identity"));
        return 0;
    }
    if (!m_runtimeAccessSetForUid.contains(compositorUid)) {
        if (!SetupRuntimeAccess()) {
            qWarning() << "Failed to set up runtime access for compositor" << compositorUid;
        }
    }


    const qint64 pid = startTransientUnit(username,
                                          displayContext,
                                          gamescopeArgs,
                                          gameCommand,
                                          workingDirectory,
                                          sharedRoots,
                                          environment,
                                          bindPaths);
    if (pid <= 0) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Failed to launch instance for user '%1'").arg(username));
        return 0;
    }
    return pid;
}

bool CouchPlayHelper::StopInstance(qint64 pid)
{
    if (!checkAuthorization(ACTION_LAUNCH_INSTANCE)) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Not authorized to stop instances"));
        return false;
    }
    if (!m_pidToUsername.contains(pid) || !m_pidOwnerUid.contains(pid)) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Unknown or untracked instance PID"));
        return false;
    }
    if (m_pidOwnerUid.value(pid) != callerUid()) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Not authorized for this instance"));
        return false;
    }
    const QString username = m_pidToUsername.value(pid);
    const QString serviceName = m_usernameToUnitName.value(username);
    if (serviceName.isEmpty()) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Instance unit is not tracked"));
        return false;
    }
    m_stoppingUnits.insert(serviceName);
    if (!stopServiceInstance(serviceName)) {
        m_stoppingUnits.remove(serviceName);
        sendErrorReply(QDBusError::Failed, QStringLiteral("Failed to stop instance unit"));
        return false;
    }
    delete m_monitors.take(serviceName);
    m_usernameToUnitName.remove(username);
    m_pidToUsername.remove(pid);
    m_pidOwnerUid.remove(pid);
    m_stoppingUnits.remove(serviceName);
    cleanupTcpListenerIfLast(username);
    saveState();
    return true;
}

bool CouchPlayHelper::KillInstance(qint64 pid)
{
    if (!checkAuthorization(ACTION_LAUNCH_INSTANCE)) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Not authorized to kill instances"));
        return false;
    }
    if (!m_pidToUsername.contains(pid) || !m_pidOwnerUid.contains(pid)) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Unknown or untracked instance PID"));
        return false;
    }
    if (m_pidOwnerUid.value(pid) != callerUid()) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Not authorized for this instance"));
        return false;
    }
    const QString username = m_pidToUsername.value(pid);
    const QString serviceName = m_usernameToUnitName.value(username);
    if (serviceName.isEmpty()) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Instance unit is not tracked"));
        return false;
    }
    m_stoppingUnits.insert(serviceName);
    QProcess *killProc = m_ops->createProcess();
    m_ops->startProcess(killProc,
                        QStringLiteral("systemctl"),
                        {QStringLiteral("kill"), serviceName, QStringLiteral("--kill-who=all"),
                         QStringLiteral("--signal=SIGKILL")});
    const bool killFinished = m_ops->waitForFinished(killProc, 10000);
    const bool killed = killFinished && m_ops->processExitCode(killProc) == 0;
    delete killProc;
    const bool stopped = killed && stopServiceInstance(serviceName);
    if (!stopped) {
        m_stoppingUnits.remove(serviceName);
        sendErrorReply(QDBusError::Failed, QStringLiteral("Failed to kill instance unit"));
        return false;
    }
    delete m_monitors.take(serviceName);
    m_usernameToUnitName.remove(username);
    m_pidToUsername.remove(pid);
    m_stoppingUnits.remove(serviceName);
    m_pidOwnerUid.remove(pid);

    cleanupTcpListenerIfLast(username);
    saveState();
    return true;
}








QString CouchPlayHelper::generateServiceName(const QString &username)
{
    return QStringLiteral("couchplay-%1.service").arg(username);
}

qint64 CouchPlayHelper::startTransientUnit(const QString &username,
                                           const QString &displayContext,
                                           const QStringList &gamescopeArgs,
                                           const QStringList &gameCommand,
                                           const QString &workingDirectory,
                                           const QStringList &sharedRoots,
                                           const QStringList &environment,
                                           const QStringList &bindPaths)

{
    Q_UNUSED(sharedRoots);
    QString serviceName = generateServiceName(username);

    struct passwd *pwd = m_ops->getpwnam(username.toLocal8Bit().constData());
    if (!pwd) {
        qWarning() << "startTransientUnit: failed to resolve UID for user" << username;
        return 0;
    }
    const uint requestUid = callerUid();
    if (requestUid == INVALID_CALLER_UID) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Could not resolve caller identity"));
        return 0;
    }
    uint runtimeUid = requestUid;
    QString waylandSocket = QStringLiteral("wayland-0");
    if (!displayContext.isEmpty()) {
        bool foundContext = false;
        for (const VirtualDisplayInfo &info : m_virtualDisplays) {
            if (info.displayContext == displayContext) {
                if (info.targetUsername != username || info.ownerUid != requestUid) {
                    sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Invalid display context owner"));
                    return 0;
                }
                runtimeUid = info.runtimeUid;
                waylandSocket = info.waylandSocket;
                foundContext = true;
                break;
            }
        }
        if (!foundContext) {
            sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Unknown display context"));
            return 0;
        }
    }
    const QString userUid = QString::number(pwd->pw_uid);
    const QString compositorRuntimeDir = QStringLiteral("/run/user/%1").arg(runtimeUid);
    const QString userRuntimeDir = QStringLiteral("/run/user/%1").arg(userUid);

    // Runtime directory must exist (requires linger) — without it, PipeWire
    // and gamescope lockfiles fail silently
    if (!m_ops->fileExists(userRuntimeDir) || !m_ops->fileExists(userRuntimeDir + QStringLiteral("/bus"))) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Target user session bus is unavailable"));
        return 0;
    }





    QStringList systemdRunArgs;
    systemdRunArgs << QStringLiteral("--unit") << serviceName;
    systemdRunArgs << QStringLiteral("--uid") << username;
    systemdRunArgs << QStringLiteral("--property=Type=simple");
    systemdRunArgs << QStringLiteral("--property=KillMode=control-group");
    systemdRunArgs << QStringLiteral("--property=TimeoutStopSec=10s");
    systemdRunArgs << QStringLiteral("--property=SendSIGKILL=yes");
    systemdRunArgs << QStringLiteral("--property=Delegate=yes");
    systemdRunArgs << QStringLiteral("--property=MemoryDenyWriteExecute=false");
    const QString effectiveWorkingDirectory =
        workingDirectory.isEmpty() ? QString::fromLocal8Bit(pwd->pw_dir) : workingDirectory;
    systemdRunArgs << QStringLiteral("--property=WorkingDirectory=%1").arg(effectiveWorkingDirectory);



    // -E flag avoids escaping issues with --property=Environment=
    auto addEnv = [&](const QString &assignment) {
        systemdRunArgs << QStringLiteral("-E") << assignment;
    };
    addEnv(QStringLiteral("DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/%1/bus").arg(userUid));
    addEnv(QStringLiteral("WAYLAND_DISPLAY=%1/%2").arg(compositorRuntimeDir, waylandSocket));
    addEnv(QStringLiteral("XDG_RUNTIME_DIR=/run/user/%1").arg(userUid));
    addEnv(QStringLiteral("PIPEWIRE_RUNTIME_DIR=%1").arg(compositorRuntimeDir));
    // Use TCP PulseAudio listener for cross-user audio routing.
    // The unix socket rejects cross-UID connections, but TCP on localhost works.
    // The TCP listener is configured by setupPulseTcpListener() during SetupRuntimeAccess().
    addEnv(QStringLiteral("PULSE_SERVER=tcp:127.0.0.1:4713"));
    addEnv(QStringLiteral("HOME=%1").arg(QString::fromLocal8Bit(pwd->pw_dir)));
    addEnv(QStringLiteral("USER=%1").arg(QString::fromLocal8Bit(pwd->pw_name)));
    addEnv(QStringLiteral("LOGNAME=%1").arg(QString::fromLocal8Bit(pwd->pw_name)));
    addEnv(QStringLiteral("SHELL=%1").arg(QString::fromLocal8Bit(pwd->pw_shell)));
    addEnv(QStringLiteral("PATH=%1/.local/bin:/usr/local/bin:/usr/bin:/bin").arg(QString::fromLocal8Bit(pwd->pw_dir)));
    addEnv(QStringLiteral("PWD=%1").arg(workingDirectory.isEmpty()
                                               ? QString::fromLocal8Bit(pwd->pw_dir)
                                               : workingDirectory));
    addEnv(QStringLiteral("XDG_CONFIG_HOME=%1/.config").arg(QString::fromLocal8Bit(pwd->pw_dir)));
    addEnv(QStringLiteral("XDG_DATA_HOME=%1/.local/share").arg(QString::fromLocal8Bit(pwd->pw_dir)));
    addEnv(QStringLiteral("XDG_STATE_HOME=%1/.local/state").arg(QString::fromLocal8Bit(pwd->pw_dir)));
    addEnv(QStringLiteral("XDG_CACHE_HOME=%1/.cache").arg(QString::fromLocal8Bit(pwd->pw_dir)));
    const QStringList protectedEnvironmentNames = {
        QStringLiteral("HOME"), QStringLiteral("USER"), QStringLiteral("LOGNAME"), QStringLiteral("SHELL"),
        QStringLiteral("PATH"), QStringLiteral("PWD"), QStringLiteral("XDG_CONFIG_HOME"),
        QStringLiteral("XDG_DATA_HOME"), QStringLiteral("XDG_STATE_HOME"), QStringLiteral("XDG_CACHE_HOME"),
        QStringLiteral("XDG_RUNTIME_DIR"), QStringLiteral("DBUS_SESSION_BUS_ADDRESS"),
        QStringLiteral("WAYLAND_DISPLAY"), QStringLiteral("PIPEWIRE_RUNTIME_DIR"), QStringLiteral("PULSE_SERVER")};
    QStringList environmentNames;
    for (const QString &var : environment) {
        int eqPos = var.indexOf(QLatin1Char('='));
        if (eqPos <= 0) {
            sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Invalid environment assignment"));
            return 0;
        }
        const QString name = var.left(eqPos);
        const QString value = var.mid(eqPos + 1);
        if (!QRegularExpression(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$")).match(name).hasMatch()
            || value.contains(QChar::Null) || value.contains(QChar::LineFeed) || value.contains(QChar::CarriageReturn)
            || protectedEnvironmentNames.contains(name) || environmentNames.contains(name)) {
            sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Invalid or reserved environment variable: %1").arg(name));
            return 0;
        }
        environmentNames.append(name);
        addEnv(var);


    }

    if (runtimeUid != pwd->pw_uid) {
        systemdRunArgs << QStringLiteral("--property=BindReadOnlyPaths=%1").arg(compositorRuntimeDir);
    }
    const QString targetHome = getUserHome(username);

    QStringList validatedBindPaths;
    for (const QString &bp : bindPaths) {
        const int separator = bp.indexOf(QLatin1Char(':'));
        if (separator <= 0 || separator == bp.size() - 1) {
            sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Malformed launch bind path"));
            return 0;
        }
        const QString source = bp.left(separator);
        const QString target = bp.mid(separator + 1);
        bool targetAllowed = !targetHome.isEmpty()
            && (target == targetHome || target.startsWith(targetHome + QLatin1Char('/')));
        for (const QString &root : sharedRoots) {
            if (target == root || target.startsWith(root + QLatin1Char('/'))) {
                targetAllowed = true;
                break;
            }
        }
        if (!targetAllowed) {
            sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Launch bind target is not a declared shared root"));
            return 0;
        }
        if (!QDir::isAbsolutePath(source) || !QDir::isAbsolutePath(target)
            || !isPathWithinAllowedPrefix(source) || !isPathWithinAllowedPrefix(target)) {
            sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Launch bind path is outside allowed prefixes"));
            return 0;
        }
        const QString canonicalSource = m_ops->canonicalFilePath(source);
        if (!canonicalSource.isEmpty() && !isPathWithinAllowedPrefix(canonicalSource)) {
            sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Launch bind source resolves outside allowed prefixes"));
            return 0;
        }
        validatedBindPaths.append(bp);
    }
    for (const QString &bp : validatedBindPaths) {
        systemdRunArgs << QStringLiteral("--property=BindReadOnlyPaths=%1").arg(bp);
    }

    // On immutable distros, gamescope may not be at /usr/bin/gamescope
    QString gamescopePath = QStringLiteral("/usr/bin/gamescope");
    if (!m_ops->fileExists(gamescopePath)) {
        QProcess *whichProc = m_ops->createProcess();
        m_ops->startProcess(whichProc, QStringLiteral("which"), {QStringLiteral("gamescope")});
        m_ops->waitForFinished(whichProc, 3000);
        if (m_ops->processExitCode(whichProc) == 0) {
            QString resolved = QString::fromLocal8Bit(m_ops->readAllStandardOutput(whichProc)).trimmed();
            if (!resolved.isEmpty()) {
                gamescopePath = resolved;
            }
        }
        delete whichProc;
    }

    QStringList cmdArgs;
    cmdArgs << gamescopePath << gamescopeArgs;
    cmdArgs << QStringLiteral("--") << gameCommand;
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
                qWarning() << "systemd-run retry failed for" << serviceName << "exit code:" << exitCode << errOutput;
                delete proc;
                return 0;
            }
            delete proc;
        } else {
            qWarning() << "systemd-run failed for" << serviceName << "exit code:" << exitCode << errOutput;
            return 0;
        }
    } else {
        delete proc;
    }

    // Poll for MainPID (systemd needs a moment to register the unit)
    qint64 mainPid = 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        QThread::msleep(500);
        QProcess *showProc = m_ops->createProcess();
        m_ops->startProcess(showProc,
                            QStringLiteral("systemctl"),
                            {QStringLiteral("show"),
                             serviceName,
                             QStringLiteral("-p"),
                             QStringLiteral("MainPID"),
                             QStringLiteral("--value")});
        m_ops->waitForFinished(showProc, 5000);
        QByteArray output = m_ops->readAllStandardOutput(showProc).trimmed();
        delete showProc;

        if (!output.isEmpty()) {
            bool ok = false;
            mainPid = output.toLongLong(&ok);
            if (ok && mainPid > 0)
                break;
        }
    }

    if (mainPid <= 0) {
        qWarning() << "Could not get MainPID for transient unit" << serviceName;
        return 0;
    }

    m_usernameToUnitName[username] = serviceName;
    m_pidToUsername[mainPid] = username;
    m_pidOwnerUid[mainPid] = requestUid;
    m_compositorUidForUsername[username] = requestUid;

    saveState();

    monitorUnitState(serviceName, username, mainPid);

    qInfo() << "Started transient unit" << serviceName << "with PID" << mainPid;
    return mainPid;
}

void CouchPlayHelper::cleanupTcpListenerIfLast(const QString &username)
{
    if (!m_compositorUidForUsername.contains(username)) {
        return;
    }
    uint compositorUid = m_compositorUidForUsername.value(username);
    m_compositorUidForUsername.remove(username);

    bool hasOtherInstances = false;
    for (auto it = m_compositorUidForUsername.constBegin(); it != m_compositorUidForUsername.constEnd(); ++it) {
        if (it.value() == compositorUid) {
            hasOtherInstances = true;
            break;
        }
    }

    if (!hasOtherInstances) {
        removePulseTcpListener(compositorUid);
        m_runtimeAccessSetForUid.remove(compositorUid);
    }
}

bool CouchPlayHelper::stopServiceInstance(const QString &serviceName)
{
    QProcess *stopProc = m_ops->createProcess();
    m_ops->startProcess(stopProc, QStringLiteral("systemctl"), {QStringLiteral("stop"), serviceName});
    const bool stopped = m_ops->waitForFinished(stopProc, 10000) && m_ops->processExitCode(stopProc) == 0;
    delete stopProc;

    QProcess *resetProc = m_ops->createProcess();
    m_ops->startProcess(resetProc, QStringLiteral("systemctl"), {QStringLiteral("reset-failed"), serviceName});
    m_ops->waitForFinished(resetProc, 5000);
    delete resetProc;
    return stopped;
}


void CouchPlayHelper::monitorUnitState(const QString &serviceName, const QString &username, qint64 mainPid)
{
    QDBusMessage getUnitMsg = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.systemd1"),
                                                             QStringLiteral("/org/freedesktop/systemd1"),
                                                             QStringLiteral("org.freedesktop.systemd1.Manager"),
                                                             QStringLiteral("GetUnit"));
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

// Only a complete ".." component climbs the tree; names like "Foo..Bar" are
// ordinary and folder pickers produce them happily. openDirBelow additionally
// rejects ".." parts during its walk as defense in depth.
static bool pathHasDotDotComponent(const QString &path)
{
    return path.split(QLatin1Char('/'), Qt::SkipEmptyParts).contains(QStringLiteral(".."));
}

bool CouchPlayHelper::isPathWithinAllowedPrefix(const QString &path) const
{
    if (pathHasDotDotComponent(path)) {
        return false;
    }

    static const QStringList allowedPrefixes = {
        QStringLiteral("/home/"),
        QStringLiteral("/var/home/"), // Bazzite/Fedora Silverblue
        QStringLiteral("/run/media/"),
        QStringLiteral("/media/"), // keep in sync with SetPathAclWithParents stop boundaries
        QStringLiteral("/mnt/"),
        QStringLiteral("/tmp/"),
    };

    for (const QString &prefix : allowedPrefixes) {
        if (path.startsWith(prefix)) {
            return true;
        }
    }

    return false;
}

bool CouchPlayHelper::pathHasSymlinkComponents(const QString &path, const QString &root)
{
    // Every existing component of `path` strictly below `root` must be a real
    // directory: a player-owned symlinked ancestor (e.g. ~/.config -> /etc)
    // would redirect root-run rm/cp/chown/mount outside the verified root.
    if (!path.startsWith(root + QLatin1Char('/'))) {
        return true; // not below the root at all — caller decides, treat as unsafe
    }

    QString current = root;
    const QStringList parts = path.mid(root.length()).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        current += QLatin1Char('/') + part;
        if (m_ops->fileExists(current) && m_ops->isSymLink(current)) {
            return true;
        }
    }
    return false;
}

bool CouchPlayHelper::secureCreateUserDir(const QString &username, const QString &absolutePath)
{
    // Race-safe mkdir+chown below the user's home: openat(O_NOFOLLOW) walk,
    // fchown on FDs — immune to symlinked-ancestor swaps between check and use
    QString userHome = getUserHome(username);
    if (userHome.isEmpty() || !absolutePath.startsWith(userHome + QLatin1Char('/'))) {
        return false;
    }

    uint userUid = getUserUid(username);
    struct passwd *pw = m_ops->getpwuid(userUid);
    if (!pw) {
        return false;
    }

    int homeFd = SecureFs::openBaseDir(userHome);
    if (homeFd < 0) {
        return false;
    }
    const QStringList parts =
        QDir(userHome).relativeFilePath(absolutePath).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    int dirFd = SecureFs::openDirBelow(homeFd, parts, true, userUid, pw->pw_gid);
    ::close(homeFd);
    if (dirFd < 0) {
        return false;
    }
    ::close(dirFd);
    return true;
}

QString CouchPlayHelper::computeMountTarget(const QString &source,
                                            const QString &alias,
                                            const QString &userHome,
                                            const QString &compositorHome)
{
    // Reject alias with path traversal (whole ".." components only — names
    // like "Foo..Bar" are valid)
    if (pathHasDotDotComponent(alias)) {
        qWarning() << "computeMountTarget: alias contains path traversal:" << alias;
        return {};
    }

    QString target;
    // Require the '/' boundary: /home/deck2/game must not be treated as
    // home-relative to /home/deck (that produced /home/<player>2/game)
    if (source.startsWith(compositorHome + QLatin1Char('/')) && alias.isEmpty()) {
        // Prevent traversal via source containing ../ after compositorHome prefix
        QString relativePath = source.mid(compositorHome.length());
        if (pathHasDotDotComponent(relativePath)) {
            qWarning() << "computeMountTarget: source relative path contains a '..' component:" << source;
            return {};
        }
        target = userHome + relativePath;
    } else if (!alias.isEmpty()) {
        if (alias.startsWith(QLatin1Char('/'))) {
            target = userHome + alias;
        } else {
            target = userHome + QStringLiteral("/") + alias;
        }
    } else {
        target = userHome + QStringLiteral("/.couchplay/mounts") + source;
    }

    // Normalize, then require a strict descendant of the home: an alias or
    // source that cleans to the home itself (".", "./", "/home/deck/.")
    // would attach the shared source over the player's entire home
    target = QDir::cleanPath(target);
    if (!target.startsWith(userHome + QLatin1Char('/'))) {
        qWarning() << "computeMountTarget: target is not a strict descendant of the user home:" << target;
        return {};
    }

    return target;
}

int CouchPlayHelper::MountSharedDirectories(const QString &username, const QStringList &directories)
{
    if (!validateUserAndAuth(username, ACTION_MANAGE_MOUNTS)) {
        return 0;
    }

    QString userHome = getUserHome(username);
    if (userHome.isEmpty()) {
        sendErrorReply(QDBusError::Failed,
                       QStringLiteral("Could not determine home directory for user '%1'").arg(username));
        return 0;
    }

    const uint compositorUid = callerUid();
    QString compositorHome = getUserHomeByUid(compositorUid);
    if (compositorHome.isEmpty()) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not determine home directory for compositor user"));
        return 0;
    }

    int successCount = 0;

    for (const QString &dirSpec : directories) {
        // Specs escape '|' and '\' inside the fields (paths like
        // /mnt/Game|Saves must survive the wire); malformed specs are skipped
        QString source;
        QString alias;
        if (!decodeMountSpec(dirSpec, source, alias)) {
            qWarning() << "MountSharedDirectories: Malformed directory spec:" << dirSpec;
            continue;
        }

        if (!m_ops->fileExists(source)) {
            qWarning() << "MountSharedDirectories: Source path does not exist:" << source;
            continue;
        }

        if (!m_ops->isDirectory(source)) {
            qWarning() << "MountSharedDirectories: Source is not a directory:" << source;
            continue;
        }

        if (!isPathWithinAllowedPrefix(source)) {
            qWarning() << "MountSharedDirectories: Source path is outside allowed prefixes:" << source;
            continue;
        }

        QString canonicalSource = m_ops->canonicalFilePath(source);
        if (!canonicalSource.isEmpty() && !isPathWithinAllowedPrefix(canonicalSource)) {
            qWarning() << "MountSharedDirectories: Source path resolves outside allowed prefixes:" << source << "->"
                       << canonicalSource;
            continue;
        }

        QString target = computeMountTarget(source, alias, userHome, compositorHome);
        if (target.isEmpty()) {
            qWarning() << "MountSharedDirectories: Invalid mount target computed";
            continue;
        }

        if (pathHasSymlinkComponents(target, userHome)) {
            qWarning() << "MountSharedDirectories: Target contains a symlinked component:" << target;
            continue;
        }

        if (!m_ops->fileExists(target)) {
            if (!secureCreateUserDir(username, target)) {
                qWarning() << "MountSharedDirectories: Failed to create target directory:" << target;
                continue;
            }
        }

        const QString targetLeaf = target.mid(target.lastIndexOf(QLatin1Char('/')) + 1);
        const QString targetParentPath = target.left(target.lastIndexOf(QLatin1Char('/')));

        MountInfo info;
        info.source = source;
        info.target = target;
        info.mountType = QStringLiteral("bind");

        // FD-anchored bind only — a mount(8) fallback would re-introduce the
        // pathname race this design exists to close, so fail closed instead
        // when the kernel mount API is unavailable
        if (!SecureFs::mountApiAvailable()) {
            qWarning() << "MountSharedDirectories: kernel mount API unavailable, refusing path-based mount";
            continue;
        }
        // Pin the target parent (no-follow walk), clone the source with
        // AT_SYMLINK_NOFOLLOW, attach with move_mount — no pathname
        // re-resolution a player could race
        int parentFd = SecureFs::openExistingDirNoFollow(targetParentPath);
        if (parentFd < 0) {
            qWarning() << "MountSharedDirectories: Could not anchor target parent:" << targetParentPath;
            continue;
        }
        int mountResult =
            SecureFs::bindMountFd(canonicalSource.isEmpty() ? source : canonicalSource, parentFd, targetLeaf);
        if (mountResult != 0) {
            ::close(parentFd);
            qWarning() << "MountSharedDirectories: FD bind mount failed for" << source << "->" << target << ":"
                       << strerror(-mountResult);
            continue;
        }
        info.targetParentFd = parentFd;
        info.targetLeaf = targetLeaf;

        m_activeMounts[username].append(info);

        successCount++;
    }

    saveState();

    return successCount;
}

bool CouchPlayHelper::SetupOverlayMount(const QString &username,
                                        const QString &sourceDir,

                                        const QString &targetAlias)
{
    if (!validateUserAndAuth(username, ACTION_MANAGE_MOUNTS)) {
        return false;
    }

    if (!m_ops->fileExists(sourceDir)) {
        qWarning() << "SetupOverlayMount: Source path does not exist:" << sourceDir;
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Source path does not exist: %1").arg(sourceDir));
        return false;
    }

    if (!m_ops->isDirectory(sourceDir)) {
        qWarning() << "SetupOverlayMount: Source is not a directory:" << sourceDir;
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Source is not a directory: %1").arg(sourceDir));
        return false;
    }

    if (!isPathWithinAllowedPrefix(sourceDir)) {
        qWarning() << "SetupOverlayMount: Source path is outside allowed prefixes:" << sourceDir;
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Source path is outside allowed prefixes"));
        return false;
    }

    QString canonicalSource = m_ops->canonicalFilePath(sourceDir);
    if (!canonicalSource.isEmpty() && !isPathWithinAllowedPrefix(canonicalSource)) {
        qWarning() << "SetupOverlayMount: Source path resolves outside allowed prefixes:" << sourceDir << "->"
                   << canonicalSource;
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Source path resolves outside allowed prefixes"));
        return false;
    }

    QString userHome = getUserHome(username);
    if (userHome.isEmpty()) {
        sendErrorReply(QDBusError::Failed,
                       QStringLiteral("Could not determine home directory for user '%1'").arg(username));
        return false;
    }

    const uint compositorUid = callerUid();
    QString compositorHome = getUserHomeByUid(compositorUid);
    if (compositorHome.isEmpty()) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not determine home directory for compositor user"));
        return false;
    }

    QString target = computeMountTarget(sourceDir, targetAlias, userHome, compositorHome);
    if (target.isEmpty()) {
        qWarning() << "SetupOverlayMount: Invalid mount target computed";
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Invalid mount target"));
        return false;
    }

    if (pathHasSymlinkComponents(target, userHome)) {
        qWarning() << "SetupOverlayMount: Target contains a symlinked component:" << target;
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Target contains a symlinked component"));
        return false;
    }

    if (!m_ops->fileExists(target)) {
        if (!secureCreateUserDir(username, target)) {
            qWarning() << "SetupOverlayMount: Failed to create target directory:" << target;
            sendErrorReply(QDBusError::Failed, QStringLiteral("Failed to create target directory: %1").arg(target));
            return false;
        }
    }

    QByteArray hashBytes =
        QCryptographicHash::hash(sourceDir.toUtf8(), QCryptographicHash::Sha256).toHex().left(16);
    QString hash = QString::fromUtf8(hashBytes);

    // Overlay layer storage lives under root-owned /var/lib/couchplay, NOT
    // the player's home: with every ancestor root-owned (the helper runs as
    // root, players have no write access to the chain), the player cannot
    // swap a component for a symlink between secure creation and the
    // pathname-based mount(8) — closing the re-resolution race. Only the
    // upper/work leaves are chowned to the player (the kernel writes overlay
    // data with the player's credentials through the mount).
    QString overlayBase = QStringLiteral("/var/lib/couchplay/overlays/%1/%2").arg(username, hash);
    QString upperDir = overlayBase + QStringLiteral("/upper");
    QString workDir = overlayBase + QStringLiteral("/work");

    uint userUid = getUserUid(username);
    struct passwd *pw = m_ops->getpwuid(userUid);
    if (!pw) {
        qWarning() << "SetupOverlayMount: Could not get user info for" << username;
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not get user info for '%1'").arg(username));
        return false;
    }

    int varLibFd = SecureFs::openBaseDir(QStringLiteral("/var/lib"));
    if (varLibFd < 0) {
        qWarning() << "SetupOverlayMount: Could not open /var/lib:" << strerror(-varLibFd);
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not open overlay storage root"));
        return false;
    }
    const QStringList baseParts =
        QStringList{QStringLiteral("couchplay"), QStringLiteral("overlays"), username, hash};
    int baseFd =
        SecureFs::openDirBelow(varLibFd, baseParts, true, 0, 0, SecureFs::ChownMode::FinalOnly);
    ::close(varLibFd);
    if (baseFd < 0) {
        qWarning() << "SetupOverlayMount: Could not create overlay storage under /var/lib/couchplay:"
                   << strerror(-baseFd);
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not create overlay storage"));
        return false;
    }
    int upperFd = SecureFs::openDirBelow(baseFd, {QStringLiteral("upper")}, true, userUid, pw->pw_gid);
    if (upperFd < 0) {
        ::close(baseFd);
        qWarning() << "SetupOverlayMount: Failed to securely create upper directory:" << upperDir;
        sendErrorReply(QDBusError::Failed, QStringLiteral("Failed to create overlay upper directory"));
        return false;
    }
    ::close(upperFd);
    int workFd = SecureFs::openDirBelow(baseFd, {QStringLiteral("work")}, true, userUid, pw->pw_gid);
    ::close(baseFd);
    if (workFd < 0) {
        qWarning() << "SetupOverlayMount: Failed to securely create work directory:" << workDir;
        sendErrorReply(QDBusError::Failed, QStringLiteral("Failed to create overlay work directory"));
        return false;
    }
    ::close(workFd);

    // FD-anchored attach when the kernel supports it: the target parent is
    // pinned with a no-follow walk and move_mount does not follow symlinks on
    // the leaf, so a raced target swap fails the mount instead of redirecting
    // it. upper/work live under root-owned storage (see above) and cannot be
    // raced. The mount(8) fallback is only for pre-5.2 kernels.
    const QString targetLeaf = target.mid(target.lastIndexOf(QLatin1Char('/')) + 1);
    const QString targetParentPath = target.left(target.lastIndexOf(QLatin1Char('/')));

    MountInfo info;
    info.source = sourceDir;
    info.target = target;
    info.mountType = QStringLiteral("overlay");
    info.upperDir = upperDir;
    info.workDir = workDir;

    // FD-anchored attach only — no path-based fallback: a failed FD mount
    // must fail closed instead of retrying through a raceable pathname
    if (!SecureFs::overlayMountApiAvailable()) {
        qWarning() << "SetupOverlayMount: overlayfs mount API unavailable, refusing path-based mount";
        sendErrorReply(QDBusError::Failed, QStringLiteral("Overlay filesystem mount API unavailable"));
        return false;
    }

    // Pin the validated source with a no-follow walk; overlayMountFd passes
    // the lowerdir through /proc/self/fd so fsconfig resolution follows the
    // pinned inode, not the (mutable) namespace path. The FD stays open until
    // the mount is attached.
    int sourceFd = SecureFs::openExistingDirNoFollow(canonicalSource.isEmpty() ? sourceDir : canonicalSource);
    if (sourceFd < 0) {
        qWarning() << "SetupOverlayMount: Could not anchor source directory:" << sourceDir;
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not anchor source directory"));
        return false;
    }

    int parentFd = SecureFs::openExistingDirNoFollow(targetParentPath);
    if (parentFd < 0) {
        ::close(sourceFd);
        qWarning() << "SetupOverlayMount: Could not anchor target parent:" << targetParentPath;
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not anchor target parent"));
        return false;
    }

    int mountResult = SecureFs::overlayMountFd(sourceFd, upperDir, workDir, parentFd, targetLeaf);
    ::close(sourceFd);
    if (mountResult != 0) {
        ::close(parentFd);
        qWarning() << "SetupOverlayMount: FD overlay mount failed on" << target << ":" << strerror(-mountResult);
        sendErrorReply(QDBusError::Failed, QStringLiteral("Failed to mount overlay filesystem"));
        return false;
    }
    info.targetParentFd = parentFd;
    info.targetLeaf = targetLeaf;

    m_activeMounts[username].append(info);

    saveState();

    return true;
}

bool CouchPlayHelper::unmountMountInfo(MountInfo &mount)
{
    // Prefer the FD-backed reference: /proc/self/fd/<pinned-parent>/leaf
    // cannot be raced by a player swapping ancestors of the target path.
    // The pinned FD is released only on success — a failed unmount (e.g.
    // persistent EBUSY) must keep the entry AND its FD, so later teardown,
    // restart reconciliation, or destruction can retry instead of orphaning
    // a root mount nobody can clean up anymore.
    if (mount.targetParentFd >= 0) {
        int result = SecureFs::umountAtFd(mount.targetParentFd, mount.targetLeaf);
        if (result != 0) {
            qWarning() << "unmountMountInfo: FD umount failed for" << mount.target << ":" << strerror(-result);
            return false;
        }
        ::close(mount.targetParentFd);
        mount.targetParentFd = -1;
        return true;
    }

    // No pinned FD (post-restart state): safely re-open the parent and use
    // the FD-anchored unmount. Never pass the restored pathname to umount(8).
    const qsizetype separator = mount.target.lastIndexOf(QLatin1Char('/'));
    if (separator < 0 || separator == mount.target.size() - 1) {
        qWarning() << "unmountMountInfo: invalid restored mount target" << mount.target;
        return false;
    }
    const QString parentPath = mount.target.left(separator);
    const QByteArray leaf = mount.target.mid(separator + 1).toLocal8Bit();
    const int parentFd = SecureFs::openExistingDirNoFollow(parentPath.isEmpty() ? QStringLiteral("/") : parentPath);
    if (parentFd < 0) {
        qWarning() << "unmountMountInfo: could not safely reopen parent for" << mount.target;
        return false;
    }
    const int result = SecureFs::umountAtFd(parentFd, QString::fromLocal8Bit(leaf));
    ::close(parentFd);
    if (result != 0) {
        qWarning() << "unmountMountInfo: restored FD umount failed for" << mount.target << ":"
                   << strerror(-result);
        return false;
    }
    return true;
}

int CouchPlayHelper::UnmountSharedDirectories(const QString &username)
{
    if (!s_validUsername.match(username).hasMatch()) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Invalid username format"));
        return 0;
    }

    if (!checkAuthorization(ACTION_MANAGE_MOUNTS)) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Not authorized to manage mounts"));
        return 0;
    }

    if (!m_activeMounts.contains(username)) {
        return 0;
    }

    int successCount = 0;
    QList<MountInfo> mounts = m_activeMounts.take(username);
    QList<MountInfo> remaining;

    for (int i = mounts.size() - 1; i >= 0; --i) {
        if (unmountMountInfo(mounts[i])) {
            successCount++;
        } else {
            remaining.prepend(mounts[i]);
        }
    }

    if (!remaining.isEmpty()) {
        m_activeMounts[username] = remaining;
    }
    saveState();
    return successCount;
}

int CouchPlayHelper::UnmountAllSharedDirectories()
{
    if (!checkAuthorization(ACTION_MANAGE_MOUNTS)) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Not authorized to manage mounts"));
        return 0;
    }

    int successCount = 0;
    QStringList users = m_activeMounts.keys();

    for (const QString &username : users) {
        QList<MountInfo> mounts = m_activeMounts.take(username);
        QList<MountInfo> remaining;

        for (int i = mounts.size() - 1; i >= 0; --i) {
            if (unmountMountInfo(mounts[i])) {
                successCount++;
            } else {
                remaining.prepend(mounts[i]);
            }
        }

        if (!remaining.isEmpty()) {
            m_activeMounts[username] = remaining;
        }
    }

    saveState();
    if (!m_activeMounts.isEmpty()) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not unmount all shared directories"));
        return 0;
    }

    return successCount;
}

bool CouchPlayHelper::CopyFileToUser(const QString &sourcePath, const QString &targetPath, const QString &username)
{
    if (!validateUserAndAuth(username, ACTION_MANAGE_MOUNTS)) {
        return false;
    }
    const uint sourceOwnerUid = callerUid();
    if (sourceOwnerUid == INVALID_CALLER_UID) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Could not resolve caller identity"));
        return false;
    }
    const QString sourceHome = getUserHomeByUid(sourceOwnerUid);
    const QString canonicalSource = m_ops->canonicalFilePath(sourcePath);
    const bool sourceInCallerHome = !sourceHome.isEmpty()
        && (sourcePath == sourceHome || sourcePath.startsWith(sourceHome + QLatin1Char('/')));
    const bool sourceInTemporaryStorage = sourcePath == QStringLiteral("/tmp")
        || sourcePath.startsWith(QStringLiteral("/tmp/"));
    if ((!sourceInCallerHome && !sourceInTemporaryStorage) || !isPathWithinAllowedPrefix(sourcePath)
        || canonicalSource.isEmpty() || !isPathWithinAllowedPrefix(canonicalSource)
        || m_ops->isSymLink(sourcePath)
        || (sourceInCallerHome && pathHasSymlinkComponents(sourcePath, sourceHome))) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Source file is outside the caller's shared roots"));
        return false;
    }
    struct stat sourceStat {};
    if (!m_ops->statPath(sourcePath, &sourceStat) || !S_ISREG(sourceStat.st_mode)
        || sourceStat.st_uid != sourceOwnerUid || !(sourceStat.st_mode & S_IRUSR)) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Source file is not owned by the caller"));
        return false;
    }

    if (!m_ops->fileExists(sourcePath)) {
        qWarning() << "CopyFileToUser: Source file does not exist:" << sourcePath;
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Source file does not exist: %1").arg(sourcePath));
        return false;
    }

    uint userUid = getUserUid(username);
    struct passwd *pw = m_ops->getpwuid(userUid);
    if (!pw) {
        qWarning() << "CopyFileToUser: Could not get user info for" << username;
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not get user info for '%1'").arg(username));
        return false;
    }

    int lastSlash = targetPath.lastIndexOf(QLatin1Char('/'));
    QString targetDir = (lastSlash >= 0) ? targetPath.left(lastSlash) : QStringLiteral(".");

    QStringList dirsToChown;
    if (!validateUserPath(targetDir, username, QStringLiteral("CopyFileToUser"), dirsToChown)) {
        return false;
    }

    if (!m_ops->createDirectorySecure(targetDir, userUid, pw->pw_gid)) {
        qWarning() << "CopyFileToUser: Failed to create directory:" << targetDir;
        sendErrorReply(QDBusError::Failed, QStringLiteral("Failed to create directory: %1").arg(targetDir));
        return false;
    }
    if (!m_ops->copyFileSecure(sourcePath, targetPath, sourceOwnerUid, userUid, pw->pw_gid)) {
        qWarning() << "CopyFileToUser: Failed to copy" << sourcePath << "to" << targetPath;
        sendErrorReply(QDBusError::Failed,
                       QStringLiteral("Failed to copy file from %1 to %2").arg(sourcePath, targetPath));
        return false;
    }
    return true;
}

// Compare the inode observed before a potentially racy directory operation.
static bool sameInodeIdentity(const struct stat &left, const struct stat &right)
{
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino && left.st_uid == right.st_uid
        && (left.st_mode & S_IFMT) == (right.st_mode & S_IFMT);
}

static bool sameFileIdentity(const struct stat &left, const struct stat &right)
{
    return sameInodeIdentity(left, right) && left.st_nlink == right.st_nlink;
}

// Recursively clean only a pinned prepared entry, never a swapped sibling.
static void removeEntryUnderIdentity(int parentFd, const char *name, const struct stat &expected)
{
    const int entryFd = ::openat(parentFd, name, O_PATH | O_NOFOLLOW | O_CLOEXEC);
    if (entryFd < 0) {
        return;
    }
    struct stat opened{};
    const bool pinned = ::fstat(entryFd, &opened) == 0 && sameFileIdentity(expected, opened);
    if (pinned && S_ISDIR(opened.st_mode)) {
        const int dirFd = ::openat(parentFd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (dirFd >= 0) {
            struct stat dirStat{};
            if (::fstat(dirFd, &dirStat) == 0 && sameInodeIdentity(opened, dirStat)) {
                SecureFs::removeTreeAt(dirFd);
            }
            ::close(dirFd);
        }
    }
    ::close(entryFd);
    if (!pinned) {
        return;
    }
    struct stat current{};
    if (::fstatat(parentFd, name, &current, AT_SYMLINK_NOFOLLOW) == 0
        && sameInodeIdentity(expected, current)) {
        (void)::unlinkat(parentFd, name, S_ISDIR(expected.st_mode) ? AT_REMOVEDIR : 0);
    }
}

bool CouchPlayHelper::CopyDirectoryToUser(const QString &username,
                                          const QString &sourceDir,
                                          const QString &targetRelativePath)
{
    if (!validateUserAndAuth(username, ACTION_MANAGE_MOUNTS)) {
        return false;
    }

    if (targetRelativePath.startsWith(QLatin1Char('/'))) {
        qWarning() << "CopyDirectoryToUser: targetRelativePath must be relative:" << targetRelativePath;
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("targetRelativePath must be relative"));
        return false;
    }

    if (pathHasDotDotComponent(targetRelativePath)) {
        qWarning() << "CopyDirectoryToUser: targetRelativePath contains '..':" << targetRelativePath;
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("targetRelativePath must not contain '..'"));
        return false;
    }

    if (!m_ops->fileExists(sourceDir)) {
        qWarning() << "CopyDirectoryToUser: Source directory does not exist:" << sourceDir;
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Source directory does not exist: %1").arg(sourceDir));
        return false;
    }

    if (!m_ops->isDirectory(sourceDir)) {
        qWarning() << "CopyDirectoryToUser: Source is not a directory:" << sourceDir;
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Source is not a directory: %1").arg(sourceDir));
        return false;
    }

    if (!isPathWithinAllowedPrefix(sourceDir)) {
        qWarning() << "CopyDirectoryToUser: Source path is outside allowed prefixes:" << sourceDir;
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Source path is outside allowed prefixes"));
        return false;
    }

    QString canonicalSource = m_ops->canonicalFilePath(sourceDir);
    if (!canonicalSource.isEmpty() && !isPathWithinAllowedPrefix(canonicalSource)) {
        qWarning() << "CopyDirectoryToUser: Source path resolves outside allowed prefixes:" << sourceDir << "->"
                   << canonicalSource;
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Source path resolves outside allowed prefixes"));
        return false;
    }

    QString userHome = getUserHome(username);
    if (userHome.isEmpty()) {
        sendErrorReply(QDBusError::Failed,
                       QStringLiteral("Could not determine home directory for user '%1'").arg(username));
        return false;
    }

    QString targetPath = userHome + QLatin1Char('/') + targetRelativePath;

    uint userUid = getUserUid(username);
    struct passwd *pw = m_ops->getpwuid(userUid);
    if (!pw) {
        qWarning() << "CopyDirectoryToUser: Could not get user info for" << username;
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not get user info for '%1'").arg(username));
        return false;
    }

    QStringList dirsToChown;
    if (!validateUserPath(targetPath, username, QStringLiteral("CopyDirectoryToUser"), dirsToChown)) {
        return false;
    }

    // Mutation phase is FD-anchored (openat O_NOFOLLOW below the home FD,
    // fchown on FDs): a player swapping an ancestor for a symlink between
    // validation and use cannot redirect root operations outside the home
    // Anchor the source by re-opening the freshly canonicalized path with a
    // whole-chain O_NOFOLLOW walk from "/": openBaseDir alone would protect
    // only the final component. If any ancestor changed into a symlink since
    // canonicalization, the walk fails closed (ELOOP).
    int srcFd = SecureFs::openExistingDirNoFollow(canonicalSource.isEmpty() ? sourceDir : canonicalSource);
    if (srcFd < 0) {
        qWarning() << "CopyDirectoryToUser: Could not open source directory:" << sourceDir;
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not open source directory"));
        return false;
    }

    int homeFd = SecureFs::openBaseDir(userHome);
    if (homeFd < 0) {
        ::close(srcFd);
        qWarning() << "CopyDirectoryToUser: Could not open home directory:" << userHome;
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not open user home directory"));
        return false;
    }

    const QString leafName = targetPath.mid(targetPath.lastIndexOf(QLatin1Char('/')) + 1);
    const QString parentRelative = QDir(userHome).relativeFilePath(targetPath.left(targetPath.lastIndexOf(QLatin1Char('/'))));
    const QStringList parentParts =
        parentRelative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    int parentFd = SecureFs::openDirBelow(homeFd, parentParts, true, userUid, pw->pw_gid);
    ::close(homeFd);
    if (parentFd < 0) {
        ::close(srcFd);
        qWarning() << "CopyDirectoryToUser: Failed to securely create parent directory:" << parentRelative;
        sendErrorReply(QDBusError::Failed, QStringLiteral("Failed to create parent directory"));
        return false;
    }

    // Build the replacement in a hidden sibling of the target and swap it in
    // with renames only after the copy fully succeeded: a mid-copy failure
    // (full disk, special file) must leave the player's previous data intact,
    // and a rename never nests into a stale destination nor follows a
    // player-planted symlink at the leaf
    const QByteArray leafNameUtf8 = leafName.toUtf8();
    const QString tempName = QStringLiteral(".%1.cptmp-%2")
                                 .arg(leafName, QString::number(QRandomGenerator::global()->generate(), 16));
    const QByteArray tempNameUtf8 = tempName.toUtf8();
    if (::mkdirat(parentFd, tempNameUtf8.constData(), 0755) != 0) {
        int err = errno;
        ::close(parentFd);
        ::close(srcFd);
        qWarning() << "CopyDirectoryToUser: Failed to create temporary copy directory:" << targetPath
                   << strerror(err);
        sendErrorReply(QDBusError::Failed, QStringLiteral("Failed to create temporary copy directory"));
        return false;
    }
    int dstFd = ::openat(parentFd, tempNameUtf8.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dstFd < 0) {
        int err = errno;
        // Preserve the sibling when it cannot be securely opened; its inode is unverified.
        ::close(parentFd);
        ::close(srcFd);
        qWarning() << "CopyDirectoryToUser: Could not open temporary copy directory:" << strerror(err);
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not open temporary copy directory"));
        return false;
    }
    ::fchown(dstFd, userUid, pw->pw_gid);

    int copyResult = SecureFs::copyTreeContents(srcFd, dstFd, userUid, pw->pw_gid);
    struct stat temporarySt{};
    const bool temporaryStatValid = ::fstat(dstFd, &temporarySt) == 0;
    ::close(dstFd);
    auto cleanupPreparedTemporary = [&]() {
        if (!temporaryStatValid) {
            return;
        }
        struct stat current{};
        if (::fstatat(parentFd, tempNameUtf8.constData(), &current, AT_SYMLINK_NOFOLLOW) == 0
            && sameFileIdentity(temporarySt, current)) {
            removeEntryUnderIdentity(parentFd, tempNameUtf8.constData(), temporarySt);
        }
    };
    if (copyResult == 0 && !temporaryStatValid) {
        copyResult = -EIO;
    }
    if (copyResult != 0) {
        // The player's previous tree is untouched — discard only the partial copy
        cleanupPreparedTemporary();
        ::close(parentFd);
        ::close(srcFd);
        qWarning() << "CopyDirectoryToUser: Failed to copy tree:" << strerror(-copyResult);
        sendErrorReply(QDBusError::Failed,
                       QStringLiteral("Failed to copy directory from %1 to %2").arg(sourceDir, targetPath));
        return false;
    }

    // Atomically exchange the prepared tree with the target. A no-replace
    // install is used when the target did not exist; an exchange is used for
    // an existing target so the previous inode remains available for an
    // identity-checked cleanup. On conflict, preserve the displaced entry.
    struct stat existingSt{};
    const int existingResult = ::fstatat(parentFd, leafNameUtf8.constData(), &existingSt, AT_SYMLINK_NOFOLLOW);
    if (existingResult != 0 && errno != ENOENT) {
        const int error = errno;
        cleanupPreparedTemporary();
        ::close(parentFd);
        ::close(srcFd);
        qWarning() << "CopyDirectoryToUser: Could not inspect target:" << targetPath << strerror(error);
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not inspect target directory"));
        return false;
    }
    const bool hadExisting = existingResult == 0;
    const unsigned int swapFlags = hadExisting ? RENAME_EXCHANGE : RENAME_NOREPLACE;
    const int swapResult = m_ops->renameAt(parentFd,
                                           tempNameUtf8.constData(),
                                           parentFd,
                                           leafNameUtf8.constData(),
                                           swapFlags);
    if (swapResult != 0) {
        const int error = errno;
        struct stat currentTemporary{};
        if (::fstatat(parentFd, tempNameUtf8.constData(), &currentTemporary, AT_SYMLINK_NOFOLLOW) == 0
            && sameFileIdentity(temporarySt, currentTemporary)) {
            cleanupPreparedTemporary();
        }
        ::close(parentFd);
        ::close(srcFd);
        qWarning() << "CopyDirectoryToUser: Failed to swap in copied directory:" << targetPath << strerror(error);
        sendErrorReply(QDBusError::Failed, QStringLiteral("Failed to replace target directory"));
        return false;
    }

    struct stat installedSt{};
    struct stat displacedSt{};
    const bool installed = ::fstatat(parentFd, leafNameUtf8.constData(), &installedSt, AT_SYMLINK_NOFOLLOW) == 0
        && sameFileIdentity(temporarySt, installedSt);
    const bool displaced = !hadExisting
        || (::fstatat(parentFd, tempNameUtf8.constData(), &displacedSt, AT_SYMLINK_NOFOLLOW) == 0
            && sameFileIdentity(existingSt, displacedSt));
    if (!installed || !displaced) {
        // A concurrent directory swap can leave an external entry under the
        // temporary name. Never exchange that unverified entry back over the
        // target; retain both names for explicit recovery.
        ::close(parentFd);
        ::close(srcFd);
        qWarning() << "CopyDirectoryToUser: Target changed during replacement:" << targetPath;
        sendErrorReply(QDBusError::Failed, QStringLiteral("Target directory changed during replacement"));
        return false;
    }

    if (hadExisting) {
        struct stat currentTemporary{};
        if (::fstatat(parentFd, tempNameUtf8.constData(), &currentTemporary, AT_SYMLINK_NOFOLLOW) != 0
            || !sameFileIdentity(existingSt, currentTemporary)) {
            ::close(parentFd);
            ::close(srcFd);
            sendErrorReply(QDBusError::Failed, QStringLiteral("Target directory changed during replacement"));
            return false;
        }
        removeEntryUnderIdentity(parentFd, tempNameUtf8.constData(), existingSt);
    }
    ::close(parentFd);
    ::close(srcFd);

    qDebug() << "CopyDirectoryToUser: Copied" << sourceDir << "to" << targetPath << "for user" << username;
    return true;
}

bool CouchPlayHelper::MirrorDirectoryContents(const QString &username,
                                              const QString &sourceDir,
                                              const QString &targetRelativePath)
{
    if (!validateUserAndAuth(username, ACTION_MANAGE_MOUNTS)) {
        return false;
    }

    if (targetRelativePath.startsWith(QLatin1Char('/'))) {
        qWarning() << "MirrorDirectoryContents: targetRelativePath must be relative:" << targetRelativePath;
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("targetRelativePath must be relative"));
        return false;
    }

    if (pathHasDotDotComponent(targetRelativePath)) {
        qWarning() << "MirrorDirectoryContents: targetRelativePath contains '..':" << targetRelativePath;
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("targetRelativePath must not contain '..'"));
        return false;
    }

    if (!m_ops->fileExists(sourceDir)) {
        qWarning() << "MirrorDirectoryContents: Source directory does not exist:" << sourceDir;
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Source directory does not exist: %1").arg(sourceDir));
        return false;
    }

    if (!m_ops->isDirectory(sourceDir)) {
        qWarning() << "MirrorDirectoryContents: Source is not a directory:" << sourceDir;
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Source is not a directory: %1").arg(sourceDir));
        return false;
    }

    if (!isPathWithinAllowedPrefix(sourceDir)) {
        qWarning() << "MirrorDirectoryContents: Source path is outside allowed prefixes:" << sourceDir;
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Source path is outside allowed prefixes"));
        return false;
    }

    QString canonicalSource = m_ops->canonicalFilePath(sourceDir);
    if (!canonicalSource.isEmpty() && !isPathWithinAllowedPrefix(canonicalSource)) {
        qWarning() << "MirrorDirectoryContents: Source path resolves outside allowed prefixes:" << sourceDir << "->"
                   << canonicalSource;
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Source path resolves outside allowed prefixes"));
        return false;
    }

    QString userHome = getUserHome(username);
    if (userHome.isEmpty()) {
        sendErrorReply(QDBusError::Failed,
                       QStringLiteral("Could not determine home directory for user '%1'").arg(username));
        return false;
    }

    QString targetPath = userHome + QLatin1Char('/') + targetRelativePath;

    if (!m_ops->fileExists(targetPath) || !m_ops->isDirectory(targetPath)) {
        qWarning() << "MirrorDirectoryContents: Target directory does not exist:" << targetPath;
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Target directory does not exist: %1").arg(targetPath));
        return false;
    }

    QStringList dirsToChown;
    if (!validateUserPath(targetPath, username, QStringLiteral("MirrorDirectoryContents"), dirsToChown)) {
        return false;
    }

    uint userUid = getUserUid(username);
    struct passwd *pw = m_ops->getpwuid(userUid);
    if (!pw) {
        qWarning() << "MirrorDirectoryContents: Could not get user info for" << username;
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not get user info for '%1'").arg(username));
        return false;
    }

    // Merge semantics: copy contents into the existing target ("src/.") with
    // FD-anchored no-follow operations — through an overlay mount the writes
    // land in the player's private upper layer, and a symlinked ancestor
    // swapped between validation and use cannot redirect the mutation
    // Same whole-chain no-follow anchoring as CopyDirectoryToUser
    int srcFd = SecureFs::openExistingDirNoFollow(canonicalSource.isEmpty() ? sourceDir : canonicalSource);
    if (srcFd < 0) {
        qWarning() << "MirrorDirectoryContents: Could not open source directory:" << sourceDir;
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not open source directory"));
        return false;
    }

    int homeFd = SecureFs::openBaseDir(userHome);
    if (homeFd < 0) {
        ::close(srcFd);
        qWarning() << "MirrorDirectoryContents: Could not open home directory:" << userHome;
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not open user home directory"));
        return false;
    }
    const QStringList targetParts =
        QDir(userHome).relativeFilePath(targetPath).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    int dstFd = SecureFs::openDirBelow(homeFd, targetParts, false, userUid, pw->pw_gid);
    ::close(homeFd);
    if (dstFd < 0) {
        ::close(srcFd);
        qWarning() << "MirrorDirectoryContents: Target directory missing or unsafe:" << targetPath;
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Target directory does not exist: %1").arg(targetPath));
        return false;
    }

    int copyResult = SecureFs::copyTreeContents(srcFd, dstFd, userUid, pw->pw_gid);
    ::close(srcFd);
    ::close(dstFd);
    if (copyResult != 0) {
        qWarning() << "MirrorDirectoryContents: Failed to merge" << sourceDir << "into" << targetPath
                   << strerror(-copyResult);
        sendErrorReply(QDBusError::Failed,
                       QStringLiteral("Failed to merge %1 into %2").arg(sourceDir, targetPath));
        return false;
    }

    qDebug() << "MirrorDirectoryContents: Merged" << sourceDir << "into" << targetPath << "for user" << username;
    return true;
}

bool CouchPlayHelper::WriteFileToUser(const QByteArray &content, const QString &targetPath, const QString &username)
{
    if (!validateUserAndAuth(username, ACTION_MANAGE_MOUNTS)) {
        return false;
    }

    const uint userUid = getUserUid(username);
    struct passwd *pw = m_ops->getpwuid(userUid);
    if (!pw) {
        qWarning() << "WriteFileToUser: Could not get user info for" << username;
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not get user info for '%1'").arg(username));
        return false;
    }
    const gid_t userGid = pw->pw_gid;
    const QString userHome = QString::fromLocal8Bit(pw->pw_dir);

    const int lastSlash = targetPath.lastIndexOf(QLatin1Char('/'));
    const QString targetDir = (lastSlash >= 0) ? targetPath.left(lastSlash) : QStringLiteral(".");
    QStringList unusedDirsToChown;
    if (!validateUserPath(targetDir, username, QStringLiteral("WriteFileToUser"), unusedDirsToChown)) {
        return false;
    }

    const QString canonicalHome = m_ops->canonicalFilePath(userHome);
    const QString safeHome = canonicalHome.isEmpty() ? userHome : canonicalHome;
    const QString relativeTarget = QDir(userHome).relativeFilePath(targetPath);
    QStringList targetParts = relativeTarget.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (relativeTarget.startsWith(QLatin1Char('/')) || targetParts.isEmpty()
        || targetParts.contains(QStringLiteral(".."))) {
        qWarning() << "WriteFileToUser: Invalid target path:" << targetPath;
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Invalid target path"));
        return false;
    }
    const QString leafName = targetParts.takeLast();
    if (leafName.isEmpty() || leafName == QStringLiteral(".") || leafName == QStringLiteral("..")) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Invalid target filename"));
        return false;
    }

    const int homeFd = SecureFs::openBaseDir(safeHome);
    if (homeFd < 0) {
        qWarning() << "WriteFileToUser: Could not securely open user home:" << safeHome;
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not open user home directory"));
        return false;
    }
    const int parentFd = SecureFs::openDirBelow(homeFd, targetParts, true, userUid, userGid);
    ::close(homeFd);
    if (parentFd < 0) {
        qWarning() << "WriteFileToUser: Failed to securely create target parent:" << targetDir;
        sendErrorReply(QDBusError::Failed, QStringLiteral("Failed to create target directory"));
        return false;
    }

    const int writeResult = SecureFs::writeFileAt(parentFd, leafName, content, userUid, userGid, 0644);
    ::close(parentFd);
    if (writeResult != 0) {
        qWarning() << "WriteFileToUser: Failed to write to" << targetPath << ":" << strerror(-writeResult);
        sendErrorReply(QDBusError::Failed, QStringLiteral("Failed to write to file"));
        return false;
    }

    return true;
}
QByteArray CouchPlayHelper::ReadSteamShortcutsForUser(const QString &username, const QString &steamId)
{
    if (!validateUserAndAuth(username, ACTION_MANAGE_MOUNTS)) {
        return {};
    }

    const uint userUid = getUserUid(username);
    struct passwd *pw = m_ops->getpwuid(userUid);
    if (!pw) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not get user info for '%1'").arg(username));
        return {};
    }
    const gid_t userGid = pw->pw_gid;
    const QString userHome = QString::fromLocal8Bit(pw->pw_dir);
    const QString steamRoot = GetUserSteamRoot(username);
    bool steamIdIsNumeric = !steamId.isEmpty() && steamId.size() <= 20;
    for (const QChar ch : steamId) {
        steamIdIsNumeric = steamIdIsNumeric && ch.unicode() >= '0' && ch.unicode() <= '9';
    }
    bool steamIdFits = false;
    steamId.toULongLong(&steamIdFits);
    if (steamRoot.isEmpty()) {
        return {};
    }
    if (!steamIdIsNumeric || !steamIdFits) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Invalid Steam account ID"));
        return {};
    }

    const QString targetPath = steamRoot + QStringLiteral("/userdata/") + steamId
        + QStringLiteral("/config/shortcuts.vdf");
    const QString relativeTarget = QDir(userHome).relativeFilePath(targetPath);
    QStringList targetParts = relativeTarget.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (relativeTarget.startsWith(QLatin1Char('/')) || targetParts.size() < 2
        || targetParts.contains(QStringLiteral(".."))) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Invalid Steam shortcuts path"));
        return {};
    }
    const QString leafName = targetParts.takeLast();
    const QString canonicalHome = m_ops->canonicalFilePath(userHome);
    const QString safeHome = canonicalHome.isEmpty() ? userHome : canonicalHome;
    const int homeFd = SecureFs::openBaseDir(safeHome);
    if (homeFd < 0) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not open user home directory"));
        return {};
    }
    const int parentFd = SecureFs::openDirBelow(homeFd, targetParts, false, userUid, userGid);
    ::close(homeFd);
    if (parentFd < 0) {
        if (parentFd == -ENOENT) {
            return {};
        }
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not securely open Steam shortcuts directory"));
        return {};
    }
    const QByteArray leafBytes = leafName.toLocal8Bit();
    const int fileFd = ::openat(parentFd,
                                 leafBytes.constData(),
                                 O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fileFd < 0) {
        if (errno == ENOENT) {
            ::close(parentFd);
            return {};
        }
        ::close(parentFd);
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not securely open Steam shortcuts file"));
        return {};
    }

    struct stat st;
    if (::fstat(fileFd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != userUid || st.st_nlink != 1
        || st.st_size <= 0 || st.st_size > 16 * 1024 * 1024) {
        ::close(fileFd);
        ::close(parentFd);
        sendErrorReply(QDBusError::Failed, QStringLiteral("Steam shortcuts file is unsafe, empty, or too large"));
        return {};
    }

    QByteArray content;
    content.reserve(static_cast<qsizetype>(st.st_size));
    char buffer[64 * 1024];
    while (true) {
        const ssize_t bytesRead = m_ops->read(fileFd, buffer, sizeof(buffer));
        if (bytesRead == 0) {
            break;
        }
        if (bytesRead < 0 && errno == EINTR) {
            continue;
        }
        if (bytesRead < 0 || content.size() + bytesRead > 16 * 1024 * 1024) {
            ::close(fileFd);
            ::close(parentFd);
            sendErrorReply(QDBusError::Failed, QStringLiteral("Could not read Steam shortcuts file"));
            return {};
        }
        content.append(buffer, static_cast<qsizetype>(bytesRead));
    }
    struct stat finalStat;
    struct stat pathnameAfter;
    const bool readChanged = ::fstat(fileFd, &finalStat) != 0
        || ::fstatat(parentFd, leafBytes.constData(), &pathnameAfter, AT_SYMLINK_NOFOLLOW) != 0
        || !sameFileIdentity(finalStat, pathnameAfter)
        || finalStat.st_size != st.st_size
        || finalStat.st_mtim.tv_sec != st.st_mtim.tv_sec || finalStat.st_mtim.tv_nsec != st.st_mtim.tv_nsec
        || finalStat.st_ctim.tv_sec != st.st_ctim.tv_sec || finalStat.st_ctim.tv_nsec != st.st_ctim.tv_nsec;
    if (readChanged || content.size() != st.st_size) {
        ::close(fileFd);
        ::close(parentFd);
        sendErrorReply(QDBusError::Failed, QStringLiteral("Steam shortcuts file changed while being read"));
        return {};
    }
    ::close(fileFd);
    ::close(parentFd);
    return content;
}

bool CouchPlayHelper::WriteSteamShortcutsForUser(const QString &username,
                                                 const QString &steamId,
                                                 const QByteArray &expectedDigest,
                                                 const QByteArray &content)
{
    constexpr qsizetype maxShortcutsSize = 16 * 1024 * 1024;
    if (!validateUserAndAuth(username, ACTION_MANAGE_MOUNTS)) {
        return false;
    }
    if (content.isEmpty() || content.size() > maxShortcutsSize
        || (expectedDigest != QByteArrayLiteral("missing") && expectedDigest.size() != 64)) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Invalid Steam shortcuts content or snapshot"));
        return false;
    }

    bool steamIdIsNumeric = !steamId.isEmpty() && steamId.size() <= 20;
    for (const QChar ch : steamId) {
        steamIdIsNumeric = steamIdIsNumeric && ch.unicode() >= '0' && ch.unicode() <= '9';
    }
    bool steamIdFits = false;
    steamId.toULongLong(&steamIdFits);
    if (!steamIdIsNumeric || !steamIdFits) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Invalid Steam account ID"));
        return false;
    }

    const uint userUid = getUserUid(username);
    struct passwd *pw = m_ops->getpwuid(userUid);
    if (!pw) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not get user info for '%1'").arg(username));
        return false;
    }
    const gid_t userGid = pw->pw_gid;
    const QString userHome = QString::fromLocal8Bit(pw->pw_dir);
    const QString steamRoot = GetUserSteamRoot(username);
    if (userHome.isEmpty() || steamRoot.isEmpty()) {
        return false;
    }

    const QString targetPath = steamRoot + QStringLiteral("/userdata/") + steamId
        + QStringLiteral("/config/shortcuts.vdf");
    const QString relativeTarget = QDir(userHome).relativeFilePath(targetPath);
    QStringList targetParts = relativeTarget.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (relativeTarget.startsWith(QLatin1Char('/')) || targetParts.size() < 2
        || targetParts.contains(QStringLiteral(".."))) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Invalid Steam shortcuts path"));
        return false;
    }
    const QString leafName = targetParts.takeLast();
    const QString canonicalHome = m_ops->canonicalFilePath(userHome);
    const QString safeHome = canonicalHome.isEmpty() ? userHome : canonicalHome;
    const int homeFd = SecureFs::openBaseDir(safeHome);
    if (homeFd < 0) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not open user home directory"));
        return false;
    }
    const int parentFd = SecureFs::openDirBelow(homeFd, targetParts, true, userUid, userGid);
    ::close(homeFd);
    if (parentFd < 0) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not securely open Steam shortcuts directory"));
        return false;
    }

    const QByteArray leafBytes = leafName.toLocal8Bit();
    auto readDigestAt = [&](const QByteArray &name, QByteArray &digest, const struct stat *expected = nullptr) {
        const int fileFd = ::openat(parentFd,
                                    name.constData(),
                                    O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        if (fileFd < 0) {
            if (errno == ENOENT) {
                if (expected) {
                    return false;
                }
                digest = QByteArrayLiteral("missing");
                return true;
            }
            return false;
        }

        struct stat before;
        if (::fstat(fileFd, &before) != 0 || !S_ISREG(before.st_mode) || before.st_uid != userUid
            || before.st_nlink != 1 || before.st_size <= 0 || before.st_size > maxShortcutsSize
            || (expected && !sameFileIdentity(*expected, before))) {
            ::close(fileFd);
            return false;
        }
        QByteArray current;
        current.reserve(static_cast<qsizetype>(before.st_size));
        char buffer[64 * 1024];
        while (true) {
            const ssize_t bytesRead = m_ops->read(fileFd, buffer, sizeof(buffer));
            if (bytesRead == 0) {
                break;
            }
            if (bytesRead < 0 && errno == EINTR) {
                continue;
            }
            if (bytesRead < 0 || current.size() + bytesRead > maxShortcutsSize) {
                ::close(fileFd);
                return false;
            }
            current.append(buffer, static_cast<qsizetype>(bytesRead));
        }
        struct stat after;
        struct stat pathnameAfter;
        const bool changedWhileReading = ::fstat(fileFd, &after) != 0
            || ::fstatat(parentFd, name.constData(), &pathnameAfter, AT_SYMLINK_NOFOLLOW) != 0
            || !sameFileIdentity(after, pathnameAfter)
            || (expected && !sameFileIdentity(*expected, after))
            || before.st_size != after.st_size
            || before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec
            || before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec;
        ::close(fileFd);
        if (changedWhileReading || current.size() != before.st_size) {
            return false;
        }
        digest = QCryptographicHash::hash(current, QCryptographicHash::Sha256).toHex();
        return true;
    };

    QByteArray currentDigest;
    if (!readDigestAt(leafBytes, currentDigest)) {
        ::close(parentFd);
        sendErrorReply(QDBusError::Failed, QStringLiteral("Steam shortcuts file is unsafe or changed while being read"));
        return false;
    }
    if (currentDigest != expectedDigest) {
        ::close(parentFd);
        sendErrorReply(QDBusError::Failed, QStringLiteral("Steam shortcuts file changed during sync"));
        return false;
    }

    QString temporaryName;
    QByteArray temporaryBytes;
    int temporaryFd = -1;
    for (int attempt = 0; attempt < 8 && temporaryFd < 0; ++attempt) {
        temporaryName = QStringLiteral(".shortcuts.vdf.couchplay-%1.tmp")
            .arg(QString::number(QRandomGenerator::global()->generate64(), 16));
        temporaryBytes = temporaryName.toLocal8Bit();
        temporaryFd = ::openat(parentFd,
                               temporaryBytes.constData(),
                               O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                               0600);
        if (temporaryFd < 0 && errno != EEXIST) {
            break;
        }
    }
    if (temporaryFd < 0) {
        ::close(parentFd);
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not create Steam shortcuts temporary file"));
        return false;
    }

    struct stat temporaryStat{};
    const bool temporaryStatValid = ::fstat(temporaryFd, &temporaryStat) == 0
        && S_ISREG(temporaryStat.st_mode) && temporaryStat.st_nlink == 1;
    auto cleanupTemporary = [&]() {
        if (!temporaryStatValid) {
            return;
        }
        struct stat current{};
        if (::fstatat(parentFd, temporaryBytes.constData(), &current, AT_SYMLINK_NOFOLLOW) == 0
            && current.st_dev == temporaryStat.st_dev && current.st_ino == temporaryStat.st_ino
            && (current.st_mode & S_IFMT) == (temporaryStat.st_mode & S_IFMT)) {
            (void)::unlinkat(parentFd, temporaryBytes.constData(), 0);
        }
    };
    bool writeSucceeded = temporaryStatValid && ::fchown(temporaryFd, userUid, userGid) == 0
        && ::fchmod(temporaryFd, 0644) == 0;
    qsizetype offset = 0;
    while (writeSucceeded && offset < content.size()) {
        const ssize_t bytesWritten = ::write(temporaryFd,
                                             content.constData() + offset,
                                             static_cast<size_t>(content.size() - offset));
        if (bytesWritten < 0 && errno == EINTR) {
            continue;
        }
        if (bytesWritten <= 0) {
            writeSucceeded = false;
            break;
        }
        offset += static_cast<qsizetype>(bytesWritten);
    }
    if (writeSucceeded && ::fsync(temporaryFd) != 0) {
        writeSucceeded = false;
    }
    if (writeSucceeded) {
        struct stat finalStat{};
        writeSucceeded = ::fstat(temporaryFd, &finalStat) == 0 && S_ISREG(finalStat.st_mode)
            && finalStat.st_nlink == 1;
        if (writeSucceeded) {
            temporaryStat = finalStat;
        }
    }
    if (::close(temporaryFd) != 0) {
        writeSucceeded = false;
    }
    if (!writeSucceeded) {
        cleanupTemporary();
        ::close(parentFd);
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not write Steam shortcuts temporary file"));
        return false;
    }

    QByteArray confirmedDigest;
    if (!readDigestAt(leafBytes, confirmedDigest) || confirmedDigest != expectedDigest) {
        cleanupTemporary();
        ::close(parentFd);
        sendErrorReply(QDBusError::Failed, QStringLiteral("Steam shortcuts file changed during sync"));
        return false;
    }
    struct stat expectedTargetStat{};
    const bool expectedTargetStatValid = expectedDigest == QByteArrayLiteral("missing")
        || (::fstatat(parentFd, leafBytes.constData(), &expectedTargetStat, AT_SYMLINK_NOFOLLOW) == 0
            && S_ISREG(expectedTargetStat.st_mode) && expectedTargetStat.st_nlink == 1);
    if (!expectedTargetStatValid) {
        cleanupTemporary();
        ::close(parentFd);
        sendErrorReply(QDBusError::Failed, QStringLiteral("Steam shortcuts file changed during sync"));
        return false;
    }
    QProcess *steamProcess = m_ops->createProcess();
    m_ops->startProcess(steamProcess, QStringLiteral("pgrep"), {
        QStringLiteral("-u"), QString::number(userUid), QStringLiteral("-f"),
        QStringLiteral("(^|/|[[:space:]])(steam|steamwebhelper|com[.]valvesoftware[.]Steam)([[:space:]]|$)")});
    const bool steamCheckFinished = m_ops->waitForFinished(steamProcess, 3000);
    const int steamCheckExitCode = steamCheckFinished ? m_ops->processExitCode(steamProcess) : -1;
    delete steamProcess;
    if (!steamCheckFinished || steamCheckExitCode != 1) {
        cleanupTemporary();
        ::close(parentFd);
        sendErrorReply(QDBusError::Failed,
                       QStringLiteral("Steam must be verifiably stopped before updating shortcuts"));
        return false;
    }
    const unsigned int replacementFlags = expectedDigest == QByteArrayLiteral("missing")
        ? RENAME_NOREPLACE
        : RENAME_EXCHANGE;
    if (m_ops->renameAt(parentFd,
                        temporaryBytes.constData(),
                        parentFd,
                        leafBytes.constData(),
                        replacementFlags) != 0) {
        const bool snapshotConflict = expectedDigest == QByteArrayLiteral("missing") && errno == EEXIST;
        cleanupTemporary();
        ::close(parentFd);
        sendErrorReply(QDBusError::Failed,
                       snapshotConflict ? QStringLiteral("Steam shortcuts file changed during sync")
                                        : QStringLiteral("Could not atomically replace Steam shortcuts file"));
        return false;
    }

    const QByteArray replacementDigest = QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex();
    QByteArray installedDigest;
    const bool installedFileIsExpected = readDigestAt(leafBytes, installedDigest, &temporaryStat)
        && installedDigest == replacementDigest;
    if (!installedFileIsExpected) {
        // The directory is writable by the target user. Do not report success
        // after the installed leaf has been swapped; retain the displaced
        // snapshot for explicit recovery.
        qWarning() << "WriteSteamShortcutsForUser: installed shortcuts leaf changed during sync; snapshot retained at"
                   << temporaryName;
        ::close(parentFd);
        sendErrorReply(QDBusError::Failed,
                       QStringLiteral("Steam shortcuts changed during sync; concurrent snapshot retained"));
        return false;
    }
    if (expectedDigest != QByteArrayLiteral("missing")) {
        QByteArray displacedDigest;
        struct stat displacedStat{};
        const bool displacedFileIsExpected = readDigestAt(temporaryBytes, displacedDigest)
            && displacedDigest == expectedDigest
            && ::fstatat(parentFd, temporaryBytes.constData(), &displacedStat, AT_SYMLINK_NOFOLLOW) == 0
            && sameFileIdentity(expectedTargetStat, displacedStat);
        if (!displacedFileIsExpected) {
            // The directory is writable by the target user. A second exchange
            // cannot safely restore an inode after either name changed, so
            // preserve both entries for explicit recovery.
            qWarning() << "WriteSteamShortcutsForUser: concurrent snapshot retained at" << temporaryName;
            ::close(parentFd);
            sendErrorReply(QDBusError::Failed,
                           QStringLiteral("Steam shortcuts changed during sync; concurrent snapshot retained"));
            return false;
        }
        if (::unlinkat(parentFd, temporaryBytes.constData(), 0) != 0) {
            qWarning() << "WriteSteamShortcutsForUser: Could not remove previous snapshot" << temporaryName;
        }
    }

    (void)::fsync(parentFd);
    ::close(parentFd);
    return true;
}

bool CouchPlayHelper::CreateUserDirectory(const QString &path, const QString &username)
{
    if (!validateUserAndAuth(username, ACTION_MANAGE_MOUNTS)) {
        return false;
    }

    uint userUid = getUserUid(username);
    struct passwd *pw = m_ops->getpwuid(userUid);
    if (!pw) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not get user info for '%1'").arg(username));
        return false;
    }

    QStringList dirsToChown;
    if (!validateUserPath(path, username, QStringLiteral("CreateUserDirectory"), dirsToChown)) {
        return false;
    }

    if (!m_ops->createDirectorySecure(path, userUid, pw->pw_gid)) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Failed to create directory: %1").arg(path));
        return false;
    }

    return true;
}

namespace
{

bool runSetfaclOnFd(SystemOps *ops, int objectFd, const QStringList &options, int timeoutMs, const QString &context)
{
    // QProcess does not provide a pathname that is anchored to an already
    // opened object. Duplicate the verified FD and make it available to the
    // child as fd 3; /proc/self/fd/3 then names that inode, even if a user
    // renames or replaces any pathname component concurrently.
    const int childFd = ::fcntl(objectFd, F_DUPFD_CLOEXEC, 10);
    if (childFd < 0) {
        qWarning() << "ACL:" << context << "failed to duplicate object FD:" << strerror(errno);
        return false;
    }

    QProcess *process = ops->createProcess();
    process->setChildProcessModifier([childFd]() {
        if (::dup2(childFd, 3) < 0) {
            ::_exit(127);
        }
    });

    QStringList args = options;
    args.append(QStringLiteral("/proc/self/fd/3"));
    ops->startProcess(process, QStringLiteral("setfacl"), args);

    const bool finished = ops->waitForFinished(process, timeoutMs);
    bool success = finished && ops->processExitCode(process) == 0;
    if (!finished) {
        qWarning() << "ACL:" << context << "setfacl timed out";
    } else if (!success) {
        qWarning() << "ACL:" << context << "setfacl failed:"
                   << QString::fromUtf8(ops->readStandardError(process));
    }

    ::close(childFd);
    delete process;
    return success;
}

int openAclEntry(int parentFd, const char *name, const struct stat &entryStat)
{
    int fd;
    if (S_ISDIR(entryStat.st_mode)) {
        fd = ::openat(parentFd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } else {
        // O_PATH avoids blocking on a raced FIFO and pins the exact object for
        // the ACL subprocess. Symlinks are rejected by the post-open fstat.
        fd = ::openat(parentFd, name, O_PATH | O_NOFOLLOW | O_CLOEXEC);
    }
    if (fd < 0) {
        return -errno;
    }

    struct stat openedStat;
    if (::fstat(fd, &openedStat) != 0) {
        const int error = errno;
        ::close(fd);
        return -error;
    }
    if (S_ISLNK(openedStat.st_mode)) {
        ::close(fd);
        return -ELOOP;
    }
    return fd;
}

bool applyAclTree(SystemOps *ops, int dirFd, const QString &username, const QString &context)
{
    if (!runSetfaclOnFd(ops, dirFd, {QStringLiteral("-m"), QStringLiteral("u:%1:rx").arg(username)}, 60000, context)) {
        return false;
    }

    const int scanFd = ::fcntl(dirFd, F_DUPFD_CLOEXEC, 10);
    if (scanFd < 0) {
        qWarning() << "ACL:" << context << "failed to duplicate directory FD:" << strerror(errno);
        return false;
    }
    DIR *directory = ::fdopendir(scanFd);
    if (!directory) {
        const int error = errno;
        ::close(scanFd);
        qWarning() << "ACL:" << context << "failed to enumerate directory:" << strerror(error);
        return false;
    }

    bool success = true;
    while (struct dirent *entry = ::readdir(directory)) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        struct stat entryStat;
        if (::fstatat(dirFd, entry->d_name, &entryStat, AT_SYMLINK_NOFOLLOW) != 0) {
            qWarning() << "ACL:" << context << "failed to inspect" << entry->d_name << ":" << strerror(errno);
            success = false;
            break;
        }
        if (S_ISLNK(entryStat.st_mode)) {
            // Never follow a link while recursively applying permissions.
            continue;
        }

        const int childFd = openAclEntry(dirFd, entry->d_name, entryStat);
        if (childFd < 0) {
            qWarning() << "ACL:" << context << "failed to open" << entry->d_name << ":" << strerror(-childFd);
            success = false;
            break;
        }

        struct stat openedStat;
        if (::fstat(childFd, &openedStat) != 0) {
            qWarning() << "ACL:" << context << "failed to stat" << entry->d_name << ":" << strerror(errno);
            ::close(childFd);
            success = false;
            break;
        }

        const QString childContext = context + QLatin1Char('/') + QString::fromLocal8Bit(entry->d_name);
        if (!runSetfaclOnFd(ops, childFd, {QStringLiteral("-m"), QStringLiteral("u:%1:rx").arg(username)}, 60000,
                             childContext)) {
            success = false;
        } else if (S_ISDIR(openedStat.st_mode) && !applyAclTree(ops, childFd, username, childContext)) {
            success = false;
        }
        ::close(childFd);
        if (!success) {
            break;
        }
    }

    ::closedir(directory);
    return success;
}

} // namespace

bool CouchPlayHelper::SetDirectoryAcl(const QString &path, const QString &username, bool recursive)
{
    if (!validateUserAndAuth(username, ACTION_MANAGE_MOUNTS)) {
        return false;
    }

    if (!isPathWithinAllowedPrefix(path)) {
        qWarning() << "SetDirectoryAcl: Path is outside allowed prefixes:" << path;
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Path is outside allowed prefixes"));
        return false;
    }

    if (!m_ops->fileExists(path)) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Path does not exist: %1").arg(path));
        return false;
    }

    const QString canonicalDir = m_ops->canonicalFilePath(path);
    if (!canonicalDir.isEmpty() && !isPathWithinAllowedPrefix(canonicalDir)) {
        qWarning() << "SetDirectoryAcl: Path resolves outside allowed prefixes:" << path << "->" << canonicalDir;
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Path resolves outside allowed prefixes"));
        return false;
    }
    const QString safePath = canonicalDir.isEmpty() ? path : canonicalDir;

    // Keep the ACL operation anchored to a no-follow FD. The canonical
    // pathname check above is only a fast rejection; it cannot close a
    // rename/symlink race before setfacl resolves a path.
    const int directoryFd = SecureFs::openExistingDirNoFollow(safePath);
    if (directoryFd < 0) {
        qWarning() << "SetDirectoryAcl: Refusing unsafe or non-directory path:" << path;
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Path is not a safe directory"));
        return false;
    }

    const QString context = QStringLiteral("SetDirectoryAcl ") + path;
    const bool success = recursive
        ? applyAclTree(m_ops, directoryFd, username, context)
        : runSetfaclOnFd(m_ops, directoryFd, {QStringLiteral("-m"), QStringLiteral("u:%1:rx").arg(username)}, 60000,
                         context);
    ::close(directoryFd);

    if (!success) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("setfacl failed for path: %1").arg(path));
    }
    return success;
}

bool CouchPlayHelper::SetPathAclWithParents(const QString &path, const QString &username)
{
    if (!validateUserAndAuth(username, ACTION_MANAGE_MOUNTS)) {
        return false;
    }

    if (!isPathWithinAllowedPrefix(path)) {
        qWarning() << "SetPathAclWithParents: Path is outside allowed prefixes:" << path;
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Path is outside allowed prefixes"));
        return false;
    }

    if (!m_ops->fileExists(path)) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Path does not exist: %1").arg(path));
        return false;
    }

    // Keep the ACL operation anchored to no-follow directory FDs. The
    // canonical check is retained as a fast rejection, but never substitutes
    // for pinning the objects that setfacl will modify.
    const QString canonicalPath = m_ops->canonicalFilePath(path);
    if (!canonicalPath.isEmpty() && !isPathWithinAllowedPrefix(canonicalPath)) {
        qWarning() << "SetPathAclWithParents: Path resolves outside allowed prefixes:" << path << "->"
                   << canonicalPath;
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Path resolves outside allowed prefixes"));
        return false;
    }

    // Stop traversing at well-known mount points we shouldn't modify
    static const QStringList stopBoundaries = {
        QStringLiteral("/run/media"),
        QStringLiteral("/media"),
        QStringLiteral("/mnt"),
        QStringLiteral("/home"),
        QStringLiteral("/var/home"), // Bazzite/Fedora Silverblue
        QStringLiteral("/"),
    };

    QStringList pathsToSet;
    QString current = canonicalPath.isEmpty() ? path : canonicalPath;

    while (current.endsWith(QLatin1Char('/')) && current.length() > 1) {
        current.chop(1);
    }

    pathsToSet.prepend(current);

    while (true) {
        int lastSlash = current.lastIndexOf(QLatin1Char('/'));
        if (lastSlash <= 0) {
            break;
        }

        current = current.left(lastSlash);
        if (current.isEmpty()) {
            current = QStringLiteral("/");
        }

        const bool atBoundary = stopBoundaries.contains(current);

        if (atBoundary) {
            break;
        }

        pathsToSet.prepend(current);
    }

    bool allSucceeded = true;
    for (const QString &p : pathsToSet) {
        if (!m_ops->fileExists(p)) {
            qWarning() << "SetPathAclWithParents: Path does not exist, skipping:" << p;
            continue;
        }

        const int directoryFd = SecureFs::openExistingDirNoFollow(p);
        if (directoryFd < 0) {
            qWarning() << "SetPathAclWithParents: Refusing unsafe or non-directory path:" << p;
            allSucceeded = false;
            continue;
        }

        // Remove a stale entry first, preserving the helper's existing
        // remove-before-set behavior. Both commands target the pinned inode.
        runSetfaclOnFd(m_ops, directoryFd, {QStringLiteral("-x"), QStringLiteral("u:%1").arg(username)}, 5000, p);
        if (!runSetfaclOnFd(m_ops,
                            directoryFd,
                            {QStringLiteral("-m"), QStringLiteral("u:%1:rx").arg(username)},
                            5000,
                            p)) {
            allSucceeded = false;
        }
        ::close(directoryFd);
    }

    return allSucceeded;
}


namespace
{
enum class SteamVdfToken {
    String,
    OpenBrace,
    CloseBrace,
    End,
    Invalid,
};

SteamVdfToken nextSteamVdfToken(const QByteArray &source, qsizetype &position, QByteArray &value)
{
    if (position == 0 && source.size() >= 3 && static_cast<unsigned char>(source.at(0)) == 0xef
        && static_cast<unsigned char>(source.at(1)) == 0xbb
        && static_cast<unsigned char>(source.at(2)) == 0xbf) {
        position = 3;
    }
    while (position < source.size()) {
        const char ch = source.at(position);
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            ++position;
            continue;
        }
        if (ch == '/' && position + 1 < source.size() && source.at(position + 1) == '/') {
            while (position < source.size() && source.at(position) != '\n') {
                ++position;
            }
            continue;
        }
        break;
    }

    if (position == source.size()) {
        return SteamVdfToken::End;
    }
    const char ch = source.at(position++);
    if (ch == '{') {
        return SteamVdfToken::OpenBrace;
    }
    if (ch == '}') {
        return SteamVdfToken::CloseBrace;
    }
    if (ch != '"') {
        return SteamVdfToken::Invalid;
    }

    value.clear();
    while (position < source.size()) {
        const char next = source.at(position++);
        if (next == '"') {
            return SteamVdfToken::String;
        }
        if (next == '\\') {
            if (position == source.size()) {
                return SteamVdfToken::Invalid;
            }
            value.append(source.at(position++));
        } else {
            value.append(next);
        }
    }
    return SteamVdfToken::Invalid;
}

enum class SteamAccountResolutionState {
    NoRecent,
    Unique,
    Ambiguous,
    Invalid,
};

struct SteamAccountResolution {
    SteamAccountResolutionState state;
    QString steamId;
};

SteamAccountResolution findMostRecentSteamAccount(const QByteArray &loginUsers)
{
    QStringList objectPath;
    QByteArray pendingKey;
    bool expectingValue = false;
    QString activeId;
    qsizetype position = 0;
    QByteArray tokenValue;

    while (true) {
        const SteamVdfToken token = nextSteamVdfToken(loginUsers, position, tokenValue);
        switch (token) {
        case SteamVdfToken::String:
            if (!expectingValue) {
                pendingKey = tokenValue;
                expectingValue = true;
                break;
            }
            if (objectPath.size() == 2 && objectPath.at(0) == QStringLiteral("users")
                && pendingKey == QByteArrayLiteral("MostRecent")) {
                if (tokenValue == QByteArrayLiteral("1")) {
                    if (!activeId.isEmpty()) {
                        return {SteamAccountResolutionState::Ambiguous, {}};
                    }
                    activeId = objectPath.at(1);
                } else if (tokenValue != QByteArrayLiteral("0")) {
                    return {SteamAccountResolutionState::Invalid, {}};
                }
            }
            pendingKey.clear();
            expectingValue = false;
            break;
        case SteamVdfToken::OpenBrace:
            if (!expectingValue) {
                return {SteamAccountResolutionState::Invalid, {}};
            }
            objectPath.append(QString::fromUtf8(pendingKey));
            pendingKey.clear();
            expectingValue = false;
            break;
        case SteamVdfToken::CloseBrace:
            if (expectingValue || objectPath.isEmpty()) {
                return {SteamAccountResolutionState::Invalid, {}};
            }
            objectPath.removeLast();
            break;
        case SteamVdfToken::End:
            if (expectingValue || !objectPath.isEmpty()) {
                return {SteamAccountResolutionState::Invalid, {}};
            }
            return {activeId.isEmpty() ? SteamAccountResolutionState::NoRecent : SteamAccountResolutionState::Unique,
                    activeId};
        case SteamVdfToken::Invalid:
            return {SteamAccountResolutionState::Invalid, {}};
        }
    }
}
}
QString CouchPlayHelper::GetUserSteamId(const QString &username)
{
    if (!s_validUsername.match(username).hasMatch()) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Invalid username format"));
        return {};
    }

    if (!userExists(username)) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("User '%1' does not exist").arg(username));
        return {};
    }

    const QString steamRoot = GetUserSteamRoot(username);
    if (steamRoot.isEmpty()) {
        return {};
    }

    const QString userdataPath = steamRoot + QStringLiteral("/userdata");
    QStringList steamIds;
    if (m_ops->fileExists(userdataPath)) {
        const QStringList entries = m_ops->entryList(userdataPath, {}, QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &entry : entries) {
            bool ok = false;
            entry.toULongLong(&ok);
            if (ok) {
                steamIds.append(entry);
            }
        }
    }
    const QString singleAccountFallback = steamIds.size() == 1 ? steamIds.constFirst() : QString();

    const QString userHome = getUserHome(username);
    if (userHome.isEmpty()) {
        return {};
    }
    const uint userUid = getUserUid(username);
    const QString loginUsersPath = steamRoot + QStringLiteral("/config/loginusers.vdf");
    const QString relativePath = QDir(userHome).relativeFilePath(loginUsersPath);
    QStringList pathParts = relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (relativePath.startsWith(QLatin1Char('/')) || pathParts.size() < 2
        || pathParts.contains(QStringLiteral(".."))) {
        return {};
    }
    const QString leafName = pathParts.takeLast();

    const QString canonicalHome = m_ops->canonicalFilePath(userHome);
    const QString safeHome = canonicalHome.isEmpty() ? userHome : canonicalHome;
    const int homeFd = SecureFs::openBaseDir(safeHome);
    if (homeFd < 0) {
        return {};
    }
    const int parentFd = SecureFs::openDirBelow(homeFd, pathParts, false, userUid, 0);
    ::close(homeFd);

    if (parentFd < 0) {
        return parentFd == -ENOENT ? singleAccountFallback : QString();
    }
    const QByteArray leafBytes = leafName.toLocal8Bit();
    const int fileFd = ::openat(parentFd,
                                leafBytes.constData(),
                                O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    const int openError = fileFd < 0 ? errno : 0;

    if (fileFd < 0) {
        ::close(parentFd);
        return openError == ENOENT ? singleAccountFallback : QString();
    }
    constexpr off_t maxLoginUsersSize = 1024 * 1024;
    struct stat before;
    if (::fstat(fileFd, &before) != 0 || !S_ISREG(before.st_mode) || before.st_uid != userUid
        || before.st_nlink != 1 || before.st_size < 0 || before.st_size > maxLoginUsersSize) {
        ::close(fileFd);
        ::close(parentFd);
        return {};
    }

    QByteArray contents;
    contents.reserve(static_cast<qsizetype>(before.st_size));
    char buffer[8192];
    while (true) {
        const ssize_t bytesRead = m_ops->read(fileFd, buffer, sizeof(buffer));
        if (bytesRead == 0) {
            break;
        }
        if (bytesRead < 0 && errno == EINTR) {
            continue;
        }
        if (bytesRead < 0 || contents.size() + bytesRead > maxLoginUsersSize) {
            ::close(fileFd);
            ::close(parentFd);
            return {};
        }
        contents.append(buffer, static_cast<qsizetype>(bytesRead));
    }

    struct stat after;
    struct stat pathnameAfter;
    const bool changedWhileReading = ::fstat(fileFd, &after) != 0
        || ::fstatat(parentFd, leafBytes.constData(), &pathnameAfter, AT_SYMLINK_NOFOLLOW) != 0
        || !sameFileIdentity(after, pathnameAfter)
        || before.st_size != after.st_size
        || before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec
        || before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec;
    ::close(fileFd);
    ::close(parentFd);
    if (changedWhileReading || contents.size() != before.st_size) {
        return {};
    }

    const SteamAccountResolution account = findMostRecentSteamAccount(contents);
    if (account.state == SteamAccountResolutionState::NoRecent) {
        return singleAccountFallback;
    }
    if (account.state != SteamAccountResolutionState::Unique) {
        return {};
    }
    const QString &activeId = account.steamId;
    bool allDigits = !activeId.isEmpty();
    for (const QChar ch : activeId) {
        if (ch.unicode() < '0' || ch.unicode() > '9') {
            allDigits = false;
            break;
        }
    }
    bool fitsInSteamId = false;
    activeId.toULongLong(&fitsInSteamId);
    return allDigits && fitsInSteamId ? activeId : QString();
}
bool CouchPlayHelper::IsSteamBootstrapped(const QString &username)
{
    if (!userExists(username)) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("User '%1' does not exist").arg(username));
        return false;
    }

    const QString steamRoot = GetUserSteamRoot(username);
    if (steamRoot.isEmpty()) {
        return false;
    }

    // Bootstrap complete when steam.sh or the ubuntu12_32 binary exists
    // (userdata alone is insufficient).
    return m_ops->fileExists(steamRoot + QStringLiteral("/steam.sh"))
        || m_ops->fileExists(steamRoot + QStringLiteral("/ubuntu12_32/steam"));
}

QVariantMap CouchPlayHelper::ReadSteamLibraryFoldersForUser(const QString &username)
{
    constexpr qsizetype maxSize = 4 * 1024 * 1024;
    if (!validateUserAndAuth(username, ACTION_MANAGE_MOUNTS)) {
        return {};
    }
    struct passwd *pw = m_ops->getpwnam(username.toLocal8Bit().constData());
    if (!pw) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not get user info for '%1'").arg(username));
        return {};
    }
    const QString home = QString::fromLocal8Bit(pw->pw_dir);
    const QString steamRoot = GetUserSteamRoot(username);
    if (home.isEmpty() || steamRoot.isEmpty()) {
        return {};
    }
    const QString relative = QDir(home).relativeFilePath(steamRoot + QStringLiteral("/config/libraryfolders.vdf"));
    QStringList parts = relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (relative.startsWith(QLatin1Char('/')) || parts.size() < 2 || parts.contains(QStringLiteral(".."))) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Invalid Steam library folders path"));
        return {};
    }
    const QString leaf = parts.takeLast();
    const QString canonicalHome = m_ops->canonicalFilePath(home);
    const int homeFd = SecureFs::openBaseDir(canonicalHome.isEmpty() ? home : canonicalHome);
    if (homeFd < 0) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not securely open user home"));
        return {};
    }
    const int parentFd = SecureFs::openDirBelow(homeFd, parts, false, pw->pw_uid, pw->pw_gid);
    ::close(homeFd);
    if (parentFd < 0) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not securely open Steam config directory"));
        return {};
    }
    const QByteArray leafBytes = leaf.toLocal8Bit();
    const int fd = ::openat(parentFd, leafBytes.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0 && errno == ENOENT) {
        ::close(parentFd);
        return {{QStringLiteral("exists"), false}, {QStringLiteral("content"), QByteArray()}};
    }
    if (fd < 0) {
        ::close(parentFd);
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not securely read Steam library folders"));
        return {};
    }
    struct stat before;
    if (::fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) || before.st_uid != pw->pw_uid
        || before.st_nlink != 1 || before.st_size < 0 || before.st_size > maxSize) {
        ::close(fd);
        ::close(parentFd);
        sendErrorReply(QDBusError::Failed, QStringLiteral("Steam library folders file is unsafe"));
        return {};
    }
    QByteArray content;
    content.reserve(static_cast<qsizetype>(before.st_size));
    char buffer[64 * 1024];
    while (true) {
        const ssize_t count = m_ops->read(fd, buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 || content.size() + count > maxSize) {
            ::close(fd); ::close(parentFd);
            sendErrorReply(QDBusError::Failed, QStringLiteral("Steam library folders file could not be read safely"));
            return {};
        }
        content.append(buffer, static_cast<qsizetype>(count));
    }
    struct stat after;
    struct stat pathnameAfter;
    const bool changed = ::fstat(fd, &after) != 0 || ::fstatat(parentFd, leafBytes.constData(), &pathnameAfter, AT_SYMLINK_NOFOLLOW) != 0
        || !S_ISREG(after.st_mode) || !S_ISREG(pathnameAfter.st_mode)
        || after.st_uid != pw->pw_uid || pathnameAfter.st_uid != pw->pw_uid
        || after.st_nlink != 1 || pathnameAfter.st_nlink != 1
        || after.st_dev != pathnameAfter.st_dev || after.st_ino != pathnameAfter.st_ino
        || before.st_size != after.st_size
        || before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec
        || before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec;
    ::close(fd); ::close(parentFd);
    if (changed || content.size() != before.st_size) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Steam library folders changed while being read"));
        return {};
    }
    return {{QStringLiteral("exists"), true}, {QStringLiteral("content"), content}};
}

bool CouchPlayHelper::RestoreSteamLibraryFoldersForUser(const QString &username, bool existed, const QByteArray &content)
{
    constexpr qsizetype maxSize = 4 * 1024 * 1024;
    if (!validateUserAndAuth(username, ACTION_MANAGE_MOUNTS) || (existed && content.size() > maxSize)) {
        return false;
    }
    struct passwd *pw = m_ops->getpwnam(username.toLocal8Bit().constData());
    if (!pw) return false;
    const QString home = QString::fromLocal8Bit(pw->pw_dir);
    const QString steamRoot = GetUserSteamRoot(username);
    if (home.isEmpty() || steamRoot.isEmpty()) return false;
    const QString relative = QDir(home).relativeFilePath(steamRoot + QStringLiteral("/config/libraryfolders.vdf"));
    QStringList parts = relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (relative.startsWith(QLatin1Char('/')) || parts.size() < 2 || parts.contains(QStringLiteral(".."))) return false;
    const QString leaf = parts.takeLast();
    const QString canonicalHome = m_ops->canonicalFilePath(home);
    const int homeFd = SecureFs::openBaseDir(canonicalHome.isEmpty() ? home : canonicalHome);
    if (homeFd < 0) return false;
    const int parentFd = SecureFs::openDirBelow(homeFd, parts, false, pw->pw_uid, pw->pw_gid);
    ::close(homeFd);
    if (parentFd < 0) return false;
    const QByteArray leafBytes = leaf.toLocal8Bit();
    bool restored = false;
    if (existed) {
        restored = SecureFs::writeFileAt(parentFd, leaf, content, pw->pw_uid, pw->pw_gid) == 0;
    } else {
        struct stat st;
        if (::fstatat(parentFd, leafBytes.constData(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
            restored = errno == ENOENT;
        } else if (S_ISREG(st.st_mode) && st.st_uid == pw->pw_uid && st.st_nlink == 1) {
            QByteArray displacedName;
            bool displaced = false;
            for (int attempt = 0; attempt < 8 && !displaced; ++attempt) {
                displacedName = QStringLiteral(".%1.couchplay-restore-%2")
                                    .arg(leaf, QString::number(QRandomGenerator::global()->generate64(), 16))
                                    .toLocal8Bit();
                if (m_ops->renameAt(parentFd,
                                    leafBytes.constData(),
                                    parentFd,
                                    displacedName.constData(),
                                    RENAME_NOREPLACE) == 0) {
                    displaced = true;
                } else if (errno != EEXIST) {
                    restored = errno == ENOENT;
                    break;
                }
            }
            if (displaced) {
                struct stat displacedStat;
                const bool movedExpected = ::fstatat(parentFd,
                                                      displacedName.constData(),
                                                      &displacedStat,
                                                      AT_SYMLINK_NOFOLLOW) == 0
                    && sameFileIdentity(st, displacedStat);
                if (!movedExpected) {
                    // The pathname changed before the atomic move. Restore
                    // only with no-replace semantics; never overwrite a
                    // concurrent target, and retain the displaced inode if
                    // another writer won the name in the meantime.
                    (void)m_ops->renameAt(parentFd,
                                          displacedName.constData(),
                                          parentFd,
                                          leafBytes.constData(),
                                          RENAME_NOREPLACE);
                    restored = false;
                } else {
                    struct stat beforeUnlink;
                    restored = ::fstatat(parentFd,
                                         displacedName.constData(),
                                         &beforeUnlink,
                                         AT_SYMLINK_NOFOLLOW) == 0
                        && sameFileIdentity(st, beforeUnlink)
                        && ::unlinkat(parentFd, displacedName.constData(), 0) == 0;
                }
            } else if (!restored) {
                restored = false;
            }
        }

    }
    ::close(parentFd);
    if (!restored) sendErrorReply(QDBusError::Failed, QStringLiteral("Could not restore Steam library folders"));
    return restored;
}

QString CouchPlayHelper::findGamescopePath()
{
    QString gamescopePath = QStringLiteral("/usr/bin/gamescope");
    if (!m_ops->fileExists(gamescopePath)) {
        QProcess *whichProc = m_ops->createProcess();
        m_ops->startProcess(whichProc, QStringLiteral("which"), {QStringLiteral("gamescope")});
        m_ops->waitForFinished(whichProc, 3000);
        if (m_ops->processExitCode(whichProc) == 0) {
            QString resolved = QString::fromLocal8Bit(m_ops->readAllStandardOutput(whichProc)).trimmed();
            if (!resolved.isEmpty()) {
                gamescopePath = resolved;
            }
        }
        delete whichProc;
    }
    return gamescopePath;
}

QString CouchPlayHelper::CreateVirtualOutput(const QString &username, int width, int height, int refreshRate)
{
    if (!validateUserAndAuth(username, ACTION_MANAGE_VIRTUAL_DISPLAY)) {
        return QString();
    }

    if (width <= 0 || height <= 0 || refreshRate <= 0) {
        sendErrorReply(QDBusError::InvalidArgs,
            QStringLiteral("Invalid resolution (%1x%2) or refresh rate (%3)")
                .arg(width).arg(height).arg(refreshRate));
        return QString();
    }

    struct passwd *pwd = m_ops->getpwnam(username.toLocal8Bit().constData());
    if (!pwd) {
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("Failed to resolve UID for user '%1'").arg(username));
        return QString();
    }

    uint userUid = pwd->pw_uid;
    QString userRuntimeDir = QStringLiteral("/run/user/%1").arg(userUid);

    if (!m_ops->fileExists(userRuntimeDir)) {
        m_ops->mkpath(userRuntimeDir);
        m_ops->chown(userRuntimeDir, userUid, pwd->pw_gid);
    }

    QStringList existingSockets = m_ops->entryList(userRuntimeDir,
        {QStringLiteral("wayland-*")}, QDir::Files);

    QString gamescopePath = findGamescopePath();
    if (gamescopePath.isEmpty() || !m_ops->fileExists(gamescopePath)) {
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("Gamescope not found on system"));
        return QString();
    }

    QString serviceName = QStringLiteral("couchplay-vdisplay-%1.service").arg(username);

    QStringList systemdArgs;
    systemdArgs << QStringLiteral("--unit") << serviceName;
    systemdArgs << QStringLiteral("--uid") << username;
    systemdArgs << QStringLiteral("--property=Type=simple");
    systemdArgs << QStringLiteral("-E")
                << QStringLiteral("XDG_RUNTIME_DIR=/run/user/%1").arg(userUid);
    systemdArgs << QStringLiteral("--");
    systemdArgs << gamescopePath
                << QStringLiteral("-W") << QString::number(width)
                << QStringLiteral("-H") << QString::number(height)
                << QStringLiteral("-r") << QString::number(refreshRate)
                << QStringLiteral("--") << QStringLiteral("sleep") << QStringLiteral("infinity");

    QProcess *proc = m_ops->createProcess();
    m_ops->startProcess(proc, QStringLiteral("systemd-run"), systemdArgs);
    m_ops->waitForFinished(proc, 10000);
    int exitCode = m_ops->processExitCode(proc);
    if (exitCode != 0) {
        QByteArray errOutput = m_ops->readStandardError(proc);
        delete proc;

        if (errOutput.contains("already loaded")) {
            stopServiceInstance(serviceName);
            QThread::msleep(200);

            proc = m_ops->createProcess();
            m_ops->startProcess(proc, QStringLiteral("systemd-run"), systemdArgs);
            m_ops->waitForFinished(proc, 10000);
            exitCode = m_ops->processExitCode(proc);
            if (exitCode != 0) {
                qWarning() << "CreateVirtualOutput: retry failed for" << serviceName;
                delete proc;
                sendErrorReply(QDBusError::Failed,
                    QStringLiteral("Failed to launch virtual display (stale unit)"));
                return QString();
            }
            delete proc;
        } else {
            qWarning() << "CreateVirtualOutput: systemd-run failed:" << errOutput;
            sendErrorReply(QDBusError::Failed,
                QStringLiteral("Failed to launch virtual display"));
            return QString();
        }
    } else {
        delete proc;
    }

    qint64 mainPid = 0;
    for (int attempt = 0; attempt < 5; ++attempt) {
        QThread::msleep(500);
        QProcess *showProc = m_ops->createProcess();
        m_ops->startProcess(showProc, QStringLiteral("systemctl"),
            {QStringLiteral("show"), serviceName,
             QStringLiteral("-p"), QStringLiteral("MainPID"),
             QStringLiteral("--value")});
        m_ops->waitForFinished(showProc, 5000);
        QByteArray output = m_ops->readAllStandardOutput(showProc).trimmed();
        delete showProc;

        bool ok = false;
        mainPid = output.toLongLong(&ok);
        if (ok && mainPid > 0) break;
    }

    if (mainPid <= 0) {
        qWarning() << "CreateVirtualOutput: Could not get MainPID for" << serviceName;
        stopServiceInstance(serviceName);
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("Virtual display process started but PID not retrievable"));
        return QString();
    }

    // Poll for a genuinely NEW Wayland socket (one not present before gamescope
    // was launched). Do NOT fall back to a pre-existing socket: in a mixed
    // session the streaming user already has wayland-0 (their own session), and
    // returning that would make Sunshine capture the wrong compositor.
    QString socketName;
    for (int attempt = 0; attempt < 6 && socketName.isEmpty(); ++attempt) {
        if (attempt > 0) {
            QThread::msleep(500);
        }
        const QStringList sockets = m_ops->entryList(userRuntimeDir,
            {QStringLiteral("wayland-*")}, QDir::Files);
        for (const QString &s : sockets) {
            if (!existingSockets.contains(s)) {
                socketName = s;
                break;
            }
        }
    }

    if (socketName.isEmpty()) {
        qWarning() << "CreateVirtualOutput: no new Wayland socket appeared for" << serviceName;
        stopServiceInstance(serviceName);
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("Virtual display started but no Wayland socket was created"));
        return QString();
    }

    VirtualDisplayInfo info;
    info.pid = mainPid;
    info.displayContext = QStringLiteral("couchplay-display-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    info.ownerUid = callerUid();
    info.runtimeUid = getUserUid(username);
    info.targetUsername = username;
    info.waylandSocket = socketName;
    info.serviceName = serviceName;
    m_virtualDisplays[username] = info;
    saveState();

    qInfo() << "Created virtual display for" << username
            << "socket:" << socketName << "PID:" << mainPid;

    return info.displayContext;
}

bool CouchPlayHelper::DestroyVirtualOutput(const QString &username, const QString &waylandSocketName)
{
    if (!validateUserAndAuth(username, ACTION_MANAGE_VIRTUAL_DISPLAY)) {
        return false;
    }

    if (!m_virtualDisplays.contains(username)) {
        sendErrorReply(QDBusError::InvalidArgs,
            QStringLiteral("No virtual display found for user '%1'").arg(username));
        return false;
    }

    VirtualDisplayInfo info = m_virtualDisplays[username];
    if (info.ownerUid != callerUid()) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Not authorized for display context"));
        return false;
    }

    if (!waylandSocketName.isEmpty() && info.displayContext != waylandSocketName) {
        sendErrorReply(QDBusError::InvalidArgs,
            QStringLiteral("Socket name mismatch: expected '%1', got '%2'")
                .arg(info.displayContext, waylandSocketName));
        return false;
    }

    if (!info.serviceName.isEmpty()) {
        stopServiceInstance(info.serviceName);
    } else if (info.pid > 0) {
        m_ops->killProcess(static_cast<pid_t>(info.pid), SIGTERM);
    }

    m_virtualDisplays.remove(username);
    saveState();

    qInfo() << "Destroyed virtual display for" << username;
    return true;
}

QString CouchPlayHelper::CreateNullSink(const QString &username, const QString &sinkName)
{
    if (!validateUserAndAuth(username, ACTION_MANAGE_AUDIO_SINK)) {
        return QString();
    }

    static QRegularExpression validSinkName(QStringLiteral("^[a-zA-Z][a-zA-Z0-9_-]{0,63}$"));
    if (!validSinkName.match(sinkName).hasMatch()) {
        sendErrorReply(QDBusError::InvalidArgs,
            QStringLiteral("Invalid sink name format: '%1'").arg(sinkName));
        return QString();
    }

    QProcess *proc = m_ops->createProcess();
    m_ops->startProcess(proc, QStringLiteral("machinectl"),
        {QStringLiteral("shell"), username + QStringLiteral("@"),
         QStringLiteral("/bin/bash"), QStringLiteral("-c"),
         QStringLiteral("pactl load-module module-null-sink sink_name=%1 "
                        "sink_properties=device.description=\\\"CouchPlay %1\\\"")
             .arg(sinkName)});
    m_ops->waitForFinished(proc, 10000);

    if (m_ops->processExitCode(proc) != 0) {
        QString errOutput = QString::fromLocal8Bit(m_ops->readStandardError(proc));
        qWarning() << "CreateNullSink: pactl failed:" << errOutput;
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("Failed to create null-sink: %1").arg(errOutput));
        delete proc;
        return QString();
    }

    QString output = QString::fromLocal8Bit(m_ops->readAllStandardOutput(proc)).trimmed();
    delete proc;

    static QRegularExpression moduleRegex(QStringLiteral("Module #(\\d+)"));
    auto match = moduleRegex.match(output);
    int moduleIndex = 0;
    if (match.hasMatch()) {
        moduleIndex = match.captured(1).toInt();
    } else {
        bool ok = false;
        moduleIndex = output.toInt(&ok);
        if (!ok || moduleIndex <= 0) {
            qWarning() << "CreateNullSink: Could not parse module index from:" << output;
            sendErrorReply(QDBusError::Failed,
                QStringLiteral("Null-sink created but module index not retrievable"));
            return QString();
        }
    }

    NullSinkInfo info;
    info.moduleIndex = moduleIndex;
    info.sinkName = sinkName;
    m_nullSinks[username].append(info);
    saveState();

    qInfo() << "Created null-sink" << sinkName << "module #" << moduleIndex << "for" << username;
    return sinkName;
}

// Re-resolve the CURRENT PipeWire module index for sinkName and unload it.
// PipeWire reuses module indexes across session restarts, so a saved index can
// point at an unrelated module -- unloading it would break that user's audio.
// Returns true if unloaded or already absent; false only on an unload failure.
bool CouchPlayHelper::unloadNullSinkModule(const QString &username, const QString &sinkName)
{
    QProcess *listProc = m_ops->createProcess();
    m_ops->startProcess(listProc, QStringLiteral("machinectl"),
        {QStringLiteral("shell"), username + QStringLiteral("@"),
         QStringLiteral("/bin/bash"), QStringLiteral("-c"),
         QStringLiteral("pactl list modules short | grep 'module-null-sink.*sink_name=%1' | awk '{print $1}'").arg(sinkName)});
    m_ops->waitForFinished(listProc, 5000);
    const QString out = QString::fromLocal8Bit(m_ops->readAllStandardOutput(listProc)).trimmed();
    delete listProc;

    bool ok = false;
    const int moduleIndex = out.split(QLatin1Char('\n')).first().toInt(&ok);
    if (!ok || moduleIndex <= 0) {
        qInfo() << "unloadNullSinkModule: sink" << sinkName << "not active for" << username << "(already removed?)";
        return true; // absent == success (idempotent destroy)
    }

    QProcess *proc = m_ops->createProcess();
    m_ops->startProcess(proc, QStringLiteral("machinectl"),
        {QStringLiteral("shell"), username + QStringLiteral("@"),
         QStringLiteral("/bin/bash"), QStringLiteral("-c"),
         QStringLiteral("pactl unload-module %1").arg(moduleIndex)});
    m_ops->waitForFinished(proc, 10000);
    const bool unloaded = (m_ops->processExitCode(proc) == 0);
    delete proc;

    if (!unloaded) {
        qWarning() << "unloadNullSinkModule: failed to unload module" << moduleIndex << "for sink" << sinkName;
    }
    return unloaded;
}

bool CouchPlayHelper::DestroyNullSink(const QString &username, const QString &sinkName)
{
    if (!validateUserAndAuth(username, ACTION_MANAGE_AUDIO_SINK)) {
        return false;
    }
    static QRegularExpression validSinkName(QStringLiteral("^[a-zA-Z][a-zA-Z0-9_-]{0,63}$"));
    if (!validSinkName.match(sinkName).hasMatch()) {
        sendErrorReply(QDBusError::InvalidArgs,
            QStringLiteral("Invalid sink name format: '%1'").arg(sinkName));
        return false;
    }

    // Drop our bookkeeping for this sink.
    if (m_nullSinks.contains(username)) {
        for (int i = 0; i < m_nullSinks[username].size(); ++i) {
            if (m_nullSinks[username][i].sinkName == sinkName) {
                m_nullSinks[username].removeAt(i);
                saveState();
                break;
            }
        }
    }

    // Re-resolve the current module by name and unload it (a saved index can go
    // stale across a session restart and unload the wrong module).
    if (!unloadNullSinkModule(username, sinkName)) {
        sendErrorReply(QDBusError::Failed,
            QStringLiteral("Failed to unload null-sink '%1'").arg(sinkName));
        return false;
    }

    qInfo() << "Destroyed null-sink" << sinkName << "for" << username;
    return true;
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

    QJsonArray hidDevicesArray;
    for (const QString &hidDevice : m_modifiedHidDevices) {
        hidDevicesArray.append(hidDevice);
    }
    root[QStringLiteral("modifiedHidDevices")] = hidDevicesArray;

    QJsonObject mountsObject;
    for (auto it = m_activeMounts.constBegin(); it != m_activeMounts.constEnd(); ++it) {
        QJsonArray mountsArray;
        for (const MountInfo &info : it.value()) {
            QJsonObject mountObj;
            mountObj[QStringLiteral("source")] = info.source;
            mountObj[QStringLiteral("target")] = info.target;
            if (!info.mountType.isEmpty()) {
                mountObj[QStringLiteral("mountType")] = info.mountType;
            }
            if (!info.upperDir.isEmpty()) {
                mountObj[QStringLiteral("upperDir")] = info.upperDir;
            }
            if (!info.workDir.isEmpty()) {
                mountObj[QStringLiteral("workDir")] = info.workDir;
            }
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
    QJsonObject virtualDisplaysObject;
    for (auto it = m_virtualDisplays.constBegin(); it != m_virtualDisplays.constEnd(); ++it) {
        QJsonObject display;
        display[QStringLiteral("pid")] = it.value().pid;
        display[QStringLiteral("context")] = it.value().displayContext;
        display[QStringLiteral("ownerUid")] = static_cast<qint64>(it.value().ownerUid);
        display[QStringLiteral("runtimeUid")] = static_cast<qint64>(it.value().runtimeUid);
        display[QStringLiteral("targetUsername")] = it.value().targetUsername;
        display[QStringLiteral("waylandSocket")] = it.value().waylandSocket;
        display[QStringLiteral("serviceName")] = it.value().serviceName;
        virtualDisplaysObject[it.key()] = display;
    }
    root[QStringLiteral("virtualDisplays")] = virtualDisplaysObject;

    QJsonObject pidObject;
    for (auto it = m_pidToUsername.constBegin(); it != m_pidToUsername.constEnd(); ++it) {
        pidObject[QString::number(it.key())] = it.value();
    }
    root[QStringLiteral("pidToUsername")] = pidObject;
    QJsonObject ownerObject;
    for (auto it = m_pidOwnerUid.constBegin(); it != m_pidOwnerUid.constEnd(); ++it) {
        ownerObject[QString::number(it.key())] = static_cast<qint64>(it.value());
    }
    root[QStringLiteral("pidOwnerUid")] = ownerObject;

    QJsonArray runtimeUids;
    for (uint uid : m_runtimeAccessSetForUid) {
        runtimeUids.append(static_cast<qint64>(uid));
    }
    root[QStringLiteral("runtimeAccessUids")] = runtimeUids;

    QJsonObject compositorUidObject;
    for (auto it = m_compositorUidForUsername.constBegin(); it != m_compositorUidForUsername.constEnd(); ++it) {
        compositorUidObject[it.key()] = static_cast<qint64>(it.value());
    }
    root[QStringLiteral("compositorUidForUsername")] = compositorUidObject;

    QJsonObject nullSinksObject;
    for (auto it = m_nullSinks.constBegin(); it != m_nullSinks.constEnd(); ++it) {
        QJsonArray sinksArray;
        for (const NullSinkInfo &sink : it.value()) {
            QJsonObject sinkObj;
            sinkObj[QStringLiteral("moduleIndex")] = sink.moduleIndex;
            sinkObj[QStringLiteral("sinkName")] = sink.sinkName;
            sinksArray.append(sinkObj);
        }
        nullSinksObject[it.key()] = sinksArray;
    }
    root[QStringLiteral("nullSinks")] = nullSinksObject;

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

CouchPlayHelper::HidDeviceInfo CouchPlayHelper::findHidDevice(const QString &devicePath) const
{
    QFileInfo info(devicePath);
    QString name = info.fileName(); // e.g. event12 or hidraw2
    QString sysfsPath;

    if (name.startsWith(QStringLiteral("event"))) {
        sysfsPath = QStringLiteral("/sys/class/input/%1/device").arg(name);
        sysfsPath = QFileInfo(sysfsPath).canonicalFilePath();
        if (!sysfsPath.isEmpty()) {
            sysfsPath = QFileInfo(sysfsPath + QStringLiteral("/..")).canonicalFilePath();
        }
    } else if (name.startsWith(QStringLiteral("hidraw"))) {
        sysfsPath = QStringLiteral("/sys/class/hidraw/%1/device").arg(name);
        sysfsPath = QFileInfo(sysfsPath).canonicalFilePath();
    }

    if (sysfsPath.isEmpty()) {
        return {};
    }

    HidDeviceInfo dev;

    // Check if it has a driver symlink
    QString driverLink = sysfsPath + QStringLiteral("/driver");
    QFileInfo driverInfo(driverLink);
    if (driverInfo.exists() && driverInfo.isSymLink()) {
        dev.driverPath = driverInfo.canonicalFilePath();
        dev.deviceId = QFileInfo(sysfsPath).fileName();
    } else {
        return {};
    }

    return dev;
}

/**
 * Scan /sys/class/hidraw/ to find the current /dev/hidrawN node for a known HID device ID.
 * Used after driver rebind, when the kernel may assign a different hidraw number.
 *
 * @param deviceId  The stable HID device ID (e.g. "0003:28DE:1304.0001")
 * @return  "/dev/hidrawN" on success, or empty string if not found
 */
QString CouchPlayHelper::findHidrawPathForDeviceId(const QString &deviceId) const
{
    QDir hidrawClass(QStringLiteral("/sys/class/hidraw"));
    const QStringList entries = hidrawClass.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &entry : entries) {
        // /sys/class/hidraw/hidrawN/device is a symlink -> .../hid/DEVID
        QString sysfsDevice = QStringLiteral("/sys/class/hidraw/%1/device").arg(entry);
        QString resolved = QFileInfo(sysfsDevice).canonicalFilePath();
        if (resolved.isEmpty()) {
            continue;
        }
        // The last path component of the resolved sysfs path is the device ID
        QString foundId = QFileInfo(resolved).fileName();
        if (foundId == deviceId) {
            return QStringLiteral("/dev/%1").arg(entry);
        }
    }
    return {};
}

QString CouchPlayHelper::getTempUdevRulePath(const QString &deviceId) const
{
    QString safeDeviceId = deviceId;
    safeDeviceId.replace(QLatin1Char(':'), QLatin1Char('-'))
                .replace(QLatin1Char('.'), QLatin1Char('-'))
                .replace(QLatin1Char('/'), QLatin1Char('-'));
    return QStringLiteral("/etc/udev/rules.d/99-couchplay-temp-%1.rules").arg(safeDeviceId);
}

bool CouchPlayHelper::writeTempUdevRule(const HidDeviceInfo &hid, const QString &username)
{
    if (!s_validUsername.match(username).hasMatch()) {
        qWarning() << "CouchPlayHelper: Refusing udev rule for invalid username" << username;
        return false;
    }
    QString devId = hid.deviceId;
    QString rulePath = getTempUdevRulePath(devId);

    // Construct udev rules to assign matching input and hidraw devices to the target user.
    // Also strip the seat/uaccess tags to prevent systemd-logind from adding active seat user ACLs.
    QByteArray content;
    content.append(QStringLiteral("SUBSYSTEM==\"input\", KERNELS==\"%1\", OWNER=\"%2\", MODE=\"0600\", TAG-=\"uaccess\", TAG-=\"seat\", ENV{ID_SEAT}=\"seat-couchplay\", ENV{ID_FOR_SEAT}=\"seat-couchplay\", ENV{ID_AUTOSEAT}=\"0\"\n").arg(devId, username).toLocal8Bit());
    content.append(QStringLiteral("SUBSYSTEM==\"hidraw\", KERNELS==\"%1\", OWNER=\"%2\", MODE=\"0600\", TAG-=\"uaccess\", TAG-=\"seat\", ENV{ID_SEAT}=\"seat-couchplay\", ENV{ID_FOR_SEAT}=\"seat-couchplay\", ENV{ID_AUTOSEAT}=\"0\"\n").arg(devId, username).toLocal8Bit());

    qDebug() << "CouchPlayHelper: Writing temporary udev rule for device" << devId << "at" << rulePath;

    if (!m_ops->writeFile(rulePath, content)) {
        qWarning() << "CouchPlayHelper: Failed to write temporary udev rule to" << rulePath;
        return false;
    }

    return true;
}

bool CouchPlayHelper::removeTempUdevRule(const QString &deviceId)
{
    QString rulePath = getTempUdevRulePath(deviceId);
    if (m_ops->fileExists(rulePath)) {
        qDebug() << "CouchPlayHelper: Removing temporary udev rule at" << rulePath;
        if (!m_ops->removeFile(rulePath)) {
            qWarning() << "CouchPlayHelper: Failed to remove temporary udev rule at" << rulePath;
            return false;
        }
    }
    return true;
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
        qWarning() << "loadAndReconcileState: Failed to parse state file:" << parseError.errorString()
                   << "- starting fresh";
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
        proc.start(QStringLiteral("systemctl"),
                   {QStringLiteral("list-units"),
                    QStringLiteral("couchplay-*.service"),
                    QStringLiteral("--no-legend"),
                    QStringLiteral("--no-pager")});
        proc.waitForFinished(5000);
        QByteArray output = proc.readAllStandardOutput();
        for (const QByteArray &line : output.split('\n')) {
            QByteArray trimmed = line.trimmed();
            if (trimmed.isEmpty())
                continue;
            int spacePos = trimmed.indexOf(' ');
            if (spacePos > 0) {
                activeSystemdUnits.insert(QString::fromUtf8(trimmed.left(spacePos)));
            }
        }
    }
    m_virtualDisplays.clear();
    const QJsonObject virtualDisplaysObject = root.value(QStringLiteral("virtualDisplays")).toObject();
    for (auto it = virtualDisplaysObject.constBegin(); it != virtualDisplaysObject.constEnd(); ++it) {
        const QJsonObject display = it.value().toObject();
        const QString serviceName = display.value(QStringLiteral("serviceName")).toString();
        const QString targetUsername = display.value(QStringLiteral("targetUsername")).toString();
        const bool active = !serviceName.isEmpty() && activeSystemdUnits.contains(serviceName);
        const bool complete = !targetUsername.isEmpty() && display.contains(QStringLiteral("context"))
            && display.contains(QStringLiteral("ownerUid")) && display.contains(QStringLiteral("runtimeUid"))
            && !display.value(QStringLiteral("context")).toString().isEmpty()
            && !display.value(QStringLiteral("waylandSocket")).toString().isEmpty();
        if (!active) {
            changed = true;
            continue;
        }
        if (!complete) {
            m_stoppingUnits.insert(serviceName);
            const bool stopped = stopServiceInstance(serviceName);
            m_stoppingUnits.remove(serviceName);
            if (!stopped) {
                VirtualDisplayInfo legacy;
                legacy.pid = display.value(QStringLiteral("pid")).toInteger();
                legacy.waylandSocket = display.value(QStringLiteral("waylandSocket")).toString();
                legacy.serviceName = serviceName;
                m_virtualDisplays[it.key()] = legacy;
                continue;
            }
            changed = true;
            continue;
        }
        VirtualDisplayInfo info;
        info.pid = display.value(QStringLiteral("pid")).toInteger();
        info.displayContext = display.value(QStringLiteral("context")).toString();
        info.ownerUid = static_cast<uint>(display.value(QStringLiteral("ownerUid")).toInteger());
        info.runtimeUid = static_cast<uint>(display.value(QStringLiteral("runtimeUid")).toInteger());
        info.targetUsername = targetUsername;
        info.waylandSocket = display.value(QStringLiteral("waylandSocket")).toString();
        info.serviceName = serviceName;
        if (info.displayContext.isEmpty() || !display.contains(QStringLiteral("ownerUid"))
            || !display.contains(QStringLiteral("runtimeUid")) || info.waylandSocket.isEmpty()) {
            changed = true;
            continue;
        }
        m_virtualDisplays[it.key()] = info;
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
        if (!ok) {
            changed = true;
            continue;
        }
        QString username = it.value().toString();
        if (activeUsernames.contains(username)) {
            loadedPidToUsername[pid] = username;
        } else {
            qDebug() << "loadAndReconcileState: Removing stale PID" << pid << "for user" << username;
            changed = true;
        }
    }
    m_pidToUsername = loadedPidToUsername;
    m_pidOwnerUid.clear();
    const QJsonObject ownerObject = root.value(QStringLiteral("pidOwnerUid")).toObject();
    for (auto it = m_pidToUsername.constBegin(); it != m_pidToUsername.constEnd();) {
        if (ownerObject.contains(QString::number(it.key()))) {
            m_pidOwnerUid[it.key()] = static_cast<uint>(ownerObject.value(QString::number(it.key())).toInteger());
            ++it;
        } else {
            const QString ownerlessUsername = it.value();
            const QString ownerlessService = m_usernameToUnitName.value(ownerlessUsername);
            bool stopped = true;
            if (!ownerlessService.isEmpty()) {
                m_stoppingUnits.insert(ownerlessService);
                stopped = stopServiceInstance(ownerlessService);
                m_stoppingUnits.remove(ownerlessService);
            }
            if (!stopped) {
                ++it;
                continue;
            }
            m_usernameToUnitName.remove(ownerlessUsername);
            activeUsernames.remove(ownerlessUsername);
            it = m_pidToUsername.erase(it);
            changed = true;
        }
    }

    for (auto it = m_usernameToUnitName.constBegin(); it != m_usernameToUnitName.constEnd(); ++it) {
        const qint64 pid = m_pidToUsername.key(it.key(), 0);
        if (pid > 0) {
            monitorUnitState(it.value(), it.key(), pid);
        }
    }
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
    for (const QString &device : m_modifiedDevices) {
        startWatchingDevice(device);
    }

    QJsonArray hidDevicesArray = root.value(QStringLiteral("modifiedHidDevices")).toArray();
    QStringList loadedHidDevices;
    for (const QJsonValue &val : hidDevicesArray) {
        loadedHidDevices.append(val.toString());
    }
    m_modifiedHidDevices = loadedHidDevices;

    QJsonObject mountsObject = root.value(QStringLiteral("activeMounts")).toObject();
    QMap<QString, QList<MountInfo>> loadedMounts;
    QByteArray mountsData;
    QFile mountsFile(QStringLiteral("/proc/mounts"));
    if (mountsFile.open(QIODevice::ReadOnly)) {
        mountsData = mountsFile.readAll();
    }
    const auto decodeProcMountField = [](const QByteArray &encoded) {
        QByteArray decoded;
        decoded.reserve(encoded.size());
        for (qsizetype i = 0; i < encoded.size(); ++i) {
            if (encoded.at(i) == '\\' && i + 3 < encoded.size()) {
                int value = 0;
                bool octal = true;
                for (qsizetype j = 1; j <= 3; ++j) {
                    const char digit = encoded.at(i + j);
                    if (digit < '0' || digit > '7') {
                        octal = false;
                        break;
                    }
                    value = value * 8 + (digit - '0');
                }
                if (octal) {
                    decoded.append(static_cast<char>(value));
                    i += 3;
                    continue;
                }
            }
            decoded.append(encoded.at(i));
        }
        return decoded;
    };
    const auto isMountedTarget = [&](const QString &target) {
        const QByteArray targetBytes = target.toUtf8();
        for (const QByteArray &line : mountsData.split('\n')) {
            const QList<QByteArray> fields = line.simplified().split(' ');
            if (fields.size() >= 2 && decodeProcMountField(fields.at(1)) == targetBytes) {
                return true;
            }
        }
        return false;
    };

    for (auto it = mountsObject.constBegin(); it != mountsObject.constEnd(); ++it) {
        const QString username = it.key();
        const QJsonArray mountsArray = it.value().toArray();
        QList<MountInfo> userMounts;
        for (const QJsonValue &mountVal : mountsArray) {
            const QJsonObject mountObj = mountVal.toObject();
            const QString target = mountObj.value(QStringLiteral("target")).toString();
            const bool isMounted = isMountedTarget(target);

            MountInfo info;
            info.source = mountObj.value(QStringLiteral("source")).toString();
            info.target = target;
            info.mountType = mountObj.value(QStringLiteral("mountType")).toString();
            info.upperDir = mountObj.value(QStringLiteral("upperDir")).toString();
            info.workDir = mountObj.value(QStringLiteral("workDir")).toString();

            if (!activeUsernames.contains(username)) {
                // Session gone: safely unmount an orphan. If reopening the
                // parent or the FD-anchored unmount fails, retain the record
                // for a later retry instead of resolving the live pathname.
                bool cleaned = !isMounted;
                if (isMounted) {
                    const qsizetype separator = target.lastIndexOf(QLatin1Char('/'));
                    const QString parentPath = separator > 0 ? target.left(separator) : QStringLiteral("/");
                    const QString leaf = separator >= 0 ? target.mid(separator + 1) : QString();
                    const int parentFd = leaf.isEmpty() ? -1 : SecureFs::openExistingDirNoFollow(parentPath);
                    if (parentFd >= 0) {
                        const int result = SecureFs::umountAtFd(parentFd, leaf);
                        ::close(parentFd);
                        cleaned = result == 0;
                        if (!cleaned) {
                            qWarning() << "loadAndReconcileState: orphan FD umount failed for" << target << ":"
                                       << strerror(-result);
                        }
                    } else {
                        qWarning() << "loadAndReconcileState: could not safely reopen orphan mount parent for"
                                   << target;
                    }
                }
                if (cleaned) {
                    changed = true;
                } else {
                    userMounts.append(info);
                }
            } else if (isMounted) {
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
        uint uid = static_cast<uint>(val.toInteger());
        QString waylandSocket = QStringLiteral("/run/user/%1/wayland-0").arg(uid);
        if (m_ops->fileExists(waylandSocket)) {
            QProcess getfaclProc;
            getfaclProc.start(QStringLiteral("getfacl"), {QStringLiteral("-q"), QStringLiteral("-c"), waylandSocket});
            getfaclProc.waitForFinished(3000);
            QString aclOutput = QString::fromLocal8Bit(getfaclProc.readAllStandardOutput());
            if (!aclOutput.contains(QStringLiteral("group:%1:").arg(COUCHPLAY_GROUP))) {
                qDebug() << "loadAndReconcileState: Runtime ACLs missing on" << waylandSocket
                         << "- will re-apply on next launch";
                changed = true;
                continue;
            }
        }
        loadedRuntimeUids.insert(uid);
    }
    m_runtimeAccessSetForUid = loadedRuntimeUids;

    QJsonObject compositorUidObject = root.value(QStringLiteral("compositorUidForUsername")).toObject();
    QHash<QString, uint> loadedCompositorUid;
    for (auto it = compositorUidObject.constBegin(); it != compositorUidObject.constEnd(); ++it) {
        QString user = it.key();
        if (m_usernameToUnitName.contains(user)) {
            loadedCompositorUid[user] = static_cast<uint>(it.value().toInteger());
        }
    }
    m_compositorUidForUsername = loadedCompositorUid;
    // Restore null sinks - restore unconditionally (PipeWire modules are per-user-session;
    // if the session is gone, the module is gone too, but tracking allows destructor cleanup)
    QJsonObject nullSinksObject = root.value(QStringLiteral("nullSinks")).toObject();
    for (auto it = nullSinksObject.constBegin(); it != nullSinksObject.constEnd(); ++it) {
        QJsonArray sinksArray = it.value().toArray();
        QList<NullSinkInfo> sinks;
        for (const QJsonValue &sinkVal : sinksArray) {
            QJsonObject sinkObj = sinkVal.toObject();
            NullSinkInfo info;
            info.moduleIndex = sinkObj.value(QStringLiteral("moduleIndex")).toInt();
            info.sinkName = sinkObj.value(QStringLiteral("sinkName")).toString();
            sinks.append(info);
        }
        if (!sinks.isEmpty()) {
            m_nullSinks[it.key()] = sinks;
        }
    }

    if (changed) {
        saveState();
    }

    qDebug() << "loadAndReconcileState: Restored" << m_usernameToUnitName.size() << "units," << m_modifiedDevices.size()
             << "devices," << m_activeMounts.size() << "mount users," << m_runtimeAccessSetForUid.size()
             << "runtime UIDs";
}

void CouchPlayHelper::startWatchingDevice(const QString &devicePath)
{
    const bool isHidraw = devicePath.startsWith(QLatin1String("/dev/hidraw"));
    const bool isEvent = devicePath.startsWith(QLatin1String("/dev/input/event"));

    if (!isHidraw && !isEvent) {
        return;
    }

    if (m_watchedDevices.contains(devicePath)) {
        return;
    }

    int fd = ::open(devicePath.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        qWarning() << "CouchPlayHelper: Failed to open device for chord monitoring:" << devicePath << strerror(errno);
        return;
    }

    auto *watcher = new WatchedDevice();
    watcher->fd = fd;
    watcher->isHidrawDevice = isHidraw;
    if (isHidraw) {
        QFileInfo info(devicePath);
        QString name = info.fileName();
        QString sysfsPath = QStringLiteral("/sys/class/hidraw/%1/device").arg(name);
        QString resolved = QFileInfo(sysfsPath).canonicalFilePath();
        if (resolved.contains(QStringLiteral("28de"), Qt::CaseInsensitive)) {
            watcher->isSteamController = true;

            // Find parent USB directory containing "idVendor"
            QString usbParent = resolved;
            while (!usbParent.isEmpty() && usbParent != QStringLiteral("/")) {
                if (QFileInfo(usbParent + QStringLiteral("/idVendor")).exists()) {
                    break;
                }
                usbParent = QFileInfo(usbParent).dir().absolutePath();
            }

            if (!usbParent.isEmpty() && usbParent != QStringLiteral("/")) {
                qWarning() << "CouchPlayHelper: Identified Steam Controller parent device on" << devicePath
                           << "(USB Parent:" << usbParent << ")";
                // Find all sibling hidraw devices under the same USB parent
                QDir hidrawDir(QStringLiteral("/sys/class/hidraw"));
                QStringList entries = hidrawDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
                for (const QString &entry : entries) {
                    QString siblingDevicePath = QStringLiteral("/dev/") + entry;
                    if (!m_watchedDevices.contains(siblingDevicePath) && siblingDevicePath != devicePath) {
                        QString siblingSysfs = QStringLiteral("/sys/class/hidraw/%1/device").arg(entry);
                        QString siblingResolved = QFileInfo(siblingSysfs).canonicalFilePath();
                        if (siblingResolved.startsWith(usbParent)) {
                            qWarning() << "CouchPlayHelper: Discovered Steam Controller sibling node:" << siblingDevicePath
                                       << "from root node" << devicePath;
                            // Watch this sibling recursively next event loop tick
                            QMetaObject::invokeMethod(this, [this, siblingDevicePath]() {
                                startWatchingDevice(siblingDevicePath);
                            }, Qt::QueuedConnection);
                        }
                    }
                }
            }
        }
    }
    watcher->notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    watcher->chordTimer = new QTimer(this);
    watcher->chordTimer->setSingleShot(true);
    watcher->chordTimer->setInterval(2000); // 2.0 seconds hold duration (matches README)

    connect(watcher->notifier, &QSocketNotifier::activated, this, [this, devicePath]() {
        onDeviceDataAvailable(devicePath);
    });

    connect(watcher->chordTimer, &QTimer::timeout, this, [this, devicePath]() {
        onExitChordTimeout(devicePath);
    });

    m_watchedDevices.insert(devicePath, watcher);
    qDebug() << "CouchPlayHelper: Started monitoring" << (isHidraw ? "hidraw" : "evdev") << "device for exit chord:" << devicePath;
}

void CouchPlayHelper::stopWatchingDevice(const QString &devicePath)
{
    auto *watcher = m_watchedDevices.take(devicePath);
    if (!watcher) {
        return;
    }

    watcher->notifier->setEnabled(false);
    delete watcher->notifier;
    watcher->chordTimer->stop();
    delete watcher->chordTimer;
    ::close(watcher->fd);
    delete watcher;

    qDebug() << "CouchPlayHelper: Stopped monitoring input device:" << devicePath;
}

void CouchPlayHelper::stopWatchingAllDevices()
{
    QStringList paths = m_watchedDevices.keys();
    for (const QString &path : paths) {
        stopWatchingDevice(path);
    }
}

void CouchPlayHelper::onDeviceDataAvailable(const QString &devicePath)
{
    auto *watcher = m_watchedDevices.value(devicePath, nullptr);
    if (!watcher) {
        return;
    }

    bool deviceError = false;

    if (watcher->isHidrawDevice) {
        // Raw HID packet path — Steam Controller puck sends 54-byte reports.
        // Kernel hid-steam.c byte layout (0-indexed from the hidraw read buffer):
        //   byte 9, bit 4 (0x10) = BTN_SELECT (menu left)
        //   byte 9, bit 6 (0x40) = BTN_START  (menu right)
        // Bytes 0–3 are the packet counter; bytes 4–8 are ABXY / shoulder / trigger;
        // bytes 10+ are trackpad, triggers (analog), and IMU data.
        static constexpr uint8_t STEAM_SELECT_MASK = 0x10; // bit 4 of byte 9
        static constexpr uint8_t STEAM_START_MASK  = 0x40; // bit 6 of byte 9

        uint8_t buf[64];
        int bytesRead;

        while (true) {
            bytesRead = ::read(watcher->fd, buf, sizeof(buf));
            if (bytesRead > 0) {
                // If it is a Steam Controller, we only parse specific valid state report sizes and headers
                // to filter out status, battery, or gyro/sensor updates that might contain garbage in buf[9].
                bool isValidSCReport = false;
                uint8_t b9 = 0;

                if (watcher->isSteamController) {
                    // Log the first time we see any packet size on a Steam Controller device,
                    // and output the first 16 bytes.
                    if (!watcher->seenSizes.contains(bytesRead)) {
                        watcher->seenSizes.insert(bytesRead);
                        QString hexDump;
                        for (int i = 0; i < qMin(bytesRead, 16); ++i) {
                            hexDump += QStringLiteral("%1 ").arg(buf[i], 2, 16, QLatin1Char('0'));
                        }
                        qWarning() << "CouchPlayHelper: First time seeing packet size" << bytesRead
                                   << "on" << devicePath << "header:" << hexDump.trimmed();
                    }

                    // Debug logging for ALL Steam Controller reports to help diagnose
                    // report length and structure variations under active Steam sessions.
                    if (bytesRead >= 10) {
                        if (buf[8] != watcher->lastB8 || buf[9] != watcher->lastB9) {
                            watcher->lastB8 = buf[8];
                            watcher->lastB9 = buf[9];
                            qWarning() << "CouchPlayHelper: SC raw buttons changed on" << devicePath
                                       << "len:" << bytesRead
                                       << "buf[0..2]:"
                                       << QString::number(buf[0], 16)
                                       << QString::number(buf[1], 16)
                                       << QString::number(buf[2], 16)
                                       << "b8:" << QString::number(buf[8], 16)
                                       << "b9:" << QString::number(buf[9], 16);
                        }
                    } else {
                        qWarning() << "CouchPlayHelper: SC short packet on" << devicePath << "len:" << bytesRead;
                    }

                    // Wireless puck state report (len=54, ID=0x42)
                    if (bytesRead == 54 && buf[0] == 0x42) {
                        b9 = buf[9];
                        isValidSCReport = true;
                    }
                    // Wired state report (len=64, ID=0x01, type=0x01)
                    else if (bytesRead == 64 && buf[0] == 0x01 && buf[1] == 0x00 && buf[2] == 0x01) {
                        b9 = buf[9];
                        isValidSCReport = true;
                    }
                    // Wired Steam Deck state report (len=64, ID=0x01, type=0x09)
                    else if (bytesRead == 64 && buf[0] == 0x01 && buf[1] == 0x00 && buf[2] == 0x09) {
                        b9 = buf[9];
                        isValidSCReport = true;
                    }
                }

                if (isValidSCReport) {
                    const bool newStart  = (b9 & STEAM_START_MASK)  != 0;
                    const bool newSelect = (b9 & STEAM_SELECT_MASK) != 0;

                    bool stateChanged = false;
                    if (newStart != watcher->startPressed) {
                        watcher->startPressed = newStart;
                        stateChanged = true;
                        qWarning() << "CouchPlayHelper: hidraw BTN_START" << (newStart ? "pressed" : "released") << "on" << devicePath;
                    }
                    if (newSelect != watcher->selectPressed) {
                        watcher->selectPressed = newSelect;
                        stateChanged = true;
                        qWarning() << "CouchPlayHelper: hidraw BTN_SELECT" << (newSelect ? "pressed" : "released") << "on" << devicePath;
                    }

                    if (stateChanged) {
                        const bool chordPressed = watcher->startPressed && watcher->selectPressed;
                        if (chordPressed) {
                            if (!watcher->chordTimer->isActive()) {
                                qDebug() << "CouchPlayHelper: Exit chord detected on" << devicePath << ", starting timer...";
                                watcher->chordTimer->start();
                            }
                        } else {
                            if (watcher->chordTimer->isActive()) {
                                qDebug() << "CouchPlayHelper: Exit chord released on" << devicePath << ", stopping timer.";
                                watcher->chordTimer->stop();
                            }
                        }
                    }
                }
            } else if (bytesRead < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                } else if (errno == EINTR) {
                    continue;
                } else {
                    qWarning() << "CouchPlayHelper: Read error on hidraw device" << devicePath << ":" << strerror(errno);
                    deviceError = true;
                    break;
                }
            } else {
                // bytesRead == 0: EOF / device disconnected
                qWarning() << "CouchPlayHelper: EOF on hidraw device" << devicePath;
                deviceError = true;
                break;
            }
        }
    } else {
        // evdev path: read struct input_event records
        struct input_event ev;
        int bytesRead;

        while (true) {
            bytesRead = ::read(watcher->fd, &ev, sizeof(ev));
            if (bytesRead > 0) {
                if (ev.type == EV_KEY) {
                    qWarning() << "CouchPlayHelper: EV_KEY on" << devicePath << "code:" << ev.code << "value:" << ev.value;
                    bool stateChanged = false;
                    bool value = (ev.value != 0);

                    if (ev.code == BTN_START) {
                        qWarning() << "CouchPlayHelper: BTN_START change on" << devicePath << "to" << value;
                        if (watcher->startPressed != value) {
                            watcher->startPressed = value;
                            stateChanged = true;
                        }
                    } else if (ev.code == BTN_SELECT) {
                        qWarning() << "CouchPlayHelper: BTN_SELECT change on" << devicePath << "to" << value;
                        if (watcher->selectPressed != value) {
                            watcher->selectPressed = value;
                            stateChanged = true;
                        }
                    }

                    if (stateChanged) {
                        bool chordPressed = watcher->startPressed && watcher->selectPressed;
                        if (chordPressed) {
                            if (!watcher->chordTimer->isActive()) {
                                qDebug() << "CouchPlayHelper: Exit chord detected on" << devicePath << ", starting timer...";
                                watcher->chordTimer->start();
                            }
                        } else {
                            if (watcher->chordTimer->isActive()) {
                                qDebug() << "CouchPlayHelper: Exit chord released on" << devicePath << ", stopping timer.";
                                watcher->chordTimer->stop();
                            }
                        }
                    }
                }
            } else if (bytesRead < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                } else if (errno == EINTR) {
                    continue;
                } else {
                    qWarning() << "CouchPlayHelper: Read error on watched device" << devicePath << ":" << strerror(errno);
                    deviceError = true;
                    break;
                }
            } else {
                qWarning() << "CouchPlayHelper: EOF on watched device" << devicePath;
                deviceError = true;
                break;
            }
        }
    }

    if (deviceError) {
        stopWatchingDevice(devicePath);
    }
}

void CouchPlayHelper::onExitChordTimeout(const QString &devicePath)
{
    qWarning() << "CouchPlayHelper: Exit chord held for 2s on" << devicePath << "! Emitting exitChordTriggered.";
    Q_EMIT exitChordTriggered();
}

void CouchPlayHelper::onInputDirectoryChanged()
{
    m_inputDebounceTimer->start();
}

void CouchPlayHelper::onInputDebounceTimeout()
{
    checkForNewVirtualDevices();
}

void CouchPlayHelper::checkForNewVirtualDevices()
{
    QDir dir(QStringLiteral("/dev/input"));
    QStringList eventFiles = dir.entryList({QStringLiteral("event*")}, QDir::Files | QDir::System);
    static const QRegularExpression eventRegex(QStringLiteral("event(\\d+)"));

    QSet<int> activeEventNumbers;
    for (const QString &eventFile : eventFiles) {
        QRegularExpressionMatch match = eventRegex.match(eventFile);
        if (match.hasMatch()) {
            activeEventNumbers.insert(match.captured(1).toInt());
        }
    }
    m_knownEventNumbers.intersect(activeEventNumbers);

    for (const QString &eventFile : eventFiles) {
        QRegularExpressionMatch match = eventRegex.match(eventFile);
        if (match.hasMatch()) {
            int eventNumber = match.captured(1).toInt();
            if (!m_knownEventNumbers.contains(eventNumber)) {
                m_knownEventNumbers.insert(eventNumber);

                if (isVirtualDevice(eventNumber)) {
                    QString devicePath = QStringLiteral("/dev/input/%1").arg(eventFile);
                    qDebug() << "CouchPlayHelper: New virtual device detected, monitoring:" << devicePath;
                    startWatchingDevice(devicePath);
                }
            }
        }
    }
}

bool CouchPlayHelper::isVirtualDevice(int eventNumber)
{
    QString sysfsPath = QStringLiteral("/sys/class/input/event%1/device").arg(eventNumber);
    QFileInfo deviceSymlink(sysfsPath);
    if (!deviceSymlink.exists() || !deviceSymlink.isSymLink()) {
        return false;
    }

    QString target = deviceSymlink.symLinkTarget();
    if (target.contains(QLatin1String("/devices/virtual/"))) {
        return true;
    }

    QString name = getDeviceName(eventNumber);
    if (name.contains(QLatin1String("Microsoft X-Box 360 pad"), Qt::CaseInsensitive) ||
        name.contains(QLatin1String("Steam Virtual Gamepad"), Qt::CaseInsensitive) ||
        name.contains(QLatin1String("Xbox 360 Controller"), Qt::CaseInsensitive)) {
        return true;
    }

    return false;
}

QString CouchPlayHelper::getDeviceName(int eventNumber) const
{
    QString namePath = QStringLiteral("/sys/class/input/event%1/device/name").arg(eventNumber);
    QFile file(namePath);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString::fromUtf8(file.readAll()).trimmed();
    }
    return QString();
}

void CouchPlayHelper::setupUinputAccess()
{
    struct group *cpGroup = m_ops->getgrnam(COUCHPLAY_GROUP.toUtf8().constData());
    if (!cpGroup) {
        qWarning() << "CouchPlayHelper: couchplay group does not exist, cannot setup /dev/uinput access";
        return;
    }

    QProcess *setfacl = m_ops->createProcess();
    m_ops->startProcess(setfacl, QStringLiteral("/usr/bin/setfacl"),
                        {QStringLiteral("-m"), QStringLiteral("g:couchplay:rw"), QStringLiteral("/dev/uinput")});
    m_ops->waitForFinished(setfacl, 5000);
    if (m_ops->processExitCode(setfacl) != 0) {
        qWarning() << "CouchPlayHelper: Failed to set ACL on /dev/uinput for couchplay group";
    } else {
        qDebug() << "CouchPlayHelper: Successfully granted /dev/uinput access to couchplay group";
    }
    delete setfacl;
}

void CouchPlayHelper::removeUinputAccess()
{
    QProcess *setfacl = m_ops->createProcess();
    m_ops->startProcess(setfacl, QStringLiteral("/usr/bin/setfacl"),
                        {QStringLiteral("-x"), QStringLiteral("g:couchplay"), QStringLiteral("/dev/uinput")});
    m_ops->waitForFinished(setfacl, 5000);
    delete setfacl;
}

#include "CouchPlayHelper.moc"
