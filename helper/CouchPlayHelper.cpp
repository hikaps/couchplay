// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "CouchPlayHelper.h"
#include "HelperApiLevel.h"
#include "InstanceBootstrap.h"
#include "HomePathMapping.h"
#include "PolkitActions.h"
#include "SecureFs.h"
#include "SteamCompatToolMapping.h"
#include "SystemOps.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
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
#include <QScopeGuard>
#include <QThread>

#include <linux/input.h>
#include <fcntl.h>
#include <sys/file.h>
#include <QSocketNotifier>
#include <QTimer>
#include <QFileSystemWatcher>

#include <cerrno>
#include <cstring>
#include <functional>
#include <utility>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <limits>
#ifndef O_PATH
#define O_PATH 010000000
#endif

struct CouchPlayHelper::PendingLaunch
{
    explicit PendingLaunch(const QDBusConnection &connection)
        : replyConnection(connection)
    {
    }
    QDBusMessage request;
    QDBusConnection replyConnection;
    QString username;
    QString token;
    uid_t callerUid = 0;
    QTimer *pollTimer = nullptr;
    QTimer *deadlineTimer = nullptr;
    bool pollInFlight = false;
    bool replySent = false;
};

class UnitMonitor : public QObject
{
    Q_OBJECT
public:
    UnitMonitor(const QString &unitPath, const QString &serviceName, quint64 generation, CouchPlayHelper *helper)
        : QObject(helper)
        , m_unitPath(unitPath)
        , m_serviceName(serviceName)
        , m_generation(generation)
        , m_helper(helper)
        , m_connection(helper->m_ops->systemBusConnection())
    {
        m_connection.connect(QStringLiteral("org.freedesktop.systemd1"),
                             m_unitPath,
                             QStringLiteral("org.freedesktop.DBus.Properties"),
                             QStringLiteral("PropertiesChanged"),
                             this,
                             SLOT(onPropertiesChanged(QString,QVariantMap,QStringList)));
    }

    ~UnitMonitor() override
    {
        m_connection.disconnect(QStringLiteral("org.freedesktop.systemd1"),
                                m_unitPath,
                                QStringLiteral("org.freedesktop.DBus.Properties"),
                                QStringLiteral("PropertiesChanged"),
                                this,
                                SLOT(onPropertiesChanged(QString,QVariantMap,QStringList)));
    }

public Q_SLOTS:
    void onPropertiesChanged(const QString &interface, const QVariantMap &changed, const QStringList &) {
        if (interface == QStringLiteral("org.freedesktop.systemd1.Service")) {
            const QString status = changed.value(QStringLiteral("StatusText")).toString();
            if (!status.isEmpty()) {
                m_statusText = status;
            }
            const int statusErrno = changed.value(QStringLiteral("StatusErrno")).toInt();
            if (statusErrno != 0) {
                m_statusErrno = statusErrno;
            }
            const QString result = changed.value(QStringLiteral("Result")).toString();
            if (!result.isEmpty()) {
                m_result = result;
            }
            return;
        }
        if (m_helper->m_unitGenerationByName.value(m_serviceName, 0) != m_generation
            || interface != QStringLiteral("org.freedesktop.systemd1.Unit")
            || !changed.contains(QStringLiteral("ActiveState"))) {
            return;
        }
        const QString activeState = changed.value(QStringLiteral("ActiveState")).toString();
        if (activeState == QStringLiteral("active")) {
            m_helper->onUnitReady(m_serviceName);
        } else if (activeState == QStringLiteral("failed") || activeState == QStringLiteral("inactive")
                   || activeState == QStringLiteral("dead")) {
            m_helper->onUnitStopped(m_serviceName, QString());
        }
    }

public:
    QString unitPath() const { return m_unitPath; }
    void observeInitialState(const QString &activeState, bool authoritativeInactive) {
        if (activeState == QStringLiteral("active")) {
            m_helper->onUnitReady(m_serviceName);
        } else if (activeState == QStringLiteral("failed")
                   || (authoritativeInactive
                       && (activeState == QStringLiteral("inactive") || activeState == QStringLiteral("dead")))) {
            m_helper->onUnitStopped(m_serviceName, QString());
        } else if (activeState == QStringLiteral("inactive") || activeState == QStringLiteral("dead")) {
            m_helper->noteInitialInactiveUnitState(m_serviceName);
        }
    }

    QString result() const { return m_result; }
    QString statusText() const { return m_statusText; }
    int statusErrno() const { return m_statusErrno; }

private:
    QString m_unitPath;
    QString m_serviceName;
    quint64 m_generation;
    CouchPlayHelper *m_helper;
    QDBusConnection m_connection;
    QString m_result;
    QString m_statusText;
    int m_statusErrno = 0;
};

class UnitJobMonitor final : public QObject
{
    Q_OBJECT
    struct JobResult { QString path; QString result; };
public:
    UnitJobMonitor(const QString &serviceName, CouchPlayHelper *helper)
        : QObject(helper)
        , m_serviceName(serviceName)
        , m_helper(helper)
        , m_connection(helper->m_ops->systemBusConnection())
    {
        m_connection.connect(QStringLiteral("org.freedesktop.systemd1"),
                             QStringLiteral("/org/freedesktop/systemd1"),
                             QStringLiteral("org.freedesktop.systemd1.Manager"),
                             QStringLiteral("JobRemoved"),
                             this,
                             SLOT(onJobRemoved(uint,QDBusObjectPath,QString,QString)));
    }

    ~UnitJobMonitor() override
    {
        m_connection.disconnect(QStringLiteral("org.freedesktop.systemd1"),
                                QStringLiteral("/org/freedesktop/systemd1"),
                                QStringLiteral("org.freedesktop.systemd1.Manager"),
                                QStringLiteral("JobRemoved"),
                                this,
                                SLOT(onJobRemoved(uint,QDBusObjectPath,QString,QString)));
    }

    void setGeneration(quint64 generation) { m_generation = generation; }

    void setExpectedJobPath(const QString &jobPath)
    {
        if (m_helper->m_unitGenerationByName.value(m_serviceName, 0) != m_generation) {
            return;
        }
        m_expectedJobPath = jobPath;
        const QList<JobResult> earlyResults = m_earlyResults;
        m_earlyResults.clear();
        for (const JobResult &event : earlyResults) {
            if (event.path == m_expectedJobPath) {
                processJobResult(event.result);
                break;
            }
        }
    }

public Q_SLOTS:
    void onJobRemoved(uint jobId, const QDBusObjectPath &jobPath, const QString &unitName, const QString &result)
    {
        Q_UNUSED(jobId)
        if (unitName != m_serviceName || m_generation == 0
            || m_helper->m_unitGenerationByName.value(m_serviceName, 0) != m_generation) {
            return;
        }
        if (m_expectedJobPath.isEmpty()) {
            m_earlyResults.append(JobResult{jobPath.path(), result});
            return;
        }
        if (jobPath.path() == m_expectedJobPath) {
            processJobResult(result);
        }
    }
private:

    void processJobResult(const QString &result)
    {
        if (m_helper->m_jobMonitors.value(m_serviceName) == this) {
            m_helper->m_jobMonitors.remove(m_serviceName);
        }
        deleteLater();
        if (result == QStringLiteral("done")) {
            return;
        }
        m_helper->m_failedJobResults.insert(m_serviceName, result);
        if (m_helper->m_unitPathForUnit.contains(m_serviceName)) {
            m_helper->m_failedJobResults.remove(m_serviceName);
            m_helper->onUnitStopped(m_serviceName, QString());
        }
    }

    QString m_serviceName;
    QString m_expectedJobPath;
    QList<JobResult> m_earlyResults;
    CouchPlayHelper *m_helper;
    QDBusConnection m_connection;
    quint64 m_generation = 0;
};

static const QRegularExpression s_validUsername(QStringLiteral("^[a-z][a-z0-9_-]{0,31}$"));
static const QString ACTION_DEVICE_OWNER = QStringLiteral("io.github.hikaps.couchplay.change-device-owner");
static const QString ACTION_CREATE_USER = QStringLiteral("io.github.hikaps.couchplay.create-user");
static const QString ACTION_DELETE_USER = QStringLiteral("io.github.hikaps.couchplay.delete-user");
static const QString ACTION_ENABLE_LINGER = QStringLiteral("io.github.hikaps.couchplay.enable-linger");
static const QString ACTION_WAYLAND_ACCESS = QStringLiteral("io.github.hikaps.couchplay.setup-wayland-access");
static const QString ACTION_LAUNCH_INSTANCE = QStringLiteral("io.github.hikaps.couchplay.launch-instance");
static const QString ACTION_MANAGE_MOUNTS = QStringLiteral("io.github.hikaps.couchplay.manage-mounts");
static const QString ACTION_MANAGE_SESSION = QStringLiteral("io.github.hikaps.couchplay.manage-session");

static const QString COUCHPLAY_GROUP = QStringLiteral("couchplay");
static constexpr uint INVALID_CALLER_UID = std::numeric_limits<uint>::max();

