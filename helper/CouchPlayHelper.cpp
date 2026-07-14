// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "CouchPlayHelper.h"
#include "PolkitActions.h"
#include "SystemOps.h"

#include <QCryptographicHash>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QThread>

#include <linux/input.h>
#include <fcntl.h>
#include <QSocketNotifier>
#include <QTimer>
#include <QFileSystemWatcher>

#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

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
            for (const MountInfo &mount : m_activeMounts[username]) {
                QProcess *umountProc = m_ops->createProcess();
                m_ops->startProcess(umountProc, QStringLiteral("/usr/bin/umount"), {mount.target});
                m_ops->waitForFinished(umountProc, 5000);
                if (m_ops->processExitCode(umountProc) != 0) {
                    QProcess *lazyProc = m_ops->createProcess();
                    m_ops->startProcess(lazyProc,
                                        QStringLiteral("/usr/bin/umount"),
                                        {QStringLiteral("-l"), mount.target});
                    m_ops->waitForFinished(lazyProc, 5000);
                    delete lazyProc;
                }
                delete umountProc;
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

    qDeleteAll(m_monitors);
    m_monitors.clear();

    stopWatchingAllDevices();

    if (!m_modifiedDevices.isEmpty() || !m_modifiedHidDevices.isEmpty()) {
        ResetAllDevices();
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
    stopWatchingAllDevices();

    int successCount = 0;
    QStringList devices = m_modifiedDevices;

    struct group *inputGroup = m_ops->getgrnam("input");
    gid_t inputGid = inputGroup ? inputGroup->gr_gid : 0;

    // Clean up temporary udev rules for physical HID devices and rebind to restore default ownership
    QStringList hidDevices = m_modifiedHidDevices;
    m_modifiedHidDevices.clear();

    bool needUdevReload = false;

    for (const QString &trackingId : hidDevices) {
        QStringList parts = trackingId.split(QLatin1Char('|'));
        if (parts.size() < 3) continue;

        QString devId = parts.at(1);
        QString driverPath = parts.at(2);

        if (removeTempUdevRule(devId)) {
            needUdevReload = true;
        }

        QString unbindPath = driverPath + QStringLiteral("/unbind");
        qDebug() << "CouchPlayHelper: Reset - Unbinding device" << devId << "from driver" << driverPath;
        m_ops->writeFile(unbindPath, devId.toLocal8Bit());

        QString bindPath = driverPath + QStringLiteral("/bind");
        qDebug() << "CouchPlayHelper: Reset - Rebinding device" << devId << "to driver" << driverPath;
        m_ops->writeFile(bindPath, devId.toLocal8Bit());
    }

    if (needUdevReload) {
        // Reload udev rules after removing all temporary rules
        runCommand(QStringLiteral("udevadm"), {QStringLiteral("control"), QStringLiteral("--reload-rules")});

        // Trigger udev to re-apply default rules to input and hidraw subsystems
        runCommand(QStringLiteral("udevadm"),
                   {QStringLiteral("trigger"), QStringLiteral("--subsystem-match=input"),
                    QStringLiteral("--action=change")});
        runCommand(QStringLiteral("udevadm"),
                   {QStringLiteral("trigger"), QStringLiteral("--subsystem-match=hidraw"),
                    QStringLiteral("--action=change")});
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

bool CouchPlayHelper::SetupRuntimeAccess(uint compositorUid)
{
    if (!checkAuthorization(ACTION_WAYLAND_ACCESS)) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Not authorized to set up runtime access"));
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

bool CouchPlayHelper::RemoveRuntimeAccess(uint compositorUid)
{
    if (!checkAuthorization(ACTION_WAYLAND_ACCESS)) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Not authorized to remove runtime access"));
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
                                       uint compositorUid,
                                       const QStringList &gamescopeArgs,
                                       const QString &gameCommand,
                                       const QStringList &environment,
                                       const QStringList &bindPaths)
{
    if (!validateUserAndAuth(username, ACTION_LAUNCH_INSTANCE)) {
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

    qint64 pid = startTransientUnit(username, compositorUid, gamescopeArgs, gameCommand, environment, bindPaths);
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
            cleanupTcpListenerIfLast(username);
            m_stoppingUnits.remove(serviceName);
            saveState();
            return true;
        }
    }

    if (m_ops->killProcess(static_cast<pid_t>(pid), SIGTERM)) {
        return true;
    }

    sendErrorReply(QDBusError::Failed, QStringLiteral("Failed to stop process %1").arg(pid));
    return false;
}

bool CouchPlayHelper::KillInstance(qint64 pid)
{
    if (!checkAuthorization(ACTION_LAUNCH_INSTANCE)) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Not authorized to kill instances"));
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
            m_ops->startProcess(killProc,
                                QStringLiteral("systemctl"),
                                {QStringLiteral("kill"), serviceName, QStringLiteral("--signal=SIGKILL")});
            m_ops->waitForFinished(killProc, 10000);
            delete killProc;

            stopServiceInstance(serviceName);
            m_usernameToUnitName.remove(username);
            m_pidToUsername.remove(pid);
            cleanupTcpListenerIfLast(username);
            m_stoppingUnits.remove(serviceName);
            saveState();
            return true;
        }
    }

    if (m_ops->killProcess(static_cast<pid_t>(pid), SIGKILL)) {
        return true;
    }

    sendErrorReply(QDBusError::Failed, QStringLiteral("Failed to kill process %1").arg(pid));
    return false;
}

QString CouchPlayHelper::generateServiceName(const QString &username)
{
    return QStringLiteral("couchplay-%1.service").arg(username);
}

qint64 CouchPlayHelper::startTransientUnit(const QString &username,
                                           uint compositorUid,
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
    QString userRuntimeDir = QStringLiteral("/run/user/%1").arg(userUid);

    // Runtime directory must exist (requires linger) — without it, PipeWire
    // and gamescope lockfiles fail silently
    if (!m_ops->fileExists(userRuntimeDir)) {
        qInfo() << "Creating missing runtime directory" << userRuntimeDir << "for" << username;
        m_ops->mkpath(userRuntimeDir);
        m_ops->chown(userRuntimeDir, pwd->pw_uid, pwd->pw_gid);
    }

    QStringList systemdRunArgs;
    systemdRunArgs << QStringLiteral("--unit") << serviceName;
    systemdRunArgs << QStringLiteral("--uid") << username;
    systemdRunArgs << QStringLiteral("--property=Type=simple");
    systemdRunArgs << QStringLiteral("--property=Delegate=yes");
    systemdRunArgs << QStringLiteral("--property=MemoryDenyWriteExecute=false");

    // -E flag avoids escaping issues with --property=Environment=
    auto addEnv = [&](const QString &assignment) {
        systemdRunArgs << QStringLiteral("-E") << assignment;
    };
    addEnv(QStringLiteral("DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/%1/bus").arg(userUid));
    addEnv(QStringLiteral("WAYLAND_DISPLAY=%1/wayland-0").arg(compositorRuntimeDir));
    addEnv(QStringLiteral("XDG_RUNTIME_DIR=/run/user/%1").arg(userUid));
    addEnv(QStringLiteral("PIPEWIRE_RUNTIME_DIR=%1").arg(compositorRuntimeDir));
    // Use TCP PulseAudio listener for cross-user audio routing.
    // The unix socket rejects cross-UID connections, but TCP on localhost works.
    // The TCP listener is configured by setupPulseTcpListener() during SetupRuntimeAccess().
    addEnv(QStringLiteral("PULSE_SERVER=tcp:127.0.0.1:4713"));
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
    cmdArgs << QStringLiteral("--") << QStringLiteral("/bin/bash") << QStringLiteral("-c") << gameCommand;
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
    m_compositorUidForUsername[username] = compositorUid;

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

QString CouchPlayHelper::computeMountTarget(const QString &source,
                                            const QString &alias,
                                            const QString &userHome,
                                            const QString &compositorHome)
{
    if (source.startsWith(compositorHome) && alias.isEmpty()) {
        QString relativePath = source.mid(compositorHome.length());
        return userHome + relativePath;
    } else if (!alias.isEmpty()) {
        if (alias.startsWith(QLatin1Char('/'))) {
            return userHome + alias;
        }
        return userHome + QStringLiteral("/") + alias;
    } else {
        return userHome + QStringLiteral("/.couchplay/mounts") + source;
    }
}

int CouchPlayHelper::MountSharedDirectories(const QString &username, uint compositorUid, const QStringList &directories)
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

    QString compositorHome = getUserHomeByUid(compositorUid);
    if (compositorHome.isEmpty()) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not determine home directory for compositor user"));
        return 0;
    }

    int successCount = 0;

    for (const QString &dirSpec : directories) {
        QStringList parts = dirSpec.split(QLatin1Char('|'));
        if (parts.isEmpty()) {
            continue;
        }

        QString source = parts.at(0);
        QString alias = parts.size() > 1 ? parts.at(1) : QString();

        if (!m_ops->fileExists(source)) {
            qWarning() << "MountSharedDirectories: Source path does not exist:" << source;
            continue;
        }

        if (!m_ops->isDirectory(source)) {
            qWarning() << "MountSharedDirectories: Source is not a directory:" << source;
            continue;
        }

        QString target = computeMountTarget(source, alias, userHome, compositorHome);

        if (!m_ops->fileExists(target)) {
            if (!m_ops->mkpath(target)) {
                qWarning() << "MountSharedDirectories: Failed to create target directory:" << target;
                continue;
            }
            uint userUid = getUserUid(username);
            struct passwd *pw = m_ops->getpwuid(userUid);
            if (pw) {
                m_ops->chown(target, userUid, pw->pw_gid);
            }
        }

        QProcess *mountProcess = m_ops->createProcess();
        m_ops->startProcess(mountProcess, QStringLiteral("/usr/bin/mount"), {QStringLiteral("--bind"), source, target});
        m_ops->waitForFinished(mountProcess, 10000);

        if (m_ops->processExitCode(mountProcess) != 0) {
            qWarning() << "MountSharedDirectories: Failed to mount" << source << "to" << target << ":"
                       << QString::fromLocal8Bit(m_ops->readStandardError(mountProcess));
            delete mountProcess;
            continue;
        }
        delete mountProcess;

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
    QList<MountInfo> mounts = m_activeMounts[username];

    for (int i = mounts.size() - 1; i >= 0; --i) {
        const MountInfo &mount = mounts.at(i);

        QProcess *umountProc = m_ops->createProcess();
        m_ops->startProcess(umountProc, QStringLiteral("/usr/bin/umount"), {mount.target});
        m_ops->waitForFinished(umountProc, 10000);

        if (m_ops->processExitCode(umountProc) != 0) {
            qWarning() << "UnmountSharedDirectories: umount failed for" << mount.target << "- trying lazy unmount";
            delete umountProc;

            QProcess *lazyProc = m_ops->createProcess();
            m_ops->startProcess(lazyProc, QStringLiteral("/usr/bin/umount"), {QStringLiteral("-l"), mount.target});
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
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Not authorized to manage mounts"));
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
                m_ops->startProcess(lazyProc, QStringLiteral("/usr/bin/umount"), {QStringLiteral("-l"), mount.target});
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

bool CouchPlayHelper::CopyFileToUser(const QString &sourcePath, const QString &targetPath, const QString &username)
{
    if (!validateUserAndAuth(username, ACTION_MANAGE_MOUNTS)) {
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

    if (!m_ops->mkpath(targetDir)) {
        qWarning() << "CopyFileToUser: Failed to create directory:" << targetDir;
        sendErrorReply(QDBusError::Failed, QStringLiteral("Failed to create directory: %1").arg(targetDir));
        return false;
    }

    for (const QString &dir : dirsToChown) {
        m_ops->chown(dir, userUid, pw->pw_gid);
    }

    // Remove target first if it exists (copyFile won't overwrite)
    if (m_ops->fileExists(targetPath)) {
        m_ops->removeFile(targetPath);
    }

    if (!m_ops->copyFile(sourcePath, targetPath)) {
        qWarning() << "CopyFileToUser: Failed to copy" << sourcePath << "to" << targetPath;
        sendErrorReply(QDBusError::Failed,
                       QStringLiteral("Failed to copy file from %1 to %2").arg(sourcePath, targetPath));
        return false;
    }

    if (m_ops->chown(targetPath, userUid, pw->pw_gid) != 0) {
        qWarning() << "CopyFileToUser: Failed to set ownership on" << targetPath;
    }

    if (m_ops->chmod(targetPath, 0644) != 0) {
        qWarning() << "CopyFileToUser: Failed to set permissions on" << targetPath;
    }

    return true;
}

bool CouchPlayHelper::WriteFileToUser(const QByteArray &content, const QString &targetPath, const QString &username)
{
    if (!validateUserAndAuth(username, ACTION_MANAGE_MOUNTS)) {
        return false;
    }

    uint userUid = getUserUid(username);
    struct passwd *pw = m_ops->getpwuid(userUid);
    if (!pw) {
        qWarning() << "WriteFileToUser: Could not get user info for" << username;
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not get user info for '%1'").arg(username));
        return false;
    }

    int lastSlash = targetPath.lastIndexOf(QLatin1Char('/'));
    QString targetDir = (lastSlash >= 0) ? targetPath.left(lastSlash) : QStringLiteral(".");

    QStringList dirsToChown;
    if (!validateUserPath(targetDir, username, QStringLiteral("WriteFileToUser"), dirsToChown)) {
        return false;
    }

    if (!m_ops->mkpath(targetDir)) {
        qWarning() << "WriteFileToUser: Failed to create directory:" << targetDir;
        sendErrorReply(QDBusError::Failed, QStringLiteral("Failed to create directory: %1").arg(targetDir));
        return false;
    }

    for (const QString &dir : dirsToChown) {
        m_ops->chown(dir, userUid, pw->pw_gid);
    }

    if (!m_ops->writeFile(targetPath, content)) {
        qWarning() << "WriteFileToUser: Failed to write to" << targetPath;
        sendErrorReply(QDBusError::Failed, QStringLiteral("Failed to write to file"));
        return false;
    }

    if (m_ops->chown(targetPath, userUid, pw->pw_gid) != 0) {
        qWarning() << "WriteFileToUser: Failed to set ownership on" << targetPath;
    }

    if (m_ops->chmod(targetPath, 0644) != 0) {
        qWarning() << "WriteFileToUser: Failed to set permissions on" << targetPath;
    }

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

    if (!m_ops->mkpath(path)) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Failed to create directory: %1").arg(path));
        return false;
    }

    for (const QString &dir : dirsToChown) {
        m_ops->chown(dir, userUid, pw->pw_gid);
    }

    return true;
}

bool CouchPlayHelper::SetDirectoryAcl(const QString &path, const QString &username, bool recursive)
{
    if (!validateUserAndAuth(username, ACTION_MANAGE_MOUNTS)) {
        return false;
    }

    if (!m_ops->fileExists(path)) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Path does not exist: %1").arg(path));
        return false;
    }

    QStringList args;
    if (recursive) {
        args << QStringLiteral("-R");
    }
    args << QStringLiteral("-m");
    args << QStringLiteral("u:%1:rx").arg(username);
    args << path;

    QProcess *setfacl = m_ops->createProcess();
    m_ops->startProcess(setfacl, QStringLiteral("setfacl"), args);

    if (!m_ops->waitForFinished(setfacl, 60000)) { // 60 second timeout for recursive operations
        sendErrorReply(QDBusError::Failed, QStringLiteral("setfacl timed out for path: %1").arg(path));
        delete setfacl;
        return false;
    }

    if (m_ops->processExitCode(setfacl) != 0) {
        QString errorOutput = QString::fromUtf8(m_ops->readStandardError(setfacl));
        sendErrorReply(QDBusError::Failed, QStringLiteral("setfacl failed for path %1: %2").arg(path, errorOutput));
        delete setfacl;
        return false;
    }
    delete setfacl;

    return true;
}

bool CouchPlayHelper::SetPathAclWithParents(const QString &path, const QString &username)
{
    if (!validateUserAndAuth(username, ACTION_MANAGE_MOUNTS)) {
        return false;
    }

    if (!m_ops->fileExists(path)) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Path does not exist: %1").arg(path));
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
    QString current = path;

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

        pathsToSet.prepend(current);
    }

    bool allSucceeded = true;
    for (const QString &p : pathsToSet) {
        if (!m_ops->fileExists(p)) {
            qWarning() << "SetPathAclWithParents: Path does not exist, skipping:" << p;
            continue;
        }

        QProcess *removeProc = m_ops->createProcess();
        m_ops->startProcess(removeProc,
                            QStringLiteral("setfacl"),
                            {QStringLiteral("-x"), QStringLiteral("u:%1").arg(username), p});
        m_ops->waitForFinished(removeProc, 5000);
        delete removeProc;

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
        }
        delete setfacl;
    }

    return allSucceeded;
}

QString CouchPlayHelper::GetUserSteamId(const QString &username)
{
    if (!s_validUsername.match(username).hasMatch()) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Invalid username format"));
        return QString();
    }

    if (!userExists(username)) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("User '%1' does not exist").arg(username));
        return QString();
    }

    QString userHome = getUserHome(username);
    if (userHome.isEmpty()) {
        return QString();
    }

    QStringList possibleRoots = {
        userHome + QStringLiteral("/.steam/steam/userdata"),
        userHome + QStringLiteral("/.local/share/Steam/userdata"),
    };

    for (const QString &userDataBase : possibleRoots) {
        if (!m_ops->fileExists(userDataBase)) {
            continue;
        }

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
    info.waylandSocket = socketName;
    info.serviceName = serviceName;
    m_virtualDisplays[username] = info;
    saveState();

    qInfo() << "Created virtual display for" << username
            << "socket:" << socketName << "PID:" << mainPid;

    return socketName;
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

    if (!waylandSocketName.isEmpty() && info.waylandSocket != waylandSocketName) {
        sendErrorReply(QDBusError::InvalidArgs,
            QStringLiteral("Socket name mismatch: expected '%1', got '%2'")
                .arg(info.waylandSocket, waylandSocketName));
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

    QJsonObject compositorUidObject;
    for (auto it = m_compositorUidForUsername.constBegin(); it != m_compositorUidForUsername.constEnd(); ++it) {
        compositorUidObject[it.key()] = static_cast<qint64>(it.value());
    }
    root[QStringLiteral("compositorUidForUsername")] = compositorUidObject;
    QJsonObject vdisplayObject;
    for (auto it = m_virtualDisplays.constBegin(); it != m_virtualDisplays.constEnd(); ++it) {
        QJsonObject infoObj;
        infoObj[QStringLiteral("pid")] = static_cast<qint64>(it.value().pid);
        infoObj[QStringLiteral("waylandSocket")] = it.value().waylandSocket;
        infoObj[QStringLiteral("serviceName")] = it.value().serviceName;
        vdisplayObject[it.key()] = infoObj;
    }
    root[QStringLiteral("virtualDisplays")] = vdisplayObject;

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
    // Restore virtual displays - reconcile against active systemd units
    QJsonObject vdisplayObject = root.value(QStringLiteral("virtualDisplays")).toObject();
    for (auto it = vdisplayObject.constBegin(); it != vdisplayObject.constEnd(); ++it) {
        QJsonObject infoObj = it.value().toObject();
        QString serviceName = infoObj.value(QStringLiteral("serviceName")).toString();
        if (activeSystemdUnits.contains(serviceName)) {
            VirtualDisplayInfo info;
            info.pid = static_cast<qint64>(infoObj.value(QStringLiteral("pid")).toVariant().toLongLong());
            info.waylandSocket = infoObj.value(QStringLiteral("waylandSocket")).toString();
            info.serviceName = serviceName;
            m_virtualDisplays[it.key()] = info;
        } else {
            qDebug() << "loadAndReconcileState: Removing stale virtual display" << serviceName << "for user" << it.key();
            changed = true;
        }
    }

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
    watcher->chordTimer->setInterval(3000); // 3.0 seconds hold duration

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
    qWarning() << "CouchPlayHelper: Exit chord held for 3s on" << devicePath << "! Emitting exitChordTriggered.";
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