CouchPlayHelper::CouchPlayHelper(SystemOps *ops, QObject *parent)
    : QObject(parent)
    , m_ops(ops ? ops : new RealSystemOps(this))
    , m_stateFilePath(QStringLiteral("/run/couchplay/state.json"))
{
    loadAndReconcileState();
    setupUinputAccess();

    m_knownEventInputs = currentEventInputs();

    // Set up directory watcher on /dev/input
    m_inputWatcher = new QFileSystemWatcher(this);
    m_inputWatcher->addPath(QStringLiteral("/dev/input"));
    connect(m_inputWatcher, &QFileSystemWatcher::directoryChanged, this, &CouchPlayHelper::onInputDirectoryChanged);

    m_inputDebounceTimer = new QTimer(this);
    m_inputDebounceTimer->setInterval(500);
    m_inputDebounceTimer->setSingleShot(true);
    connect(m_inputDebounceTimer, &QTimer::timeout, this, &CouchPlayHelper::onInputDebounceTimeout);

    // Watch any virtual controllers already present at startup
    for (auto it = m_knownEventInputs.constBegin(); it != m_knownEventInputs.constEnd(); ++it) {
        bool isNumber = false;
        const int eventNumber = it.key().mid(5).toInt(&isNumber); // "eventN"
        if (isNumber && isVirtualDevice(eventNumber)) {
            QString devicePath = QStringLiteral("/dev/input/%1").arg(it.key());
            qDebug() << "CouchPlayHelper: Pre-existing virtual device found at startup, watching:" << devicePath;
            startWatchingDevice(devicePath);
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


    const QStringList ownedServices = m_usernameToUnitName.values();
    for (const QString &serviceName : ownedServices) {
        m_stoppingUnits.insert(serviceName);
        if (stopServiceInstance(serviceName)) {
            releaseOwnedUnit(serviceName, true);
        } else {
            m_stoppingUnits.remove(serviceName);
            qWarning() << "Helper shutdown could not stop owned unit" << serviceName;
        }
    }
    for (auto it = m_pendingLaunches.begin(); it != m_pendingLaunches.end(); ++it) {
        PendingLaunch *pending = it.value();
        pending->pollTimer->stop();
        pending->deadlineTimer->stop();
        if (!pending->replySent) {
            pending->replyConnection.send(pending->request.createErrorReply(
                QDBusError::Failed, QStringLiteral("Helper stopped while instance preparation was pending")));
        }
    }
    saveState();

    for (PendingLaunch *pending : m_pendingLaunches) {
        delete pending->pollTimer;
        delete pending->deadlineTimer;
        delete pending;
    }
    m_pendingLaunches.clear();
    qDeleteAll(m_monitors);
    m_monitors.clear();

    stopWatchingAllDevices();

    if (!m_modifiedDevices.isEmpty() || !m_modifiedHidDevices.isEmpty()) {
        resetAllDevicesInternal();
    }
    removeUinputAccess();
}

bool CouchPlayHelper::ChangeDeviceOwner(const QString &requestedPath, uint uid)
{
    // A rebind earlier in this session may have renumbered (or removed) the node the caller listed
    const QString devicePath = m_reboundNodes.value(requestedPath, requestedPath);
    if (!isValidDevicePath(devicePath)) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Invalid device path: %1").arg(requestedPath));
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
            const QStringList nodesBeforeRebind = gamepadNodePaths(hid);
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

            // After rebind, the node may have received a new number. Re-resolve the actual current path
            // from sysfs using the stable device ID: the hidraw node, or the gamepad's event node when the
            // device has no hidraw node (USB interface drivers such as xpad).
            const QStringList nodesAfterRebind = gamepadNodePaths(hid);
            for (const QString &node : nodesAfterRebind) {
                m_reboundNodes.remove(node);
            }
            // Callers list the event and joystick nodes they saw before the rebind; map the old ones
            for (qsizetype i = 0; i < qMin(nodesBeforeRebind.size(), nodesAfterRebind.size()); ++i) {
                if (nodesBeforeRebind.at(i) != nodesAfterRebind.at(i)) {
                    m_reboundNodes.insert(nodesBeforeRebind.at(i), nodesAfterRebind.at(i));
                }
            }
            QString resolvedDevicePath = findHidrawPathForDeviceId(devId);
            if (resolvedDevicePath.isEmpty() && devicePath.startsWith(QStringLiteral("/dev/input/event"))) {
                resolvedDevicePath = nodesAfterRebind.value(0);
            }
            if (resolvedDevicePath.isEmpty()) {
                qWarning() << "CouchPlayHelper: Could not resolve post-rebind path for" << devId
                           << "-- falling back to original path" << devicePath;
                resolvedDevicePath = devicePath;
            } else if (resolvedDevicePath != devicePath) {
                qDebug() << "CouchPlayHelper: Post-rebind path changed from" << devicePath << "to"
                         << resolvedDevicePath;
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

bool CouchPlayHelper::ResetDeviceOwner(const QString &requestedPath)
{
    const QString devicePath = m_reboundNodes.value(requestedPath, requestedPath);
    if (!isValidDevicePath(devicePath)) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Invalid device path: %1").arg(requestedPath));
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
    m_reboundNodes.remove(requestedPath);
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
    return resetAllDevicesInternal();
}

int CouchPlayHelper::resetAllDevicesInternal()
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
    m_reboundNodes.clear();
    m_sessionVirtualDevices.clear();

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

    auto runUseradd = [&](const QString &supplementaryGroups, QString *errorOutput) {
        QProcess *process = m_ops->createProcess();
        m_ops->startProcess(process, QStringLiteral("useradd"),
                            {QStringLiteral("-m"), QStringLiteral("-c"), fullName, QStringLiteral("-s"),
                             QStringLiteral("/bin/bash"), QStringLiteral("-G"), supplementaryGroups, username});
        m_ops->waitForFinished(process, 30000);
        const bool ok = m_ops->processExitCode(process) == 0;
        *errorOutput = ok ? QString() : QString::fromLocal8Bit(m_ops->readStandardError(process));
        delete process;
        return ok;
    };

    // The "input" group is optional (absent on some Bazzite versions, issue #23). getgrnam() can see it even when
    // useradd cannot: Bazzite 45 defines it in /usr/lib/group (nss altfiles) while useradd reads only /etc/group and
    // fails with "group 'input' does not exist". In that case retry without it.
    QString useraddError;
    bool created = false;
    if (m_ops->getgrnam("input")) {
        created = runUseradd(QStringLiteral("input,") + COUCHPLAY_GROUP, &useraddError);
        if (!created && useraddError.contains(QStringLiteral("'input' does not exist"))) {
            qWarning() << "useradd cannot resolve the input group, creating" << username << "without it";
            created = runUseradd(COUCHPLAY_GROUP, &useraddError);
        }
    } else {
        created = runUseradd(COUCHPLAY_GROUP, &useraddError);
    }

    if (!created) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Failed to create user: %1").arg(useraddError));
        return 0;
    }

    uint uid = getUserUid(username);
    if (uid == 0) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("User created but could not retrieve UID"));
        return 0;
    }

    // Enable linger so the player's transient systemd instance units can start after logout.
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

QVariantMap CouchPlayHelper::GetVersionInfo()
{
    return {{QStringLiteral("version"), QCoreApplication::applicationVersion()},
            {QStringLiteral("apiLevel"), COUCHPLAY_HELPER_API_LEVEL}};
}

QVariantMap CouchPlayHelper::GetUserHomeInfoByUid(uint uid)
{
    struct passwd *pw = m_ops->getpwuid(uid);
    if (!pw) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("No user with uid %1").arg(uid));
        return {};
    }
    const QString home = QString::fromLocal8Bit(pw->pw_dir);
    return {{QStringLiteral("home"), home},
            {QStringLiteral("canonicalHome"), m_ops->canonicalFilePath(home)}};
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
    };

    for (const QString &candidate : candidates) {
        const bool rootExists = m_ops->fileExists(candidate) || m_ops->fileExists(candidate + QStringLiteral("/steam.sh"))
            || m_ops->fileExists(candidate + QStringLiteral("/ubuntu12_32/steam"))
            || m_ops->fileExists(candidate + QStringLiteral("/userdata"))
            || m_ops->fileExists(candidate + QStringLiteral("/config"));
        if (!rootExists) {
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

    QString waylandSocket = waylandSocketPath(runtimeDir);
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

// KWin takes the first free wayland-N name, so the desktop's socket is not always wayland-0 (a stale lock or another
// compositor can leave it wayland-1). Pick the lowest-numbered socket; a nested compositor starts later and gets a
// higher number. Falls back to wayland-0 when none is found.
QString CouchPlayHelper::waylandSocketPath(const QString &runtimeDir)
{
    static const QRegularExpression socketName(QStringLiteral("^wayland-([0-9]+)$"));
    int lowest = -1;
    const QStringList entries = m_ops->entryList(runtimeDir, {QStringLiteral("wayland-*")},
                                                 QDir::System | QDir::NoDotAndDotDot);
    for (const QString &entry : entries) {
        const QRegularExpressionMatch match = socketName.match(entry);
        if (!match.hasMatch()) {
            continue;
        }
        const int number = match.captured(1).toInt();
        if (lowest < 0 || number < lowest) {
            lowest = number;
        }
    }
    return runtimeDir + QStringLiteral("/wayland-") + QString::number(lowest < 0 ? 0 : lowest);
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

    removeAcl(waylandSocketPath(runtimeDir));
    removeAcl(runtimeDir);
}

namespace {

constexpr int PULSE_TCP_PORT = 4713;

QString pulseTcpModuleName()
{
    return QStringLiteral("module-native-protocol-tcp");
}

// Exact arguments the helper loads the module with; also used to recognise its own module on teardown.
QString pulseTcpModuleArgs()
{
    return QStringLiteral("port=%1 listen=127.0.0.1 auth-anonymous=1").arg(PULSE_TCP_PORT);
}

struct PulseTcpModule {
    QString id;
    QString args;
};

// Parses `pactl list short modules` ("<id>\t<name>\t<args>"); tolerates CRLF line endings and trailing tabs.
QList<PulseTcpModule> parsePulseTcpModules(const QByteArray &output)
{
    QList<PulseTcpModule> modules;
    const QStringList lines = QString::fromUtf8(output).split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QStringList fields = line.trimmed().split(QLatin1Char('\t'));
        if (fields.size() < 2 || fields.at(1) != pulseTcpModuleName()) {
            continue;
        }
        modules.append({fields.at(0), fields.value(2).simplified()});
    }
    return modules;
}

} // namespace

bool CouchPlayHelper::runAsUser(const struct passwd &pw,
                                const QStringList &command,
                                QByteArray *standardOutput,
                                QString *standardError)
{
    // Drop to the user and point at their runtime directory so their PipeWire/PulseAudio socket is reachable.
    // Unlike `machinectl shell`, this returns the command's real exit status. Arguments are argv, never a shell.
    QStringList args{QStringLiteral("--reuid=%1").arg(pw.pw_uid),
                     QStringLiteral("--regid=%1").arg(pw.pw_gid),
                     QStringLiteral("--init-groups"),
                     QStringLiteral("--"),
                     QStringLiteral("/usr/bin/env"),
                     QStringLiteral("-i"),
                     QStringLiteral("HOME=%1").arg(QString::fromLocal8Bit(pw.pw_dir)),
                     QStringLiteral("XDG_RUNTIME_DIR=/run/user/%1").arg(pw.pw_uid),
                     QStringLiteral("PATH=/usr/bin:/bin")};
    args += command;

    QProcess *proc = m_ops->createProcess();
    m_ops->startProcess(proc, QStringLiteral("setpriv"), args);
    const bool finished = m_ops->waitForFinished(proc, 10000);
    const bool success = finished && m_ops->processExitCode(proc) == 0;
    if (standardOutput) {
        *standardOutput = m_ops->readAllStandardOutput(proc);
    }
    if (standardError) {
        *standardError = QString::fromLocal8Bit(m_ops->readStandardError(proc));
    }
    delete proc;
    return success;
}

bool CouchPlayHelper::setupPulseTcpListener(uint compositorUid)
{
    struct passwd *pw = m_ops->getpwuid(compositorUid);
    if (!pw) {
        qWarning() << "setupPulseTcpListener: compositor user not found for UID" << compositorUid;
        return false;
    }

    const struct passwd &user = *pw;
    const QString username = QString::fromLocal8Bit(user.pw_name);
    const QString pactl = QStringLiteral("/usr/bin/pactl");
    const QString portArg = QStringLiteral("port=%1").arg(PULSE_TCP_PORT);

    QByteArray modulesOutput;
    QString error;
    if (!runAsUser(user, {pactl, QStringLiteral("list"), QStringLiteral("short"), QStringLiteral("modules")},
                   &modulesOutput, &error)) {
        qWarning() << "Failed to list PulseAudio modules for" << username << ":" << error;
        return false;
    }

    for (const PulseTcpModule &module : parsePulseTcpModules(modulesOutput)) {
        if (module.args.split(QLatin1Char(' '), Qt::SkipEmptyParts).contains(portArg)) {
            // Loaded earlier (by this helper or the user): leave it alone
            return true;
        }
    }

    // A pipewire-pulse drop-in (e.g. the one written by CouchPlay 0.6.0) serves the port without a loaded module
    if (runAsUser(user,
                  {QStringLiteral("PULSE_SERVER=tcp:127.0.0.1:%1").arg(PULSE_TCP_PORT), pactl, QStringLiteral("info")})) {
        return true;
    }

    qInfo() << "Loading PulseAudio TCP listener module for UID" << compositorUid;
    QStringList loadCommand{pactl, QStringLiteral("load-module"), pulseTcpModuleName()};
    loadCommand += pulseTcpModuleArgs().split(QLatin1Char(' '));
    if (!runAsUser(user, loadCommand, nullptr, &error)) {
        qWarning() << "Failed to load" << pulseTcpModuleName() << "for" << username << ":" << error;
        return false;
    }

    m_pulseTcpLoadedForUid.insert(compositorUid);
    return true;
}

void CouchPlayHelper::removePulseTcpListener(uint compositorUid)
{
    const bool helperLoaded = m_pulseTcpLoadedForUid.remove(compositorUid);

    struct passwd *pw = m_ops->getpwuid(compositorUid);
    if (!pw) {
        return;
    }

    // CouchPlay 0.6.0 wrote a pipewire-pulse drop-in and restarted the service. Remove a leftover one without
    // restarting: it is harmless until the next natural restart and must not interrupt the user's audio.
    const QString homeDir = QString::fromLocal8Bit(pw->pw_dir);
    const QString legacyConf = homeDir + QStringLiteral("/.config/pipewire/pipewire-pulse.conf.d/50-couchplay.conf");
    if (m_ops->fileExists(legacyConf)) {
        qInfo() << "Removing legacy PipeWire TCP listener config for UID" << compositorUid;
        m_ops->removeFile(legacyConf);
    }

    if (!helperLoaded) {
        return; // Never unload a module this helper did not load
    }

    const struct passwd &user = *pw;
    const QString username = QString::fromLocal8Bit(user.pw_name);
    QByteArray modulesOutput;
    QString error;
    if (!runAsUser(user,
                   {QStringLiteral("/usr/bin/pactl"), QStringLiteral("list"), QStringLiteral("short"),
                    QStringLiteral("modules")},
                   &modulesOutput, &error)) {
        qWarning() << "Failed to list PulseAudio modules for" << username << ":" << error;
        return;
    }

    for (const PulseTcpModule &module : parsePulseTcpModules(modulesOutput)) {
        if (module.args != pulseTcpModuleArgs()) {
            continue;
        }
        qInfo() << "Unloading PulseAudio TCP listener module" << module.id << "for UID" << compositorUid;
        if (!runAsUser(user,
                       {QStringLiteral("/usr/bin/pactl"), QStringLiteral("unload-module"), module.id}, nullptr,
                       &error)) {
            qWarning() << "Failed to unload PulseAudio module" << module.id << "for" << username << ":" << error;
        }
    }
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
    const bool sessionOperation = action == ACTION_DEVICE_OWNER || action == ACTION_WAYLAND_ACCESS
        || action == ACTION_LAUNCH_INSTANCE || action == ACTION_MANAGE_MOUNTS;
    return m_ops->checkAuthorization(sessionOperation ? ACTION_MANAGE_SESSION : action, message().service());
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
    if (action == ACTION_LAUNCH_INSTANCE
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
                                       const QStringList &gamescopeArgs,
                                       const QStringList &gameCommand,
                                       const QString &workingDirectory,
                                       const QStringList &sharedRoots,
                                       const QStringList &environment,
                                       bool privateTemporaryDirectories,
                                       const QVariantList &directoryPreparations)
{
    if (!validateUserAndAuth(username, ACTION_LAUNCH_INSTANCE)) {
        return 0;
    }
    if (gameCommand.isEmpty() || gameCommand.constFirst().isEmpty()) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Child launch command cannot be empty"));
        return 0;
    }
    const QDBusMessage request = message();
    const QDBusConnection replyConnection = connection();
    setDelayedReply(true);
    startTransientUnit(username,
                       gamescopeArgs,
                       gameCommand,
                       workingDirectory,
                       sharedRoots,
                       environment,
                       privateTemporaryDirectories,
                       directoryPreparations,
                       request,
                       replyConnection);
    return 0;
}


bool CouchPlayHelper::StopInstance(qint64 pid)
{
    if (!checkAuthorization(ACTION_LAUNCH_INSTANCE)) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Not authorized to stop instances"));
        return false;
    }
    if (!m_pidToUsername.contains(pid) || !m_pidToUnitName.contains(pid) || !m_pidOwnerUid.contains(pid)) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Unknown or untracked instance PID"));
        return false;
    }
    if (m_pidOwnerUid.value(pid) != callerUid()) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Not authorized for this instance"));
        return false;
    }
    const QString serviceName = m_pidToUnitName.value(pid);
    if (serviceName.isEmpty() || !m_unitUsernameByName.contains(serviceName)) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Instance unit is not tracked"));
        return false;
    }
    m_stoppingUnits.insert(serviceName);
    if (!stopServiceInstance(serviceName)) {
        m_stoppingUnits.remove(serviceName);
        sendErrorReply(QDBusError::Failed, QStringLiteral("Failed to stop instance unit"));
        return false;
    }
    releaseOwnedUnit(serviceName, true);
    return true;
}

bool CouchPlayHelper::KillInstance(qint64 pid)
{
    if (!checkAuthorization(ACTION_LAUNCH_INSTANCE)) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Not authorized to kill instances"));
        return false;
    }
    if (!m_pidToUsername.contains(pid) || !m_pidToUnitName.contains(pid) || !m_pidOwnerUid.contains(pid)) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Unknown or untracked instance PID"));
        return false;
    }
    if (m_pidOwnerUid.value(pid) != callerUid()) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Not authorized for this instance"));
        return false;
    }
    const QString serviceName = m_pidToUnitName.value(pid);
    if (serviceName.isEmpty() || !m_unitUsernameByName.contains(serviceName)) {
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
    if (!killed || !stopServiceInstance(serviceName)) {
        m_stoppingUnits.remove(serviceName);
        sendErrorReply(QDBusError::Failed, QStringLiteral("Failed to kill instance unit"));
        return false;
    }
    releaseOwnedUnit(serviceName, true);
    return true;
}









static QString systemdUnitObjectPath(const QString &unitName)
{
    QString escaped;
    escaped.reserve(unitName.size() * 3);
    for (const QChar character : unitName) {
        const ushort code = character.unicode();
        if ((code >= QLatin1Char('a').unicode() && code <= QLatin1Char('z').unicode())
            || (code >= QLatin1Char('A').unicode() && code <= QLatin1Char('Z').unicode())
            || (code >= QLatin1Char('0').unicode() && code <= QLatin1Char('9').unicode())) {
            escaped.append(character);
        } else {
            escaped += QLatin1Char('_') + QString::number(code, 16).rightJustified(2, QLatin1Char('0'));
        }
    }
    return QStringLiteral("/org/freedesktop/systemd1/unit/") + escaped;
}
QString CouchPlayHelper::generateServiceName(const QString &username)
{
    return QStringLiteral("couchplay-%1.service").arg(username);
}

QString CouchPlayHelper::usernameForUnit(const QString &serviceName) const
{
    return m_unitUsernameByName.value(serviceName);
}

bool CouchPlayHelper::subscribeSystemdManager(QString *error) const
{
    QDBusMessage subscribe = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.systemd1"),
                                                            QStringLiteral("/org/freedesktop/systemd1"),
                                                            QStringLiteral("org.freedesktop.systemd1.Manager"),
                                                            QStringLiteral("Subscribe"));
    const QDBusMessage reply = m_ops->systemBusConnection().call(subscribe, QDBus::Block, 5000);
    if (reply.type() != QDBusMessage::ReplyMessage
        && reply.errorName() != QStringLiteral("org.freedesktop.systemd1.AlreadySubscribed")) {
        if (error) {
            *error = reply.errorMessage();
        }
        return false;
    }
    return true;
}

void CouchPlayHelper::startTransientUnit(const QString &username,
                                         const QStringList &gamescopeArgs,
                                         const QStringList &gameCommand,
                                         const QString &workingDirectory,
                                         const QStringList &sharedRoots,
                                         const QStringList &environment,
                                         bool privateTemporaryDirectories,
                                         const QVariantList &directoryPreparations,
                                         const QDBusMessage &request,
                                         const QDBusConnection &replyConnection)
{
    const auto fail = [&](QDBusError::ErrorType type, const QString &reason) {
        replyConnection.send(request.createErrorReply(type, reason));
    };
    if (m_stateRecoveryPending) {
        loadAndReconcileState();
        if (m_stateRecoveryPending) {
            fail(QDBusError::Failed, QStringLiteral("Helper state recovery has not completed; retry shortly"));
            return;
        }
    }
    QString subscriptionError;
    if (!subscribeSystemdManager(&subscriptionError)) {
        fail(QDBusError::Failed, QStringLiteral("Failed to subscribe to systemd Manager: %1").arg(subscriptionError));
        return;
    }
    const QString serviceName = generateServiceName(username);
    if (m_usernameToUnitName.contains(username) || m_pendingLaunches.contains(serviceName)
        || m_launchTokenByUnitName.contains(serviceName)) {
        fail(QDBusError::Failed,
             QStringLiteral("An instance is active, preparing, or has pending cleanup for %1")
                 .arg(username));
        return;
    }

    QList<DirectoryPreparation> plans;
    QString error;
    if (!DirectoryPlan::parseDirectoryPreparations(directoryPreparations, &plans, &error)) {
        fail(QDBusError::InvalidArgs, error);
        return;
    }
    const uint requestUid = callerUid();
    struct passwd *callerRecord = requestUid == INVALID_CALLER_UID ? nullptr : m_ops->getpwuid(requestUid);
    if (!callerRecord || requestUid == INVALID_CALLER_UID) {
        fail(QDBusError::AccessDenied, QStringLiteral("Could not resolve authenticated caller identity"));
        return;
    }
    const uid_t authenticatedCallerUid = callerRecord->pw_uid;
    const gid_t authenticatedCallerGid = callerRecord->pw_gid;
    const QString callerHome = QString::fromLocal8Bit(callerRecord->pw_dir ? callerRecord->pw_dir : "");
    struct passwd *targetRecord = m_ops->getpwnam(username.toLocal8Bit().constData());
    if (!targetRecord) {
        fail(QDBusError::AccessDenied, QStringLiteral("Could not resolve target player identity"));
        return;
    }
    const uid_t targetUid = targetRecord->pw_uid;
    const gid_t targetGid = targetRecord->pw_gid;
    const QString targetHome = QString::fromLocal8Bit(targetRecord->pw_dir ? targetRecord->pw_dir : "");
    const QString targetShell = QString::fromLocal8Bit(targetRecord->pw_shell ? targetRecord->pw_shell : "/bin/sh");
    if (targetUid == 0) {
        fail(QDBusError::AccessDenied, QStringLiteral("Target player must not be root"));
        return;
    }
    const QString callerCanonicalHome = m_ops->canonicalFilePath(callerHome);
    const QString targetCanonicalHome = m_ops->canonicalFilePath(targetHome);
    if (callerHome.isEmpty() || targetHome.isEmpty() || callerCanonicalHome.isEmpty() || targetCanonicalHome.isEmpty()) {
        fail(QDBusError::Failed, QStringLiteral("Could not resolve caller or player home directory"));
        return;
    }
    const auto below = [](const QString &path, const QString &root) {
        return !root.isEmpty() && (path == root || path.startsWith(root + QLatin1Char('/')));
    };
    const auto sourceAllowed = [&](const QString &path) {
        if (below(path, callerHome) || below(path, callerCanonicalHome)) {
            return true;
        }
        if (path.startsWith(QStringLiteral("/home/")) || path.startsWith(QStringLiteral("/var/home/"))) {
            return false;
        }
        return isPathWithinAllowedPrefix(path);
    };
    for (const DirectoryPreparation &plan : plans) {
        const bool privateTmpSource = plan.source.startsWith(QStringLiteral("/tmp/"));
        if (!sourceAllowed(plan.source)) {
            fail(QDBusError::InvalidArgs,
                 QStringLiteral("Directory source is not admitted for this caller: %1").arg(plan.source));
            return;
        }
        // systemd PrivateTmp changes /tmp in this broker's mount namespace. Do not
        // resolve a caller-selected host path here; the bootstrap pins and validates
        // its source FD after entering the host mount namespace.
        if (!privateTmpSource) {
            if (!m_ops->isDirectory(plan.source)) {
                fail(QDBusError::InvalidArgs,
                     QStringLiteral("Directory source is missing or not admitted for this caller: %1").arg(plan.source));
                return;
            }
            const QString canonicalSource = m_ops->canonicalFilePath(plan.source);
            if (canonicalSource.isEmpty() || !sourceAllowed(canonicalSource)) {
                fail(QDBusError::InvalidArgs,
                     QStringLiteral("Directory source resolves outside the caller's homes or permitted external roots: %1")
                         .arg(plan.source));
                return;
            }
            struct stat sourceStat {};
            if (!m_ops->statPath(canonicalSource, &sourceStat) || !S_ISDIR(sourceStat.st_mode)) {
                fail(QDBusError::InvalidArgs,
                     QStringLiteral("Directory source is not a stable directory: %1").arg(plan.source));
                return;
            }
            if ((below(plan.source, callerHome) || below(plan.source, callerCanonicalHome)
                 || below(canonicalSource, callerHome) || below(canonicalSource, callerCanonicalHome))
                && sourceStat.st_uid != requestUid) {
                fail(QDBusError::AccessDenied,
                     QStringLiteral("Directory source is not owned by the authenticated caller: %1").arg(plan.source));
                return;
            }
        }
        if (!plan.stagingPath.isEmpty()) {
            const bool privateTmpStaging = plan.stagingPath.startsWith(QStringLiteral("/tmp/"));
            if ((plan.mode != QStringLiteral("copy") && plan.mode != QStringLiteral("overlay"))
                || !sourceAllowed(plan.stagingPath)) {
                fail(QDBusError::AccessDenied,
                     QStringLiteral("Staging directory is missing or not owned by the authenticated caller: %1")
                         .arg(plan.stagingPath));
                return;
            }
            if (!privateTmpStaging) {
                const QString canonicalStaging = m_ops->canonicalFilePath(plan.stagingPath);
                struct stat stagingStat {};
                if (!m_ops->isDirectory(plan.stagingPath) || canonicalStaging.isEmpty()
                    || !sourceAllowed(canonicalStaging) || !m_ops->statPath(canonicalStaging, &stagingStat)
                    || !S_ISDIR(stagingStat.st_mode) || stagingStat.st_uid != requestUid) {
                    fail(QDBusError::AccessDenied,
                         QStringLiteral("Staging directory is missing or not owned by the authenticated caller: %1")
                             .arg(plan.stagingPath));
                    return;
                }
            }
        }
    }

    for (const QString &root : sharedRoots) {
        if (!QDir::isAbsolutePath(root) || DirectoryPlan::directoryPathHasTraversal(root)
            || root.startsWith(QStringLiteral("/tmp/")) || !sourceAllowed(root)) {
            fail(QDBusError::InvalidArgs, QStringLiteral("Invalid shared root: %1").arg(root));
            return;
        }
        const QString canonicalRoot = m_ops->canonicalFilePath(root);
        if (canonicalRoot.isEmpty() || !sourceAllowed(canonicalRoot)) {
            fail(QDBusError::InvalidArgs, QStringLiteral("Invalid shared root: %1").arg(root));
            return;
        }
    }
    const QString selectedWorkingDirectory = workingDirectory.isEmpty() ? targetHome : workingDirectory;
    if (!QDir::isAbsolutePath(selectedWorkingDirectory)
        || DirectoryPlan::directoryPathHasTraversal(selectedWorkingDirectory)) {
        fail(QDBusError::InvalidArgs, QStringLiteral("Working directory must be an absolute traversal-free path"));
        return;
    }
    bool workingDirectoryAllowed = selectedWorkingDirectory == targetHome
        || selectedWorkingDirectory.startsWith(targetHome + QLatin1Char('/'))
        || selectedWorkingDirectory == targetCanonicalHome
        || selectedWorkingDirectory.startsWith(targetCanonicalHome + QLatin1Char('/'));
    for (const DirectoryPreparation &plan : plans) {
        if (!plan.targetAlias.isEmpty()) {
            const QString projected = QDir(targetHome).filePath(plan.targetAlias);
            workingDirectoryAllowed = workingDirectoryAllowed || selectedWorkingDirectory == projected
                || selectedWorkingDirectory.startsWith(projected + QLatin1Char('/'));
        }
    }
    for (const QString &root : sharedRoots) {
        workingDirectoryAllowed = workingDirectoryAllowed || selectedWorkingDirectory == root
            || selectedWorkingDirectory.startsWith(root + QLatin1Char('/'));
    }
    if (!workingDirectoryAllowed) {
        fail(QDBusError::InvalidArgs,
             QStringLiteral("Working directory is not in the player home or a declared shared root"));
        return;
    }

    const QString userUid = QString::number(targetUid);
    const QString userRuntimeDir = QStringLiteral("/run/user/%1").arg(userUid);
    if (!m_ops->fileExists(userRuntimeDir) || !m_ops->fileExists(userRuntimeDir + QStringLiteral("/bus"))) {
        fail(QDBusError::Failed, QStringLiteral("Target user session bus is unavailable"));
        return;
    }
    uint runtimeUid = requestUid;

    QMap<QString, QString> gameEnvironment;
    gameEnvironment.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"),
                           QStringLiteral("unix:path=%1/bus").arg(userRuntimeDir));
    gameEnvironment.insert(QStringLiteral("WAYLAND_DISPLAY"),
                           waylandSocketPath(QStringLiteral("/run/user/%1").arg(runtimeUid)));
    gameEnvironment.insert(QStringLiteral("XDG_RUNTIME_DIR"), userRuntimeDir);
    gameEnvironment.insert(QStringLiteral("PIPEWIRE_RUNTIME_DIR"), QStringLiteral("/run/user/%1").arg(runtimeUid));
    gameEnvironment.insert(QStringLiteral("PULSE_SERVER"), QStringLiteral("tcp:127.0.0.1:4713"));
    gameEnvironment.insert(QStringLiteral("HOME"), targetHome);
    gameEnvironment.insert(QStringLiteral("USER"), username);
    gameEnvironment.insert(QStringLiteral("LOGNAME"), username);
    gameEnvironment.insert(QStringLiteral("SHELL"), targetShell);
    gameEnvironment.insert(QStringLiteral("PATH"), QStringLiteral("%1/.local/bin:/usr/local/bin:/usr/bin:/bin").arg(targetHome));
    gameEnvironment.insert(QStringLiteral("PWD"), selectedWorkingDirectory);
    gameEnvironment.insert(QStringLiteral("XDG_CONFIG_HOME"), targetHome + QStringLiteral("/.config"));
    gameEnvironment.insert(QStringLiteral("XDG_DATA_HOME"), targetHome + QStringLiteral("/.local/share"));
    gameEnvironment.insert(QStringLiteral("XDG_STATE_HOME"), targetHome + QStringLiteral("/.local/state"));
    gameEnvironment.insert(QStringLiteral("XDG_CACHE_HOME"), targetHome + QStringLiteral("/.cache"));

    static const QSet<QString> protectedEnvironmentNames = {
        QStringLiteral("HOME"), QStringLiteral("USER"), QStringLiteral("LOGNAME"), QStringLiteral("SHELL"),
        QStringLiteral("PATH"), QStringLiteral("PWD"), QStringLiteral("XDG_CONFIG_HOME"),
        QStringLiteral("XDG_DATA_HOME"), QStringLiteral("XDG_STATE_HOME"), QStringLiteral("XDG_CACHE_HOME"),
        QStringLiteral("XDG_RUNTIME_DIR"), QStringLiteral("DBUS_SESSION_BUS_ADDRESS"),
        QStringLiteral("WAYLAND_DISPLAY"), QStringLiteral("PIPEWIRE_RUNTIME_DIR"), QStringLiteral("PULSE_SERVER"),
        QStringLiteral("NOTIFY_SOCKET"), QStringLiteral("LISTEN_PID"), QStringLiteral("LISTEN_FDS"),
        QStringLiteral("LISTEN_FDNAMES"), QStringLiteral("INVOCATION_ID"), QStringLiteral("JOURNAL_STREAM"),
        QStringLiteral("SYSTEMD_EXEC_PID"), QStringLiteral("WATCHDOG_USEC"), QStringLiteral("WATCHDOG_PID")};
    static const QRegularExpression environmentNamePattern(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
    for (const QString &assignment : environment) {
        const qsizetype equals = assignment.indexOf(QLatin1Char('='));
        const QString name = equals < 0 ? QString() : assignment.left(equals);
        if (equals <= 0 || !environmentNamePattern.match(name).hasMatch()
            || assignment.contains(QChar::Null) || assignment.contains(QChar(0x0a))
            || assignment.contains(QChar(0x0d)) || protectedEnvironmentNames.contains(name)
            || (name.startsWith(QStringLiteral("XDG_")) && name != QStringLiteral("XDG_CURRENT_DESKTOP"))
            || name.startsWith(QStringLiteral("DBUS_")) || name.startsWith(QStringLiteral("WAYLAND_"))
            || name.startsWith(QStringLiteral("PIPEWIRE_"))
            || (name.startsWith(QStringLiteral("PULSE_")) && name != QStringLiteral("PULSE_SINK"))
            || name.startsWith(QStringLiteral("LISTEN_")) || name.startsWith(QStringLiteral("WATCHDOG_"))) {
            fail(QDBusError::InvalidArgs, QStringLiteral("Invalid or protected environment assignment"));
            return;
        }
        gameEnvironment.insert(name, assignment.mid(equals + 1));
    }

    const QString gamescopePath = findGamescopePath();
    const QString helperPath = QCoreApplication::applicationFilePath();
    if (gamescopePath.isEmpty() || !QDir::isAbsolutePath(helperPath)) {
        fail(QDBusError::Failed, QStringLiteral("Gamescope or installed helper executable was not found"));
        return;
    }
    LaunchManifest manifest;
    manifest.schemaVersion = 1;
    manifest.unitName = serviceName;
    manifest.callerUid = authenticatedCallerUid;
    manifest.callerGid = authenticatedCallerGid;
    manifest.targetUsername = username;
    manifest.targetUid = targetUid;
    manifest.targetGid = targetGid;
    manifest.declaredHome = callerHome;
    manifest.canonicalHome = callerCanonicalHome;
    manifest.targetDeclaredHome = targetHome;
    manifest.targetCanonicalHome = targetCanonicalHome;
    manifest.directories = plans;
    manifest.gamescopeArgv << gamescopePath << gamescopeArgs << QStringLiteral("--") << gameCommand;
    manifest.workingDirectory = selectedWorkingDirectory;
    manifest.environment = gameEnvironment;

    QList<int> leaseFds;
    QStringList leasePaths;
    if (!acquireOverlayLeases(username, plans, &leaseFds, &leasePaths, &error)) {
        fail(QDBusError::Failed, error);
        return;
    }
    QString token;
    const bool manifestWritten = m_ops->writeLaunchManifest(manifest, &token, &error);
    if (!manifestWritten || !DirectoryPlan::isLaunchToken(token)) {
        if (DirectoryPlan::isLaunchToken(token)) {
            QString ignored;
            m_ops->unlinkLaunchManifest(token, &ignored);
        }
        for (int fd : leaseFds) {
            ::close(fd);
        }
        fail(QDBusError::Failed, error.isEmpty() ? QStringLiteral("Failed to create a secure launch manifest") : error);
        return;
    }

    if (!m_runtimeAccessSetForUid.contains(requestUid) && !SetupRuntimeAccess()) {
        qWarning() << "Failed to set up runtime access for compositor" << requestUid;
    }
    const QString helperPathArg = helperPath;
    const QStringList bootstrapArgv{helperPathArg, QStringLiteral("--run-instance"), token};
    const QList<SystemdExecCommand> execStart = {
        SystemdExecCommand{helperPathArg, bootstrapArgv, {QStringLiteral("no-setuid")}}};
    QList<SystemdProperty> properties;
    const auto addProperty = [&properties](const QString &name, const QVariant &value) {
        properties.append(SystemdProperty{name, QDBusVariant(value)});
    };
    addProperty(QStringLiteral("Type"), QStringLiteral("notify"));
    addProperty(QStringLiteral("User"), username);
    addProperty(QStringLiteral("Delegate"), true);
    addProperty(QStringLiteral("KillMode"), QStringLiteral("control-group"));
    addProperty(QStringLiteral("SendSIGKILL"), true);
    addProperty(QStringLiteral("TimeoutStopUSec"), QVariant::fromValue<qulonglong>(10'000'000));
    addProperty(QStringLiteral("TimeoutStartUSec"), QVariant::fromValue<qulonglong>(60'000'000));
    addProperty(QStringLiteral("NotifyAccess"), QStringLiteral("main"));
    addProperty(QStringLiteral("CollectMode"), QStringLiteral("inactive-or-failed"));
    addProperty(QStringLiteral("PrivateMounts"), true);
    addProperty(QStringLiteral("PrivateTmp"), privateTemporaryDirectories);
    addProperty(QStringLiteral("MemoryDenyWriteExecute"), false);
    addProperty(QStringLiteral("WorkingDirectory"), QStringLiteral("/"));
    addProperty(QStringLiteral("ExecStartEx"), QVariant::fromValue(execStart));
    if (runtimeUid != targetUid) {
        const QString runtimePath = QStringLiteral("/run/user/%1").arg(runtimeUid);
        const QList<SystemdBindPath> runtimeBindings{SystemdBindPath{runtimePath, runtimePath, false, 0}};
        addProperty(QStringLiteral("BindReadOnlyPaths"), QVariant::fromValue(runtimeBindings));
    }

    const quint64 unitGeneration = ++m_nextUnitGeneration;
    m_unitGenerationByName.insert(serviceName, unitGeneration);
    m_failedJobResults.remove(serviceName);
    m_readyUnitNames.remove(serviceName);
    m_initialInactiveUnitNames.remove(serviceName);
    m_announcedReadyUnitNames.remove(serviceName);
    m_stoppingUnits.remove(serviceName);
    const QString expectedUnitPath = systemdUnitObjectPath(serviceName);
    auto *propertyMonitor = new UnitMonitor(expectedUnitPath, serviceName, unitGeneration, this);
    auto *jobMonitor = new UnitJobMonitor(serviceName, this);
    jobMonitor->setGeneration(unitGeneration);
    m_jobMonitors.insert(serviceName, jobMonitor);
    m_usernameToUnitName.insert(username, serviceName);
    m_unitUsernameByName.insert(serviceName, username);
    m_unitOwnerUid.insert(serviceName, requestUid);
    m_monitors.insert(serviceName, propertyMonitor);
    m_targetIdentityByUnit.insert(serviceName, TargetIdentity{targetUid, targetGid, targetHome, targetCanonicalHome});
    m_launchTokenByUnitName.insert(serviceName, token);
    m_overlayLeaseFdsByUnit.insert(serviceName, leaseFds);
    m_overlayLeasePathsByUnit.insert(serviceName, leasePaths);
    m_compositorUidForUsername.insert(username, requestUid);
    if (!saveState()) {
        releaseOwnedUnit(serviceName, true);
        fail(QDBusError::Failed, QStringLiteral("Failed to persist instance ownership before startup"));
        return;
    }
    auto *pending = new PendingLaunch(replyConnection);
    pending->request = request;
    pending->replyConnection = replyConnection;
    pending->username = username;
    pending->token = token;
    pending->callerUid = requestUid;
    pending->pollTimer = new QTimer(this);
    pending->pollTimer->setInterval(100);
    pending->deadlineTimer = new QTimer(this);
    pending->deadlineTimer->setSingleShot(true);
    m_pendingLaunches.insert(serviceName, pending);
    const QDBusObjectPath jobPath = m_ops->startTransientService(serviceName, properties, &error);
    const bool jobPathReturned = jobPath.path().startsWith(QStringLiteral("/org/freedesktop/systemd1/job/"));
    if (jobPathReturned) {
        jobMonitor->setExpectedJobPath(jobPath.path());
    }
    if (!jobPathReturned) {
        const QString failureReason = error.isEmpty() ? QStringLiteral("systemd did not return a valid job path") : error;
        pending->replySent = true;
        pending->replyConnection.send(request.createErrorReply(QDBusError::Failed, failureReason));
        failPendingLaunch(serviceName, failureReason);
        return;
    }
    const quint64 generation = m_unitGenerationByName.value(serviceName, 0);
    connect(pending->pollTimer, &QTimer::timeout, this, [this, serviceName, generation]() {
        if (m_unitGenerationByName.value(serviceName, 0) == generation) {
            pollUnitMainPid(serviceName);
        }
    });
    connect(pending->deadlineTimer, &QTimer::timeout, this, [this, serviceName, generation]() {
        if (m_unitGenerationByName.value(serviceName, 0) == generation) {
            failPendingLaunch(serviceName, QStringLiteral("Timed out waiting for systemd to report the preparing PID"));
        }
    });
    pending->pollTimer->start();
    pending->deadlineTimer->start(5000);
    pollUnitMainPid(serviceName);
}

bool CouchPlayHelper::acquireOverlayLeases(const QString &username,
                                           const QList<DirectoryPreparation> &plans,
                                           QList<int> *leases,
                                           QStringList *leasePaths,
                                           QString *error)
{
    if (!leases || !leasePaths) {
        if (error) {
            *error = QStringLiteral("Invalid overlay lease output");
        }
        return false;
    }
    leases->clear();
    leasePaths->clear();
    bool hasOverlay = false;
    for (const DirectoryPreparation &plan : plans) {
        hasOverlay = hasOverlay || plan.mode == QStringLiteral("overlay");
    }
    if (!hasOverlay) {
        return true;
    }
    struct passwd *target = m_ops->getpwnam(username.toLocal8Bit().constData());
    if (!target) {
        if (error) {
            *error = QStringLiteral("Could not resolve overlay owner for %1").arg(username);
        }
        return false;
    }
    const int varLibFd = SecureFs::openBaseDir(QStringLiteral("/var/lib"));
    if (varLibFd < 0) {
        if (error) {
            *error = QStringLiteral("Could not securely open overlay storage: %1")
                         .arg(QString::fromLocal8Bit(strerror(-varLibFd)));
        }
        return false;
    }
    QSet<QString> seenLocks;
    auto closeAll = [&]() {
        for (int fd : *leases) {
            ::close(fd);
        }
        leases->clear();
        leasePaths->clear();
    };
    const auto ensurePrivateDirectory = [&](int parentFd, const char *name, QString *failure) {
        struct stat before {};
        bool created = false;
        if (::fstatat(parentFd, name, &before, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno != ENOENT || ::mkdirat(parentFd, name, 0700) != 0) {
                *failure = QStringLiteral("Could not create overlay %1 directory: %2")
                               .arg(QString::fromLatin1(name), QString::fromLocal8Bit(strerror(errno)));
                return false;
            }
            created = true;
        } else if (!S_ISDIR(before.st_mode)) {
            *failure = QStringLiteral("Overlay %1 path is not a real directory").arg(QString::fromLatin1(name));
            return false;
        }
        const int fd = ::openat(parentFd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
            *failure = QStringLiteral("Could not open overlay %1 directory: %2")
                           .arg(QString::fromLatin1(name), QString::fromLocal8Bit(strerror(errno)));
            return false;
        }
        struct stat st {};
        if (::fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode)
            || (!created && (st.st_dev != before.st_dev || st.st_ino != before.st_ino))) {
            *failure = QStringLiteral("Overlay %1 path changed or is not a directory").arg(QString::fromLatin1(name));
            ::close(fd);
            return false;
        }
        if (created && (::fchown(fd, target->pw_uid, target->pw_gid) != 0 || ::fchmod(fd, 0700) != 0)) {
            *failure = QStringLiteral("Could not protect new overlay %1 directory: %2")
                           .arg(QString::fromLatin1(name), QString::fromLocal8Bit(strerror(errno)));
            ::close(fd);
            return false;
        }
        // Older helpers created target-owned upper/work directories as 0755.
        // Normalize only the pinned directory after authenticating both owners.
        if (!created && st.st_uid == target->pw_uid && st.st_gid == target->pw_gid
            && ::fchmod(fd, 0700) != 0) {
            *failure = QStringLiteral("Could not protect existing overlay %1 directory: %2")
                           .arg(QString::fromLatin1(name), QString::fromLocal8Bit(strerror(errno)));
            ::close(fd);
            return false;
        }
        if (::fstat(fd, &st) != 0) {
            *failure = QStringLiteral("Could not verify overlay %1 ownership").arg(QString::fromLatin1(name));
            ::close(fd);
            return false;
        }
        const bool finalOk = st.st_uid == target->pw_uid && st.st_gid == target->pw_gid
            && (st.st_mode & 0077) == 0 && (st.st_mode & 0700) == 0700;
        ::close(fd);
        if (!finalOk) {
            *failure = QStringLiteral("Existing overlay %1 directory has unsafe owner or permissions")
                           .arg(QString::fromLatin1(name));
            return false;
        }
        return true;
    };
    for (const DirectoryPreparation &plan : plans) {
        if (plan.mode != QStringLiteral("overlay")) {
            continue;
        }
        const QString sourceHash = QString::fromLatin1(
            QCryptographicHash::hash(plan.source.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
        const QString basePath = QStringLiteral("/var/lib/couchplay/overlays/%1/%2").arg(username, sourceHash);
        const QString lockPath = basePath + QStringLiteral("/.lease");
        if (seenLocks.contains(lockPath)) {
            continue;
        }
        const int baseFd = SecureFs::openDirBelow(varLibFd,
                                                  {QStringLiteral("couchplay"), QStringLiteral("overlays"), username, sourceHash},
                                                  true,
                                                  0,
                                                  0,
                                                  SecureFs::ChownMode::FinalOnly);
        if (baseFd < 0) {
            if (error) {
                *error = QStringLiteral("Could not securely open overlay storage for %1: %2")
                             .arg(plan.source, QString::fromLocal8Bit(strerror(-baseFd)));
            }
            ::close(varLibFd);
            closeAll();
            return false;
        }
        struct stat baseStat {};
        if (::fstat(baseFd, &baseStat) != 0 || !S_ISDIR(baseStat.st_mode) || baseStat.st_uid != 0
            || baseStat.st_gid != 0 || (baseStat.st_mode & 0022) != 0) {
            if (error) {
                *error = QStringLiteral("Overlay storage parent is not protected for %1").arg(plan.source);
            }
            ::close(baseFd);
            ::close(varLibFd);
            closeAll();
            return false;
        }

        int leaseFd = ::openat(baseFd,
                               ".lease",
                               O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                               0600);
        bool createdLease = leaseFd >= 0;
        if (leaseFd < 0 && errno == EEXIST) {
            leaseFd = ::openat(baseFd, ".lease", O_RDWR | O_NOFOLLOW | O_CLOEXEC);
        }
        if (leaseFd < 0) {
            if (error) {
                *error = QStringLiteral("Could not open overlay lease for %1: %2")
                             .arg(plan.source, QString::fromLocal8Bit(strerror(errno)));
            }
            ::close(baseFd);
            ::close(varLibFd);
            closeAll();
            return false;
        }
        struct stat lockStat {};
        if ((createdLease && (::fchown(leaseFd, 0, 0) != 0 || ::fchmod(leaseFd, 0600) != 0))
            || ::fstat(leaseFd, &lockStat) != 0 || !S_ISREG(lockStat.st_mode) || lockStat.st_uid != 0
            || lockStat.st_gid != 0 || (lockStat.st_mode & 0777) != 0600 || lockStat.st_nlink != 1) {
            if (error) {
                *error = QStringLiteral("Overlay lease file is not a protected root-owned regular file for %1")
                             .arg(plan.source);
            }
            ::close(leaseFd);
            ::close(baseFd);
            ::close(varLibFd);
            closeAll();
            return false;
        }
        if (::flock(leaseFd, LOCK_EX | LOCK_NB) != 0) {
            if (error) {
                *error = QStringLiteral("Overlay storage is already in use for %1").arg(plan.source);
            }
            ::close(leaseFd);
            ::close(baseFd);
            ::close(varLibFd);
            closeAll();
            return false;
        }
        QString directoryError;
        if (!ensurePrivateDirectory(baseFd, "upper", &directoryError)
            || !ensurePrivateDirectory(baseFd, "work", &directoryError)) {
            if (error) {
                *error = directoryError;
            }
            ::close(leaseFd);
            ::close(baseFd);
            ::close(varLibFd);
            closeAll();
            return false;
        }
        ::close(baseFd);
        leases->append(leaseFd);
        leasePaths->append(lockPath);
        seenLocks.insert(lockPath);
    }
    ::close(varLibFd);
    return true;
}

bool CouchPlayHelper::reacquireOverlayLeases(const QString &username,
                                             const QStringList &leasePaths,
                                             QList<int> *leases,
                                             QString *error)
{
    if (!leases || !s_validUsername.match(username).hasMatch()) {
        if (error) {
            *error = QStringLiteral("Invalid overlay lease recovery identity");
        }
        return false;
    }
    leases->clear();
    if (leasePaths.isEmpty()) {
        return true;
    }
    struct passwd *record = m_ops->getpwnam(username.toLocal8Bit().constData());
    if (!record) {
        if (error) {
            *error = QStringLiteral("Could not resolve overlay recovery owner for %1").arg(username);
        }
        return false;
    }
    const uid_t targetUid = record->pw_uid;
    const gid_t targetGid = record->pw_gid;
    const QString prefix = QStringLiteral("/var/lib/couchplay/overlays/%1/").arg(username);
    static const QRegularExpression hashPattern(QStringLiteral("^[0-9a-f]{16}$"));
    const int varLibFd = SecureFs::openBaseDir(QStringLiteral("/var/lib"));
    if (varLibFd < 0) {
        if (error) {
            *error = QStringLiteral("Could not securely open overlay recovery root: %1")
                         .arg(QString::fromLocal8Bit(strerror(-varLibFd)));
        }
        return false;
    }
    QSet<QString> seen;
    const auto closeAll = [&]() {
        for (int fd : *leases) {
            ::close(fd);
        }
        leases->clear();
    };
    for (const QString &path : leasePaths) {
        if (seen.contains(path)) {
            continue;
        }
        if (!path.startsWith(prefix) || !path.endsWith(QStringLiteral("/.lease"))) {
            if (error) {
                *error = QStringLiteral("Invalid persisted overlay lease path");
            }
            ::close(varLibFd);
            closeAll();
            return false;
        }
        const QString hash = path.mid(prefix.size(), path.size() - prefix.size() - QStringLiteral("/.lease").size());
        if (!hashPattern.match(hash).hasMatch() || path != prefix + hash + QStringLiteral("/.lease")) {
            if (error) {
                *error = QStringLiteral("Invalid persisted overlay lease identity");
            }
            ::close(varLibFd);
            closeAll();
            return false;
        }
        const int baseFd = SecureFs::openDirBelow(varLibFd,
                                                  {QStringLiteral("couchplay"), QStringLiteral("overlays"), username, hash},
                                                  false,
                                                  0,
                                                  0,
                                                  SecureFs::ChownMode::FinalOnly);
        if (baseFd < 0) {
            if (error) {
                *error = QStringLiteral("Could not open persisted overlay storage: %1")
                             .arg(QString::fromLocal8Bit(strerror(-baseFd)));
            }
            ::close(varLibFd);
            closeAll();
            return false;
        }
        struct stat baseStat {};
        const bool protectedBase = ::fstat(baseFd, &baseStat) == 0 && S_ISDIR(baseStat.st_mode)
            && baseStat.st_uid == 0 && baseStat.st_gid == 0 && (baseStat.st_mode & 0022) == 0;
        int leaseFd = protectedBase ? ::openat(baseFd, ".lease", O_RDWR | O_NOFOLLOW | O_CLOEXEC) : -1;
        struct stat lockStat {};
        const bool validLock = leaseFd >= 0 && ::fstat(leaseFd, &lockStat) == 0 && S_ISREG(lockStat.st_mode)
            && lockStat.st_uid == 0 && lockStat.st_gid == 0 && (lockStat.st_mode & 0777) == 0600
            && lockStat.st_nlink == 1;
        const bool locked = validLock && ::flock(leaseFd, LOCK_EX | LOCK_NB) == 0;
        bool validLayers = locked;
        for (const char *leaf : {"upper", "work"}) {
            if (!validLayers) {
                break;
            }
            const int layerFd = ::openat(baseFd, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            struct stat layerStat {};
            validLayers = layerFd >= 0 && ::fstat(layerFd, &layerStat) == 0 && S_ISDIR(layerStat.st_mode)
                && layerStat.st_uid == targetUid && layerStat.st_gid == targetGid
                && (layerStat.st_mode & 0077) == 0 && (layerStat.st_mode & 0700) == 0700;
            if (layerFd >= 0) {
                ::close(layerFd);
            }
        }
        ::close(baseFd);
        if (!validLayers) {
            if (leaseFd >= 0) {
                ::close(leaseFd);
            }
            if (error) {
                *error = QStringLiteral("Persisted overlay storage has unsafe ownership or is already in use");
            }
            ::close(varLibFd);
            closeAll();
            return false;
        }
        leases->append(leaseFd);
        seen.insert(path);
    }
    ::close(varLibFd);
    return true;
}

void CouchPlayHelper::pollUnitMainPid(const QString &serviceName)
{
    const quint64 generation = m_unitGenerationByName.value(serviceName, 0);
    if (generation == 0 || m_pidPollGenerationByUnit.value(serviceName, 0) == generation) {
        return;
    }
    PendingLaunch *pending = m_pendingLaunches.value(serviceName, nullptr);
    const QDBusObjectPath unitPath = m_unitPathForUnit.value(serviceName);
    const QString username = usernameForUnit(serviceName);
    if (username.isEmpty() || (pending && (pending->replySent || pending->pollInFlight))) {
        return;
    }
    const auto scheduleRecoveryPoll = [this, serviceName, generation]() {
        if (m_pendingLaunches.contains(serviceName)) {
            return;
        }
        QTimer::singleShot(100, this, [this, serviceName, generation]() {
            if (m_unitGenerationByName.value(serviceName, 0) == generation
                && m_pidToUnitName.key(serviceName, 0) <= 0 && m_unitUsernameByName.contains(serviceName)) {
                pollUnitMainPid(serviceName);
            }
        });
    };
    if (unitPath.path().isEmpty()) {
        if (!pending) {
            return;
        }
        pending->pollInFlight = true;
        m_pidPollGenerationByUnit.insert(serviceName, generation);
        QDBusMessage getUnit = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.systemd1"),
                                                              QStringLiteral("/org/freedesktop/systemd1"),
                                                              QStringLiteral("org.freedesktop.systemd1.Manager"),
                                                              QStringLiteral("GetUnit"));
        getUnit << serviceName;
        auto *watcher = new QDBusPendingCallWatcher(m_ops->systemBusConnection().asyncCall(getUnit), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, serviceName, generation]() {
            watcher->deleteLater();
            if (m_unitGenerationByName.value(serviceName, 0) != generation) {
                return;
            }
            if (m_pidPollGenerationByUnit.value(serviceName, 0) == generation) {
                m_pidPollGenerationByUnit.remove(serviceName);
            }
            PendingLaunch *current = m_pendingLaunches.value(serviceName, nullptr);
            if (current) {
                current->pollInFlight = false;
            }
            if (!current || current->replySent || m_unitPathForUnit.contains(serviceName)) {
                return;
            }
            QDBusPendingReply<QDBusObjectPath> reply = *watcher;
            if (reply.isError()) {
                if (reply.error().name() == QStringLiteral("org.freedesktop.systemd1.NoSuchUnit")) {
                    QString reason = QStringLiteral("Instance exited before systemd exposed its preparing PID");
                    if (UnitMonitor *monitor = m_monitors.value(serviceName, nullptr)) {
                        const QString status = monitor->statusText();
                        if (!status.isEmpty()) {
                            reason = QStringLiteral("Instance preparation failed: %1").arg(status);
                            if (monitor->statusErrno() != 0) {
                                reason += QStringLiteral(" (errno %1)").arg(monitor->statusErrno());
                            }
                        }
                    }
                    current->replySent = true;
                    current->replyConnection.send(current->request.createErrorReply(QDBusError::Failed, reason));
                    releaseOwnedUnit(serviceName, true);
                }
                return;
            }
            const QDBusObjectPath resolvedPath = reply.value();
            if (resolvedPath.path() != systemdUnitObjectPath(serviceName)) {
                failPendingLaunch(serviceName, QStringLiteral("systemd returned an unexpected unit path"));
                return;
            }
            m_unitPathForUnit.insert(serviceName, resolvedPath);
            monitorUnitState(serviceName, current->username, resolvedPath);
            if (!m_failedJobResults.take(serviceName).isEmpty()) {
                onUnitStopped(serviceName, QString());
            }
            pollUnitMainPid(serviceName);
        });
        return;
    }
    if (!pending && m_pidToUnitName.key(serviceName, 0) > 0) {
        return;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.systemd1"),
                                                       unitPath.path(),
                                                       QStringLiteral("org.freedesktop.DBus.Properties"),
                                                       QStringLiteral("Get"));
    call << QStringLiteral("org.freedesktop.systemd1.Service") << QStringLiteral("MainPID");
    if (pending) {
        pending->pollInFlight = true;
    }
    m_pidPollGenerationByUnit.insert(serviceName, generation);
    auto *watcher = new QDBusPendingCallWatcher(m_ops->systemBusConnection().asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, serviceName, generation, scheduleRecoveryPoll]() {
        watcher->deleteLater();
        if (m_unitGenerationByName.value(serviceName, 0) != generation) {
            return;
        }
        if (m_pidPollGenerationByUnit.value(serviceName, 0) == generation) {
            m_pidPollGenerationByUnit.remove(serviceName);
        }
        PendingLaunch *current = m_pendingLaunches.value(serviceName, nullptr);
        if (current) {
            current->pollInFlight = false;
        }
        if (!m_unitPathForUnit.contains(serviceName)) {
            scheduleRecoveryPoll();
            return;
        }
        QDBusPendingReply<QDBusVariant> reply = *watcher;
        if (reply.isError()) {
            scheduleRecoveryPoll();
            return;
        }
        bool ok = false;
        const qlonglong pid = reply.value().variant().toLongLong(&ok);
        if (!ok || pid <= 0) {
            scheduleRecoveryPoll();
            return;
        }
        if (current && !current->replySent) {
            completePendingLaunch(serviceName, pid);
            return;
        }
        const QString username = usernameForUnit(serviceName);
        const uint ownerUid = m_unitOwnerUid.value(serviceName, INVALID_CALLER_UID);
        if (username.isEmpty() || ownerUid == INVALID_CALLER_UID) {
            return;
        }
        m_pidToUsername.insert(pid, username);
        m_pidToUnitName.insert(pid, serviceName);
        m_pidOwnerUid.insert(pid, ownerUid);
        saveState();
        publishUnitReady(serviceName);
    });
}

void CouchPlayHelper::completePendingLaunch(const QString &serviceName, qint64 pid)
{
    PendingLaunch *pending = m_pendingLaunches.value(serviceName, nullptr);
    if (!pending || pending->replySent || pid <= 0) {
        return;
    }
    pending->replySent = true;
    pending->pollTimer->stop();
    pending->deadlineTimer->stop();
    m_pidToUsername.insert(pid, pending->username);
    m_pidToUnitName.insert(pid, serviceName);
    m_pidOwnerUid.insert(pid, pending->callerUid);
    m_unitOwnerUid.insert(serviceName, pending->callerUid);
    const QDBusMessage request = pending->request;
    const QDBusConnection replyConnection = pending->replyConnection;
    replyConnection.send(request.createReply(QVariantList{QVariant::fromValue<qlonglong>(pid)}));
    m_pendingLaunches.remove(serviceName);
    delete pending->pollTimer;
    delete pending->deadlineTimer;
    delete pending;
    saveState();
    if (m_initialInactiveUnitNames.remove(serviceName)) {
        onUnitStopped(serviceName, QString());
    }
    publishUnitReady(serviceName);
}

void CouchPlayHelper::failPendingLaunch(const QString &serviceName, const QString &reason)
{
    PendingLaunch *pending = m_pendingLaunches.value(serviceName, nullptr);
    if (!pending) {
        return;
    }
    const quint64 generation = m_unitGenerationByName.value(serviceName, 0);
    const bool pidAcknowledged = m_pidToUnitName.key(serviceName, 0) > 0;
    pending->pollTimer->stop();
    pending->deadlineTimer->stop();
    if (!pending->replySent) {
        pending->replySent = true;
        pending->replyConnection.send(pending->request.createErrorReply(QDBusError::Failed, reason));
    }
    if (stopServiceInstance(serviceName)) {
        if (pidAcknowledged) {
            finalizeStoppedUnit(serviceName, reason);
        } else {
            releaseOwnedUnit(serviceName, true);
        }
    } else {
        qWarning() << "Could not stop failed launch unit" << serviceName << ":" << reason;
        saveState();
        QTimer::singleShot(1000, this, [this, serviceName, generation, pidAcknowledged, reason]() {
            if (m_unitGenerationByName.value(serviceName, 0) != generation
                || !m_pendingLaunches.contains(serviceName)) {
                return;
            }
            if (stopServiceInstance(serviceName)) {
                if (pidAcknowledged) {
                    finalizeStoppedUnit(serviceName, reason);
                } else {
                    releaseOwnedUnit(serviceName, true);
                }
                return;
            }
            QTimer::singleShot(1000, this, [this, serviceName, generation]() {
                if (m_unitGenerationByName.value(serviceName, 0) == generation
                    && m_pendingLaunches.contains(serviceName)) {
                    failPendingLaunch(serviceName, QStringLiteral("Retrying cleanup of failed instance unit"));
                }
            });
        });
    }
}

void CouchPlayHelper::onUnitReady(const QString &serviceName)
{
    if (!m_unitPathForUnit.contains(serviceName)) {
        return;
    }
    if (!m_readyUnitNames.contains(serviceName)) {
        m_readyUnitNames.insert(serviceName);
        saveState();
    }
    pollUnitMainPid(serviceName);
    publishUnitReady(serviceName);

}

void CouchPlayHelper::publishUnitReady(const QString &serviceName)
{
    if (m_terminalStateQueries.contains(serviceName) || !m_readyUnitNames.contains(serviceName)
        || m_announcedReadyUnitNames.contains(serviceName)) {
        return;
    }
    const QString username = usernameForUnit(serviceName);
    if (username.isEmpty()) {
        return;
    }
    qint64 pid = 0;
    for (auto it = m_pidToUnitName.constBegin(); it != m_pidToUnitName.constEnd(); ++it) {
        if (it.value() == serviceName) {
            pid = it.key();
            break;
        }
    }
    if (pid <= 0) {
        return;
    }
    m_announcedReadyUnitNames.insert(serviceName);
    saveState();
    Q_EMIT instanceReady(username, pid);
}

void CouchPlayHelper::noteInitialInactiveUnitState(const QString &serviceName)
{
    if (!m_unitPathForUnit.contains(serviceName)) {
        return;
    }
    if (m_pidToUnitName.key(serviceName, 0) > 0) {
        onUnitStopped(serviceName, QString());
        return;
    }
    if (m_pendingLaunches.contains(serviceName)) {
        m_initialInactiveUnitNames.insert(serviceName);
    }
}

void CouchPlayHelper::onUnitStopped(const QString &serviceName, const QString &reason)
{
    const quint64 generation = m_unitGenerationByName.value(serviceName, 0);
    if (!m_unitPathForUnit.contains(serviceName) || generation == 0 || m_stoppingUnits.contains(serviceName)
        || m_terminalStateQueries.contains(serviceName)) {
        return;
    }
    m_terminalStateQueries.insert(serviceName);
    queryUnitActiveState(serviceName, generation, m_unitPathForUnit.value(serviceName), reason, true);
}

void CouchPlayHelper::queryUnitActiveState(const QString &serviceName,
                                          quint64 generation,
                                          const QDBusObjectPath &unitPath,
                                          const QString &reason,
                                          bool resolveMissingUnit)
{
    if (m_unitGenerationByName.value(serviceName, 0) != generation
        || !m_terminalStateQueries.contains(serviceName)
        || m_unitPathForUnit.value(serviceName).path() != unitPath.path()) {
        return;
    }
    QDBusMessage getActive = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.systemd1"),
                                                            unitPath.path(),
                                                            QStringLiteral("org.freedesktop.DBus.Properties"),
                                                            QStringLiteral("Get"));
    getActive << QStringLiteral("org.freedesktop.systemd1.Unit") << QStringLiteral("ActiveState");
    auto *watcher = new QDBusPendingCallWatcher(m_ops->systemBusConnection().asyncCall(getActive), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, serviceName, generation, unitPath, reason, resolveMissingUnit]() {
        watcher->deleteLater();
        if (m_unitGenerationByName.value(serviceName, 0) != generation
            || !m_terminalStateQueries.contains(serviceName)
            || m_unitPathForUnit.value(serviceName).path() != unitPath.path()) {
            return;
        }
        const QDBusPendingReply<QDBusVariant> stateReply = *watcher;
        if (stateReply.isError()) {
            if (resolveMissingUnit) {
                resolveUnitAfterPropertyError(serviceName, generation, reason);
            } else {
                m_terminalStateQueries.remove(serviceName);
            }
            return;
        }
        const QString activeState = stateReply.value().variant().toString();
        if (activeState == QStringLiteral("active")) {
            m_terminalStateQueries.remove(serviceName);
            onUnitReady(serviceName);
            return;
        }
        if (activeState != QStringLiteral("inactive") && activeState != QStringLiteral("failed")) {
            m_terminalStateQueries.remove(serviceName);
            return;
        }
        if (reason.isEmpty()) {
            queryTerminalUnitProperties(serviceName, generation);
        } else {
            m_terminalStateQueries.remove(serviceName);
            finalizeStoppedUnit(serviceName, reason);
        }
    });
}

void CouchPlayHelper::resolveUnitAfterPropertyError(const QString &serviceName,
                                                    quint64 generation,
                                                    const QString &reason)
{
    if (m_unitGenerationByName.value(serviceName, 0) != generation
        || !m_terminalStateQueries.contains(serviceName)) {
        return;
    }
    QDBusMessage getUnit = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.systemd1"),
                                                          QStringLiteral("/org/freedesktop/systemd1"),
                                                          QStringLiteral("org.freedesktop.systemd1.Manager"),
                                                          QStringLiteral("GetUnit"));
    getUnit << serviceName;
    auto *watcher = new QDBusPendingCallWatcher(m_ops->systemBusConnection().asyncCall(getUnit), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, serviceName, generation, reason]() {
        watcher->deleteLater();
        if (m_unitGenerationByName.value(serviceName, 0) != generation
            || !m_terminalStateQueries.contains(serviceName)) {
            return;
        }
        const QDBusPendingReply<QDBusObjectPath> unitReply = *watcher;
        if (unitReply.isError()) {
            m_terminalStateQueries.remove(serviceName);
            if (unitReply.error().name() != QStringLiteral("org.freedesktop.systemd1.NoSuchUnit")) {
                return;
            }
            QString terminalReason = reason;
            const bool ready = m_readyUnitNames.contains(serviceName);
            if (terminalReason.isEmpty()) {
                terminalReason = ready ? QStringLiteral("exited")
                                      : QStringLiteral("Instance preparation failed before READY");
                if (!ready) {
                    if (UnitMonitor *monitor = m_monitors.value(serviceName, nullptr)) {
                        if (!monitor->statusText().isEmpty()) {
                            terminalReason = QStringLiteral("Instance preparation failed: %1")
                                                 .arg(monitor->statusText());
                            if (monitor->statusErrno() != 0) {
                                terminalReason += QStringLiteral(" (errno %1)").arg(monitor->statusErrno());
                            }
                        }
                    }
                }
            }
            finalizeStoppedUnit(serviceName, terminalReason);
            return;
        }
        if (unitReply.value().path() != systemdUnitObjectPath(serviceName)) {
            m_terminalStateQueries.remove(serviceName);
            return;
        }
        m_unitPathForUnit.insert(serviceName, unitReply.value());
        queryUnitActiveState(serviceName, generation, unitReply.value(), reason, false);
    });
}

void CouchPlayHelper::queueStoppedNotification(const QString &username, qint64 pid, const QString &reason)
{
    m_deferredStoppedNotifications.append(StoppedInstanceNotification{username, pid, reason});
    QMetaObject::invokeMethod(this, [this]() {
        auto notifications = std::move(m_deferredStoppedNotifications);
        m_deferredStoppedNotifications.clear();
        for (const StoppedInstanceNotification &item : notifications) {
            Q_EMIT instanceStopped(item.username, item.pid, item.reason);
        }
    }, Qt::QueuedConnection);
}

void CouchPlayHelper::finalizeStoppedUnit(const QString &serviceName, const QString &reason, bool deferNotification)
{
    if (!m_unitUsernameByName.contains(serviceName)) {
        return;
    }
    if (m_stoppingUnits.contains(serviceName)) {
        return;
    }
    PendingLaunch *pending = m_pendingLaunches.value(serviceName, nullptr);
    if (pending && !pending->replySent) {
        pending->replySent = true;
        pending->replyConnection.send(pending->request.createErrorReply(
            QDBusError::Failed, reason.isEmpty() ? QStringLiteral("Instance stopped before READY") : reason));
    }
    const QString username = usernameForUnit(serviceName);
    const qint64 pid = m_pidToUnitName.key(serviceName, 0);
    if (pid > 0) {
        if (deferNotification) {
            queueStoppedNotification(username, pid, reason);
        } else {
            Q_EMIT instanceStopped(username, pid, reason);
        }
    }
    releaseOwnedUnit(serviceName, true);
}

void CouchPlayHelper::queryTerminalUnitProperties(const QString &serviceName, quint64 generation)
{
    if (m_unitGenerationByName.value(serviceName, 0) != generation
        || !m_unitPathForUnit.contains(serviceName)) {
        return;
    }
    const QDBusObjectPath unitPath = m_unitPathForUnit.value(serviceName);
    QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.systemd1"),
                                                       unitPath.path(),
                                                       QStringLiteral("org.freedesktop.DBus.Properties"),
                                                       QStringLiteral("GetAll"));
    call << QStringLiteral("org.freedesktop.systemd1.Service");
    auto *watcher = new QDBusPendingCallWatcher(m_ops->systemBusConnection().asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, serviceName, generation]() {
        watcher->deleteLater();
        if (m_unitGenerationByName.value(serviceName, 0) != generation) {
            return;
        }
        m_terminalStateQueries.remove(serviceName);
        if (!m_unitPathForUnit.contains(serviceName)) {
            return;
        }
        QDBusPendingReply<QVariantMap> reply = *watcher;
        QVariantMap properties;
        if (!reply.isError()) {
            properties = reply.value();
        }
        QString result = properties.value(QStringLiteral("Result")).toString();
        QString status = properties.value(QStringLiteral("StatusText")).toString();
        int statusErrno = properties.value(QStringLiteral("StatusErrno")).toInt();
        if (UnitMonitor *monitor = m_monitors.value(serviceName, nullptr)) {
            if (result.isEmpty()) {
                result = monitor->result();
            }
            if (status.isEmpty()) {
                status = monitor->statusText();
            }
            if (statusErrno == 0) {
                statusErrno = monitor->statusErrno();
            }
        }
        const bool ready = m_readyUnitNames.contains(serviceName);
        QString reason;
        if (!ready) {
            reason = status.isEmpty() ? QStringLiteral("Instance preparation failed before READY")
                                      : QStringLiteral("Instance preparation failed: %1").arg(status);
            if (statusErrno != 0) {
                reason += QStringLiteral(" (errno %1)").arg(statusErrno);
            }
        } else if (result == QStringLiteral("signal") || result == QStringLiteral("core-dump")) {
            reason = QStringLiteral("crashed");
        } else if (result == QStringLiteral("exit-code") || result == QStringLiteral("success")) {
            reason = QStringLiteral("exited");
        } else {
            reason = result.isEmpty() ? QStringLiteral("failed") : result;
        }
        if (ready && !status.isEmpty()) {
            reason += QStringLiteral(": %1").arg(status);
            if (statusErrno != 0) {
                reason += QStringLiteral(" (errno %1)").arg(statusErrno);
            }
        }
        onUnitStopped(serviceName, reason);
    });
}

bool CouchPlayHelper::readLaunchManifestForUnit(const QString &serviceName,
                                                const QString &token,
                                                LaunchManifest *manifest,
                                                QString *error) const
{
    if (!manifest || !DirectoryPlan::isLaunchToken(token)) {
        if (error) {
            *error = QStringLiteral("Invalid launch manifest recovery identity");
        }
        return false;
    }
    if (!m_ops->readLaunchManifest(token, manifest, error)) {
        return false;
    }
    if (manifest->unitName != serviceName) {
        if (error) {
            *error = QStringLiteral("Launch manifest does not belong to %1").arg(serviceName);
        }
        return false;
    }
    return true;
}

bool CouchPlayHelper::cleanupCopyScratchForUnit(const QString &serviceName, const QString &token) const
{
    LaunchManifest manifest;
    QString error;
    if (!readLaunchManifestForUnit(serviceName, token, &manifest, &error)) {
        qWarning() << "Could not validate inactive launch manifest for" << serviceName << ":" << error;
        return false;
    }
    bool hasCopyDirectories = false;
    for (const DirectoryRecord &directory : manifest.directories) {
        if (directory.mode == QStringLiteral("copy")) {
            hasCopyDirectories = true;
            break;
        }
    }
    if (hasCopyDirectories && !InstanceBootstrap::cleanupCopyScratchDirectories(manifest, token, &error)) {
        qWarning() << "Could not clean copy scratch directories for" << serviceName << ":" << error;
        return false;
    }
    return true;
}

void CouchPlayHelper::releaseOwnedUnit(const QString &serviceName, bool unlinkManifest)
{
    if (m_reconcilingState) {
        m_deferredReleaseUnits.insert(serviceName);
        return;
    }
    m_deferredReleaseUnits.remove(serviceName);
    QString username = usernameForUnit(serviceName);
    PendingLaunch *pending = m_pendingLaunches.value(serviceName, nullptr);
    if (username.isEmpty() && pending) {
        username = pending->username;
    }
    if (pending) {
        pending->pollTimer->stop();
        pending->deadlineTimer->stop();
        if (!pending->replySent) {
            pending->replyConnection.send(pending->request.createErrorReply(
                QDBusError::Failed, QStringLiteral("Instance stopped before its preparing PID was available")));
        }
        m_pendingLaunches.remove(serviceName);
        delete pending->pollTimer;
        delete pending->deadlineTimer;
        delete pending;
    }
    const QString token = m_launchTokenByUnitName.value(serviceName);
    // Keep the manifest as cleanup authority until the unit is terminal and
    // scratch cleanup succeeds. Never unlink ahead of durable token removal.
    const bool manifestCleaned = unlinkManifest && !token.isEmpty()
        && cleanupCopyScratchForUnit(serviceName, token);
    if (UnitMonitor *monitor = m_monitors.take(serviceName)) {
        monitor->deleteLater();
    }
    if (UnitJobMonitor *monitor = m_jobMonitors.take(serviceName)) {
        monitor->deleteLater();
    }
    m_failedJobResults.remove(serviceName);
    for (int fd : m_overlayLeaseFdsByUnit.take(serviceName)) {
        ::close(fd);
    }
    m_overlayLeasePathsByUnit.remove(serviceName);
    m_unitPathForUnit.remove(serviceName);
    m_readyUnitNames.remove(serviceName);
    m_announcedReadyUnitNames.remove(serviceName);
    m_terminalStateQueries.remove(serviceName);
    m_initialInactiveUnitNames.remove(serviceName);
    m_stoppingUnits.remove(serviceName);
    if (m_usernameToUnitName.value(username) == serviceName) {
        m_usernameToUnitName.remove(username);
    }
    m_unitUsernameByName.remove(serviceName);
    m_unitGenerationByName.remove(serviceName);
    m_pidPollGenerationByUnit.remove(serviceName);
    m_unitOwnerUid.remove(serviceName);
    m_targetIdentityByUnit.remove(serviceName);
    for (auto it = m_pidToUsername.begin(); it != m_pidToUsername.end();) {
        if (m_pidToUnitName.value(it.key()) == serviceName) {
            m_pidToUnitName.remove(it.key());
            m_pidOwnerUid.remove(it.key());
            it = m_pidToUsername.erase(it);
        } else {
            ++it;
        }
    }
    cleanupTcpListenerIfLast(username);
    if (manifestCleaned) {
        m_launchTokenByUnitName.remove(serviceName);
    }
    if (!saveState()) {
        if (manifestCleaned) {
            m_launchTokenByUnitName.insert(serviceName, token);
        }
        return;
    }
    if (manifestCleaned) {
        m_cleanedLaunchManifestTokens.insert(token);
        QString error;
        if (!m_ops->unlinkLaunchManifest(token, &error)) {
            qWarning() << "Could not unlink inactive launch manifest for" << serviceName << ":" << error;
        }
    }
}

void CouchPlayHelper::cleanupTcpListenerIfLast(const QString &username)
{
    if (m_usernameToUnitName.contains(username) || !m_compositorUidForUsername.contains(username)) {
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

bool CouchPlayHelper::confirmUnitInactive(const QString &serviceName, QString *error) const
{
    const QDBusConnection connection = m_ops->systemBusConnection();
    QDBusMessage getUnit = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.systemd1"),
                                                          QStringLiteral("/org/freedesktop/systemd1"),
                                                          QStringLiteral("org.freedesktop.systemd1.Manager"),
                                                          QStringLiteral("GetUnit"));
    getUnit << serviceName;
    const QDBusReply<QDBusObjectPath> unitReply = connection.call(getUnit, QDBus::Block, 2000);
    if (!unitReply.isValid()) {
        if (unitReply.error().name() == QStringLiteral("org.freedesktop.systemd1.NoSuchUnit")) {
            return true;
        }
        if (error) {
            *error = unitReply.error().message();
        }
        return false;
    }
    if (unitReply.value().path() != systemdUnitObjectPath(serviceName)) {
        if (error) {
            *error = QStringLiteral("systemd returned an unexpected object path for %1").arg(serviceName);
        }
        return false;
    }

    QDBusMessage getState = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.systemd1"),
                                                           unitReply.value().path(),
                                                           QStringLiteral("org.freedesktop.DBus.Properties"),
                                                           QStringLiteral("Get"));
    getState << QStringLiteral("org.freedesktop.systemd1.Unit") << QStringLiteral("ActiveState");
    const QDBusReply<QDBusVariant> stateReply = connection.call(getState, QDBus::Block, 2000);
    if (!stateReply.isValid()) {
        if (error) {
            *error = stateReply.error().message();
        }
        return false;
    }
    const QString state = stateReply.value().variant().toString();
    if (state == QStringLiteral("inactive") || state == QStringLiteral("failed")) {
        return true;
    }
    if (error) {
        *error = QStringLiteral("systemd unit %1 is still %2").arg(serviceName, state);
    }
    return false;
}

bool CouchPlayHelper::stopServiceInstance(const QString &serviceName)
{
    QProcess *stopProc = m_ops->createProcess();
    m_ops->startProcess(stopProc, QStringLiteral("systemctl"), {QStringLiteral("stop"), serviceName});
    const bool stopped = m_ops->waitForFinished(stopProc, 10000) && m_ops->processExitCode(stopProc) == 0;
    if (!stopped) {
        // A unit that never loaded or was already collected makes `systemctl stop` fail; systemd decides.
        qWarning() << "systemctl stop failed for" << serviceName << m_ops->readStandardError(stopProc);
    }
    delete stopProc;

    QString confirmationError;
    if (!confirmUnitInactive(serviceName, &confirmationError)) {
        qWarning() << "Systemd has not confirmed that" << serviceName << "is inactive:" << confirmationError;
        return false;
    }
    return true;
}
void CouchPlayHelper::monitorUnitState(const QString &serviceName,
                                       const QString &username,
                                       const QDBusObjectPath &unitPath,
                                       bool initialStateIsAuthoritative)
{
    const QString path = unitPath.path();
    const quint64 generation = m_unitGenerationByName.value(serviceName, 0);
    if (path != systemdUnitObjectPath(serviceName) || usernameForUnit(serviceName) != username || generation == 0) {
        qWarning() << "monitorUnitState: refusing invalid or unowned unit path for" << serviceName;
        return;
    }
    UnitMonitor *monitor = m_monitors.value(serviceName, nullptr);
    if (monitor && monitor->unitPath() != path) {
        m_monitors.remove(serviceName);
        monitor->deleteLater();
        monitor = nullptr;
    }
    if (!monitor) {
        monitor = new UnitMonitor(path, serviceName, generation, this);
        m_monitors.insert(serviceName, monitor);
    }
    QDBusMessage getActive = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.systemd1"),
                                                            path,
                                                            QStringLiteral("org.freedesktop.DBus.Properties"),
                                                            QStringLiteral("Get"));
    getActive << QStringLiteral("org.freedesktop.systemd1.Unit") << QStringLiteral("ActiveState");
    auto *watcher = new QDBusPendingCallWatcher(m_ops->systemBusConnection().asyncCall(getActive), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, serviceName, generation, initialStateIsAuthoritative]() {
        watcher->deleteLater();
        if (m_unitGenerationByName.value(serviceName, 0) != generation) {
            return;
        }
        QDBusPendingReply<QDBusVariant> reply = *watcher;
        if (!m_unitPathForUnit.contains(serviceName)) {
            return;
        }
        if (reply.isError()) {
            onUnitStopped(serviceName, QString());
            return;
        }
        if (UnitMonitor *currentMonitor = m_monitors.value(serviceName, nullptr)) {
            currentMonitor->observeInitialState(reply.value().variant().toString(), initialStateIsAuthoritative);
        }
    });
}


bool CouchPlayHelper::isPathWithinAllowedPrefix(const QString &path) const
{
    if (pathHasDotDotComponent(path)) {
        return false;
    }

    static const QString canonicalMntPrefix = QStringLiteral("/var/mnt/");
    if (path.startsWith(canonicalMntPrefix) && path.length() > canonicalMntPrefix.length()
        && path.at(canonicalMntPrefix.length()) != QLatin1Char('/')
        && m_ops->canonicalFilePath(QStringLiteral("/mnt")) == QStringLiteral("/var/mnt")) {
        return true;
    }

    static const QString canonicalOptPrefix = QStringLiteral("/var/opt/");
    if (path.startsWith(canonicalOptPrefix) && path.length() > canonicalOptPrefix.length()
        && path.at(canonicalOptPrefix.length()) != QLatin1Char('/')
        && m_ops->canonicalFilePath(QStringLiteral("/opt")) == QStringLiteral("/var/opt")) {
        return true;
    }

    static const QString canonicalSrvPrefix = QStringLiteral("/var/srv/");
    if (path.startsWith(canonicalSrvPrefix) && path.length() > canonicalSrvPrefix.length()
        && path.at(canonicalSrvPrefix.length()) != QLatin1Char('/')
        && m_ops->canonicalFilePath(QStringLiteral("/srv")) == QStringLiteral("/var/srv")) {
        return true;
    }


    static const QStringList allowedPrefixes = {
        QStringLiteral("/home/"),
        QStringLiteral("/var/home/"), // Bazzite/Fedora Silverblue
        QStringLiteral("/opt/"),
        QStringLiteral("/srv/"),
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
    if (!m_ops->copyFileSecure(canonicalSource, targetPath, sourceOwnerUid, userUid, pw->pw_gid)) {
        qWarning() << "CopyFileToUser: Failed to copy" << sourcePath << "to" << targetPath;
        sendErrorReply(QDBusError::Failed,
                       QStringLiteral("Failed to copy file from %1 to %2").arg(sourcePath, targetPath));
        return false;
    }
    return true;
}

// Remove an entry (recursively for directories) below an open parent FD,
// never following symlinks: a leaf symlink is unlinked, not descended into
static void removeEntryUnder(int parentFd, const char *name)
{
    struct stat st;
    if (::fstatat(parentFd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        int fd = ::openat(parentFd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd >= 0) {
            SecureFs::removeTreeAt(fd);
            ::close(fd);
        }
        ::unlinkat(parentFd, name, AT_REMOVEDIR);
    } else {
        ::unlinkat(parentFd, name, 0);
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
        ::unlinkat(parentFd, tempNameUtf8.constData(), AT_REMOVEDIR);
        ::close(parentFd);
        ::close(srcFd);
        qWarning() << "CopyDirectoryToUser: Could not open temporary copy directory:" << strerror(err);
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not open temporary copy directory"));
        return false;
    }
    ::fchown(dstFd, userUid, pw->pw_gid);

    int copyResult = SecureFs::copyTreeContents(srcFd, dstFd, userUid, pw->pw_gid);
    ::close(dstFd);
    if (copyResult != 0) {
        // The player's previous tree is untouched — discard only the partial copy
        removeEntryUnder(parentFd, tempNameUtf8.constData());
        ::close(parentFd);
        ::close(srcFd);
        qWarning() << "CopyDirectoryToUser: Failed to copy tree:" << strerror(-copyResult);
        sendErrorReply(QDBusError::Failed,
                       QStringLiteral("Failed to copy directory from %1 to %2").arg(sourceDir, targetPath));
        return false;
    }

    // Swap: move any existing target aside, rename the fresh copy into place,
    // then discard the old tree. If the swap fails, restore the old target.
    struct stat existingSt;
    const bool hadExisting =
        ::fstatat(parentFd, leafNameUtf8.constData(), &existingSt, AT_SYMLINK_NOFOLLOW) == 0;
    QString oldName;
    bool asideMoved = false;
    if (hadExisting) {
        oldName = QStringLiteral(".%1.cpold-%2")
                      .arg(leafName, QString::number(QRandomGenerator::global()->generate(), 16));
        asideMoved = ::renameat(parentFd, leafNameUtf8.constData(), parentFd, oldName.toUtf8().constData()) == 0;
    }
    bool renamedIn = asideMoved || !hadExisting;
    if (renamedIn) {
        renamedIn = ::renameat(parentFd, tempNameUtf8.constData(), parentFd, leafNameUtf8.constData()) == 0;
    }
    if (!renamedIn) {
        int err = errno;
        if (asideMoved
            && ::renameat(parentFd, oldName.toUtf8().constData(), parentFd, leafNameUtf8.constData()) != 0) {
            qWarning() << "CopyDirectoryToUser: Could not restore previous target" << targetPath << strerror(errno);
        }
        removeEntryUnder(parentFd, tempNameUtf8.constData());
        ::close(parentFd);
        ::close(srcFd);
        qWarning() << "CopyDirectoryToUser: Failed to swap in copied directory:" << targetPath << strerror(err);
        sendErrorReply(QDBusError::Failed, QStringLiteral("Failed to replace target directory"));
        return false;
    }
    if (hadExisting) {
        removeEntryUnder(parentFd, oldName.toUtf8().constData());
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

bool CouchPlayHelper::SetUserCompatToolMapping(const QString &username, const QVariantMap &entries)
{
    if (!validateUserAndAuth(username, ACTION_MANAGE_MOUNTS)) {
        return false;
    }
    if (getUserUid(username) != callerUid() && !IsInCouchPlayGroup(username)) {
        sendErrorReply(QDBusError::AccessDenied, QStringLiteral("Target user is not a CouchPlay-managed user"));
        return false;
    }
    const auto changes = SteamCompatToolMapping::fromVariantMap(entries);
    if (!changes) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Invalid compatibility tool mapping"));
        return false;
    }

    const uint userUid = getUserUid(username);
    struct passwd *pw = m_ops->getpwuid(userUid);
    if (!pw) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not get user info for '%1'").arg(username));
        return false;
    }
    const QString userHome = QString::fromLocal8Bit(pw->pw_dir);
    const QString steamRoot = GetUserSteamRoot(username);
    if (steamRoot.isEmpty() || !steamRoot.startsWith(userHome + QLatin1Char('/'))) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Steam is not set up for '%1'").arg(username));
        return false;
    }
    QStringList configParts = steamRoot.mid(userHome.length()).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    configParts << QStringLiteral("config");

    const QString canonicalHome = m_ops->canonicalFilePath(userHome);
    const int homeFd = SecureFs::openBaseDir(canonicalHome.isEmpty() ? userHome : canonicalHome);
    if (homeFd < 0) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not open user home directory"));
        return false;
    }
    const int configFd = SecureFs::openDirBelow(homeFd, configParts, false, userUid, pw->pw_gid);
    ::close(homeFd);
    if (configFd < 0) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Steam config directory not found for '%1'").arg(username));
        return false;
    }
    const auto closeConfig = qScopeGuard([configFd] { ::close(configFd); });

    const QString configName = QStringLiteral("config.vdf");
    constexpr qsizetype maxConfigSize = 16 * 1024 * 1024;
    QByteArray original;
    struct stat configStat{};
    const int readResult = SecureFs::readFileAt(configFd, configName, maxConfigSize, original, &configStat);
    if (readResult != 0 || configStat.st_uid != userUid) {
        qWarning() << "SetUserCompatToolMapping: Cannot use config.vdf of" << username << ":"
                   << (readResult != 0 ? strerror(-readResult) : "not owned by the user");
        sendErrorReply(QDBusError::Failed, QStringLiteral("Could not read Steam config of '%1'").arg(username));
        return false;
    }

    const auto updated = SteamCompatToolMapping::apply(original, *changes);
    if (!updated) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Unrecognized Steam config of '%1'").arg(username));
        return false;
    }
    if (*updated == original) {
        return true;
    }

    const int writeResult =
        SecureFs::replaceFileAt(configFd, configName, *updated, userUid, pw->pw_gid, configStat.st_mode & 0777);
    if (writeResult != 0) {
        qWarning() << "SetUserCompatToolMapping: Failed to write config.vdf of" << username << ":"
                   << strerror(-writeResult);
        sendErrorReply(QDBusError::Failed, QStringLiteral("Failed to write Steam config of '%1'").arg(username));
        return false;
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
    if (!success && finished && options.size() == 2 && options.constFirst() == QStringLiteral("-m")
        && options.at(1).startsWith(QStringLiteral("u:")) && options.at(1).endsWith(QStringLiteral(":rx"))) {
        // ACL-less filesystems may already provide access. Check the pinned inode
        // with the target account's effective credentials, never the root helper's.
        const QString username = options.at(1).mid(2, options.at(1).size() - 5);
        QProcess *probe = ops->createProcess();
        probe->setChildProcessModifier([childFd]() {
            if (::dup2(childFd, 3) < 0) {
                ::_exit(127);
            }
        });
        ops->startProcess(probe, QStringLiteral("/usr/sbin/runuser"),
                          {QStringLiteral("-u"), username, QStringLiteral("--"), QStringLiteral("/bin/sh"),
                           QStringLiteral("-c"),
                           QStringLiteral("test -r /proc/self/fd/3 && test -x /proc/self/fd/3")});
        success = ops->waitForFinished(probe, 5000) && ops->processExitCode(probe) == 0;
        delete probe;
    }

    ::close(childFd);
    delete process;
    return success;
}

} // namespace

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
        QStringLiteral("/var/mnt"), // canonical target of the trusted Bazzite /mnt alias
        QStringLiteral("/var/opt"), // canonical target of the trusted Fedora Atomic /opt alias
        QStringLiteral("/var/srv"), // canonical target of the trusted Fedora Atomic /srv alias
        QStringLiteral("/opt"),
        QStringLiteral("/srv"),
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

    const QString steamRoot = GetUserSteamRoot(username);
    if (steamRoot.isEmpty()) {
        return QString();
    }

    const QString userDataBase = steamRoot + QStringLiteral("/userdata");
    if (!m_ops->fileExists(userDataBase)) {
        return QString();
    }

    const QStringList entries = m_ops->entryList(userDataBase, QStringList(), QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &entry : entries) {
        bool ok;
        entry.toULongLong(&ok);
        if (ok) {
            return entry;
        }
    }

    return QString();
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

bool CouchPlayHelper::saveState()
{
    // Recovery may dispatch nested D-Bus callbacks before the snapshot is hydrated.
    // Refuse persistence (and therefore manifest unlink) until recovery is complete.
    if (m_reconcilingState) {
        m_recoverySaveRequested = true;
        return false;
    }
    // Without a completed recovery the tracking maps are empty; saving would overwrite the persisted state.
    if (m_stateRecoveryPending) {
        return false;
    }
    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("activeMounts")] = m_legacyActiveMounts;

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


    QJsonObject unitsObject;
    for (auto it = m_usernameToUnitName.constBegin(); it != m_usernameToUnitName.constEnd(); ++it) {
        unitsObject[it.key()] = it.value();
    }
    root[QStringLiteral("activeUnits")] = unitsObject;
    QJsonObject launchTokensObject;
    for (auto it = m_launchTokenByUnitName.constBegin(); it != m_launchTokenByUnitName.constEnd(); ++it) {
        launchTokensObject[it.key()] = it.value();
    }
    root[QStringLiteral("launchTokens")] = launchTokensObject;
    QJsonArray readyUnitsArray;
    for (const QString &unit : m_readyUnitNames) {
        readyUnitsArray.append(unit);
    }
    root[QStringLiteral("readyUnits")] = readyUnitsArray;
    QJsonObject leasePathsObject;
    for (auto it = m_overlayLeasePathsByUnit.constBegin(); it != m_overlayLeasePathsByUnit.constEnd(); ++it) {
        QJsonArray paths;
        for (const QString &path : it.value()) {
            paths.append(path);
        }
        leasePathsObject[it.key()] = paths;
    }
    root[QStringLiteral("overlayLeasePaths")] = leasePathsObject;
    QJsonObject unitOwnerObject;
    for (auto it = m_unitOwnerUid.constBegin(); it != m_unitOwnerUid.constEnd(); ++it) {
        unitOwnerObject[it.key()] = static_cast<qint64>(it.value());
    }
    root[QStringLiteral("unitOwnerUid")] = unitOwnerObject;
    QJsonObject targetIdentityObject;
    for (auto it = m_targetIdentityByUnit.constBegin(); it != m_targetIdentityByUnit.constEnd(); ++it) {
        QJsonObject identity;
        identity[QStringLiteral("uid")] = static_cast<qint64>(it.value().uid);
        identity[QStringLiteral("gid")] = static_cast<qint64>(it.value().gid);
        identity[QStringLiteral("declaredHome")] = it.value().declaredHome;
        identity[QStringLiteral("canonicalHome")] = it.value().canonicalHome;
        targetIdentityObject[it.key()] = identity;
    }
    root[QStringLiteral("targetIdentityByUnit")] = targetIdentityObject;
    QJsonObject pidObject;
    QJsonObject pidUnitObject;
    for (auto it = m_pidToUsername.constBegin(); it != m_pidToUsername.constEnd(); ++it) {
        const QString pid = QString::number(it.key());
        pidObject[pid] = it.value();
        pidUnitObject[pid] = m_pidToUnitName.value(it.key());
    }
    root[QStringLiteral("pidToUsername")] = pidObject;
    root[QStringLiteral("pidToUnitName")] = pidUnitObject;
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

    QJsonDocument doc(root);
    QByteArray data = doc.toJson(QJsonDocument::Compact);

    QDir().mkpath(QFileInfo(m_stateFilePath).absolutePath());

    QSaveFile file(m_stateFilePath);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "saveState: Failed to open" << m_stateFilePath << ":" << file.errorString();
        return false;
    }
    if (file.write(data) != data.size()) {
        qWarning() << "saveState: Failed to write" << m_stateFilePath << ":" << file.errorString();
        file.cancelWriting();
        return false;
    }
    if (!file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)) {
        qWarning() << "saveState: Failed to protect" << m_stateFilePath << ":" << file.errorString();
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        qWarning() << "saveState: Failed to commit" << m_stateFilePath << ":" << file.errorString();
        return false;
    }
    return true;
}
CouchPlayHelper::HidDeviceInfo CouchPlayHelper::findHidDevice(const QString &devicePath) const
{
    QFileInfo info(devicePath);
    QString name = info.fileName(); // e.g. event12 or hidraw2
    QString sysfsPath;

    if (name.startsWith(QStringLiteral("event"))) {
        // eventN/device is inputN, which sits in an "input" directory under the device that owns it. HID
        // gamepads are rebound through their hidraw node instead. Gamepads driven directly by a USB
        // interface driver (xpad) have no hidraw node, so only rebinding their interface closes handles
        // other users opened before the session, such as the desktop's Steam.
        const QString inputDevice =
            QFileInfo(QStringLiteral("/sys/class/input/%1/device").arg(name)).canonicalFilePath();
        if (inputDevice.isEmpty() || QDir(inputDevice).entryList({QStringLiteral("js*")}, QDir::Dirs).isEmpty()) {
            return {};
        }
        const QString owner = QFileInfo(inputDevice + QStringLiteral("/../..")).canonicalFilePath();
        const bool isUsbInterface = QFileInfo(owner + QStringLiteral("/subsystem")).canonicalFilePath()
                == QStringLiteral("/sys/bus/usb")
            && QFileInfo(owner).fileName().contains(QLatin1Char(':'));
        if (!isUsbInterface) {
            return {};
        }
        sysfsPath = owner;
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

// The gamepad's current event node followed by its joystick nodes, below a bound device, or an empty list.
QStringList CouchPlayHelper::gamepadNodePaths(const HidDeviceInfo &device) const
{
    // The driver directory links every device bound to it
    const QString inputRoot =
        QFileInfo(device.driverPath + QLatin1Char('/') + device.deviceId).canonicalFilePath() + QStringLiteral("/input");
    const QStringList inputDevices = QDir(inputRoot).entryList({QStringLiteral("input*")}, QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &inputDevice : inputDevices) {
        const QDir dir(inputRoot + QLatin1Char('/') + inputDevice);
        const QStringList events = dir.entryList({QStringLiteral("event*")}, QDir::Dirs);
        const QStringList joysticks = dir.entryList({QStringLiteral("js*")}, QDir::Dirs);
        if (!events.isEmpty() && !joysticks.isEmpty()) {
            QStringList nodes{QStringLiteral("/dev/input/") + events.first()};
            for (const QString &js : joysticks) {
                nodes.append(QStringLiteral("/dev/input/") + js);
            }
            return nodes;
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
    if (m_reconcilingState) {
        return;
    }
    m_reconcilingState = true;
    const auto finishRecovery = qScopeGuard([this] { m_reconcilingState = false; });
    m_cleanedLaunchManifestTokens.clear();
    QString subscriptionError;
    if (!subscribeSystemdManager(&subscriptionError)) {
        qWarning() << "loadAndReconcileState: Could not subscribe to systemd Manager:" << subscriptionError;
        m_stateRecoveryPending = true;
        return;
    }
    m_stateRecoveryPending = false;
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

    m_legacyActiveMounts = root.value(QStringLiteral("activeMounts")).toObject();
    bool changed = false;

    QSet<QString> activeSystemdUnits;
    QSet<QString> stoppedUnitNames;
    bool activeSystemdUnitsKnown = false;
    {
        QProcess proc;
        proc.start(QStringLiteral("systemctl"),
                   {QStringLiteral("list-units"),
                    QStringLiteral("couchplay-*.service"),
                    QStringLiteral("--no-legend"),
                    QStringLiteral("--no-pager")});
        activeSystemdUnitsKnown = proc.waitForFinished(5000) && proc.exitCode() == 0;
        const QByteArray output = activeSystemdUnitsKnown ? proc.readAllStandardOutput() : QByteArray();
        for (const QByteArray &line : output.split('\n')) {
            const QByteArray trimmed = line.trimmed();
            if (trimmed.isEmpty()) {
                continue;
            }
            const int spacePos = trimmed.indexOf(' ');
            if (spacePos > 0) {
                activeSystemdUnits.insert(QString::fromUtf8(trimmed.left(spacePos)));
            }
        }
    }
    const QJsonObject unitsObject = root.value(QStringLiteral("activeUnits")).toObject();
    // State files written before 0.6.0 may list a per-user auxiliary stream unit. The helper no longer
    // manages such units, so stop any that are still running instead of leaking them, then drop the entry.
    const QJsonObject legacyAuxiliaryUnits = root.value(QStringLiteral("auxiliaryUnits")).toObject();
    for (auto it = legacyAuxiliaryUnits.constBegin(); it != legacyAuxiliaryUnits.constEnd(); ++it) {
        const QString username = it.key();
        const QString unitName = it.value().toString();
        changed = true;
        if (!s_validUsername.match(username).hasMatch()
            || (unitName != QStringLiteral("couchplay-stream@%1.service").arg(username)
                && unitName != QStringLiteral("couchplay-%1-stream.service").arg(username))) {
            continue;
        }
        if (stopServiceInstance(unitName)) {
            stoppedUnitNames.insert(unitName);
        } else {
            qWarning() << "Could not stop leftover auxiliary stream unit" << unitName;
        }
    }
    const QJsonObject launchTokensObject = root.value(QStringLiteral("launchTokens")).toObject();
    QMap<QString, QString> manifestsToUnlink;
    const QJsonObject unitOwnerObject = root.value(QStringLiteral("unitOwnerUid")).toObject();
    const QJsonObject targetIdentityObject = root.value(QStringLiteral("targetIdentityByUnit")).toObject();
    const QJsonObject storedLeasePathsObject = root.value(QStringLiteral("overlayLeasePaths")).toObject();
    const QJsonObject storedPidObject = root.value(QStringLiteral("pidToUsername")).toObject();
    const QJsonObject storedPidUnits = root.value(QStringLiteral("pidToUnitName")).toObject();
    const QJsonObject storedPidOwners = root.value(QStringLiteral("pidOwnerUid")).toObject();
    QSet<QString> storedReadyUnits;
    for (const QJsonValue &value : root.value(QStringLiteral("readyUnits")).toArray()) {
        if (value.isString()) {
            storedReadyUnits.insert(value.toString());
        }
    }
    m_usernameToUnitName.clear();
    m_unitUsernameByName.clear();
    m_unitGenerationByName.clear();
    m_pidPollGenerationByUnit.clear();
    m_pidToUsername.clear();
    m_pidToUnitName.clear();
    m_pidOwnerUid.clear();
    m_unitOwnerUid.clear();
    m_targetIdentityByUnit.clear();
    m_launchTokenByUnitName.clear();
    m_readyUnitNames.clear();
    m_overlayLeasePathsByUnit.clear();
    QMap<QString, QString> recoveredUnits;

    const auto trackRecoveredUnit = [&](const QString &username, const QString &unitName) {
        recoveredUnits.insert(username, unitName);
        m_unitUsernameByName.insert(unitName, username);
        m_unitGenerationByName.insert(unitName, ++m_nextUnitGeneration);
    };
    const auto manifestMatchesCurrentIdentity = [&](const LaunchManifest &manifest,
                                                    const QString &username,
                                                    const QString &unitName) {
        if (manifest.schemaVersion != 1 || manifest.unitRole != QStringLiteral("main")
            || manifest.unitName != unitName
            || manifest.targetUsername != username
            || !DirectoryPlan::validateDirectoryPreparations(manifest.directories)) {
            return false;
        }
        struct passwd *targetRecord = m_ops->getpwnam(username.toLocal8Bit().constData());
        if (!targetRecord) {
            return false;
        }
        const uid_t targetUid = targetRecord->pw_uid;
        const gid_t targetGid = targetRecord->pw_gid;
        const QString targetHome = QString::fromLocal8Bit(targetRecord->pw_dir ? targetRecord->pw_dir : "");
        const QString targetCanonicalHome = m_ops->canonicalFilePath(targetHome);
        struct passwd *callerRecord = m_ops->getpwuid(manifest.callerUid);
        if (!callerRecord) {
            return false;
        }
        const uid_t callerUid = callerRecord->pw_uid;
        const gid_t callerGid = callerRecord->pw_gid;
        const QString callerHome = QString::fromLocal8Bit(callerRecord->pw_dir ? callerRecord->pw_dir : "");
        const QString callerCanonicalHome = m_ops->canonicalFilePath(callerHome);
        return targetUid == manifest.targetUid && targetGid == manifest.targetGid
            && targetHome == manifest.targetDeclaredHome && targetCanonicalHome == manifest.targetCanonicalHome
            && callerUid == manifest.callerUid && callerGid == manifest.callerGid
            && callerHome == manifest.declaredHome && callerCanonicalHome == manifest.canonicalHome;
    };
    const auto currentTargetIdentity = [&](const QString &username, TargetIdentity *identity) {
        struct passwd *record = m_ops->getpwnam(username.toLocal8Bit().constData());
        if (!record) {
            return false;
        }
        identity->uid = record->pw_uid;
        identity->gid = record->pw_gid;
        identity->declaredHome = QString::fromLocal8Bit(record->pw_dir ? record->pw_dir : "");
        identity->canonicalHome = m_ops->canonicalFilePath(identity->declaredHome);
        return !identity->declaredHome.isEmpty() && !identity->canonicalHome.isEmpty();
    };

    const auto queueStoredPidStops = [&](const QString &username,
                                         const QString &unitName,
                                         const QString &reason) {
        for (auto it = storedPidObject.constBegin(); it != storedPidObject.constEnd(); ++it) {
            if (it.value().toString() != username) {
                continue;
            }
            QString storedUnitName = storedPidUnits.value(it.key()).toString();
            if (storedUnitName.isEmpty()) {
                storedUnitName = generateServiceName(username);
            }
            if (storedUnitName != unitName) {
                continue;
            }
            bool pidOk = false;
            const qint64 pid = it.key().toLongLong(&pidOk);
            if (pidOk && pid > 0) {
                queueStoppedNotification(username, pid, reason);
            }
        }
    };

    for (auto it = unitsObject.constBegin(); it != unitsObject.constEnd(); ++it) {
        const QString username = it.key();
        const QString unitName = it.value().toString();
        if (!s_validUsername.match(username).hasMatch() || unitName != generateServiceName(username)) {
            changed = true;
            continue;
        }
        QString token = launchTokensObject.value(unitName).toString();
        if (token.isEmpty()) {
            token = launchTokensObject.value(username).toString();
        }

        const bool ready = storedReadyUnits.contains(unitName);
        LaunchManifest manifest;
        bool ownedStateValid = false;
        uint ownerUid = INVALID_CALLER_UID;
        TargetIdentity targetIdentity;
        if (!ready && DirectoryPlan::isLaunchToken(token)
            && m_ops->readLaunchManifest(token, &manifest, nullptr)
            && manifestMatchesCurrentIdentity(manifest, username, unitName)) {
            ownerUid = manifest.callerUid;
            targetIdentity = TargetIdentity{manifest.targetUid,
                                            manifest.targetGid,
                                            manifest.targetDeclaredHome,
                                            manifest.targetCanonicalHome};
            ownedStateValid = true;
        } else if (ready && unitOwnerObject.contains(unitName)
                   && targetIdentityObject.value(unitName).isObject()) {
            const qint64 serializedOwner = unitOwnerObject.value(unitName).toInteger(-1);
            const QJsonObject serializedIdentity = targetIdentityObject.value(unitName).toObject();
            TargetIdentity currentIdentity;
            if (serializedOwner >= 0 && serializedOwner <= std::numeric_limits<uint>::max()
                && currentTargetIdentity(username, &currentIdentity)
                && currentIdentity.uid == static_cast<uid_t>(serializedIdentity.value(QStringLiteral("uid")).toInteger(-1))
                && currentIdentity.gid == static_cast<gid_t>(serializedIdentity.value(QStringLiteral("gid")).toInteger(-1))
                && currentIdentity.declaredHome == serializedIdentity.value(QStringLiteral("declaredHome")).toString()
                && currentIdentity.canonicalHome == serializedIdentity.value(QStringLiteral("canonicalHome")).toString()) {
                ownerUid = static_cast<uint>(serializedOwner);
                targetIdentity = currentIdentity;
                ownedStateValid = true;
            }
        }

        if (!ownedStateValid || ownerUid == INVALID_CALLER_UID) {
            m_stoppingUnits.insert(unitName);
            const bool stopped = stopServiceInstance(unitName);
            m_stoppingUnits.remove(unitName);
            if (stopped) {
                stoppedUnitNames.insert(unitName);
            }
            if (stopped) {
                queueStoredPidStops(username, unitName,
                                    QStringLiteral("Instance was stopped because helper ownership could not be verified"));
                changed = true;
                continue;
            }
            qWarning() << "Could not stop active unit with invalid persisted ownership" << unitName;
            trackRecoveredUnit(username, unitName);
            if (ownerUid != INVALID_CALLER_UID) {
                m_unitOwnerUid.insert(unitName, ownerUid);
            }
            if (DirectoryPlan::isLaunchToken(token)) {
                m_launchTokenByUnitName.insert(unitName, token);
            }
            changed = true;
            continue;
        }

        QList<int> leaseFds;
        QStringList leasePaths;
        QString leaseError;
        if (ready) {
            for (const QJsonValue &value : storedLeasePathsObject.value(unitName).toArray()) {
                if (!value.isString()) {
                    leaseError = QStringLiteral("Malformed persisted overlay lease state");
                    break;
                }
                leasePaths.append(value.toString());
            }
            if (leaseError.isEmpty()
                && !reacquireOverlayLeases(username, leasePaths, &leaseFds, &leaseError)) {
                leaseFds.clear();
            }
        } else if (!acquireOverlayLeases(username, manifest.directories, &leaseFds, &leasePaths, &leaseError)) {
            leaseFds.clear();
        }
        if (!leaseError.isEmpty()) {
            qWarning() << "Could not reacquire overlay lease for" << unitName << ":" << leaseError;
            m_stoppingUnits.insert(unitName);
            const bool stopped = stopServiceInstance(unitName);
            m_stoppingUnits.remove(unitName);
            if (stopped) {
                stoppedUnitNames.insert(unitName);
            }
            if (stopped) {
                queueStoredPidStops(username, unitName,
                                    QStringLiteral("Instance was stopped because its overlay lease could not be recovered"));
                changed = true;
                continue;
            }
            trackRecoveredUnit(username, unitName);
            m_unitOwnerUid.insert(unitName, ownerUid);
            m_targetIdentityByUnit.insert(unitName, targetIdentity);
            if (DirectoryPlan::isLaunchToken(token)) {
                m_launchTokenByUnitName.insert(unitName, token);
            }
            changed = true;
            continue;
        }

        trackRecoveredUnit(username, unitName);
        m_unitOwnerUid.insert(unitName, ownerUid);
        m_targetIdentityByUnit.insert(unitName, targetIdentity);
        m_overlayLeaseFdsByUnit.insert(unitName, leaseFds);
        m_overlayLeasePathsByUnit.insert(unitName, leasePaths);
        if (ready) {
            m_readyUnitNames.insert(unitName);
        }
        if (DirectoryPlan::isLaunchToken(token)) {
            m_launchTokenByUnitName.insert(unitName, token);
        }
    }
    m_usernameToUnitName = recoveredUnits;

    for (auto it = storedPidObject.constBegin(); it != storedPidObject.constEnd(); ++it) {
        bool pidOk = false;
        const qint64 pid = it.key().toLongLong(&pidOk);
        const QString username = it.value().toString();
        QString unitName = storedPidUnits.value(it.key()).toString();
        if (unitName.isEmpty()) {
            unitName = m_usernameToUnitName.value(username);
        }
        if (!pidOk || pid <= 0 || unitName.isEmpty() || m_unitUsernameByName.value(unitName) != username) {
            changed = true;
            continue;
        }
        const qint64 serializedOwner = storedPidOwners.value(QString::number(pid)).toInteger(-1);
        const uint ownerUid = serializedOwner >= 0 && serializedOwner <= std::numeric_limits<uint>::max()
            ? static_cast<uint>(serializedOwner) : m_unitOwnerUid.value(unitName, INVALID_CALLER_UID);
        if (ownerUid == INVALID_CALLER_UID) {
            changed = true;
            continue;
        }
        m_pidToUsername.insert(pid, username);
        m_pidToUnitName.insert(pid, unitName);
        m_pidOwnerUid.insert(pid, ownerUid);
    }

    const QMap<QString, QString> recoveredUnitNames = m_unitUsernameByName;
    for (auto it = recoveredUnitNames.constBegin(); it != recoveredUnitNames.constEnd(); ++it) {
        const QString unitName = it.key();
        const QString username = it.value();

        QDBusMessage getUnit = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.systemd1"),
                                                              QStringLiteral("/org/freedesktop/systemd1"),
                                                              QStringLiteral("org.freedesktop.systemd1.Manager"),
                                                              QStringLiteral("GetUnit"));
        getUnit << unitName;
        const QDBusReply<QDBusObjectPath> reply = m_ops->systemBusConnection().call(getUnit);
        if (!reply.isValid() && reply.error().name() == QStringLiteral("org.freedesktop.systemd1.NoSuchUnit")) {
            stoppedUnitNames.insert(unitName);
            finalizeStoppedUnit(unitName, QStringLiteral("Instance exited while the helper was unavailable"), true);
            changed = true;
            continue;
        }
        if (!reply.isValid() || reply.value().path() != systemdUnitObjectPath(unitName)) {
            m_stoppingUnits.insert(unitName);
            const bool stopped = stopServiceInstance(unitName);
            m_stoppingUnits.remove(unitName);
            if (stopped) {
                stoppedUnitNames.insert(unitName);
            }
            if (stopped) {
                finalizeStoppedUnit(unitName,
                                    QStringLiteral("Instance was stopped because its systemd unit could not be recovered"),
                                    true);
                changed = true;
            }
            continue;
        }
        m_unitPathForUnit.insert(unitName, reply.value());
        monitorUnitState(unitName, username, reply.value(), true);
        pollUnitMainPid(unitName);
    }
    for (auto it = launchTokensObject.constBegin(); it != launchTokensObject.constEnd(); ++it) {
        const QString unitName = it.key();
        const QString token = it.value().toString();
        if (!DirectoryPlan::isLaunchToken(token)) {
            changed = true;
            continue;
        }
        if (m_unitUsernameByName.contains(unitName)) {
            continue;
        }
        if (activeSystemdUnits.contains(unitName) && !stoppedUnitNames.contains(unitName)) {
            m_launchTokenByUnitName.insert(unitName, token);
            continue;
        }
        if (m_cleanedLaunchManifestTokens.contains(token)) {
            continue;
        }
        m_launchTokenByUnitName.insert(unitName, token);
        if (!cleanupCopyScratchForUnit(unitName, token)) {
            continue;
        }
        m_launchTokenByUnitName.remove(unitName);
        m_cleanedLaunchManifestTokens.insert(token);
        manifestsToUnlink.insert(unitName, token);
        changed = true;
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


    QJsonArray runtimeUids = root.value(QStringLiteral("runtimeAccessUids")).toArray();
    QSet<uint> loadedRuntimeUids;
    for (const QJsonValue &val : runtimeUids) {
        uint uid = static_cast<uint>(val.toInteger());
        QString waylandSocket = waylandSocketPath(QStringLiteral("/run/user/%1").arg(uid));
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
    changed = reconcileLegacyMounts(activeSystemdUnits, stoppedUnitNames, activeSystemdUnitsKnown) || changed;
    changed = std::exchange(m_recoverySaveRequested, false) || changed;
    m_reconcilingState = false;
    const QSet<QString> deferredReleases = std::exchange(m_deferredReleaseUnits, {});
    for (const QString &unitName : deferredReleases) {
        releaseOwnedUnit(unitName, true);
        changed = true;
    }
    if (changed && saveState()) {
        // Only unreferenced manifests may be deleted. A crash before unlink
        // leaves a harmless protected file, not a token with missing authority.
        for (auto it = manifestsToUnlink.constBegin(); it != manifestsToUnlink.constEnd(); ++it) {
            QString error;
            if (!m_ops->unlinkLaunchManifest(it.value(), &error)) {
                qWarning() << "Could not unlink reconciled launch manifest for" << it.key() << ":" << error;
            }
        }
    } else if (changed) {
        for (auto it = manifestsToUnlink.constBegin(); it != manifestsToUnlink.constEnd(); ++it) {
            m_launchTokenByUnitName.insert(it.key(), it.value());
            m_cleanedLaunchManifestTokens.remove(it.value());
        }
    }

    qDebug() << "loadAndReconcileState: Restored" << m_usernameToUnitName.size()
             << "units," << m_modifiedDevices.size() << "devices," << m_runtimeAccessSetForUid.size() << "runtime UIDs";
}

bool CouchPlayHelper::reconcileLegacyMounts(const QSet<QString> &activeSystemdUnits,
                                             const QSet<QString> &stoppedUnitNames,
                                             bool systemdStateKnown)
{
    if (m_legacyActiveMounts.isEmpty()) {
        return false;
    }

    QProcess *mountInfoProcess = m_ops->createProcess();
    m_ops->startProcess(mountInfoProcess, QStringLiteral("cat"), {QStringLiteral("/proc/self/mountinfo")});
    const bool mountInfoAvailable = m_ops->waitForFinished(mountInfoProcess, 5000)
        && m_ops->processExitCode(mountInfoProcess) == 0;
    const QByteArray mountInfo = mountInfoAvailable ? m_ops->readAllStandardOutput(mountInfoProcess) : QByteArray();
    delete mountInfoProcess;

    const auto decodeMountInfoField = [](const QByteArray &encoded, QByteArray *decoded) {
        decoded->clear();
        for (int i = 0; i < encoded.size(); ++i) {
            if (encoded.at(i) != '\\') {
                decoded->append(encoded.at(i));
                continue;
            }
            if (i + 3 >= encoded.size()) {
                return false;
            }
            const char first = encoded.at(i + 1);
            const char second = encoded.at(i + 2);
            const char third = encoded.at(i + 3);
            if (first < '0' || first > '7' || second < '0' || second > '7' || third < '0' || third > '7') {
                return false;
            }
            const int value = ((first - '0') << 6) | ((second - '0') << 3) | (third - '0');
            decoded->append(static_cast<char>(value));
            i += 3;
        }
        return true;
    };
    const auto findMount = [&](const QByteArray &target, const QByteArray &source, const QByteArray &upperDir,
                               bool *found, bool *sourceMatches) {
        *found = false;
        *sourceMatches = false;
        for (const QByteArray &line : mountInfo.split('\n')) {
            if (line.isEmpty()) {
                continue;
            }
            const QList<QByteArray> fields = line.split(' ');
            const int separator = fields.indexOf(QByteArray("-"));
            if (fields.size() < 10 || separator < 6 || separator + 3 >= fields.size()) {
                return false;
            }
            QByteArray mountedTarget;
            QByteArray mountedType;
            QByteArray mountedSource;
            QByteArray superOptions;
            if (!decodeMountInfoField(fields.at(4), &mountedTarget)
                || !decodeMountInfoField(fields.at(separator + 1), &mountedType)
                || !decodeMountInfoField(fields.at(separator + 2), &mountedSource)
                || !decodeMountInfoField(fields.at(separator + 3), &superOptions)) {
                return false;
            }
            if (mountedTarget == target) {
                if (*found) {
                    return false;
                }
                *found = true;
                *sourceMatches = mountedSource == source;
                if (!*sourceMatches && mountedType == QByteArray("overlay") && !upperDir.isEmpty()) {
                    // Legacy overlays passed lowerdir as /proc/self/fd/N, so mountinfo never shows the
                    // source path; the persisted private upper directory is the stable identity.
                    const QByteArray upperDirPrefix("upperdir=");
                    for (const QByteArray &option : superOptions.split(',')) {
                        if (option.startsWith(upperDirPrefix) && option.mid(upperDirPrefix.size()) == upperDir) {
                            *sourceMatches = true;
                            break;
                        }
                    }
                }
            }
        }
        return true;
    };
    // A bind mount's mountinfo source is the backing device, not the bound directory, so the
    // string comparison cannot authenticate it. The mounted target is the same inode as its source.
    // The legacy helper recorded the requested source but mounted its canonical path, so resolve it first.
    const auto sameDirectoryIdentity = [this](const QString &source, const QString &target) {
        const QString canonicalSource = m_ops->canonicalFilePath(source);
        const int sourceFd = SecureFs::openExistingDirNoFollow(canonicalSource.isEmpty() ? source : canonicalSource);
        if (sourceFd < 0) {
            return false;
        }
        const int targetFd = SecureFs::openExistingDirNoFollow(target);
        struct stat sourceStat {};
        struct stat targetStat {};
        const bool same = targetFd >= 0 && ::fstat(sourceFd, &sourceStat) == 0
            && ::fstat(targetFd, &targetStat) == 0 && S_ISDIR(sourceStat.st_mode)
            && sourceStat.st_dev == targetStat.st_dev && sourceStat.st_ino == targetStat.st_ino;
        ::close(sourceFd);
        if (targetFd >= 0) {
            ::close(targetFd);
        }
        return same;
    };

    QJsonObject remainingMounts;
    bool changed = false;
    for (auto it = m_legacyActiveMounts.constBegin(); it != m_legacyActiveMounts.constEnd(); ++it) {
        const QString username = it.key();
        const QJsonValue storedMounts = it.value();
        if (!s_validUsername.match(username).hasMatch() || !storedMounts.isArray()) {
            remainingMounts.insert(username, storedMounts);
            continue;
        }

        const QString mainUnit = generateServiceName(username);
        const bool mainStopped = stoppedUnitNames.contains(mainUnit);
        const bool unitStillActive = m_usernameToUnitName.contains(username)
            || (activeSystemdUnits.contains(mainUnit) && !mainStopped);
        if (unitStillActive || (!systemdStateKnown && !mainStopped)) {
            remainingMounts.insert(username, storedMounts);
            continue;
        }

        struct passwd *record = m_ops->getpwnam(username.toLocal8Bit().constData());
        const QString declaredHome = record ? QString::fromLocal8Bit(record->pw_dir ? record->pw_dir : "") : QString();
        const QString home = declaredHome.isEmpty() ? QString() : m_ops->canonicalFilePath(declaredHome);
        if (!QDir::isAbsolutePath(home) || home == QStringLiteral("/") || QDir::cleanPath(home) != home) {
            remainingMounts.insert(username, storedMounts);
            continue;
        }

        const QJsonArray mounts = storedMounts.toArray();
        QList<QJsonValue> retained;
        for (qsizetype i = mounts.size(); i-- > 0;) {
            const QJsonValue value = mounts.at(i);
            if (!value.isObject()) {
                retained.append(value);
                continue;
            }
            const QJsonObject object = value.toObject();
            QString source = object.value(QStringLiteral("source")).toString();
            // The old API accepted and persisted sources with trailing separators; the same directory is meant.
            while (source.size() > 1 && source.endsWith(QLatin1Char('/'))) {
                source.chop(1);
            }
            const QString target = object.value(QStringLiteral("target")).toString();
            const QString targetPrefix = home + QLatin1Char('/');
            if (!QDir::isAbsolutePath(source) || QDir::cleanPath(source) != source || source.contains(QChar::Null)
                || !QDir::isAbsolutePath(target) || QDir::cleanPath(target) != target || target.contains(QChar::Null)
                || !target.startsWith(targetPrefix)) {
                retained.append(value);
                continue;
            }
            if (!mountInfoAvailable) {
                retained.append(value);
                continue;
            }

            bool mounted = false;
            bool sourceMatches = false;
            const QByteArray upperDir = object.value(QStringLiteral("upperDir")).toString().toUtf8();
            if (!findMount(target.toUtf8(), source.toUtf8(), upperDir, &mounted, &sourceMatches)) {
                retained.append(value);
                continue;
            }
            if (mounted && !sourceMatches) {
                sourceMatches = sameDirectoryIdentity(source, target);
            }
            if (mounted && !sourceMatches) {
                retained.append(value);
                continue;
            }
            if (!mounted) {
                changed = true;
                continue;
            }

            const int targetFd = SecureFs::openExistingDirNoFollow(target);
            struct stat targetStat {};
            if (targetFd < 0 || ::fstat(targetFd, &targetStat) != 0 || !S_ISDIR(targetStat.st_mode)) {
                if (targetFd >= 0) {
                    ::close(targetFd);
                }
                retained.append(value);
                continue;
            }
            QProcess *unmountProcess = m_ops->createProcess();
            const QString fdPath = QStringLiteral("/proc/self/fd/%1").arg(targetFd);
            unmountProcess->setStandardInputFile(fdPath);
            // Our own descriptors pin the mount, so a plain umount always fails with EBUSY; detach lazily.
            m_ops->startProcess(unmountProcess, QStringLiteral("umount"),
                                {QStringLiteral("-l"), QStringLiteral("--"), QStringLiteral("/proc/self/fd/0")});
            const bool unmounted = m_ops->waitForFinished(unmountProcess, 5000)
                && m_ops->processExitCode(unmountProcess) == 0;
            delete unmountProcess;
            ::close(targetFd);
            if (unmounted) {
                changed = true;
            } else {
                retained.append(value);
            }
        }
        if (!retained.isEmpty()) {
            std::reverse(retained.begin(), retained.end());
            QJsonArray remaining;
            for (const QJsonValue &value : retained) {
                remaining.append(value);
            }
            remainingMounts.insert(username, remaining);
        } else {
            changed = true;
        }
    }
    m_legacyActiveMounts = remainingMounts;
    return changed;
}

void CouchPlayHelper::startWatchingDevice(const QString &devicePath)
{
    const bool isHidraw = devicePath.startsWith(QLatin1String("/dev/hidraw"));
    const bool isEvent = devicePath.startsWith(QLatin1String("/dev/input/event"));

    if (!isHidraw && !isEvent) {
        return;
    }

    // A rebind or reconnect recreates the node under the same path before the old watcher reads ENODEV;
    // that watcher's fd still points at the removed device, so watch the current node instead
    stopWatchingDevice(devicePath);

    int fd = m_ops->openInputDevice(devicePath);
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

// Maps each /dev/input/eventN name to its sysfs input device ("input171"). A device that is removed and recreated
// within the debounce window (Steam does this) may keep its event number but always gets a new input name.
QHash<QString, QString> CouchPlayHelper::currentEventInputs() const
{
    QHash<QString, QString> inputs;
    const QStringList eventFiles =
        QDir(QStringLiteral("/dev/input")).entryList({QStringLiteral("event*")}, QDir::Files | QDir::System);
    for (const QString &eventFile : eventFiles) {
        const QString inputDir =
            QFileInfo(QStringLiteral("/sys/class/input/%1/device").arg(eventFile)).canonicalFilePath();
        inputs.insert(eventFile, QFileInfo(inputDir).fileName());
    }
    return inputs;
}

void CouchPlayHelper::checkForNewVirtualDevices()
{
    const QHash<QString, QString> previous = std::exchange(m_knownEventInputs, currentEventInputs());

    // Virtual pads handed to players disappear when their Steam exits; stop tracking them
    bool pruned = false;
    for (qsizetype i = m_sessionVirtualDevices.size() - 1; i >= 0; --i) {
        const QString path = m_sessionVirtualDevices.at(i);
        if (!QFileInfo::exists(path)) {
            m_modifiedDevices.removeAll(path);
            m_sessionVirtualDevices.removeAt(i);
            pruned = true;
        }
    }
    if (pruned) {
        saveState();
    }

    static const QRegularExpression eventRegex(QStringLiteral("^event(\\d+)$"));
    QHash<QString, QStringList> newVirtualNodesByInput;
    for (auto it = m_knownEventInputs.constBegin(); it != m_knownEventInputs.constEnd(); ++it) {
        const QString &eventFile = it.key();
        const QString &inputName = it.value();
        const QRegularExpressionMatch match = eventRegex.match(eventFile);
        if (inputName.isEmpty() || previous.value(eventFile) == inputName || !match.hasMatch()
            || !isVirtualDevice(match.captured(1).toInt())) {
            continue;
        }
        const QString devicePath = QStringLiteral("/dev/input/%1").arg(eventFile);
        qDebug() << "CouchPlayHelper: New virtual device detected, monitoring:" << devicePath;
        startWatchingDevice(devicePath);

        QStringList nodes{devicePath};
        const QString inputDir = QStringLiteral("/sys/class/input/%1").arg(inputName);
        for (const QString &js : QDir(inputDir).entryList({QStringLiteral("js*")}, QDir::Dirs | QDir::NoDotAndDotDot)) {
            nodes.append(QStringLiteral("/dev/input/") + js);
        }
        newVirtualNodesByInput.insert(inputName, nodes);
    }
    assignSessionVirtualDevices(newVirtualNodesByInput);
}

// With Steam Input on, a player's Steam re-emits their pad as a uinput virtual pad and games read only that one.
// udev gives new input nodes to the seat user, so hand each virtual pad to the player whose processes created it.
void CouchPlayHelper::assignSessionVirtualDevices(const QHash<QString, QStringList> &nodesByInput)
{
    if (nodesByInput.isEmpty() || m_usernameToUnitName.isEmpty()) {
        return;
    }
    QHash<uid_t, gid_t> sessionUsers;
    for (auto it = m_usernameToUnitName.constBegin(); it != m_usernameToUnitName.constEnd(); ++it) {
        if (const struct passwd *pw = m_ops->getpwnam(it.key().toLocal8Bit().constData())) {
            sessionUsers.insert(pw->pw_uid, pw->pw_gid);
        }
    }
    const QSet<uid_t> uids(sessionUsers.keyBegin(), sessionUsers.keyEnd());
    const QHash<QString, uid_t> creators = m_ops->uinputDeviceCreators(uids);

    bool settled = false;
    bool changed = false;
    for (auto it = nodesByInput.constBegin(); it != nodesByInput.constEnd(); ++it) {
        const auto creator = creators.constFind(it.key());
        if (creator == creators.constEnd() || !sessionUsers.contains(creator.value())) {
            continue;
        }
        if (!settled) {
            // Let udev finish applying the seat ACLs first, or they would land after ours
            runCommand(QStringLiteral("udevadm"), {QStringLiteral("settle"), QStringLiteral("--timeout=3")});
            settled = true;
        }
        const uid_t uid = creator.value();
        for (const QString &node : it.value()) {
            if (m_ops->chown(node, uid, sessionUsers.value(uid)) != 0 || m_ops->chmod(node, 0600) != 0) {
                qWarning() << "CouchPlayHelper: Could not give virtual device" << node << "to UID" << uid << ":"
                           << strerror(errno);
                continue;
            }
            runCommand(QStringLiteral("setfacl"), {QStringLiteral("-b"), node});
            qInfo() << "CouchPlayHelper: Gave virtual device" << node << "(" << it.key() << ") to UID" << uid;
            // Track the event node for reset; joystick nodes go away with it
            if (node.startsWith(QLatin1String("/dev/input/event")) && !m_modifiedDevices.contains(node)) {
                m_modifiedDevices.append(node);
                m_sessionVirtualDevices.append(node);
                changed = true;
            }
        }
    }
    if (changed) {
        saveState();
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
