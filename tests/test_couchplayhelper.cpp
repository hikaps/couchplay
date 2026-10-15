// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <tuple>
#include <functional>
#include <cerrno>
#include <utility>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QDBusVariant>
#include <QDBusVirtualObject>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <sys/acl.h>
#include <acl/libacl.h>
#include <sys/wait.h>
#include <grp.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <QTimer>
#include <cstring>

#define private public
#include "../helper/CouchPlayHelper.h"
#undef private
#include "../helper/SecureFs.h"
#include "../helper/SystemOps.h"
#include "../helper/HomePathMapping.h"
#include "../helper/DirectoryPlan.h"
class MockSystemdUnit final : public QDBusVirtualObject
{
public:
    explicit MockSystemdUnit(const QString &objectPath, qulonglong mainPid)
        : m_objectPath(objectPath)
        , m_defaultMainPid(mainPid)
        , m_mainPid(mainPid)
    {
    }
    void setManagerSubscriptionCheck(std::function<bool()> subscribed)
    {
        m_isManagerSubscribed = std::move(subscribed);
    }
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override
    {
        if (message.interface() != QLatin1String("org.freedesktop.DBus.Properties")) {
            return false;
        }
        const QList<QVariant> args = message.arguments();
        if (message.member() == QLatin1String("Get") && args.size() == 2) {
            const QString interface = args.at(0).toString();
            const QString property = args.at(1).toString();
            if (interface == QLatin1String("org.freedesktop.systemd1.Unit")
                && property == QLatin1String("ActiveState") && m_failActiveStateGet) {
                connection.send(message.createErrorReply(QDBusError::UnknownObject, QStringLiteral("unit unavailable")));
                return true;
            }
            QVariant value;
            if (interface == QLatin1String("org.freedesktop.systemd1.Unit")
                && property == QLatin1String("ActiveState") && m_hasNextActiveStateRead) {
                value = m_nextActiveStateRead;
                m_hasNextActiveStateRead = false;
            } else {
                value = propertyValue(interface, property);
            }
            if (!value.isValid()) {
                connection.send(message.createErrorReply(QDBusError::UnknownProperty, property));
            } else {
                connection.send(message.createReply(QVariantList{QVariant::fromValue(QDBusVariant(value))}));
            }
            return true;
        }
        if (message.member() == QLatin1String("GetAll") && args.size() == 1) {
            const QString interface = args.first().toString();
            if (interface == QLatin1String("org.freedesktop.systemd1.Service") && m_failServiceGetAll) {
                connection.send(message.createErrorReply(QDBusError::UnknownObject,
                                                         QStringLiteral("unit was collected")));
                return true;
            }
            QVariantMap properties;
            if (interface == QLatin1String("org.freedesktop.systemd1.Unit")) {
                properties.insert(QStringLiteral("ActiveState"), m_activeState);
                properties.insert(QStringLiteral("SubState"), m_subState);
            } else if (interface == QLatin1String("org.freedesktop.systemd1.Service")) {
                properties.insert(QStringLiteral("MainPID"), QVariant::fromValue<qulonglong>(m_mainPid));
                properties.insert(QStringLiteral("Result"), m_result);
                properties.insert(QStringLiteral("StatusText"), m_statusText);
                properties.insert(QStringLiteral("StatusErrno"), m_statusErrno);
            }
            connection.send(message.createReply(QVariantList{properties}));
            return true;
        }
        return false;
    }

    QString introspect(const QString &path) const override
    {
        Q_UNUSED(path)
        return QStringLiteral(
            "<node>"
            "<interface name='org.freedesktop.DBus.Properties'>"
            "<method name='Get'><arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='v' direction='out'/></method>"
            "<method name='GetAll'><arg type='s' direction='in'/><arg type='a{sv}' direction='out'/></method>"
            "<signal name='PropertiesChanged'><arg type='s'/><arg type='a{sv}'/><arg type='as'/></signal>"
            "</interface>"
            "<interface name='org.freedesktop.systemd1.Unit'>"
            "<property name='ActiveState' type='s' access='read'/><property name='SubState' type='s' access='read'/>"
            "</interface>"
            "<interface name='org.freedesktop.systemd1.Service'>"
            "<property name='MainPID' type='t' access='read'/><property name='Result' type='s' access='read'/>"
            "<property name='StatusText' type='s' access='read'/><property name='StatusErrno' type='i' access='read'/>"
            "</interface>"
            "</node>");
    }
    void reset()
    {
        m_activeState = QStringLiteral("activating");
        m_subState = QStringLiteral("start");
        m_mainPid = m_defaultMainPid;
        m_result = QStringLiteral("success");
        m_statusText.clear();
        m_statusErrno = 0;
        m_failServiceGetAll = false;
        m_failActiveStateGet = false;
        m_hasNextActiveStateRead = false;
    }

    void setNextActiveStateRead(const QString &state)
    {
        m_nextActiveStateRead = state;
        m_hasNextActiveStateRead = true;
    }

    void setMainPid(qulonglong pid)
    {
        m_mainPid = pid;
    }
    void setServiceGetAllFailure(bool fail)
    {
        m_failServiceGetAll = fail;
    }
    void setActiveStateGetFailure(bool fail)
    {
        m_failActiveStateGet = fail;
    }
    void setFailure(const QString &reason, int errorNumber)
    {
        m_result = QStringLiteral("exit-code");
        m_statusText = reason;
        m_statusErrno = errorNumber;
        m_subState = QStringLiteral("failed");
        QVariantMap changed;
        changed.insert(QStringLiteral("Result"), m_result);
        changed.insert(QStringLiteral("StatusText"), m_statusText);
        changed.insert(QStringLiteral("StatusErrno"), m_statusErrno);
        QDBusMessage statusSignal = QDBusMessage::createSignal(
            m_objectPath, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("PropertiesChanged"));
        statusSignal.setArguments({QStringLiteral("org.freedesktop.systemd1.Service"), changed, QStringList{}});
        if (!m_isManagerSubscribed || m_isManagerSubscribed()) {
            QDBusConnection::sessionBus().send(statusSignal);
        }
        setActiveState(QStringLiteral("failed"));
    }

    void emitActiveStateChanged(const QString &state)
    {
        QVariantMap changed;
        changed.insert(QStringLiteral("ActiveState"), state);
        QDBusMessage signal = QDBusMessage::createSignal(
            m_objectPath,
            QStringLiteral("org.freedesktop.DBus.Properties"),
            QStringLiteral("PropertiesChanged"));
        signal.setArguments({QStringLiteral("org.freedesktop.systemd1.Unit"), changed, QStringList{}});
        if (!m_isManagerSubscribed || m_isManagerSubscribed()) {
            QDBusConnection::sessionBus().send(signal);
        }
    }

    void setActiveState(const QString &state)
    {
        m_activeState = state;
        emitActiveStateChanged(state);
    }

private:
    QVariant propertyValue(const QString &interface, const QString &property) const
    {
        if (interface == QLatin1String("org.freedesktop.systemd1.Unit")) {
            if (property == QLatin1String("ActiveState")) return m_activeState;
            if (property == QLatin1String("SubState")) return m_subState;
        }
        if (interface == QLatin1String("org.freedesktop.systemd1.Service")) {
            if (property == QLatin1String("MainPID")) return QVariant::fromValue<qulonglong>(m_mainPid);
            if (property == QLatin1String("Result")) return m_result;
            if (property == QLatin1String("StatusText")) return m_statusText;
            if (property == QLatin1String("StatusErrno")) return m_statusErrno;
        }
        return {};
    }

    QString m_objectPath;
    qulonglong m_defaultMainPid = 12345;
    QString m_activeState = QStringLiteral("activating");
    QString m_subState = QStringLiteral("start");
    qulonglong m_mainPid = 12345;
    QString m_result = QStringLiteral("success");
    QString m_statusText;
    int m_statusErrno = 0;
    bool m_failServiceGetAll = false;
    bool m_failActiveStateGet = false;
    QString m_nextActiveStateRead;
    bool m_hasNextActiveStateRead = false;
    std::function<bool()> m_isManagerSubscribed;
};

#include <fcntl.h>

class MockSystemdManager final : public QDBusVirtualObject
{
public:
    MockSystemdManager(const QString &mainUnitPath, const QString &auxiliaryUnitPath)
        : m_mainUnitPath(mainUnitPath)
        , m_auxiliaryUnitPath(auxiliaryUnitPath)
    {
    }

    void emitJobRemoved(const QString &jobPath, const QString &unitName, const QString &result) const
    {
        QDBusMessage signal = QDBusMessage::createSignal(QStringLiteral("/org/freedesktop/systemd1"),
                                                         QStringLiteral("org.freedesktop.systemd1.Manager"),
                                                         QStringLiteral("JobRemoved"));
        const uint jobId = jobPath.section(QLatin1Char('/'), -1).toUInt();
        signal.setArguments({QVariant::fromValue(jobId),
                             QVariant::fromValue(QDBusObjectPath(jobPath)),
                             unitName,
                             result});
        QDBusConnection::sessionBus().send(signal);
    }

    bool isSubscribed() const { return m_subscribed; }
    bool m_failSubscribe = false;
    bool m_unitMissing = false;
    QMap<QString, QString> m_extraUnitPaths;
    std::function<void()> m_beforeGetUnitReply;
    void resetSubscription()
    {
        m_subscribed = false;
    }

    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override
    {
        if (message.interface() != QLatin1String("org.freedesktop.systemd1.Manager")) {
            return false;
        }
        if (message.member() == QLatin1String("Subscribe") && message.arguments().isEmpty()) {
            if (m_failSubscribe) {
                connection.send(message.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.Failed"),
                                                         QStringLiteral("subscribe unavailable")));
                return true;
            }
            if (m_subscribed) {
                connection.send(message.createErrorReply(QStringLiteral("org.freedesktop.systemd1.AlreadySubscribed"),
                                                         QStringLiteral("Client is already subscribed")));
            } else {
                m_subscribed = true;
                connection.send(message.createReply());
            }
            return true;
        }
        if (message.member() != QLatin1String("GetUnit") || message.arguments().size() != 1) {
            return false;
        }
        const QString unitName = message.arguments().constFirst().toString();
        const auto beforeReply = std::exchange(m_beforeGetUnitReply, {});
        if (beforeReply) {
            beforeReply();
        }
        if (m_unitMissing) {
            connection.send(message.createErrorReply(QStringLiteral("org.freedesktop.systemd1.NoSuchUnit"), unitName));
            return true;
        }
        if (m_extraUnitPaths.contains(unitName)) {
            connection.send(message.createReply(QVariantList{
                QVariant::fromValue(QDBusObjectPath(m_extraUnitPaths.value(unitName)))}));
            return true;
        }
        QString unitPath;
        if (unitName == QStringLiteral("couchplay-player1.service")) {
            unitPath = m_mainUnitPath;
        } else if (unitName == QStringLiteral("couchplay-stream@player1.service")) {
            unitPath = m_auxiliaryUnitPath;
        }
        if (unitPath.isEmpty()) {
            connection.send(message.createErrorReply(QStringLiteral("org.freedesktop.systemd1.NoSuchUnit"), unitName));
        } else {
            connection.send(message.createReply(QVariantList{QVariant::fromValue(QDBusObjectPath(unitPath))}));
        }
        return true;
    }

    QString introspect(const QString &path) const override
    {
        Q_UNUSED(path)
        return QStringLiteral("<node><interface name='org.freedesktop.systemd1.Manager'>"
                              "<method name='Subscribe'/>"
                              "<method name='GetUnit'><arg type='s' direction='in'/>"
                              "<arg type='o' direction='out'/></method></interface></node>");
    }
private:
    QString m_mainUnitPath;
    QString m_auxiliaryUnitPath;
    bool m_subscribed = false;
};
#include <unistd.h>

class MockSystemOps : public SystemOps
{
public:
    MockSystemOps() = default;
    ~MockSystemOps() override = default;

    void setAuthResult(bool authorized)
    {
        m_authorized = authorized;
    }
    void setConnectionUid(uid_t uid)
    {
        m_connectionUid = uid;
    }
    void setStatOwner(uid_t uid, gid_t gid)
    {
        m_statOwnerUid = uid;
        m_statOwnerGid = gid;
    }
    void setRealCopyFileSecure(bool enabled)
    {
        m_realCopyFileSecure = enabled;
    }
    void setProcessExitCode(int exitCode)
    {
        m_processExitCode = exitCode;
    }
    void setProcessExitCodeForCommand(const QString &program, int exitCode)
    {
        m_processExitCodes.insert(program, exitCode);
    }
    void setMockProcessStart(bool mock)
    {
        m_mockProcessStart = mock;
    }
    void setStandardOutput(const QByteArray &output)
    {
        m_standardOutput = output;
    }
    void setStandardError(const QByteArray &output)
    {
        m_standardError = output;
    }
    void
    setUserExists(const QString &username, bool exists, uint uid = 0, gid_t gid = 0, const QString &home = QString())
    {
        if (exists) {
            struct passwd pw;
            QByteArray usernameBytes = username.toLocal8Bit();
            QByteArray homeBytes = home.toLocal8Bit();
            m_pwEntries[username] = {pw, usernameBytes, homeBytes, uid, gid, home};
        } else {
            m_pwEntries.remove(username);
        }
    }

    void setGroupExists(const QString &groupname, bool exists, gid_t gid = 0, const QStringList &members = {})
    {
        if (exists) {
            struct group gr;
            QByteArray groupBytes = groupname.toLocal8Bit();
            m_grEntries[groupname] = {gr, groupBytes, gid, members};
        } else {
            m_grEntries.remove(groupname);
        }
    }
    void setOnSystemctlStop(std::function<void(const QString &)> callback)
    {
        m_onSystemctlStop = std::move(callback);
    }
    void setFileExists(const QString &path, bool exists)
    {
        m_files[path] = exists;
    }
    void setDirectoryExists(const QString &path, bool exists)
    {
        m_directories[path] = exists;
        if (exists) {
            m_directoryStats[path] = true;
        } else {
            m_directoryStats.remove(path);
        }
    }
    void setSymlink(const QString &path, const QString &canonicalTarget)
    {
        m_files[path] = true;
        m_symlinks[path] = canonicalTarget;
    }
    void setCanonicalMapping(const QString &path, const QString &canonical)
    {
        m_canonical[path] = canonical;
    }
    void setChownResult(int result)
    {
        m_chownResult = result;
    }
    void setChmodResult(int result)
    {
        m_chmodResult = result;
    }

    void clear()
    {
        m_pwEntries.clear();
        m_grEntries.clear();
        m_files.clear();
        m_directories.clear();
        m_directoryStats.clear();
        m_symlinks.clear();
        m_canonical.clear();
        m_directoryQueries.clear();
        m_canonicalQueries.clear();
        m_statQueries.clear();
        m_authorized = true;
        m_connectionUid = 1000;
        m_statOwnerUid = 1000;
        m_statOwnerGid = 1000;
        m_realCopyFileSecure = false;
        realAccessProbe = false;
        m_processExitCode = 0;
        m_processExitCodes.clear();
        m_chownResult = 0;
        m_chmodResult = 0;
        m_processArgs.clear();
        m_processInvocations.clear();
        m_mockProcessStart = false;
        m_onSystemctlStop = {};
        m_standardOutput.clear();
        m_standardError.clear();
        m_transientServiceName.clear();
        m_transientProperties.clear();
        m_launchManifests.clear();
        m_nextLaunchToken = 0;
        m_transientStartSucceeds = true;
        m_beforeTransientStartReply = {};
        m_beforeManifestUnlink = {};
        m_lastTransientJobPath.clear();
    }

    QStringList getLastProcessArgs() const
    {
        return m_processArgs;
    }
    QString getLastProcessCommand() const
    {
        return m_processCommand;
    }

    struct ProcessInvocation {
        QString command;
        QStringList args;
    };
    QList<ProcessInvocation> m_processInvocations;
    QStringList m_directoryQueries;
    QStringList m_canonicalQueries;
    QStringList m_statQueries;
    bool realAccessProbe = false;
    QString m_transientServiceName;
    QList<SystemdProperty> m_transientProperties;
    QMap<QString, LaunchManifest> m_launchManifests;
    uint m_nextLaunchToken = 0;
    uint m_nextTransientJobId = 0;
    QString m_lastTransientJobPath;
    std::function<void()> m_beforeTransientStartReply;
    bool m_transientStartSucceeds = true;
    std::function<void(const QString &)> m_beforeManifestUnlink;

    struct passwd *getpwnam(const char *name) override
    {
        QString username = QString::fromLocal8Bit(name);
        if (m_pwEntries.contains(username)) {
            auto &entry = m_pwEntries[username];
            entry.updatePointers();
            return &entry.pw;
        }
        return nullptr;
    }

    struct passwd *getpwuid(uid_t uid) override
    {
        for (auto &entry : m_pwEntries) {
            if (entry.uid == uid) {
                entry.updatePointers();
                return &entry.pw;
            }
        }
        return nullptr;
    }

    struct group *getgrnam(const char *name) override
    {
        QString groupname = QString::fromLocal8Bit(name);
        if (m_grEntries.contains(groupname)) {
            auto &entry = m_grEntries[groupname];
            entry.updatePointers();
            return &entry.gr;
        }
        return nullptr;
    }

    uid_t connectionUnixUser(const QString &busName) override
    {
        Q_UNUSED(busName)
        return m_connectionUid;
    }
    QDBusObjectPath startTransientService(const QString &name,
                                         const QList<SystemdProperty> &properties,
                                         QString *error) override
    {
        m_transientServiceName = name;
        m_transientProperties = properties;
        if (!m_transientStartSucceeds) {
            if (error) {
                *error = QStringLiteral("mock StartTransientUnit failure");
            }
            return {};
        }
        m_lastTransientJobPath = QStringLiteral("/org/freedesktop/systemd1/job/%1").arg(++m_nextTransientJobId);
        const auto beforeReply = m_beforeTransientStartReply;
        m_beforeTransientStartReply = {};
        if (beforeReply) {
            beforeReply();
        }
        return QDBusObjectPath(m_lastTransientJobPath);
    }

    QDBusConnection systemBusConnection() const override
    {
        return QDBusConnection::sessionBus();
    }

    bool writeLaunchManifest(const LaunchManifest &manifest, QString *token, QString *error) override
    {
        Q_UNUSED(error)
        const QString newToken = QStringLiteral("00000000-0000-4000-8000-%1")
                                     .arg(++m_nextLaunchToken, 12, 10, QLatin1Char('0'));
        m_launchManifests.insert(newToken, manifest);
        *token = newToken;
        return true;
    }

    bool readLaunchManifest(const QString &token, LaunchManifest *manifest, QString *error) override
    {
        if (!m_launchManifests.contains(token)) {
            if (error) {
                *error = QStringLiteral("mock manifest not found");
            }
            return false;
        }
        *manifest = m_launchManifests.value(token);
        return true;
    }

    bool unlinkLaunchManifest(const QString &token, QString *error) override
    {
        Q_UNUSED(error)
        if (m_beforeManifestUnlink) {
            m_beforeManifestUnlink(token);
        }
        m_launchManifests.remove(token);
        return true;
    }

    bool fileExists(const QString &path) override
    {
        return m_files.value(path, false);
    }

    bool isDirectory(const QString &path) override
    {
        m_directoryQueries.append(path);
        return m_directories.value(path, false);
    }

    bool isSymLink(const QString &path) override
    {
        return m_symlinks.contains(path);
    }

    QString canonicalFilePath(const QString &path) override
    {
        m_canonicalQueries.append(path);
        return m_canonical.value(path, path);
    }

    bool mkpath(const QString &path) override
    {
        Q_UNUSED(path)
        return true;
    }

    bool copyFileSecure(const QString &source,
                        const QString &dest,
                        uid_t sourceOwner,
                        uid_t destOwner,
                        gid_t destGroup) override
    {
        return m_realCopyFileSecure ? m_realSystemOps.copyFileSecure(source, dest, sourceOwner, destOwner, destGroup)
                                    : true;
    }

    bool createDirectorySecure(const QString &path, uid_t owner, gid_t group) override
    {
        Q_UNUSED(path)
        Q_UNUSED(owner)
        Q_UNUSED(group)
        return true;
    }
    bool removeFile(const QString &path) override
    {
        Q_UNUSED(path)
        return true;
    }

    bool copyFile(const QString &source, const QString &dest) override
    {
        Q_UNUSED(source)
        Q_UNUSED(dest)
        return true;
    }

    bool writeFile(const QString &path, const QByteArray &content) override
    {
        Q_UNUSED(path)
        Q_UNUSED(content)
        return true;
    }

    bool statPath(const QString &path, struct stat *buf) override
    {
        m_statQueries.append(path);
        buf->st_mode = (m_directoryStats.value(path, false) ? S_IFDIR : S_IFREG) | 0644;
        buf->st_uid = m_statOwnerUid;
        buf->st_gid = m_statOwnerGid;
        return true;
    }

    bool isCharDevice(mode_t mode) override
    {
        Q_UNUSED(mode)
        return true;
    }

    int chown(const QString &path, uid_t owner, gid_t group) override
    {
        Q_UNUSED(path)
        Q_UNUSED(owner)
        Q_UNUSED(group)
        return m_chownResult;
    }

    int chmod(const QString &path, mode_t mode) override
    {
        Q_UNUSED(path)
        Q_UNUSED(mode)
        return m_chmodResult;
    }

    QProcess *createProcess(QObject *parent = nullptr) override
    {
        return new QProcess(parent);
    }

    void startProcess(QProcess *process, const QString &program, const QStringList &arguments) override
    {
        m_processCommand = program;
        m_processArgs = arguments;
        m_processInvocations.append({program, arguments});
        process->setProperty("mockCommand", program);
        if (program == QStringLiteral("systemctl") && arguments.value(0) == QStringLiteral("stop")
            && m_onSystemctlStop) {
            m_onSystemctlStop(arguments.value(1));
        }
        if (realAccessProbe && program == QStringLiteral("/usr/sbin/runuser")) {
            process->setProperty("realAccessProbe", true);
            process->start(program, arguments);
        } else if (!m_mockProcessStart) {
            process->start(program, arguments);
        }
    }

    bool waitForFinished(QProcess *process, int msecs) override
    {
        Q_UNUSED(process)
        Q_UNUSED(msecs)
        if (m_mockProcessStart && !process->property("realAccessProbe").toBool()) {
            return true;
        }
        return process->waitForFinished(msecs);
    }

    int processExitCode(QProcess *process) override
    {
        Q_UNUSED(process)
        if (process->property("realAccessProbe").toBool()) {
            return process->exitCode();
        }
        return m_processExitCodes.value(process->property("mockCommand").toString(), m_processExitCode);
    }

    QByteArray readStandardError(QProcess *process) override
    {
        Q_UNUSED(process)
        return m_standardError;
    }

    QByteArray readAllStandardOutput(QProcess *process) override
    {
        Q_UNUSED(process)
        return m_standardOutput;
    }

    QStringList entryList(const QString &path, const QStringList &nameFilters, QDir::Filters filters) override
    {
        Q_UNUSED(path)
        Q_UNUSED(nameFilters)
        Q_UNUSED(filters)
        return QStringList();
    }

    bool killProcess(pid_t pid, int signal) override
    {
        Q_UNUSED(pid)
        Q_UNUSED(signal)
        return true;
    }

    bool checkAuthorization(const QString &action, const QString &callerBusName) override
    {
        Q_UNUSED(action)
        Q_UNUSED(callerBusName)
        return m_authorized;
    }

private:
    struct PwEntry {
        struct passwd pw;
        QByteArray usernameBytes;
        QByteArray homeBytes;
        uint uid;
        gid_t gid;
        QString home;

        void updatePointers()
        {
            pw.pw_name = usernameBytes.data();
            pw.pw_dir = homeBytes.data();
            pw.pw_uid = uid;
            pw.pw_gid = gid;
            pw.pw_shell = const_cast<char *>("/bin/bash");
            pw.pw_passwd = const_cast<char *>("x");
        }
    };

    struct GrEntry {
        struct group gr;
        QByteArray groupBytes;
        gid_t gid;
        QStringList members;

        void updatePointers()
        {
            gr.gr_name = groupBytes.data();
            gr.gr_gid = gid;

            m_memberPtrs.clear();
            m_memberStrings.clear();
            for (const QString &member : members) {
                m_memberStrings.append(member.toLocal8Bit());
                m_memberPtrs.append(m_memberStrings.last().data());
            }
            m_memberPtrs.append(nullptr);
            gr.gr_mem = m_memberPtrs.data();
        }

        QList<QByteArray> m_memberStrings;
        QList<char *> m_memberPtrs;
    };

    QMap<QString, PwEntry> m_pwEntries;
    QMap<QString, GrEntry> m_grEntries;
    QMap<QString, bool> m_files;
    QMap<QString, bool> m_directories;
    QMap<QString, bool> m_directoryStats;
    QMap<QString, QString> m_symlinks;
    QMap<QString, QString> m_canonical;
    RealSystemOps m_realSystemOps;
    bool m_authorized = true;
    uid_t m_connectionUid = 1000;
    uid_t m_statOwnerUid = 1000;
    gid_t m_statOwnerGid = 1000;
    bool m_realCopyFileSecure = false;
    int m_processExitCode = 0;
    QMap<QString, int> m_processExitCodes;
    int m_chownResult = 0;
    int m_chmodResult = 0;
    QString m_processCommand;
    QStringList m_processArgs;

    bool m_mockProcessStart = false;
    std::function<void(const QString &)> m_onSystemctlStop;
    QByteArray m_standardOutput;
    QByteArray m_standardError;
};

class TestCouchPlayHelper : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // User management tests
    void testCreateUserSuccess();
    void testCreateUserInvalidUsername();
    void testCreateUserAlreadyExists();
    void testCreateUserAuthDenied();
    void testCreateUserProcessFailure();
    void testCreateUserLingerFailure();
    void testCreateUserWithoutInputGroup();
    void testCreateUserWithInputGroup();
    void testDeleteUserSuccess();
    void testDeleteUserInvalidUsername();
    void testDeleteUserNonexistent();
    void testDeleteUserAuthDenied();
    void testDeleteUserNotInCouchPlayGroup();
    void testDeleteUserProcessFailure();
    void testIsInCouchPlayGroupTrue();
    void testIsInCouchPlayGroupFalse();
    void testIsInCouchPlayGroupNonexistent();
    void testIsInCouchPlayGroupNonexistentGroup();
    void testEnableLingerSuccess();
    void testEnableLingerInvalidUsername();
    void testEnableLingerNonexistent();
    void testEnableLingerAuthDenied();
    void testEnableLingerProcessFailure();
    void testIsLingerEnabledTrue();
    void testIsLingerEnabledFalse();
    void testListCouchPlayUsers();
    void testListCouchPlayUsersFilters();
    void testGetUserInfo();
    void testGetUserInfoNotFound();
    void testIsSteamBootstrappedTrue();
    void testIsSteamBootstrappedFalse();

    // Copy file tests
    void testCopyFileToUserTrustedHomeAliasUsesCanonicalSource();
    void testCopyFileToUserRejectsSymlinkedSourceComponent();
    void testCopyFileToUserRejectsSymlinkedSourceLeaf();

    // Copy directory tests
    void testCopyDirectoryToUserAbsoluteTarget();
    void testCopyDirectoryToUserTraversalTarget();
    void testCopyDirectoryToUserSourceOutsideAllowedPrefixes();
    void testCopyDirectoryToUserSourceNotExists();
    void testCopyDirectoryToUserSuccessReplacesAndChowns();
    void testCopyDirectoryToUserPreservesTargetOnFailedCopy();
    void testCopyDirectoryToUserDotDotNameAccepted();
    void testCopyDirectoryToUserSymlinkedTargetRejected();
    void testCopyDirectoryToUserSourceSymlinkResolvesOutside();
    void testMirrorDirectoryContentsSuccess();
    void testMirrorDirectoryContentsReplacesExistingSymlink();
    void testMirrorDirectoryContentsReplacesNonEmptyDirectoryWithSymlink();
    void testMirrorDirectoryContentsTargetNotExists();
    void testMirrorDirectoryContentsTraversalTarget();
    void testIsPathWithinAllowedPrefixMountRoots();
    void testIsPathWithinAllowedPrefixCanonicalAtomicAliases();
    void testIsPathWithinAllowedPrefixCanonicalAtomicAliasesAbsent();
    void testDirectoryPreparationValidation();
    void testLaunchInstanceRejectsTooManyDirectoryPreparations();
    void testSetPathAclWithParentsSymlinkEscapeRejected();
    void testSetPathAclWithParentsCanonicalMntAliasEscapeRejected();
    void testSecureReadExecuteAclTreePreservesAclAndSkipsSymlinks();
    void testSecureReadExecuteAclTreeSeedsOwnerOnlyDefault();
    void testSecureWriteRejectsSymlinkLeaf();

    // Device ownership tests
    void testChangeDeviceOwnerInvalidPathNotUnderDevInput();
    void testChangeDeviceOwnerInvalidPathTraversal();
    void testChangeDeviceOwnerInvalidPathNotExists();
    void testChangeDeviceOwnerInvalidPathNotCharDevice();
    void testChangeDeviceOwnerSuccess();
    void testChangeDeviceOwnerAuthorizationDenied();
    void testChangeDeviceOwnerUserNotFound();
    void testChangeDeviceOwnerChownFails();
    void testChangeDeviceOwnerChmodFails();
    void testResetDeviceOwnerSuccess();
    void testResetDeviceOwnerInvalidPathNotUnderDevInput();
    void testResetDeviceOwnerInvalidPathTraversal();
    void testResetDeviceOwnerAuthorizationDenied();
    void testResetDeviceOwnerChownFails();
    void testResetDeviceOwnerChmodFails();
    void testResetAllDevicesEmpty();
    void testResetAllDevicesSuccess();
    void testResetAllDevicesPartialFailure();
    void testChangeDeviceOwnerBatchEmpty();
    void testChangeDeviceOwnerBatchAllSuccess();
    void testChangeDeviceOwnerBatchPartialFailure();
    void testChangeDeviceOwnerBatchAllFailure();

    // Runtime access tests (Polkit + D-Bus integration)
    void testSetupRuntimeAccessSuccess();
    void testSetupRuntimeAccessAuthorizationDenied();
    void testSetupRuntimeAccessUserNotFound();
    void testRemoveRuntimeAccessSuccess();
    void testRemoveRuntimeAccessAuthorizationDenied();
    void testRemoveRuntimeAccessUserNotFound();

    // Launch/Stop/Kill instance tests
    void testGenerateServiceName();
    void testLaunchInstance_basicLaunch();
    void testLaunchInstance_subscribesSystemdManagerBeforeSignals();
    void testLegacyMountCleanupUnmountsValidatedTarget();
    void testLegacyMountCleanupPreservesFailedUnmount();
    void testLegacyMountCleanupRecognizesBindMountByDirectoryIdentity();
    void testFailedSubscriptionDoesNotOverwritePersistedState();
    void testTreeRemovalReportsProgress();
    void testLegacyMountCleanupRecognizesOverlayByPersistedUpperDir();
    void testLegacyMountCleanupRejectsUntrustedTarget();
    void testLaunchInstance_validationEmptyUsername();
    void testLaunchInstance_validationNonexistentUser();
    void testLaunchInstance_rejectsRootTarget();
    void testStopInstance_serviceStop();
    void testStopInstancePreservesManifestOnScratchCleanupFailure();
    void testManifestCleanupPersistsBeforeUnlink_data();
    void testManifestCleanupPersistsBeforeUnlink();
    void testManifestCleanupRetainsManifestOnStateSaveFailure();
    void testRecoveryCleanupPreservesDurableResources_data();
    void testRecoveryCleanupPreservesDurableResources();
    void testManifestRecoveryRefusesUnverifiableManifest_data();
    void testManifestRecoveryRefusesUnverifiableManifest();
    void testFailedLaunchRetryDoesNotStopReplacement_data();
    void testFailedLaunchRetryDoesNotStopReplacement();
    void testKillInstance_serviceKill();
    void testStopInstance_fallbackToDirectKill();
    void testLaunchInstance_startTransientFailure();
    void testLaunchInstance_startTransientFailureWhenUnitWasNeverLoaded();
    void testLaunchInstance_preparationFailureReason();
    void testLaunchInstance_collectedUnitPreservesFailureReason();
    void testAuxiliaryInstanceCoexistsWithMainInstance();
    void testUnitRolesRemainIndependentAcrossUsernames();
    void testRecoveryStopsLegacyStreamUnit_data();
    void testRecoveryStopsLegacyStreamUnit();
    void testHostTmpSourceIsDeferredToBootstrap();
    void testLaunchInstancePreparingPidTimeout();
    void testInitialInactiveStateDoesNotReleasePreparingLaunch();
    void testTerminalUnitStopsWithoutObservedNonterminalState();
    void testInitialInactiveAfterPreparingPidReportsStopped();
    void testStaleJobRemovedForPreviousLaunchIsIgnored();
    void testTerminalStateErrorsRetainOwnedUnit();
    void testStaleTerminalUnitSignalDoesNotStopActiveUnit();
    void testLaunchInstance_rejectsProtectedEnvironment_data();
    void testLaunchInstance_rejectsProtectedEnvironment();

private:
    void configureLaunchFixture();
    QDBusReply<qint64> launchPlayer1(const QVariantList &directoryPreparations = {},
                                    const QStringList &additionalEnvironment = {},
                                    const QString &username = QStringLiteral("player1"));
    QDBusReply<qint64> launchAuxiliaryPlayer1(const QString &username = QStringLiteral("player1"));
    CouchPlayHelper *m_helper = nullptr;
    MockSystemOps *m_ops = nullptr;
    QTemporaryDir m_stateDirectory;
    QString m_serviceName;
    QString m_objectPath;
    QDBusInterface *m_dbusInterface = nullptr;
    MockSystemdUnit *m_systemdUnit = nullptr;
    MockSystemdUnit *m_auxSystemdUnit = nullptr;
    MockSystemdManager *m_systemdManager = nullptr;
    QString m_systemdManagerPath = QStringLiteral("/org/freedesktop/systemd1");
    QString m_systemdUnitPath = QStringLiteral("/org/freedesktop/systemd1/unit/couchplay_2dplayer1_2eservice");
    QString m_auxSystemdUnitPath = QStringLiteral("/org/freedesktop/systemd1/unit/couchplay_2dstream_40player1_2eservice");
};
void TestCouchPlayHelper::initTestCase()
{
    m_serviceName = QStringLiteral("io.github.hikaps.CouchPlayHelper.Test");
    m_objectPath = QStringLiteral("/io/github/hikaps/CouchPlayHelper");

    m_ops = new MockSystemOps();
    m_ops->clear();

    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});

    if (!m_stateDirectory.isValid()) {
        qFatal("Failed to create helper state test directory");
    }
    m_systemdUnit = new MockSystemdUnit(m_systemdUnitPath, 12345);
    m_auxSystemdUnit = new MockSystemdUnit(m_auxSystemdUnitPath, 12346);
    m_systemdManager = new MockSystemdManager(m_systemdUnitPath, m_auxSystemdUnitPath);
    if (!QDBusConnection::sessionBus().registerService(QStringLiteral("org.freedesktop.systemd1"))) {
        qFatal("Failed to register fake systemd service on session bus");
    }
    if (!QDBusConnection::sessionBus().registerVirtualObject(m_systemdManagerPath, m_systemdManager)) {
        qFatal("Failed to register fake systemd Manager object");
    }
    if (!QDBusConnection::sessionBus().registerVirtualObject(m_systemdUnitPath, m_systemdUnit)) {
        qFatal("Failed to register fake main systemd unit object");
    }
    if (!QDBusConnection::sessionBus().registerVirtualObject(m_auxSystemdUnitPath, m_auxSystemdUnit)) {
        qFatal("Failed to register fake auxiliary systemd unit object");
    }
    m_systemdUnit->setManagerSubscriptionCheck([this]() { return m_systemdManager->isSubscribed(); });
    m_auxSystemdUnit->setManagerSubscriptionCheck([this]() { return m_systemdManager->isSubscribed(); });

    if (!QDBusConnection::sessionBus().registerService(m_serviceName)) {
        qFatal("Failed to register service name %s on session bus", qUtf8Printable(m_serviceName));
    }
}

void TestCouchPlayHelper::cleanupTestCase()
{
     QDBusConnection::sessionBus().unregisterObject(m_objectPath);
     QDBusConnection::sessionBus().unregisterService(m_serviceName);
    QDBusConnection::sessionBus().unregisterObject(m_systemdManagerPath);
    QDBusConnection::sessionBus().unregisterObject(m_systemdUnitPath);
    QDBusConnection::sessionBus().unregisterObject(m_auxSystemdUnitPath);
    QDBusConnection::sessionBus().unregisterService(QStringLiteral("org.freedesktop.systemd1"));
    delete m_dbusInterface;
    delete m_helper;
    delete m_systemdUnit;
    delete m_auxSystemdUnit;
    delete m_systemdManager;
    delete m_ops;
    m_dbusInterface = nullptr;
    m_helper = nullptr;
    m_systemdUnit = nullptr;
    m_auxSystemdUnit = nullptr;
    m_systemdManager = nullptr;
    m_ops = nullptr;
}
void TestCouchPlayHelper::init()
{
    const QString statePath = m_stateDirectory.filePath(QStringLiteral("state.json"));
    QVERIFY(!QFile::exists(statePath) || QFile::remove(statePath));
    m_helper = new CouchPlayHelper(m_ops);
    m_helper->m_stateFilePath = statePath;
    QVERIFY(QDBusConnection::sessionBus().registerObject(
        m_objectPath, m_helper, QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals));
    m_systemdUnit->reset();
    m_auxSystemdUnit->reset();
    m_systemdManager->resetSubscription();
    m_dbusInterface = new QDBusInterface(m_serviceName,
                                         m_objectPath,
                                         QStringLiteral("io.github.hikaps.CouchPlayHelper"),
                                         QDBusConnection::sessionBus());

    if (!m_dbusInterface->isValid()) {
        qFatal("Failed to create QDBusInterface: %s", qUtf8Printable(m_dbusInterface->lastError().message()));
    }
}

void TestCouchPlayHelper::cleanup()
{
    delete m_dbusInterface;
    m_dbusInterface = nullptr;
    m_ops->m_beforeManifestUnlink = {};
    m_ops->setOnSystemctlStop([this](const QString &unitName) {
        if (unitName == QStringLiteral("couchplay-player1.service")) {
            m_systemdUnit->setActiveState(QStringLiteral("inactive"));
        } else if (unitName == QStringLiteral("couchplay-stream@player1.service")) {
            m_auxSystemdUnit->setActiveState(QStringLiteral("inactive"));
        }
    });
    QDBusConnection::sessionBus().unregisterObject(m_objectPath);
    delete m_helper;
    m_helper = nullptr;
    m_ops->clear();
}
void TestCouchPlayHelper::configureLaunchFixture()
{
    m_ops->clear();
    m_systemdUnit->reset();
    m_auxSystemdUnit->reset();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {QStringLiteral("player1")});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, QStringLiteral("/home/player1"));
    m_ops->setUserExists(QStringLiteral("compositor"), true, 1000, 1000, QStringLiteral("/home/compositor"));
    m_ops->setConnectionUid(1000);
    m_ops->setStatOwner(1000, 1000);
    m_ops->setFileExists(QStringLiteral("/usr/bin/gamescope"), true);
    m_ops->setFileExists(QStringLiteral("/run/user/1000"), true);
    m_ops->setFileExists(QStringLiteral("/run/user/1001"), true);
    m_ops->setFileExists(QStringLiteral("/run/user/1000/wayland-0"), true);
    m_ops->setFileExists(QStringLiteral("/run/user/1000/bus"), true);
    m_ops->setFileExists(QStringLiteral("/run/user/1001/bus"), true);
    m_ops->setDirectoryExists(QStringLiteral("/run/user/1000"), true);
    m_ops->setDirectoryExists(QStringLiteral("/run/user/1001"), true);
    m_ops->setDirectoryExists(QStringLiteral("/home/compositor"), true);
    m_ops->setDirectoryExists(QStringLiteral("/home/player1"), true);
    m_ops->setDirectoryExists(QStringLiteral("/home/compositor/.config/example app"), true);
    m_ops->setFileExists(QStringLiteral("/home/compositor/.config/example app"), true);
    m_ops->setMockProcessStart(true);
    m_ops->setProcessExitCode(0);
    m_ops->setOnSystemctlStop([this](const QString &unitName) {
        if (unitName == QStringLiteral("couchplay-player1.service")) {
            m_systemdUnit->setActiveState(QStringLiteral("inactive"));
        } else if (unitName == QStringLiteral("couchplay-stream@player1.service")) {
            m_auxSystemdUnit->setActiveState(QStringLiteral("inactive"));
        }
    });
}

QDBusReply<qint64> TestCouchPlayHelper::launchPlayer1(const QVariantList &directoryPreparations,
                                                    const QStringList &additionalEnvironment,
                                                    const QString &username)
{
    const QString connectionName = QStringLiteral("couchplay-helper-delayed-reply-test");
    const QDBusReply<qint64> reply = [&]() {
        const QDBusConnection clientConnection =
            QDBusConnection::connectToBus(QDBusConnection::SessionBus, connectionName);
        QDBusMessage message = QDBusMessage::createMethodCall(
            m_serviceName, m_objectPath, QStringLiteral("io.github.hikaps.CouchPlayHelper"),
            QStringLiteral("LaunchInstance"));
        QStringList environment{QStringLiteral("ENABLE_GAMESCOPE_WSI=1"),
                                QStringLiteral("XDG_CURRENT_DESKTOP=KDE"),
                                QStringLiteral("PULSE_SINK=couchplay-fixture-sink")};
        environment.append(additionalEnvironment);
        message.setArguments(QVariantList{username,
                         QString(),
                         QStringList{QStringLiteral("-W"), QStringLiteral("960")},
                         QStringList{QStringLiteral("steam"), QStringLiteral("-bigpicture")},
                         QString(),
                         QStringList(),
                         environment,
                         false,
                         directoryPreparations});
        const QDBusPendingCall call = clientConnection.asyncCall(message);
        QDBusPendingCallWatcher watcher(call);
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&watcher, &QDBusPendingCallWatcher::finished, &loop, &QEventLoop::quit);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeout.start(7000);
        if (!watcher.isFinished()) {
            loop.exec();
        }
        return QDBusReply<qint64>(watcher.reply());
    }();
    QDBusConnection::disconnectFromBus(connectionName);
    return reply;
}

QDBusReply<qint64> TestCouchPlayHelper::launchAuxiliaryPlayer1(const QString &username)
{
    const QString connectionName = QStringLiteral("couchplay-helper-delayed-aux-reply-test");
    const QDBusReply<qint64> reply = [&]() {
        const QDBusConnection clientConnection =
            QDBusConnection::connectToBus(QDBusConnection::SessionBus, connectionName);
        QDBusMessage message = QDBusMessage::createMethodCall(
            m_serviceName, m_objectPath, QStringLiteral("io.github.hikaps.CouchPlayHelper"),
            QStringLiteral("LaunchAuxiliaryInstance"));
        message.setArguments(QVariantList{username,
                         QString(),
                         QStringList{QStringLiteral("-W"), QStringLiteral("960")},
                         QStringList{QStringLiteral("sunshine")},
                         QString(),
                         QStringList(),
                         QStringList(),
                         false});
        const QDBusPendingCall call = clientConnection.asyncCall(message);
        QDBusPendingCallWatcher watcher(call);
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&watcher, &QDBusPendingCallWatcher::finished, &loop, &QEventLoop::quit);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeout.start(7000);
        if (!watcher.isFinished()) {
            loop.exec();
        }
        return QDBusReply<qint64>(watcher.reply());
    }();
    QDBusConnection::disconnectFromBus(connectionName);
    return reply;
}

void TestCouchPlayHelper::testCreateUserSuccess()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setUserExists(QStringLiteral("testuser1"), true, 1002, 1002);
    m_ops->setProcessExitCode(0);

    QDBusReply<uint> reply =
        m_dbusInterface->call(QStringLiteral("CreateUser"), QStringLiteral("testuser1"), QStringLiteral("Test User 1"));

    // User already exists, so should fail with "already exists"
    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::Failed);
    QVERIFY(reply.error().message().contains(QStringLiteral("already exists")));
}

void TestCouchPlayHelper::testCreateUserInvalidUsername()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});

    QDBusReply<uint> reply = m_dbusInterface->call(QStringLiteral("CreateUser"),
                                                   QStringLiteral("INVALID-USER"), // Invalid: uppercase and hyphen
                                                   QStringLiteral("Invalid User"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testCreateUserAlreadyExists()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setUserExists(QStringLiteral("existinguser"), true, 1002, 1002);

    QDBusReply<uint> reply = m_dbusInterface->call(QStringLiteral("CreateUser"),
                                                   QStringLiteral("existinguser"),
                                                   QStringLiteral("Existing User"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::Failed);
    QVERIFY(reply.error().message().contains(QStringLiteral("already exists")));
}

void TestCouchPlayHelper::testCreateUserAuthDenied()
{
    m_ops->clear();
    m_ops->setAuthResult(false);
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});

    QDBusReply<uint> reply =
        m_dbusInterface->call(QStringLiteral("CreateUser"), QStringLiteral("testuser"), QStringLiteral("Test User"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::AccessDenied);
}

void TestCouchPlayHelper::testCreateUserProcessFailure()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setProcessExitCode(1);

    QDBusReply<uint> reply =
        m_dbusInterface->call(QStringLiteral("CreateUser"), QStringLiteral("testuser"), QStringLiteral("Test User"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::Failed);
}

void TestCouchPlayHelper::testCreateUserLingerFailure()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setUserExists(QStringLiteral("testuser"), true, 1002, 1002);
    m_ops->setProcessExitCode(0);

    QDBusReply<uint> reply =
        m_dbusInterface->call(QStringLiteral("CreateUser"), QStringLiteral("testuser"), QStringLiteral("Test User"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::Failed);
}

void TestCouchPlayHelper::testCreateUserWithoutInputGroup()
{
    // "input" group absent (simulates Bazzite) — must not appear in -G
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setProcessExitCode(0);

    m_dbusInterface->call(QStringLiteral("CreateUser"), QStringLiteral("newuser"), QStringLiteral("New User"));

    QStringList useraddArgs;
    for (const auto &inv : m_ops->m_processInvocations) {
        if (inv.command == QStringLiteral("useradd")) {
            useraddArgs = inv.args;
            break;
        }
    }
    QVERIFY(!useraddArgs.isEmpty());

    int gIdx = useraddArgs.indexOf(QStringLiteral("-G"));
    QVERIFY(gIdx >= 0);
    QVERIFY(gIdx + 1 < useraddArgs.size());
    QCOMPARE(useraddArgs.at(gIdx + 1), QStringLiteral("couchplay"));
}

void TestCouchPlayHelper::testCreateUserWithInputGroup()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setProcessExitCode(0);

    m_dbusInterface->call(QStringLiteral("CreateUser"), QStringLiteral("newuser"), QStringLiteral("New User"));

    QStringList useraddArgs;
    for (const auto &inv : m_ops->m_processInvocations) {
        if (inv.command == QStringLiteral("useradd")) {
            useraddArgs = inv.args;
            break;
        }
    }
    QVERIFY(!useraddArgs.isEmpty());

    int gIdx = useraddArgs.indexOf(QStringLiteral("-G"));
    QVERIFY(gIdx >= 0);
    QVERIFY(gIdx + 1 < useraddArgs.size());
    QCOMPARE(useraddArgs.at(gIdx + 1), QStringLiteral("input,couchplay"));
}

void TestCouchPlayHelper::testDeleteUserSuccess()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {QStringLiteral("testuser")});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setUserExists(QStringLiteral("testuser"), true, 1002, 1001);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("DeleteUser"), QStringLiteral("testuser"), false);

    QVERIFY(reply.isValid());
    QVERIFY(reply.value());
}

void TestCouchPlayHelper::testDeleteUserInvalidUsername()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("DeleteUser"), QStringLiteral("INVALID-USER"), false);

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testDeleteUserNonexistent()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("DeleteUser"), QStringLiteral("nonexistent"), false);

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testDeleteUserAuthDenied()
{
    m_ops->clear();
    m_ops->setAuthResult(false);
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {QStringLiteral("testuser")});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setUserExists(QStringLiteral("testuser"), true, 1002, 1001);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("DeleteUser"), QStringLiteral("testuser"), false);

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::AccessDenied);
}

void TestCouchPlayHelper::testDeleteUserNotInCouchPlayGroup()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setUserExists(QStringLiteral("testuser"), true, 1002, 1002);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("DeleteUser"), QStringLiteral("testuser"), false);

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::AccessDenied);
}

void TestCouchPlayHelper::testDeleteUserProcessFailure()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {QStringLiteral("testuser")});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setUserExists(QStringLiteral("testuser"), true, 1002, 1001);
    m_ops->setProcessExitCode(1);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("DeleteUser"), QStringLiteral("testuser"), false);

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::Failed);
}

void TestCouchPlayHelper::testIsInCouchPlayGroupTrue()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {QStringLiteral("testuser")});

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("IsInCouchPlayGroup"), QStringLiteral("testuser"));

    QVERIFY(reply.isValid());
    QVERIFY(reply.value());
}

void TestCouchPlayHelper::testIsInCouchPlayGroupFalse()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("IsInCouchPlayGroup"), QStringLiteral("testuser"));

    QVERIFY(reply.isValid());
    QVERIFY(!reply.value());
}

void TestCouchPlayHelper::testIsInCouchPlayGroupNonexistent()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("IsInCouchPlayGroup"), QStringLiteral("nonexistent"));

    QVERIFY(reply.isValid());
    QVERIFY(!reply.value());
}

void TestCouchPlayHelper::testIsInCouchPlayGroupNonexistentGroup()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), false, 0, {});

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("IsInCouchPlayGroup"), QStringLiteral("testuser"));

    QVERIFY(reply.isValid());
    QVERIFY(!reply.value());
}

void TestCouchPlayHelper::testEnableLingerSuccess()
{
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("testuser"), true, 1002, 1002);
    m_ops->setProcessExitCode(0);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("EnableLinger"), QStringLiteral("testuser"));

    QVERIFY(reply.isValid());
    QVERIFY(reply.value());
}

void TestCouchPlayHelper::testEnableLingerInvalidUsername()
{
    m_ops->clear();

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("EnableLinger"), QStringLiteral("INVALID-USER"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testEnableLingerNonexistent()
{
    m_ops->clear();

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("EnableLinger"), QStringLiteral("nonexistent"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testEnableLingerAuthDenied()
{
    m_ops->clear();
    m_ops->setAuthResult(false);
    m_ops->setUserExists(QStringLiteral("testuser"), true, 1002, 1002);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("EnableLinger"), QStringLiteral("testuser"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::AccessDenied);
}

void TestCouchPlayHelper::testEnableLingerProcessFailure()
{
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("testuser"), true, 1002, 1002);
    m_ops->setProcessExitCode(1);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("EnableLinger"), QStringLiteral("testuser"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::Failed);
}

void TestCouchPlayHelper::testIsLingerEnabledTrue()
{
    m_ops->clear();
    m_ops->setFileExists(QStringLiteral("/var/lib/systemd/linger/testuser"), true);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("IsLingerEnabled"), QStringLiteral("testuser"));

    QVERIFY(reply.isValid());
    QVERIFY(reply.value());
}

void TestCouchPlayHelper::testIsLingerEnabledFalse()
{
    m_ops->clear();
    m_ops->setFileExists(QStringLiteral("/var/lib/systemd/linger/testuser"), false);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("IsLingerEnabled"), QStringLiteral("testuser"));

    QVERIFY(reply.isValid());
    QVERIFY(!reply.value());
}

void TestCouchPlayHelper::testIsSteamBootstrappedTrue()
{
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, QStringLiteral("/home/player1"));
    m_ops->setFileExists(QStringLiteral("/home/player1/.local/share/Steam/steam.sh"), true);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("IsSteamBootstrapped"), QStringLiteral("player1"));

    QVERIFY(reply.isValid());
    QVERIFY(reply.value());
}

void TestCouchPlayHelper::testIsSteamBootstrappedFalse()
{
    // Home present but no steam.sh (e.g. failed bootstrap left only userdata) — must report not bootstrapped.
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, QStringLiteral("/home/player1"));

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("IsSteamBootstrapped"), QStringLiteral("player1"));

    QVERIFY(reply.isValid());
    QVERIFY(!reply.value());
}

void TestCouchPlayHelper::testCopyDirectoryToUserAbsoluteTarget()
{
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, QStringLiteral("/home/player1"));

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("CopyDirectoryToUser"),
                                                   QStringLiteral("player1"),
                                                   QStringLiteral("/home/compositor/games"),
                                                   QStringLiteral("/etc/passwd")); // absolute target

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testCopyDirectoryToUserTraversalTarget()
{
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, QStringLiteral("/home/player1"));

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("CopyDirectoryToUser"),
                                                   QStringLiteral("player1"),
                                                   QStringLiteral("/home/compositor/games"),
                                                   QStringLiteral("../escape")); // traversal

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testCopyDirectoryToUserSourceOutsideAllowedPrefixes()
{
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, QStringLiteral("/home/player1"));
    m_ops->setFileExists(QStringLiteral("/etc"), true); // exists and is a directory, but out of prefixes
    m_ops->setDirectoryExists(QStringLiteral("/etc"), true);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("CopyDirectoryToUser"),
                                                   QStringLiteral("player1"),
                                                   QStringLiteral("/etc"),
                                                   QStringLiteral("games"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
    QCOMPARE(m_ops->m_processInvocations.size(), 0); // nothing ran
}

void TestCouchPlayHelper::testCopyDirectoryToUserSourceNotExists()
{
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, QStringLiteral("/home/player1"));

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("CopyDirectoryToUser"),
                                                   QStringLiteral("player1"),
                                                   QStringLiteral("/home/compositor/games"), // not mocked
                                                   QStringLiteral("games"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testCopyDirectoryToUserSuccessReplacesAndChowns()
{
    // Real filesystem layout: mutations are FD-anchored in-process, not
    // subprocess calls, so this verifies end behavior rather than argv
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, homeDir.path());

    QDir sourceDir(homeDir.path() + QStringLiteral("/source-game"));
    QVERIFY(sourceDir.mkpath(QStringLiteral(".")));
    m_ops->setFileExists(sourceDir.path(), true); // validation layer consults the mock
    m_ops->setDirectoryExists(sourceDir.path(), true);
    {
        QFile f(sourceDir.filePath(QStringLiteral("config.ini")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("player=1\n");
    }
    QVERIFY(sourceDir.mkpath(QStringLiteral("subdir")));
    {
        QFile f(sourceDir.filePath(QStringLiteral("subdir/data.bin")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("data");
    }

    // Stale target from a previous session: must be replaced, not nested into
    QDir targetDir(homeDir.path() + QStringLiteral("/games"));
    QVERIFY(targetDir.mkpath(QStringLiteral(".")));
    {
        QFile stale(targetDir.filePath(QStringLiteral("stale.txt")));
        QVERIFY(stale.open(QIODevice::WriteOnly));
        stale.write("old");
    }

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("CopyDirectoryToUser"),
                                                   QStringLiteral("player1"),
                                                   sourceDir.path(),
                                                   QStringLiteral("games"));

    QVERIFY(reply.isValid());
    QVERIFY(reply.value());

    QFileInfo replaced(targetDir.filePath(QStringLiteral("config.ini")));
    QVERIFY(replaced.exists());
    QFile replacedFile(replaced.absoluteFilePath());
    QVERIFY(replacedFile.open(QIODevice::ReadOnly));
    QCOMPARE(replacedFile.readAll(), QByteArray("player=1\n"));
    QVERIFY(QFileInfo(targetDir.filePath(QStringLiteral("subdir/data.bin"))).exists());
    QVERIFY(!QFileInfo(targetDir.filePath(QStringLiteral("stale.txt"))).exists());

    if (geteuid() == 0) { // ownership transfer requires root (CI runs as root)
        struct stat st;
        QCOMPARE(::stat(replaced.absoluteFilePath().toLocal8Bit().constData(), &st), 0);
        QCOMPARE(st.st_uid, static_cast<uid_t>(1001));
        QCOMPARE(st.st_gid, static_cast<gid_t>(1001));
    }

    // The temp-sibling swap must not leave hidden siblings behind
    const QStringList parentEntries =
        QDir(homeDir.path()).entryList(QDir::Dirs | QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
    QCOMPARE(parentEntries.size(), 2); // source-game + games
}

void TestCouchPlayHelper::testCopyDirectoryToUserPreservesTargetOnFailedCopy()
{
    // A replacement copy that fails midway (special file in the source) must
    // leave the player's previous data intact — never delete-then-copy
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, homeDir.path());

    QDir sourceDir(homeDir.path() + QStringLiteral("/source-game"));
    QVERIFY(sourceDir.mkpath(QStringLiteral(".")));
    m_ops->setFileExists(sourceDir.path(), true); // validation layer consults the mock
    m_ops->setDirectoryExists(sourceDir.path(), true);
    {
        QFile f(sourceDir.filePath(QStringLiteral("config.ini")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("new");
    }
    // The copier refuses special files -> the copy fails partway through
    QCOMPARE(::mkfifo(sourceDir.filePath(QStringLiteral("pipe")).toLocal8Bit().constData(), 0666), 0);

    QDir targetDir(homeDir.path() + QStringLiteral("/games"));
    QVERIFY(targetDir.mkpath(QStringLiteral(".")));
    {
        QFile previous(targetDir.filePath(QStringLiteral("save.dat")));
        QVERIFY(previous.open(QIODevice::WriteOnly));
        previous.write("previous");
    }

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("CopyDirectoryToUser"),
                                                   QStringLiteral("player1"),
                                                   sourceDir.path(),
                                                   QStringLiteral("games"));

    QVERIFY(!reply.isValid()); // failed copy -> D-Bus error

    // The player's previous data survived the failed replacement
    QFile survived(targetDir.filePath(QStringLiteral("save.dat")));
    QVERIFY(survived.open(QIODevice::ReadOnly));
    QCOMPARE(survived.readAll(), QByteArray("previous"));
    QVERIFY(!QFileInfo(targetDir.filePath(QStringLiteral("config.ini"))).exists());

    // No temporary/backup siblings left next to the target
    const QStringList parentEntries =
        QDir(homeDir.path()).entryList(QDir::Dirs | QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
    QVERIFY(parentEntries.contains(QStringLiteral("source-game")));
    QVERIFY(parentEntries.contains(QStringLiteral("games")));
    QCOMPARE(parentEntries.size(), 2);
}

void TestCouchPlayHelper::testCopyDirectoryToUserDotDotNameAccepted()
{
    // ".." inside a name is valid; only whole ".." components climb the tree
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, homeDir.path());

    QDir sourceDir(homeDir.path() + QStringLiteral("/source-game"));
    QVERIFY(sourceDir.mkpath(QStringLiteral(".")));
    m_ops->setFileExists(sourceDir.path(), true);
    m_ops->setDirectoryExists(sourceDir.path(), true);
    {
        QFile f(sourceDir.filePath(QStringLiteral("config.ini")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
    }

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("CopyDirectoryToUser"),
                                                   QStringLiteral("player1"),
                                                   sourceDir.path(),
                                                   QStringLiteral("Saves/Foo..Bar"));

    QVERIFY(reply.isValid());
    QVERIFY(reply.value());
    QVERIFY(QFileInfo(homeDir.filePath(QStringLiteral("Saves/Foo..Bar/config.ini"))).exists());
}

void TestCouchPlayHelper::testCopyDirectoryToUserSymlinkedTargetRejected()
{
    m_ops->clear();
    m_ops->setMockProcessStart(true);
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, QStringLiteral("/home/player1"));
    m_ops->setFileExists(QStringLiteral("/home/compositor/games"), true);
    m_ops->setDirectoryExists(QStringLiteral("/home/compositor/games"), true);
    // Player replaced ~/.config with a symlink: root-run rm/cp/chown must not follow it
    m_ops->setSymlink(QStringLiteral("/home/player1/.config"), QStringLiteral("/etc"));

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("CopyDirectoryToUser"),
                                                   QStringLiteral("player1"),
                                                   QStringLiteral("/home/compositor/games"),
                                                   QStringLiteral(".config/games"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
    QCOMPARE(m_ops->m_processInvocations.size(), 0);
}

void TestCouchPlayHelper::testCopyDirectoryToUserSourceSymlinkResolvesOutside()
{
    m_ops->clear();
    m_ops->setMockProcessStart(true);
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, QStringLiteral("/home/player1"));
    // An allowed-prefix source that is really a symlink to /etc
    m_ops->setFileExists(QStringLiteral("/home/compositor/link"), true);
    m_ops->setDirectoryExists(QStringLiteral("/home/compositor/link"), true);
    m_ops->setCanonicalMapping(QStringLiteral("/home/compositor/link"), QStringLiteral("/etc"));

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("CopyDirectoryToUser"),
                                                   QStringLiteral("player1"),
                                                   QStringLiteral("/home/compositor/link"),
                                                   QStringLiteral("games"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
    QCOMPARE(m_ops->m_processInvocations.size(), 0);
}

void TestCouchPlayHelper::testCopyFileToUserTrustedHomeAliasUsesCanonicalSource()
{
    m_ops->clear();
    QTemporaryDir sourceStorage(QStringLiteral("/tmp/couchplay-source-XXXXXX"));
    QTemporaryDir aliasParent(QStringLiteral("/tmp/couchplay-alias-XXXXXX"));
    QTemporaryDir destinationHome(QStringLiteral("/tmp/couchplay-destination-XXXXXX"));
    QVERIFY(sourceStorage.isValid());
    QVERIFY(aliasParent.isValid());
    QVERIFY(destinationHome.isValid());

    const QString actualSource = QDir(sourceStorage.path()).filePath(QStringLiteral("compositor/game/config.ini"));
    QVERIFY(QDir().mkpath(QFileInfo(actualSource).absolutePath()));
    const QByteArray contents("trusted home alias content");
    QFile sourceFile(actualSource);
    QVERIFY(sourceFile.open(QIODevice::WriteOnly));
    QCOMPARE(sourceFile.write(contents), contents.size());
    sourceFile.close();

    // Model the root-owned system home alias above, not inside, the caller's home.
    const QString aliasPrefix = QDir(aliasParent.path()).filePath(QStringLiteral("root-owned-system-home-alias"));
    const QByteArray aliasTargetBytes = sourceStorage.path().toLocal8Bit();
    const QByteArray aliasPathBytes = aliasPrefix.toLocal8Bit();
    QVERIFY(::symlink(aliasTargetBytes.constData(), aliasPathBytes.constData()) == 0);
    const QString callerHome = aliasPrefix + QStringLiteral("/compositor");
    const QString sourcePath = callerHome + QStringLiteral("/game/config.ini");
    const QString canonicalSource = QFileInfo(sourcePath).canonicalFilePath();
    QVERIFY(!canonicalSource.isEmpty());
    QCOMPARE(canonicalSource, actualSource);

    const QString targetDir = QDir(destinationHome.path()).filePath(QStringLiteral("Saves"));
    QVERIFY(QDir().mkpath(targetDir));
    const QString targetPath = QDir(targetDir).filePath(QStringLiteral("config.ini"));
    const uid_t uid = getuid();
    const gid_t gid = getgid();
    m_ops->setConnectionUid(uid);
    m_ops->setStatOwner(uid, gid);
    m_ops->setUserExists(QStringLiteral("compositor"), true, uid, gid, callerHome);
    m_ops->setUserExists(QStringLiteral("player1"), true, uid, gid, destinationHome.path());
    m_ops->setSymlink(aliasPrefix, sourceStorage.path());
    m_ops->setFileExists(sourcePath, true);
    m_ops->setCanonicalMapping(sourcePath, canonicalSource);
    m_ops->setRealCopyFileSecure(true);

    QDBusReply<bool> reply =
        m_dbusInterface->call(QStringLiteral("CopyFileToUser"), sourcePath, targetPath, QStringLiteral("player1"));

    QVERIFY(reply.isValid());
    QVERIFY(reply.value());
    QFile copiedFile(targetPath);
    QVERIFY(copiedFile.open(QIODevice::ReadOnly));
    QCOMPARE(copiedFile.readAll(), contents);
    struct stat copiedStat{};
    QVERIFY(::stat(targetPath.toLocal8Bit().constData(), &copiedStat) == 0);
    QCOMPARE(copiedStat.st_uid, uid);
    QCOMPARE(copiedStat.st_gid, gid);
}

void TestCouchPlayHelper::testCopyFileToUserRejectsSymlinkedSourceComponent()
{
    m_ops->clear();
    QTemporaryDir sourceStorage(QStringLiteral("/tmp/couchplay-source-XXXXXX"));
    QTemporaryDir aliasParent(QStringLiteral("/tmp/couchplay-alias-XXXXXX"));
    QTemporaryDir destinationHome(QStringLiteral("/tmp/couchplay-destination-XXXXXX"));
    QVERIFY(sourceStorage.isValid());
    QVERIFY(aliasParent.isValid());
    QVERIFY(destinationHome.isValid());

    const QString actualCallerHome = QDir(sourceStorage.path()).filePath(QStringLiteral("compositor"));
    const QString redirectedSourceDir = QDir(sourceStorage.path()).filePath(QStringLiteral("redirected"));
    QVERIFY(QDir().mkpath(actualCallerHome));
    QVERIFY(QDir().mkpath(redirectedSourceDir));
    const QString actualSource = QDir(redirectedSourceDir).filePath(QStringLiteral("config.ini"));
    QFile sourceFile(actualSource);
    QVERIFY(sourceFile.open(QIODevice::WriteOnly));
    QVERIFY(sourceFile.write("symlinked component content") > 0);
    sourceFile.close();

    const QString aliasPrefix = QDir(aliasParent.path()).filePath(QStringLiteral("root-owned-system-home-alias"));
    const QByteArray aliasTargetBytes = sourceStorage.path().toLocal8Bit();
    const QByteArray aliasPathBytes = aliasPrefix.toLocal8Bit();
    QVERIFY(::symlink(aliasTargetBytes.constData(), aliasPathBytes.constData()) == 0);
    const QString callerHome = aliasPrefix + QStringLiteral("/compositor");
    const QString sourceComponent = callerHome + QStringLiteral("/game");
    const QString sourcePath = sourceComponent + QStringLiteral("/config.ini");
    const QByteArray componentTargetBytes = redirectedSourceDir.toLocal8Bit();
    const QByteArray componentPathBytes = sourceComponent.toLocal8Bit();
    QVERIFY(::symlink(componentTargetBytes.constData(), componentPathBytes.constData()) == 0);
    const QString canonicalSource = QFileInfo(sourcePath).canonicalFilePath();
    QVERIFY(!canonicalSource.isEmpty());
    QCOMPARE(canonicalSource, actualSource);

    const QString targetDir = QDir(destinationHome.path()).filePath(QStringLiteral("Saves"));
    QVERIFY(QDir().mkpath(targetDir));
    const QString targetPath = QDir(targetDir).filePath(QStringLiteral("config.ini"));
    const uid_t uid = getuid();
    const gid_t gid = getgid();
    m_ops->setConnectionUid(uid);
    m_ops->setStatOwner(uid, gid);
    m_ops->setUserExists(QStringLiteral("compositor"), true, uid, gid, callerHome);
    m_ops->setUserExists(QStringLiteral("player1"), true, uid, gid, destinationHome.path());
    m_ops->setSymlink(aliasPrefix, sourceStorage.path());
    m_ops->setSymlink(sourceComponent, redirectedSourceDir);
    m_ops->setFileExists(sourcePath, true);
    m_ops->setCanonicalMapping(sourcePath, canonicalSource);
    m_ops->setRealCopyFileSecure(true);
    QVERIFY(!QFileInfo::exists(targetPath));

    QDBusReply<bool> reply =
        m_dbusInterface->call(QStringLiteral("CopyFileToUser"), sourcePath, targetPath, QStringLiteral("player1"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
    QVERIFY(!QFileInfo::exists(targetPath));
}

void TestCouchPlayHelper::testCopyFileToUserRejectsSymlinkedSourceLeaf()
{
    m_ops->clear();
    QTemporaryDir sourceStorage(QStringLiteral("/tmp/couchplay-source-XXXXXX"));
    QTemporaryDir aliasParent(QStringLiteral("/tmp/couchplay-alias-XXXXXX"));
    QTemporaryDir destinationHome(QStringLiteral("/tmp/couchplay-destination-XXXXXX"));
    QVERIFY(sourceStorage.isValid());
    QVERIFY(aliasParent.isValid());
    QVERIFY(destinationHome.isValid());

    const QString actualCallerHome = QDir(sourceStorage.path()).filePath(QStringLiteral("compositor"));
    const QString actualCallerGameDir = QDir(actualCallerHome).filePath(QStringLiteral("game"));
    QVERIFY(QDir().mkpath(actualCallerGameDir));
    const QString actualSource = QDir(sourceStorage.path()).filePath(QStringLiteral("redirected.ini"));
    QFile sourceFile(actualSource);
    QVERIFY(sourceFile.open(QIODevice::WriteOnly));
    QVERIFY(sourceFile.write("symlinked leaf content") > 0);
    sourceFile.close();

    const QString aliasPrefix = QDir(aliasParent.path()).filePath(QStringLiteral("root-owned-system-home-alias"));
    const QByteArray aliasTargetBytes = sourceStorage.path().toLocal8Bit();
    const QByteArray aliasPathBytes = aliasPrefix.toLocal8Bit();
    QVERIFY(::symlink(aliasTargetBytes.constData(), aliasPathBytes.constData()) == 0);
    const QString callerHome = aliasPrefix + QStringLiteral("/compositor");
    const QString sourcePath = callerHome + QStringLiteral("/game/config.ini");
    const QByteArray sourceTargetBytes = actualSource.toLocal8Bit();
    const QByteArray sourcePathBytes = sourcePath.toLocal8Bit();
    QVERIFY(::symlink(sourceTargetBytes.constData(), sourcePathBytes.constData()) == 0);
    const QString canonicalSource = QFileInfo(sourcePath).canonicalFilePath();
    QVERIFY(!canonicalSource.isEmpty());
    QCOMPARE(canonicalSource, actualSource);

    const QString targetDir = QDir(destinationHome.path()).filePath(QStringLiteral("Saves"));
    QVERIFY(QDir().mkpath(targetDir));
    const QString targetPath = QDir(targetDir).filePath(QStringLiteral("config.ini"));
    const uid_t uid = getuid();
    const gid_t gid = getgid();
    m_ops->setConnectionUid(uid);
    m_ops->setStatOwner(uid, gid);
    m_ops->setUserExists(QStringLiteral("compositor"), true, uid, gid, callerHome);
    m_ops->setUserExists(QStringLiteral("player1"), true, uid, gid, destinationHome.path());
    m_ops->setSymlink(aliasPrefix, sourceStorage.path());
    m_ops->setSymlink(sourcePath, canonicalSource);
    m_ops->setCanonicalMapping(sourcePath, canonicalSource);
    m_ops->setRealCopyFileSecure(true);
    QVERIFY(!QFileInfo::exists(targetPath));

    QDBusReply<bool> reply =
        m_dbusInterface->call(QStringLiteral("CopyFileToUser"), sourcePath, targetPath, QStringLiteral("player1"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
    QVERIFY(!QFileInfo::exists(targetPath));
}

void TestCouchPlayHelper::testMirrorDirectoryContentsSuccess()
{
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, homeDir.path());

    QDir stagingDir(homeDir.path() + QStringLiteral("/staging"));
    QVERIFY(stagingDir.mkpath(QStringLiteral(".")));
    m_ops->setFileExists(stagingDir.path(), true); // validation layer consults the mock
    m_ops->setDirectoryExists(stagingDir.path(), true);
    {
        QFile f(stagingDir.filePath(QStringLiteral("config.ini")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("player=1\n");
    }

    // Existing merge target (e.g. an overlay mount point) with its own file
    QDir targetDir(homeDir.path() + QStringLiteral("/Games/MyGame"));
    QVERIFY(targetDir.mkpath(QStringLiteral(".")));
    m_ops->setFileExists(targetDir.path(), true);
    m_ops->setDirectoryExists(targetDir.path(), true);
    {
        QFile existing(targetDir.filePath(QStringLiteral("keep.txt")));
        QVERIFY(existing.open(QIODevice::WriteOnly));
        existing.write("keep");
    }

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("MirrorDirectoryContents"),
                                                   QStringLiteral("player1"),
                                                   stagingDir.path(),
                                                   QStringLiteral("Games/MyGame"));

    QVERIFY(reply.isValid());
    QVERIFY(reply.value());

    // Merge: staged file copied in, pre-existing file untouched
    QFile copied(targetDir.filePath(QStringLiteral("config.ini")));
    QVERIFY(copied.open(QIODevice::ReadOnly));
    QCOMPARE(copied.readAll(), QByteArray("player=1\n"));
    QFile kept(targetDir.filePath(QStringLiteral("keep.txt")));
    QVERIFY(kept.open(QIODevice::ReadOnly));
    QCOMPARE(kept.readAll(), QByteArray("keep"));

    if (geteuid() == 0) {
        struct stat st;
        QCOMPARE(::stat(targetDir.filePath(QStringLiteral("config.ini")).toLocal8Bit().constData(), &st), 0);
        QCOMPARE(st.st_uid, static_cast<uid_t>(1001));
    }
}

void TestCouchPlayHelper::testMirrorDirectoryContentsReplacesExistingSymlink()
{
    // Merge-overwrite semantics: a staged symlink replaces a stale symlink
    // (and a regular file) of the same name instead of being silently dropped
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, homeDir.path());

    QDir stagingDir(homeDir.path() + QStringLiteral("/staging"));
    QVERIFY(stagingDir.mkpath(QStringLiteral(".")));
    m_ops->setFileExists(stagingDir.path(), true); // validation layer consults the mock
    m_ops->setDirectoryExists(stagingDir.path(), true);
    QCOMPARE(::symlink("new-target", stagingDir.filePath(QStringLiteral("link")).toLocal8Bit().constData()), 0);
    QCOMPARE(::symlink("entry-link", stagingDir.filePath(QStringLiteral("entry")).toLocal8Bit().constData()), 0);

    QDir targetDir(homeDir.path() + QStringLiteral("/Games/MyGame"));
    QVERIFY(targetDir.mkpath(QStringLiteral(".")));
    m_ops->setFileExists(targetDir.path(), true);
    m_ops->setDirectoryExists(targetDir.path(), true);
    QCOMPARE(::symlink("old-target", targetDir.filePath(QStringLiteral("link")).toLocal8Bit().constData()), 0);
    {
        QFile stale(targetDir.filePath(QStringLiteral("entry"))); // regular file replaced by a link
        QVERIFY(stale.open(QIODevice::WriteOnly));
        stale.write("data");
    }

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("MirrorDirectoryContents"),
                                                   QStringLiteral("player1"),
                                                   stagingDir.path(),
                                                   QStringLiteral("Games/MyGame"));

    QVERIFY(reply.isValid());
    QVERIFY(reply.value());

    // symLinkTarget() resolves the raw link string against the link's directory
    QCOMPARE(QFileInfo(targetDir.filePath(QStringLiteral("link"))).symLinkTarget(),
             targetDir.filePath(QStringLiteral("new-target")));
    const QFileInfo replacedEntry(targetDir.filePath(QStringLiteral("entry")));
    QVERIFY(replacedEntry.isSymLink());
    QCOMPARE(replacedEntry.symLinkTarget(), targetDir.filePath(QStringLiteral("entry-link")));
}

void TestCouchPlayHelper::testMirrorDirectoryContentsReplacesNonEmptyDirectoryWithSymlink()
{
    // Mirroring preserves the source entry type, recursively replacing stale destinations.
    QTemporaryDir homeDir;
    QVERIFY(homeDir.isValid());
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, homeDir.path());

    QDir stagingDir(homeDir.path() + QStringLiteral("/staging"));
    QVERIFY(stagingDir.mkpath(QStringLiteral(".")));
    m_ops->setFileExists(stagingDir.path(), true);
    m_ops->setDirectoryExists(stagingDir.path(), true);
    QCOMPARE(::symlink("target", stagingDir.filePath(QStringLiteral("entry")).toLocal8Bit().constData()), 0);

    QDir targetDir(homeDir.path() + QStringLiteral("/Games/MyGame"));
    QVERIFY(targetDir.mkpath(QStringLiteral(".")));
    m_ops->setFileExists(targetDir.path(), true);
    m_ops->setDirectoryExists(targetDir.path(), true);
    QVERIFY(targetDir.mkpath(QStringLiteral("entry")));
    {
        QFile f(targetDir.filePath(QStringLiteral("entry/keep.txt")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("keep");
    }

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("MirrorDirectoryContents"),
                                                   QStringLiteral("player1"),
                                                   stagingDir.path(),
                                                   QStringLiteral("Games/MyGame"));

    QVERIFY(reply.isValid());
    QVERIFY(reply.value());
    QVERIFY(QFileInfo(targetDir.filePath(QStringLiteral("entry"))).isSymLink());
    QVERIFY(!QFileInfo(targetDir.filePath(QStringLiteral("entry/keep.txt"))).exists());
}

void TestCouchPlayHelper::testMirrorDirectoryContentsTargetNotExists()
{
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, QStringLiteral("/home/player1"));
    m_ops->setFileExists(QStringLiteral("/home/compositor/staging"), true);
    m_ops->setDirectoryExists(QStringLiteral("/home/compositor/staging"), true);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("MirrorDirectoryContents"),
                                                   QStringLiteral("player1"),
                                                   QStringLiteral("/home/compositor/staging"),
                                                   QStringLiteral("Games/MyGame")); // not mocked -> missing

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
    QCOMPARE(m_ops->m_processInvocations.size(), 0);
}

void TestCouchPlayHelper::testMirrorDirectoryContentsTraversalTarget()
{
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, QStringLiteral("/home/player1"));
    m_ops->setFileExists(QStringLiteral("/home/compositor/staging"), true);
    m_ops->setDirectoryExists(QStringLiteral("/home/compositor/staging"), true);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("MirrorDirectoryContents"),
                                                   QStringLiteral("player1"),
                                                   QStringLiteral("/home/compositor/staging"),
                                                   QStringLiteral("../escape"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
    QCOMPARE(m_ops->m_processInvocations.size(), 0);
}

void TestCouchPlayHelper::testIsPathWithinAllowedPrefixMountRoots()
{
    // Must accept the same mount roots SetPathAclWithParents treats as
    // traversal stop boundaries, so external libraries keep working
    QVERIFY(m_helper->isPathWithinAllowedPrefix(QStringLiteral("/home/player1/games")));
    QVERIFY(m_helper->isPathWithinAllowedPrefix(QStringLiteral("/var/home/player1/games")));
    QVERIFY(m_helper->isPathWithinAllowedPrefix(QStringLiteral("/run/media/user/disk/steamlibrary")));
    QVERIFY(m_helper->isPathWithinAllowedPrefix(QStringLiteral("/media/steamlibrary")));
    QVERIFY(m_helper->isPathWithinAllowedPrefix(QStringLiteral("/mnt/steamlibrary")));
    QVERIFY(m_helper->isPathWithinAllowedPrefix(QStringLiteral("/tmp/couchplay/share")));
    QVERIFY(m_helper->isPathWithinAllowedPrefix(QStringLiteral("/opt/MyGame")));
    QVERIFY(m_helper->isPathWithinAllowedPrefix(QStringLiteral("/srv/games/MyGame")));
    QVERIFY(!m_helper->isPathWithinAllowedPrefix(QStringLiteral("/opt")));
    QVERIFY(!m_helper->isPathWithinAllowedPrefix(QStringLiteral("/srv")));

    QVERIFY(!m_helper->isPathWithinAllowedPrefix(QStringLiteral("/etc")));
    QVERIFY(!m_helper->isPathWithinAllowedPrefix(QStringLiteral("/usr/share/games")));
    QVERIFY(!m_helper->isPathWithinAllowedPrefix(QStringLiteral("/home"))); // root itself, not a subdir

    // ".." inside a name is fine; a whole ".." component is traversal
    QVERIFY(m_helper->isPathWithinAllowedPrefix(QStringLiteral("/home/deck/Games/Foo..Bar")));
    QVERIFY(m_helper->isPathWithinAllowedPrefix(QStringLiteral("/mnt/library..2/saves")));
    QVERIFY(!m_helper->isPathWithinAllowedPrefix(QStringLiteral("/home/deck/Games/..")));
    QVERIFY(!m_helper->isPathWithinAllowedPrefix(QStringLiteral("/tmp/a/../../etc")));
}

void TestCouchPlayHelper::testIsPathWithinAllowedPrefixCanonicalAtomicAliases()
{
    m_ops->clear();
    m_ops->setCanonicalMapping(QStringLiteral("/mnt"), QStringLiteral("/var/mnt"));
    m_ops->setCanonicalMapping(QStringLiteral("/opt"), QStringLiteral("/var/opt"));
    m_ops->setCanonicalMapping(QStringLiteral("/srv"), QStringLiteral("/var/srv"));

    QVERIFY(m_helper->isPathWithinAllowedPrefix(QStringLiteral("/var/mnt/steamlibrary")));
    QVERIFY(m_helper->isPathWithinAllowedPrefix(QStringLiteral("/var/mnt/library..2/saves")));
    QVERIFY(m_helper->isPathWithinAllowedPrefix(QStringLiteral("/var/opt/MyApp")));
    QVERIFY(m_helper->isPathWithinAllowedPrefix(QStringLiteral("/var/srv/games/MyApp")));
    QVERIFY(!m_helper->isPathWithinAllowedPrefix(QStringLiteral("/var/opt")));
    QVERIFY(!m_helper->isPathWithinAllowedPrefix(QStringLiteral("/var/srv")));
    QVERIFY(!m_helper->isPathWithinAllowedPrefix(QStringLiteral("/var/opt/../etc")));
    QVERIFY(!m_helper->isPathWithinAllowedPrefix(QStringLiteral("/var/mnt")));
    QVERIFY(!m_helper->isPathWithinAllowedPrefix(QStringLiteral("/var/mnt/")));
    QVERIFY(!m_helper->isPathWithinAllowedPrefix(QStringLiteral("/var/mnt/../etc")));
    QVERIFY(!m_helper->isPathWithinAllowedPrefix(QStringLiteral("/var/mnted/steamlibrary")));
    QVERIFY(!m_helper->isPathWithinAllowedPrefix(QStringLiteral("/var/other/steamlibrary")));
}

void TestCouchPlayHelper::testIsPathWithinAllowedPrefixCanonicalAtomicAliasesAbsent()
{
    m_ops->clear();
    QVERIFY(!m_helper->isPathWithinAllowedPrefix(QStringLiteral("/var/mnt/steamlibrary")));
    QVERIFY(!m_helper->isPathWithinAllowedPrefix(QStringLiteral("/var/opt/Applications")));
    QVERIFY(!m_helper->isPathWithinAllowedPrefix(QStringLiteral("/var/srv/games/MyGame")));
}

void TestCouchPlayHelper::testDirectoryPreparationValidation()
{
    QList<DirectoryPreparation> parsed;
    const auto item = [](const QString &source, const QString &mode, const QString &target, const QString &stage) {
        return QVariant(DirectoryPreparation{source, mode, target, stage}.toVariant());
    };
    const QVariantList valid{item(QStringLiteral("/home/source/config"), QStringLiteral("bind"),
                                  QStringLiteral(".config/game"), QString())};
    QVERIFY(DirectoryPlan::parseDirectoryPreparations(valid, &parsed));
    QCOMPARE(parsed.size(), 1);
    QCOMPARE(parsed.constFirst().targetAlias, QStringLiteral(".config/game"));

    const QVariantList traversal{item(QStringLiteral("/home/source/config"), QStringLiteral("bind"),
                                      QStringLiteral("../outside"), QString())};
    QVERIFY(!DirectoryPlan::parseDirectoryPreparations(traversal, &parsed));
    QVERIFY(parsed.isEmpty());

    const QVariantList overlap{item(QStringLiteral("/home/source/a"), QStringLiteral("bind"),
                                    QStringLiteral(".config"), QString()),
                              item(QStringLiteral("/home/source/b"), QStringLiteral("copy"),
                                    QStringLiteral(".config/game"), QString())};
    QVERIFY(!DirectoryPlan::parseDirectoryPreparations(overlap, &parsed));
    QVERIFY(parsed.isEmpty());
    const QVariantList siblingTargets{item(QStringLiteral("/home/source/a"), QStringLiteral("bind"),
                                           QStringLiteral(".config"), QString()),
                                      item(QStringLiteral("/home/source/b"), QStringLiteral("copy"),
                                           QStringLiteral(".local/game"), QString())};
    QVERIFY(DirectoryPlan::parseDirectoryPreparations(siblingTargets, &parsed));

    const QVariantList aclWithTarget{item(QStringLiteral("/home/source"), QStringLiteral("acl"),
                                          QStringLiteral(".config"), QString())};
    QVERIFY(!DirectoryPlan::parseDirectoryPreparations(aclWithTarget, &parsed));

    const QVariantList wrongType{QStringLiteral("not a directory map")};
    QVERIFY(!DirectoryPlan::parseDirectoryPreparations(wrongType, &parsed));
    QVariantMap unknownField = item(QStringLiteral("/home/source"), QStringLiteral("bind"),
                                    QStringLiteral(".config/game"), QString()).toMap();
    unknownField.insert(QStringLiteral("extra"), QStringLiteral("value"));
    QVERIFY(!DirectoryPlan::parseDirectoryPreparations({unknownField}, &parsed));
    const QVariantList duplicateTarget{item(QStringLiteral("/home/source/a"), QStringLiteral("bind"),
                                             QStringLiteral(".config/game"), QString()),
                                       item(QStringLiteral("/home/source/b"), QStringLiteral("copy"),
                                             QStringLiteral(".config/game"), QString())};
    QVERIFY(!DirectoryPlan::parseDirectoryPreparations(duplicateTarget, &parsed));
}


void TestCouchPlayHelper::testLaunchInstanceRejectsTooManyDirectoryPreparations()
{
    configureLaunchFixture();
    QVariantList preparations;
    preparations.reserve(513);
    for (int index = 0; index < 513; ++index) {
        const QString source = QStringLiteral("/home/compositor/source-%1").arg(index);
        m_ops->setDirectoryExists(source, true);
        preparations.append(DirectoryPreparation{source,
                                                  QStringLiteral("bind"),
                                                  QStringLiteral(".config/item-%1").arg(index),
                                                  {}}
                                .toVariant());
    }
    const QDBusReply<qint64> reply = launchPlayer1(preparations);
    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
    QVERIFY(m_ops->m_launchManifests.isEmpty());
    QVERIFY(m_ops->m_transientServiceName.isEmpty());
}



void TestCouchPlayHelper::testSetPathAclWithParentsSymlinkEscapeRejected()
{
    // setfacl follows symlinks: an allowed-prefix path resolving outside the
    // allowed roots must be refused before any ACL is applied
    m_ops->clear();
    m_ops->setMockProcessStart(true);
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, QStringLiteral("/home/player1"));
    m_ops->setFileExists(QStringLiteral("/home/deck/shared-link"), true);
    m_ops->setDirectoryExists(QStringLiteral("/home/deck/shared-link"), true);
    m_ops->setCanonicalMapping(QStringLiteral("/home/deck/shared-link"), QStringLiteral("/root/private"));

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("SetPathAclWithParents"),
                                                   QStringLiteral("/home/deck/shared-link"),
                                                   QStringLiteral("player1"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
    QCOMPARE(m_ops->m_processInvocations.size(), 0);
}

void TestCouchPlayHelper::testSetPathAclWithParentsCanonicalMntAliasEscapeRejected()
{
    m_ops->clear();
    m_ops->setMockProcessStart(true);
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, QStringLiteral("/home/player1"));
    m_ops->setCanonicalMapping(QStringLiteral("/mnt"), QStringLiteral("/var/mnt"));
    m_ops->setFileExists(QStringLiteral("/var/mnt/shared-link"), true);
    m_ops->setDirectoryExists(QStringLiteral("/var/mnt/shared-link"), true);
    m_ops->setCanonicalMapping(QStringLiteral("/var/mnt/shared-link"), QStringLiteral("/etc/private"));

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("SetPathAclWithParents"),
                                                   QStringLiteral("/var/mnt/shared-link"),
                                                   QStringLiteral("player1"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
    QCOMPARE(m_ops->m_processInvocations.size(), 0);
}

void TestCouchPlayHelper::testSecureReadExecuteAclTreePreservesAclAndSkipsSymlinks()
{
    QTemporaryDir directory(QStringLiteral("/tmp/couchplay-acl-test-XXXXXX"));
    QVERIFY(directory.isValid());
    const uid_t ownerUid = ::geteuid();
    const uid_t targetUid = ownerUid == 65534 ? 65533 : 65534;
    uid_t unrelatedUid = targetUid == 65534 ? 65533 : 65534;
    if (unrelatedUid == ownerUid) unrelatedUid = 65532;
    const QString root = directory.filePath(QStringLiteral("tree"));
    const QString child = root + QStringLiteral("/folder");
    const QString gamePath = child + QStringLiteral("/game");
    const QString outside = directory.filePath(QStringLiteral("outside"));
    const QString outsideFile = outside + QStringLiteral("/private");
    QVERIFY(QDir().mkpath(child));
    QVERIFY(QDir().mkpath(outside));
    QVERIFY(QFile::setPermissions(directory.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                                       | QFileDevice::ExeOwner | QFileDevice::ExeGroup
                                                       | QFileDevice::ExeOther));
    QFile game(gamePath);
    QVERIFY(game.open(QIODevice::WriteOnly));
    QCOMPARE(game.write("game"), qint64(4));
    game.close();
    QFile privateFile(outsideFile);
    QVERIFY(privateFile.open(QIODevice::WriteOnly));
    QCOMPARE(privateFile.write("private"), qint64(7));
    privateFile.close();
    QVERIFY(QFile::setPermissions(root, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    QVERIFY(QFile::setPermissions(child, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    QVERIFY(QFile::setPermissions(gamePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    QVERIFY(QFile::setPermissions(outside, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    QVERIFY(QFile::setPermissions(outsideFile, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    const QByteArray linkPath = QFile::encodeName(root + QStringLiteral("/escape"));
    const QByteArray outsidePath = QFile::encodeName(outside);
    QCOMPARE(::symlink(outsidePath.constData(), linkPath.constData()), 0);

    QString userEntries;
    if (targetUid < unrelatedUid) {
        userEntries = QStringLiteral("u:%1:r--,u:%2:rw-").arg(targetUid).arg(unrelatedUid);
    } else {
        userEntries = QStringLiteral("u:%1:rw-,u:%2:r--").arg(unrelatedUid).arg(targetUid);
    }
    const QByteArray aclText = QStringLiteral("u::rw-,%1,g::r--,m::r--,o::---").arg(userEntries).toLatin1();
    acl_t seededAcl = ::acl_from_text(aclText.constData());
    QVERIFY(seededAcl);
    QCOMPARE(::acl_valid(seededAcl), 0);
    const QByteArray gamePathBytes = QFile::encodeName(gamePath);
    QCOMPARE(::acl_set_file(gamePathBytes.constData(), ACL_TYPE_ACCESS, seededAcl), 0);
    const QByteArray childPath = QFile::encodeName(child);
    QCOMPARE(::acl_set_file(childPath.constData(), ACL_TYPE_DEFAULT, seededAcl), 0);
    ::acl_free(seededAcl);

    const QByteArray rootPath = QFile::encodeName(root);
    const int rootFd = ::open(rootPath.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(rootFd >= 0);
    QCOMPARE(SecureFs::applyReadExecuteAclTree(rootFd, targetUid), 0);
    ::close(rootFd);

    acl_t gameAcl = ::acl_get_file(gamePathBytes.constData(), ACL_TYPE_ACCESS);
    QVERIFY(gameAcl);
    int targetPerms = 0;
    int unrelatedPerms = 0;
    int maskPerms = 0;
    int ownerPerms = 0;
    int groupPerms = 0;
    int otherPerms = 0;
    bool targetFound = false;
    bool unrelatedFound = false;
    acl_entry_t entry;
    int next = ::acl_get_entry(gameAcl, ACL_FIRST_ENTRY, &entry);
    while (next == 1) {
        acl_tag_t tag;
        QVERIFY(::acl_get_tag_type(entry, &tag) == 0);
        const auto permissionBits = [](acl_entry_t aclEntry) {
            acl_permset_t permissions;
            if (::acl_get_permset(aclEntry, &permissions) != 0) return -1;
            const int read = ::acl_get_perm(permissions, ACL_READ);
            const int write = ::acl_get_perm(permissions, ACL_WRITE);
            const int execute = ::acl_get_perm(permissions, ACL_EXECUTE);
            if (read < 0 || write < 0 || execute < 0) return -1;
            return (read ? ACL_READ : 0) | (write ? ACL_WRITE : 0) | (execute ? ACL_EXECUTE : 0);
        };
        if (tag == ACL_MASK) {
            maskPerms = permissionBits(entry);
            QVERIFY(maskPerms >= 0);
        } else if (tag == ACL_USER_OBJ) {
            ownerPerms = permissionBits(entry);
            QVERIFY(ownerPerms >= 0);
        } else if (tag == ACL_GROUP_OBJ) {
            groupPerms = permissionBits(entry);
            QVERIFY(groupPerms >= 0);
        } else if (tag == ACL_OTHER) {
            otherPerms = permissionBits(entry);
            QVERIFY(otherPerms >= 0);
        } else if (tag == ACL_USER) {
            auto *qualifier = static_cast<uid_t *>(::acl_get_qualifier(entry));
            QVERIFY(qualifier);
            const uid_t entryUid = *qualifier;
            ::acl_free(qualifier);
            const int bits = permissionBits(entry);
            QVERIFY(bits >= 0);
            if (entryUid == targetUid) {
                targetFound = true;
                targetPerms = bits;
            } else if (entryUid == unrelatedUid) {
                unrelatedFound = true;
                unrelatedPerms = bits;
            }
        }
        next = ::acl_get_entry(gameAcl, ACL_NEXT_ENTRY, &entry);
    }
    QCOMPARE(next, 0);
    ::acl_free(gameAcl);
    QVERIFY(targetFound);
    QVERIFY(unrelatedFound);
    QCOMPARE(targetPerms & maskPerms, ACL_READ | ACL_EXECUTE);
    QCOMPARE(unrelatedPerms & maskPerms, ACL_READ);
    QCOMPARE(ownerPerms, ACL_READ | ACL_WRITE);
    QCOMPARE(groupPerms & maskPerms, ACL_READ);
    QCOMPARE(otherPerms, 0);

    // New downloads inherit read/traverse access without acquiring write access.
    const QByteArray downloadDirectory = QFile::encodeName(root + QStringLiteral("/new-download"));
    QCOMPARE(::mkdir(downloadDirectory.constData(), 0770), 0);
    const QByteArray inheritedPath = downloadDirectory + "/game";
    const QByteArray existingDefaultPath = childPath + "/new-game";
    for (const QByteArray &path : {inheritedPath, existingDefaultPath}) {
        const int fd = ::open(path.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0660);
        QVERIFY(fd >= 0);
        QCOMPARE(::write(fd, "game", 4), ssize_t(4));
        QCOMPARE(::close(fd), 0);
    }
    const QByteArray escapedPath = QFile::encodeName(root + QStringLiteral("/escape/private"));

    if (::geteuid() == 0) {
        const pid_t childPid = ::fork();
        QVERIFY(childPid >= 0);
        if (childPid == 0) {
            if (::setgroups(0, nullptr) != 0 || ::setgid(targetUid) != 0 || ::setuid(targetUid) != 0) {
                _exit(77);
            }
            const int readableFd = ::open(gamePathBytes.constData(), O_RDONLY | O_CLOEXEC);
            if (readableFd < 0) _exit(1);
            char contents[4];
            const bool readOk = ::read(readableFd, contents, sizeof(contents)) == sizeof(contents)
                && std::memcmp(contents, "game", sizeof(contents)) == 0;
            ::close(readableFd);
            const int escapedFd = ::open(escapedPath.constData(), O_RDONLY | O_CLOEXEC);
            if (escapedFd >= 0) ::close(escapedFd);
            bool inheritedReadOnly = true;
            for (const QByteArray &path : {inheritedPath, existingDefaultPath}) {
                const int readFd = ::open(path.constData(), O_RDONLY | O_CLOEXEC);
                const int writeFd = ::open(path.constData(), O_WRONLY | O_CLOEXEC);
                inheritedReadOnly = inheritedReadOnly && readFd >= 0 && writeFd < 0;
                if (readFd >= 0) ::close(readFd);
                if (writeFd >= 0) ::close(writeFd);
            }
            _exit(readOk && inheritedReadOnly && escapedFd < 0 ? 0 : 1);
        }
        int status = 0;
        QCOMPARE(::waitpid(childPid, &status, 0), childPid);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 77) {
            QSKIP("The test environment cannot drop credentials to the ACL target UID");
        }
        QVERIFY(WIFEXITED(status));
        QCOMPARE(WEXITSTATUS(status), 0);
    }
}

void TestCouchPlayHelper::testSecureReadExecuteAclTreeSeedsOwnerOnlyDefault()
{
    QTemporaryDir directory(QStringLiteral("/tmp/couchplay-acl-test-XXXXXX"));
    QVERIFY(directory.isValid());
    const uid_t targetUid = ::geteuid() == 65534 ? 65533 : 65534;
    const QString root = directory.filePath(QStringLiteral("share"));
    QVERIFY(QDir().mkpath(root));
    const QByteArray rootPath = QFile::encodeName(root);
    QCOMPARE(::chmod(rootPath.constData(), 0777), 0);
    const int rootFd = ::open(rootPath.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(rootFd >= 0);
    QCOMPARE(SecureFs::applyReadExecuteAclTree(rootFd, targetUid), 0);
    ::close(rootFd);

    acl_t defaultAcl = ::acl_get_file(rootPath.constData(), ACL_TYPE_DEFAULT);
    QVERIFY(defaultAcl);
    QHash<int, int> permissionsByTag;
    int targetPermissions = -1;
    acl_entry_t entry;
    int next = ::acl_get_entry(defaultAcl, ACL_FIRST_ENTRY, &entry);
    while (next == 1) {
        acl_tag_t tag;
        QVERIFY(::acl_get_tag_type(entry, &tag) == 0);
        acl_permset_t permissions;
        QVERIFY(::acl_get_permset(entry, &permissions) == 0);
        const int bits = (::acl_get_perm(permissions, ACL_READ) > 0 ? ACL_READ : 0)
            | (::acl_get_perm(permissions, ACL_WRITE) > 0 ? ACL_WRITE : 0)
            | (::acl_get_perm(permissions, ACL_EXECUTE) > 0 ? ACL_EXECUTE : 0);
        if (tag == ACL_USER) {
            auto *qualifier = static_cast<uid_t *>(::acl_get_qualifier(entry));
            QVERIFY(qualifier);
            if (*qualifier == targetUid) targetPermissions = bits;
            ::acl_free(qualifier);
        } else {
            permissionsByTag.insert(static_cast<int>(tag), bits);
        }
        next = ::acl_get_entry(defaultAcl, ACL_NEXT_ENTRY, &entry);
    }
    QCOMPARE(next, 0);
    ::acl_free(defaultAcl);
    // New files inherit only the target's read access, never group/other access copied from a 0777 directory.
    QCOMPARE(targetPermissions, ACL_READ | ACL_EXECUTE);
    QCOMPARE(permissionsByTag.value(ACL_GROUP_OBJ, -1), 0);
    QCOMPARE(permissionsByTag.value(ACL_OTHER, -1), 0);
    QCOMPARE(permissionsByTag.value(ACL_MASK, -1), ACL_READ | ACL_EXECUTE);
}

void TestCouchPlayHelper::testSecureWriteRejectsSymlinkLeaf()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString protectedFile = tempDir.path() + QStringLiteral("/protected");
    QFile file(protectedFile);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write("original") == 8);
    file.close();
    QVERIFY(QFile::link(protectedFile, tempDir.path() + QStringLiteral("/target")));

    const int parentFd = SecureFs::openBaseDir(tempDir.path());
    QVERIFY(parentFd >= 0);
    QCOMPARE(SecureFs::writeFileAt(parentFd,
                                   QStringLiteral("target"),
                                   QByteArrayLiteral("replacement"),
                                   getuid(),
                                   getgid()),
             -ELOOP);
    ::close(parentFd);

    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArrayLiteral("original"));
}


void TestCouchPlayHelper::testChangeDeviceOwnerInvalidPathNotUnderDevInput()
{
    m_ops->clear();

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("ChangeDeviceOwner"),
                                                   QStringLiteral("/dev/sda"), // Not under /dev/input/
                                                   1000u);

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testChangeDeviceOwnerInvalidPathTraversal()
{
    m_ops->clear();

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("ChangeDeviceOwner"),
                                                   QStringLiteral("/dev/input/../sda"), // Path traversal
                                                   1000u);

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testChangeDeviceOwnerInvalidPathNotExists()
{
    m_ops->clear();

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("ChangeDeviceOwner"),
                                                   QStringLiteral("/dev/input/event0"), // Doesn't exist
                                                   1000u);

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testChangeDeviceOwnerInvalidPathNotCharDevice()
{
    m_ops->clear();
    m_ops->setFileExists(QStringLiteral("/dev/input/event0"), true);
    m_ops->setChownResult(0);
    m_ops->setChmodResult(0);

    QDBusReply<bool> reply =
        m_dbusInterface->call(QStringLiteral("ChangeDeviceOwner"), QStringLiteral("/dev/input/event0"), 1000u);

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testChangeDeviceOwnerSuccess()
{
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("testuser"), true, 1000, 1000);
    m_ops->setFileExists(QStringLiteral("/dev/input/event0"), true);
    m_ops->setChownResult(0);
    m_ops->setChmodResult(0);

    QDBusReply<bool> reply =
        m_dbusInterface->call(QStringLiteral("ChangeDeviceOwner"), QStringLiteral("/dev/input/event0"), 1000u);

    QVERIFY(reply.isValid());
    QVERIFY(reply.value());
}

void TestCouchPlayHelper::testChangeDeviceOwnerAuthorizationDenied()
{
    m_ops->clear();
    m_ops->setAuthResult(false);
    m_ops->setFileExists(QStringLiteral("/dev/input/event0"), true);
    m_ops->setChownResult(0);
    m_ops->setChmodResult(0);

    QDBusReply<bool> reply =
        m_dbusInterface->call(QStringLiteral("ChangeDeviceOwner"), QStringLiteral("/dev/input/event0"), 1000u);

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::AccessDenied);
}

void TestCouchPlayHelper::testChangeDeviceOwnerUserNotFound()
{
    m_ops->clear();
    m_ops->setFileExists(QStringLiteral("/dev/input/event0"), true);
    m_ops->setChownResult(0);
    m_ops->setChmodResult(0);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("ChangeDeviceOwner"),
                                                   QStringLiteral("/dev/input/event0"),
                                                   9999u // Nonexistent UID
    );

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testChangeDeviceOwnerChownFails()
{
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("testuser"), true, 1000, 1000);
    m_ops->setFileExists(QStringLiteral("/dev/input/event0"), true);
    m_ops->setChownResult(-1);

    QDBusReply<bool> reply =
        m_dbusInterface->call(QStringLiteral("ChangeDeviceOwner"), QStringLiteral("/dev/input/event0"), 1000u);

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::Failed);
}

void TestCouchPlayHelper::testChangeDeviceOwnerChmodFails()
{
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("testuser"), true, 1000, 1000);
    m_ops->setFileExists(QStringLiteral("/dev/input/event0"), true);
    m_ops->setChownResult(0);
    m_ops->setChmodResult(-1);

    QDBusReply<bool> reply =
        m_dbusInterface->call(QStringLiteral("ChangeDeviceOwner"), QStringLiteral("/dev/input/event0"), 1000u);

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::Failed);
}

void TestCouchPlayHelper::testResetDeviceOwnerSuccess()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setFileExists(QStringLiteral("/dev/input/event0"), true);
    m_ops->setChownResult(0);
    m_ops->setChmodResult(0);

    QDBusReply<bool> reply =
        m_dbusInterface->call(QStringLiteral("ResetDeviceOwner"), QStringLiteral("/dev/input/event0"));

    QVERIFY(reply.isValid());
    QVERIFY(reply.value());
}

void TestCouchPlayHelper::testResetDeviceOwnerInvalidPathNotUnderDevInput()
{
    m_ops->clear();

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("ResetDeviceOwner"), QStringLiteral("/dev/sda"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testResetDeviceOwnerInvalidPathTraversal()
{
    m_ops->clear();

    QDBusReply<bool> reply =
        m_dbusInterface->call(QStringLiteral("ResetDeviceOwner"), QStringLiteral("/dev/input/../sda"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testResetDeviceOwnerAuthorizationDenied()
{
    m_ops->clear();
    m_ops->setAuthResult(false);
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setFileExists(QStringLiteral("/dev/input/event0"), true);
    m_ops->setChownResult(0);
    m_ops->setChmodResult(0);

    QDBusReply<bool> reply =
        m_dbusInterface->call(QStringLiteral("ResetDeviceOwner"), QStringLiteral("/dev/input/event0"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::AccessDenied);
}

void TestCouchPlayHelper::testResetDeviceOwnerChownFails()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setFileExists(QStringLiteral("/dev/input/event0"), true);
    m_ops->setChownResult(-1);

    QDBusReply<bool> reply =
        m_dbusInterface->call(QStringLiteral("ResetDeviceOwner"), QStringLiteral("/dev/input/event0"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::Failed);
}

void TestCouchPlayHelper::testResetDeviceOwnerChmodFails()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setFileExists(QStringLiteral("/dev/input/event0"), true);
    m_ops->setChownResult(0);
    m_ops->setChmodResult(-1);

    QDBusReply<bool> reply =
        m_dbusInterface->call(QStringLiteral("ResetDeviceOwner"), QStringLiteral("/dev/input/event0"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::Failed);
}

void TestCouchPlayHelper::testResetAllDevicesEmpty()
{
    m_ops->clear();

    QDBusReply<int> reply = m_dbusInterface->call(QStringLiteral("ResetAllDevices"));

    QVERIFY(reply.isValid());
    QCOMPARE(reply.value(), 0);
}

void TestCouchPlayHelper::testResetAllDevicesSuccess()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setUserExists(QStringLiteral("testuser1"), true, 1000, 1000);
    m_ops->setUserExists(QStringLiteral("testuser2"), true, 1001, 1001);
    m_ops->setFileExists(QStringLiteral("/dev/input/event0"), true);
    m_ops->setFileExists(QStringLiteral("/dev/input/event1"), true);
    m_ops->setChownResult(0);
    m_ops->setChmodResult(0);

    QDBusReply<bool> reply1 =
        m_dbusInterface->call(QStringLiteral("ChangeDeviceOwner"), QStringLiteral("/dev/input/event0"), 1000u);
    QDBusReply<bool> reply2 =
        m_dbusInterface->call(QStringLiteral("ChangeDeviceOwner"), QStringLiteral("/dev/input/event1"), 1001u);

    QVERIFY(reply1.isValid() && reply2.isValid());

    QDBusReply<int> resetReply = m_dbusInterface->call(QStringLiteral("ResetAllDevices"));

    QVERIFY(resetReply.isValid());
    QCOMPARE(resetReply.value(), 2);
}

void TestCouchPlayHelper::testResetAllDevicesPartialFailure()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setUserExists(QStringLiteral("testuser1"), true, 1000, 1000);
    m_ops->setUserExists(QStringLiteral("testuser2"), true, 1001, 1001);
    m_ops->setFileExists(QStringLiteral("/dev/input/event0"), true);
    m_ops->setFileExists(QStringLiteral("/dev/input/event1"), true);
    m_ops->setChownResult(0);
    m_ops->setChmodResult(0);

    QDBusReply<bool> reply1 =
        m_dbusInterface->call(QStringLiteral("ChangeDeviceOwner"), QStringLiteral("/dev/input/event0"), 1000u);
    QDBusReply<bool> reply2 =
        m_dbusInterface->call(QStringLiteral("ChangeDeviceOwner"), QStringLiteral("/dev/input/event1"), 1001u);

    QVERIFY(reply1.isValid() && reply2.isValid());

    m_ops->setChownResult(-1); // Make next chown fail

    QDBusReply<int> resetReply = m_dbusInterface->call(QStringLiteral("ResetAllDevices"));

    QVERIFY(resetReply.isValid());
    QCOMPARE(resetReply.value(), 0); // All fail due to chown error
}

void TestCouchPlayHelper::testChangeDeviceOwnerBatchEmpty()
{
    m_ops->clear();

    QDBusReply<int> reply = m_dbusInterface->call(QStringLiteral("ChangeDeviceOwnerBatch"), QStringList(), 1000u);

    QVERIFY(reply.isValid());
    QCOMPARE(reply.value(), 0);
}

void TestCouchPlayHelper::testChangeDeviceOwnerBatchAllSuccess()
{
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("testuser"), true, 1000, 1000);
    m_ops->setFileExists(QStringLiteral("/dev/input/event0"), true);
    m_ops->setFileExists(QStringLiteral("/dev/input/event1"), true);
    m_ops->setFileExists(QStringLiteral("/dev/input/event2"), true);
    m_ops->setChownResult(0);
    m_ops->setChmodResult(0);

    QStringList paths = {QStringLiteral("/dev/input/event0"),
                         QStringLiteral("/dev/input/event1"),
                         QStringLiteral("/dev/input/event2")};

    QDBusReply<int> reply = m_dbusInterface->call(QStringLiteral("ChangeDeviceOwnerBatch"), paths, 1000u);

    QVERIFY(reply.isValid());
    QCOMPARE(reply.value(), 3);
}

void TestCouchPlayHelper::testChangeDeviceOwnerBatchPartialFailure()
{
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("testuser"), true, 1000, 1000);
    m_ops->setFileExists(QStringLiteral("/dev/input/event0"), true);
    m_ops->setChownResult(0);
    m_ops->setChmodResult(0);

    // Clear any existing modified devices before this test
    QDBusReply<int> clearReply = m_dbusInterface->call(QStringLiteral("ResetAllDevices"));

    QStringList paths = {QStringLiteral("/dev/input/event0"), QStringLiteral("/dev/sda")};

    QDBusReply<int> reply = m_dbusInterface->call(QStringLiteral("ChangeDeviceOwnerBatch"), paths, 1000u);

    QVERIFY(!reply.isValid());
}

void TestCouchPlayHelper::testChangeDeviceOwnerBatchAllFailure()
{
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("testuser"), true, 1000, 1000);

    QStringList paths = {QStringLiteral("/dev/sda"), QStringLiteral("/dev/sdb")};

    QDBusReply<int> reply = m_dbusInterface->call(QStringLiteral("ChangeDeviceOwnerBatch"), paths, 1000u);

    QVERIFY(!reply.isValid());
}

void TestCouchPlayHelper::testSetupRuntimeAccessSuccess()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setUserExists(QStringLiteral("compositor"), true, 1000, 1000, QStringLiteral("/home/compositor"));
    m_ops->setFileExists(QStringLiteral("/run/user/1000"), true);
    m_ops->setFileExists(QStringLiteral("/run/user/1000/wayland-0"), true);
    m_ops->setFileExists(QStringLiteral("/run/user/1000/bus"), true);
    m_ops->setProcessExitCode(0);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("SetupRuntimeAccess"));

    QVERIFY(reply.isValid());
    QVERIFY(reply.value());
}

void TestCouchPlayHelper::testSetupRuntimeAccessAuthorizationDenied()
{
    m_ops->clear();
    m_ops->setAuthResult(false);
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setUserExists(QStringLiteral("compositor"), true, 1000, 1000, QStringLiteral("/home/compositor"));
    m_ops->setDirectoryExists(QStringLiteral("/run/user/1000"), true);
    m_ops->setChownResult(0);
    m_ops->setChmodResult(0);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("SetupRuntimeAccess"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::AccessDenied);
}

void TestCouchPlayHelper::testSetupRuntimeAccessUserNotFound()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("SetupRuntimeAccess"));



    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);

}

void TestCouchPlayHelper::testRemoveRuntimeAccessSuccess()
{

    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setUserExists(QStringLiteral("compositor"), true, 1000, 1000, QStringLiteral("/home/compositor"));
    m_ops->setFileExists(QStringLiteral("/run/user/1000"), true);
    m_ops->setFileExists(QStringLiteral("/run/user/1000/wayland-0"), true);
    m_ops->setFileExists(QStringLiteral("/run/user/1000/bus"), true);
    m_ops->setProcessExitCode(0);

    QDBusReply<bool> setupReply = m_dbusInterface->call(QStringLiteral("SetupRuntimeAccess"));
    QVERIFY(setupReply.isValid());

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("RemoveRuntimeAccess"));

    QVERIFY(reply.isValid());
    QVERIFY(reply.value());
}

void TestCouchPlayHelper::testRemoveRuntimeAccessAuthorizationDenied()
{
    m_ops->clear();
    m_ops->setAuthResult(false);
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setUserExists(QStringLiteral("compositor"), true, 1000, 1000, QStringLiteral("/home/compositor"));
    m_ops->setDirectoryExists(QStringLiteral("/run/user/1000"), true);
    m_ops->setChownResult(0);
    m_ops->setChmodResult(0);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("RemoveRuntimeAccess"));

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::AccessDenied);
}

void TestCouchPlayHelper::testRemoveRuntimeAccessUserNotFound()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setProcessExitCode(0);
    // Compositor user does not exist, but RemoveRuntimeAccess doesn't check
    // It just runs setfacl commands and returns success if all paths don't exist

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("RemoveRuntimeAccess"));



    // Method returns success even if user doesn't exist
    // (it just runs setfacl on non-existent paths which is a no-op)
    QVERIFY(reply.isValid());
    QVERIFY(reply.value());
}

void TestCouchPlayHelper::testGenerateServiceName()
{
    QCOMPARE(m_helper->generateServiceName(QStringLiteral("player1")), QStringLiteral("couchplay-player1.service"));
    QCOMPARE(m_helper->generateServiceName(QStringLiteral("test_user")), QStringLiteral("couchplay-test_user.service"));
    QCOMPARE(m_helper->generateServiceName(QStringLiteral("a")), QStringLiteral("couchplay-a.service"));
}

void TestCouchPlayHelper::testLaunchInstance_basicLaunch()
{
    configureLaunchFixture();
    QSignalSpy readySpy(m_helper, &CouchPlayHelper::instanceReady);
    const QVariantList preparations{DirectoryPreparation{QStringLiteral("/home/compositor/.config/example app"),
                                                         QStringLiteral("bind"),
                                                         QStringLiteral(".config/example app"),
                                                         {}}
                                       .toVariant()};

    const QDBusReply<qint64> reply = launchPlayer1(preparations);
    QVERIFY2(reply.isValid(), qPrintable(reply.error().message()));
    QCOMPARE(reply.value(), qint64(12345));
    QCOMPARE(readySpy.count(), 0);
    QCOMPARE(m_ops->m_transientServiceName, QStringLiteral("couchplay-player1.service"));
    QCOMPARE(m_ops->m_launchManifests.size(), 1);
    const LaunchManifest manifest = m_ops->m_launchManifests.cbegin().value();
    QCOMPARE(manifest.targetUsername, QStringLiteral("player1"));
    QCOMPARE(manifest.unitRole, QStringLiteral("main"));
    QCOMPARE(manifest.directories.size(), 1);
    QCOMPARE(manifest.directories.constFirst().mode, QStringLiteral("bind"));
    QVERIFY(manifest.gamescopeArgv.contains(QStringLiteral("steam")));
    QCOMPARE(manifest.environment.value(QStringLiteral("ENABLE_GAMESCOPE_WSI")), QStringLiteral("1"));

    const auto propertyValue = [this](const QString &name) {
        for (const SystemdProperty &property : m_ops->m_transientProperties) {
            if (property.name == name) {
                return property.value.variant();
            }
        }
        return QVariant();
    };
    QCOMPARE(propertyValue(QStringLiteral("Type")).toString(), QStringLiteral("notify"));
    QCOMPARE(propertyValue(QStringLiteral("WorkingDirectory")).toString(), QStringLiteral("/"));
    QCOMPARE(propertyValue(QStringLiteral("PrivateMounts")).toBool(), true);
    const auto runtimeBindings = propertyValue(QStringLiteral("BindReadOnlyPaths")).value<QList<SystemdBindPath>>();
    QCOMPARE(runtimeBindings.size(), 1);
    QCOMPARE(runtimeBindings.constFirst().source, QStringLiteral("/run/user/1000"));
    QCOMPARE(runtimeBindings.constFirst().target, QStringLiteral("/run/user/1000"));
    QVERIFY(!runtimeBindings.constFirst().ignoreMissing);
    QCOMPARE(runtimeBindings.constFirst().flags, quint64(0));
    const auto commands = propertyValue(QStringLiteral("ExecStartEx")).value<QList<SystemdExecCommand>>();
    QCOMPARE(commands.size(), 1);
    const SystemdExecCommand &exec = commands.constFirst();
    QCOMPARE(exec.path, QCoreApplication::applicationFilePath());
    QVERIFY(exec.argv.contains(QStringLiteral("--run-instance")));
    QVERIFY(exec.argv.contains(m_ops->m_launchManifests.firstKey()));
    QCOMPARE(exec.flags, QStringList{QStringLiteral("no-setuid")});

    m_systemdUnit->setActiveState(QStringLiteral("active"));
    QTRY_COMPARE(readySpy.count(), 1);
    QCOMPARE(readySpy.first().at(0).toString(), QStringLiteral("player1"));
    QCOMPARE(readySpy.first().at(1).toLongLong(), qint64(12345));

    const QDBusReply<bool> stopReply = m_dbusInterface->call(QStringLiteral("StopInstance"), qint64(12345));
    QVERIFY(stopReply.isValid());
    QVERIFY(stopReply.value());
}

void TestCouchPlayHelper::testLaunchInstance_subscribesSystemdManagerBeforeSignals()
{
    configureLaunchFixture();
    QVERIFY(!m_systemdManager->isSubscribed());
    QSignalSpy readySpy(m_helper, &CouchPlayHelper::instanceReady);

    const QDBusReply<qint64> reply = launchPlayer1();
    QVERIFY2(reply.isValid(), qPrintable(reply.error().message()));

    m_systemdUnit->setActiveState(QStringLiteral("active"));
    QTRY_COMPARE(readySpy.count(), 1);

    const QDBusReply<bool> stopReply = m_dbusInterface->call(QStringLiteral("StopInstance"), reply.value());
    QVERIFY2(stopReply.isValid(), qPrintable(stopReply.error().message()));
    QVERIFY(stopReply.value());
}
void TestCouchPlayHelper::testLegacyMountCleanupUnmountsValidatedTarget()
{
    QTemporaryDir homeDirectory;
    QVERIFY(homeDirectory.isValid());
    const QString home = QFileInfo(homeDirectory.path()).canonicalFilePath();
    QVERIFY(!home.isEmpty());
    const QString target = QDir(home).filePath(QStringLiteral(".couchplay/mounts/shared volume"));
    QVERIFY(QDir().mkpath(target));
    const QString source = QStringLiteral("/home/compositor/shared");
    QByteArray escapedTarget = target.toUtf8();
    escapedTarget.replace("\\", "\\134");
    escapedTarget.replace(" ", "\\040");

    m_ops->clear();
    m_ops->setMockProcessStart(true);
    m_ops->setStandardOutput(QByteArray("31 1 0:31 / ") + escapedTarget + " rw - ext4 " + source.toUtf8() + " rw\n");
    m_ops->setProcessExitCode(0);
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, home);
    m_ops->setOnSystemctlStop([this](const QString &unitName) {
        if (unitName == QStringLiteral("couchplay-player1.service")) {
            m_systemdUnit->setActiveState(QStringLiteral("inactive"));
        } else if (unitName == QStringLiteral("couchplay-stream@player1.service")) {
            m_auxSystemdUnit->setActiveState(QStringLiteral("inactive"));
        }
    });

    QJsonObject mount;
    mount.insert(QStringLiteral("source"), source);
    mount.insert(QStringLiteral("target"), target);
    QJsonArray mounts;
    mounts.append(mount);
    QJsonObject stored;
    stored.insert(QStringLiteral("player1"), mounts);
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    QJsonObject activeUnits;
    activeUnits.insert(QStringLiteral("player1"), QStringLiteral("couchplay-player1.service"));
    root.insert(QStringLiteral("activeUnits"), activeUnits);
    QJsonObject auxiliaryUnits;
    auxiliaryUnits.insert(QStringLiteral("player1"), QStringLiteral("couchplay-stream@player1.service"));
    root.insert(QStringLiteral("auxiliaryUnits"), auxiliaryUnits);
    root.insert(QStringLiteral("activeMounts"), stored);
    QFile stateFile(m_helper->m_stateFilePath);
    QVERIFY(stateFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray state = QJsonDocument(root).toJson(QJsonDocument::Compact);
    QCOMPARE(stateFile.write(state), static_cast<qint64>(state.size()));
    stateFile.close();

    m_helper->loadAndReconcileState();
    QVERIFY(m_helper->m_legacyActiveMounts.isEmpty());
    int stoppedUnits = 0;
    int unmountCount = 0;
    for (const MockSystemOps::ProcessInvocation &invocation : m_ops->m_processInvocations) {
        if (invocation.command == QStringLiteral("systemctl")
            && invocation.args.value(0) == QStringLiteral("stop")) {
            ++stoppedUnits;
        }
        if (invocation.command == QStringLiteral("umount")) {
            ++unmountCount;
            QCOMPARE(invocation.args, (QStringList{QStringLiteral("-l"), QStringLiteral("--"),
                                                   QStringLiteral("/proc/self/fd/0")}));
        }
    }
    QCOMPARE(stoppedUnits, 2);
    QCOMPARE(unmountCount, 1);
}

void TestCouchPlayHelper::testTreeRemovalReportsProgress()
{
    // The start-timeout heartbeat only extends while progressCount() advances; removal must count.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("a/b")));
    for (const QString &name : {QStringLiteral("a/one"), QStringLiteral("a/b/two"), QStringLiteral("three")}) {
        QFile file(QDir(root.path()).filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
    }
    const int fd = ::open(QFile::encodeName(root.path()).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QVERIFY(fd >= 0);
    const uint64_t before = SecureFs::progressCount();
    QCOMPARE(SecureFs::removeTreeAt(fd), 0);
    ::close(fd);
    QVERIFY(SecureFs::progressCount() - before >= 5); // a, a/one, a/b, a/b/two, three
}

void TestCouchPlayHelper::testLegacyMountCleanupPreservesFailedUnmount()
{
    QTemporaryDir homeDirectory;
    QVERIFY(homeDirectory.isValid());
    const QString home = QFileInfo(homeDirectory.path()).canonicalFilePath();
    const QString target = QDir(home).filePath(QStringLiteral(".couchplay/mounts/shared"));
    QVERIFY(QDir().mkpath(target));
    const QString source = QStringLiteral("/home/compositor/shared");

    m_ops->clear();
    m_ops->setMockProcessStart(true);
    m_ops->setStandardOutput(QByteArray("32 1 0:32 / ") + target.toUtf8() + " rw - ext4 " + source.toUtf8() + " rw\n");
    m_ops->setProcessExitCode(0);
    m_ops->setProcessExitCodeForCommand(QStringLiteral("umount"), 1);
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, home);
    m_helper->m_usernameToUnitName.clear();
    m_helper->m_auxiliaryUnitNameForUsername.clear();
    m_helper->m_unitUsernameByName.clear();

    QJsonObject mount;
    mount.insert(QStringLiteral("source"), source);
    mount.insert(QStringLiteral("target"), target);
    QJsonArray mounts;
    mounts.append(mount);
    QJsonObject stored;
    stored.insert(QStringLiteral("player1"), mounts);
    m_helper->m_legacyActiveMounts = stored;

    const QSet<QString> noUnits;
    QVERIFY(!m_helper->reconcileLegacyMounts(noUnits, noUnits, true));
    QCOMPARE(m_helper->m_legacyActiveMounts, stored);
    QVERIFY(m_helper->saveState());
    QFile stateFile(m_helper->m_stateFilePath);
    QVERIFY(stateFile.open(QIODevice::ReadOnly));
    QJsonParseError parseError;
    const QJsonDocument state = QJsonDocument::fromJson(stateFile.readAll(), &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    QCOMPARE(state.object().value(QStringLiteral("activeMounts")).toObject(), stored);
    m_helper->m_legacyActiveMounts = {};
}

void TestCouchPlayHelper::testLegacyMountCleanupRecognizesBindMountByDirectoryIdentity()
{
    QTemporaryDir homeDirectory;
    QVERIFY(homeDirectory.isValid());
    const QString home = QFileInfo(homeDirectory.path()).canonicalFilePath();
    const QString target = QDir(home).filePath(QStringLiteral(".couchplay/mounts/shared"));
    const QString otherSource = QDir(home).filePath(QStringLiteral("unrelated"));
    QVERIFY(QDir().mkpath(target));
    QVERIFY(QDir().mkpath(otherSource));

    // A bind mount of ext4 reports the backing device, not the bound directory, as its mount source.
    // The mounted target and its source are one inode; this stands in for that identity.
    const auto reconcile = [&](const QString &source, bool unmountSucceeds) {
        m_ops->clear();
        m_ops->setMockProcessStart(true);
        m_ops->setStandardOutput(QByteArray("33 1 8:1 /shared ") + target.toUtf8() + " rw - ext4 /dev/sda1 rw\n");
        m_ops->setProcessExitCode(unmountSucceeds ? 0 : 1);
        m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, home);
        m_helper->m_usernameToUnitName.clear();
        m_helper->m_auxiliaryUnitNameForUsername.clear();
        m_helper->m_unitUsernameByName.clear();
        QJsonObject mount;
        mount.insert(QStringLiteral("source"), source);
        mount.insert(QStringLiteral("target"), target);
        QJsonObject stored;
        stored.insert(QStringLiteral("player1"), QJsonArray{mount});
        m_helper->m_legacyActiveMounts = stored;
        const QSet<QString> noUnits;
        const bool changed = m_helper->reconcileLegacyMounts(noUnits, noUnits, true);
        int unmounts = 0;
        for (const MockSystemOps::ProcessInvocation &invocation : m_ops->m_processInvocations) {
            unmounts += invocation.command == QStringLiteral("umount");
        }
        return std::make_tuple(changed, unmounts, m_helper->m_legacyActiveMounts.isEmpty());
    };

    QCOMPARE(reconcile(target, true), std::make_tuple(true, 1, true));
    QCOMPARE(reconcile(otherSource, true), std::make_tuple(false, 0, false));
    m_helper->m_legacyActiveMounts = {};
}

void TestCouchPlayHelper::testLegacyMountCleanupRecognizesOverlayByPersistedUpperDir()
{
    QTemporaryDir homeDirectory;
    QVERIFY(homeDirectory.isValid());
    const QString home = QFileInfo(homeDirectory.path()).canonicalFilePath();
    const QString target = QDir(home).filePath(QStringLiteral(".couchplay/mounts/overlay"));
    QVERIFY(QDir().mkpath(target));
    const QString source = QDir(home).filePath(QStringLiteral("shared-lower"));
    QVERIFY(QDir().mkpath(source));
    const QString upper = QStringLiteral("/var/lib/couchplay/overlays/player1/abc/upper");
    const QString work = QStringLiteral("/var/lib/couchplay/overlays/player1/abc/work");

    // The legacy FD-based mount recorded lowerdir=/proc/self/fd/N, never the source path.
    const auto reconcile = [&](const QString &mountedUpper) {
        m_ops->clear();
        m_ops->setMockProcessStart(true);
        m_ops->setStandardOutput(QByteArray("34 1 0:40 / ") + target.toUtf8()
                                 + " rw - overlay overlay rw,lowerdir=/proc/self/fd/7,upperdir="
                                 + mountedUpper.toUtf8() + ",workdir=" + work.toUtf8() + "\n");
        m_ops->setProcessExitCode(0);
        m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, home);
        m_helper->m_usernameToUnitName.clear();
        m_helper->m_auxiliaryUnitNameForUsername.clear();
        m_helper->m_unitUsernameByName.clear();
        QJsonObject mount;
        mount.insert(QStringLiteral("source"), source);
        mount.insert(QStringLiteral("target"), target);
        mount.insert(QStringLiteral("mountType"), QStringLiteral("overlay"));
        mount.insert(QStringLiteral("upperDir"), upper);
        mount.insert(QStringLiteral("workDir"), work);
        QJsonObject stored;
        stored.insert(QStringLiteral("player1"), QJsonArray{mount});
        m_helper->m_legacyActiveMounts = stored;
        const QSet<QString> noUnits;
        const bool changed = m_helper->reconcileLegacyMounts(noUnits, noUnits, true);
        int unmounts = 0;
        for (const MockSystemOps::ProcessInvocation &invocation : m_ops->m_processInvocations) {
            unmounts += invocation.command == QStringLiteral("umount");
        }
        return std::make_tuple(changed, unmounts, m_helper->m_legacyActiveMounts.isEmpty());
    };

    QCOMPARE(reconcile(upper), std::make_tuple(true, 1, true));
    // Another overlay at the same target (different upper) is not ours to unmount.
    QCOMPARE(reconcile(QStringLiteral("/var/lib/couchplay/overlays/other/abc/upper")),
             std::make_tuple(false, 0, false));
    m_helper->m_legacyActiveMounts = {};
}

void TestCouchPlayHelper::testLegacyMountCleanupRejectsUntrustedTarget()
{
    QTemporaryDir homeDirectory;
    QVERIFY(homeDirectory.isValid());
    const QString home = QFileInfo(homeDirectory.path()).canonicalFilePath();
    const QString target = QStringLiteral("/tmp/couchplay-untrusted-target");
    const QString source = QStringLiteral("/home/compositor/shared");

    m_ops->clear();
    m_ops->setMockProcessStart(true);
    m_ops->setStandardOutput(QByteArray("33 1 0:33 / ") + target.toUtf8() + " rw - ext4 " + source.toUtf8() + " rw\n");
    m_ops->setProcessExitCode(0);
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, home);
    m_helper->m_usernameToUnitName.clear();
    m_helper->m_auxiliaryUnitNameForUsername.clear();
    m_helper->m_unitUsernameByName.clear();

    QJsonObject mount;
    mount.insert(QStringLiteral("source"), source);
    mount.insert(QStringLiteral("target"), target);
    QJsonArray mounts;
    mounts.append(mount);
    QJsonObject stored;
    stored.insert(QStringLiteral("player1"), mounts);
    m_helper->m_legacyActiveMounts = stored;

    const QSet<QString> noUnits;
    QVERIFY(!m_helper->reconcileLegacyMounts(noUnits, noUnits, true));
    QCOMPARE(m_helper->m_legacyActiveMounts, stored);
    for (const MockSystemOps::ProcessInvocation &invocation : m_ops->m_processInvocations) {
        QVERIFY(invocation.command != QStringLiteral("umount"));
    }
    m_helper->m_legacyActiveMounts = {};
}

void TestCouchPlayHelper::testStopInstancePreservesManifestOnScratchCleanupFailure()
{
    configureLaunchFixture();
    QTemporaryDir playerHome;
    QVERIFY(playerHome.isValid());
    struct stat homeStat {};
    const QByteArray homePath = QFile::encodeName(playerHome.path());
    QVERIFY(::stat(homePath.constData(), &homeStat) == 0);
    const uid_t mismatchedTargetUid = homeStat.st_uid == 1001 ? 1002 : 1001;
    m_ops->setUserExists(QStringLiteral("player1"), true, mismatchedTargetUid, 1001, playerHome.path());
    const QString targetRuntimeDirectory = QStringLiteral("/run/user/%1").arg(mismatchedTargetUid);
    m_ops->setFileExists(targetRuntimeDirectory, true);
    m_ops->setFileExists(targetRuntimeDirectory + QStringLiteral("/bus"), true);
    m_ops->setDirectoryExists(targetRuntimeDirectory, true);
    const QVariantList preparations{DirectoryPreparation{QStringLiteral("/home/compositor/.config/example app"),
                                                         QStringLiteral("copy"),
                                                         QStringLiteral(".config/example app"),
                                                         {}}
                                       .toVariant()};
    const QDBusReply<qint64> launchReply = launchPlayer1(preparations);
    QVERIFY2(launchReply.isValid(), qPrintable(launchReply.error().message()));
    QSignalSpy readySpy(m_helper, &CouchPlayHelper::instanceReady);
    m_systemdUnit->setActiveState(QStringLiteral("active"));
    QTRY_COMPARE(readySpy.count(), 1);
    const QString serviceName = QStringLiteral("couchplay-player1.service");
    const QString token = m_helper->m_launchTokenByUnitName.value(serviceName);
    QVERIFY(!token.isEmpty());
    QVERIFY(m_ops->m_launchManifests.contains(token));

    const QDBusReply<bool> stopReply = m_dbusInterface->call(QStringLiteral("StopInstance"), launchReply.value());
    QVERIFY2(stopReply.isValid(), qPrintable(stopReply.error().message()));
    QVERIFY(stopReply.value());
    QVERIFY(m_helper->m_launchTokenByUnitName.contains(serviceName));
    QVERIFY(m_ops->m_launchManifests.contains(token));
    const QDBusReply<qint64> blockedLaunch = launchPlayer1(preparations);
    QVERIFY(!blockedLaunch.isValid());
    QCOMPARE(blockedLaunch.error().type(), QDBusError::Failed);
    QCOMPARE(m_helper->m_launchTokenByUnitName.value(serviceName), token);
    m_helper->m_launchTokenByUnitName.remove(serviceName);
    m_ops->m_launchManifests.remove(token);
}

void TestCouchPlayHelper::testManifestCleanupPersistsBeforeUnlink_data()
{
    QTest::addColumn<bool>("ready");
    QTest::newRow("preparing") << false;
    QTest::newRow("ready") << true;
}

void TestCouchPlayHelper::testManifestCleanupPersistsBeforeUnlink()
{
    QFETCH(bool, ready);
    configureLaunchFixture();
    const QString unit = QStringLiteral("couchplay-player1.service");
    const QDBusReply<qint64> launch = launchPlayer1();
    QVERIFY2(launch.isValid(), qPrintable(launch.error().message()));
    if (ready) {
        QSignalSpy readySpy(m_helper, &CouchPlayHelper::instanceReady);
        m_systemdUnit->setActiveState(QStringLiteral("active"));
        QTRY_COMPARE(readySpy.count(), 1);
    }
    QCOMPARE(m_ops->m_launchManifests.size(), 1);
    const QString token = m_helper->m_launchTokenByUnitName.value(unit);
    QVERIFY(!token.isEmpty());
    QByteArray crashState;
    const auto resetUnlinkCallback = qScopeGuard([&] { m_ops->m_beforeManifestUnlink = {}; });
    m_ops->m_beforeManifestUnlink = [&](const QString &unlinkedToken) {
        QCOMPARE(unlinkedToken, token);
        QFile file(m_helper->m_stateFilePath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        crashState = file.readAll();
        const QJsonObject persisted = QJsonDocument::fromJson(crashState).object();
        QVERIFY(!persisted.value(QStringLiteral("launchTokens")).toObject().contains(unit));
        QVERIFY(!persisted.value(QStringLiteral("activeUnits")).toObject().contains(QStringLiteral("player1")));
    };
    const QDBusReply<bool> stop = m_dbusInterface->call(QStringLiteral("StopInstance"), launch.value());
    QVERIFY(stop.isValid() && stop.value());
    QVERIFY(!crashState.isEmpty());
    QVERIFY(!m_ops->m_launchManifests.contains(token));
    m_ops->m_beforeManifestUnlink = {};
    // Restart from exactly the state committed before unlink, not a later save.
    QFile state(m_helper->m_stateFilePath);
    QVERIFY(state.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(state.write(crashState), qint64(crashState.size()));
    state.close();
    m_helper->loadAndReconcileState();
    m_systemdUnit->reset();
    const QDBusReply<qint64> replacement = launchPlayer1();
    QVERIFY2(replacement.isValid(), qPrintable(replacement.error().message()));
}

void TestCouchPlayHelper::testFailedSubscriptionDoesNotOverwritePersistedState()
{
    configureLaunchFixture();
    const QDBusReply<qint64> launch = launchPlayer1();
    QVERIFY2(launch.isValid(), qPrintable(launch.error().message()));
    QVERIFY(m_helper->saveState());
    QFile persisted(m_helper->m_stateFilePath);
    QVERIFY(persisted.open(QIODevice::ReadOnly));
    const QByteArray before = persisted.readAll();
    persisted.close();
    QVERIFY(before.contains("player1"));

    // Restart with empty tracking maps while systemd refuses the subscription.
    m_helper->m_usernameToUnitName.clear();
    m_helper->m_unitUsernameByName.clear();
    m_systemdManager->resetSubscription();
    m_systemdManager->m_failSubscribe = true;
    m_helper->loadAndReconcileState();
    QVERIFY(!m_helper->saveState());
    QVERIFY(persisted.open(QIODevice::ReadOnly));
    QCOMPARE(persisted.readAll(), before);
    persisted.close();

    // A launch retries recovery; it must fail while the subscription is unavailable...
    QVERIFY(!launchPlayer1().isValid());
    QVERIFY(!m_helper->saveState());
    // ...and recover the persisted state once systemd answers.
    m_systemdManager->m_failSubscribe = false;
    m_helper->loadAndReconcileState();
    QVERIFY(m_helper->m_usernameToUnitName.contains(QStringLiteral("player1")));
    QVERIFY(m_helper->saveState());
}

void TestCouchPlayHelper::testManifestCleanupRetainsManifestOnStateSaveFailure()
{
    configureLaunchFixture();
    const QDBusReply<qint64> launch = launchPlayer1();
    QVERIFY(launch.isValid());
    const QString unit = QStringLiteral("couchplay-player1.service");
    const QString token = m_helper->m_launchTokenByUnitName.value(unit);
    const QString statePath = m_helper->m_stateFilePath;
    // A regular file cannot be the parent directory of the replacement state.
    m_helper->m_stateFilePath = statePath + QStringLiteral("/state.json");
    const QDBusReply<bool> stop = m_dbusInterface->call(QStringLiteral("StopInstance"), launch.value());
    QVERIFY(stop.isValid() && stop.value());
    QVERIFY(m_ops->m_launchManifests.contains(token));
    QCOMPARE(m_helper->m_launchTokenByUnitName.value(unit), token);
    m_helper->m_stateFilePath = statePath;
    QVERIFY(m_helper->saveState());
    m_helper->loadAndReconcileState();
    QVERIFY(!m_ops->m_launchManifests.contains(token));
    QVERIFY(!m_helper->m_launchTokenByUnitName.contains(unit));
    m_systemdUnit->reset();
    const QDBusReply<qint64> replacement = launchPlayer1();
    QVERIFY2(replacement.isValid(), qPrintable(replacement.error().message()));
}

void TestCouchPlayHelper::testRecoveryCleanupPreservesDurableResources_data()
{
    QTest::addColumn<bool>("nestedCallback");
    QTest::newRow("no-such-unit") << false;
    QTest::newRow("nested-dbus-callback") << true;
}

void TestCouchPlayHelper::testRecoveryCleanupPreservesDurableResources()
{
    QFETCH(bool, nestedCallback);
    configureLaunchFixture();
    const QDBusReply<qint64> launch = launchPlayer1();
    QVERIFY(launch.isValid());
    const QString device = QStringLiteral("/dev/input/event987");
    const QString hid = QStringLiteral("0003:28DE:1304.0001");
    m_ops->setFileExists(device, true);
    m_helper->m_modifiedDevices = {device};
    m_helper->m_modifiedHidDevices = {hid};
    m_helper->m_runtimeAccessSetForUid = {54321};
    QVERIFY(m_helper->saveState());
    m_helper->m_modifiedDevices.clear();
    m_helper->m_modifiedHidDevices.clear();
    m_helper->m_runtimeAccessSetForUid.clear();
    QByteArray crashState;
    const auto reset = qScopeGuard([&] {
        m_ops->m_beforeManifestUnlink = {};
        m_systemdManager->m_unitMissing = false;
        m_systemdManager->m_beforeGetUnitReply = {};
    });
    m_ops->m_beforeManifestUnlink = [&](const QString &) {
        QFile file(m_helper->m_stateFilePath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        crashState = file.readAll();
        const QJsonObject root = QJsonDocument::fromJson(crashState).object();
        QCOMPARE(root.value(QStringLiteral("modifiedDevices")).toArray(), QJsonArray{device});
        QCOMPARE(root.value(QStringLiteral("modifiedHidDevices")).toArray(), QJsonArray{hid});
        QCOMPARE(root.value(QStringLiteral("runtimeAccessUids")).toArray(), QJsonArray{54321});
    };
    m_systemdManager->m_unitMissing = !nestedCallback;
    if (nestedCallback) {
        m_systemdManager->m_beforeGetUnitReply = [&] {
            QTimer::singleShot(0, m_helper, [&] {
                // Dispatch persistence and cleanup inside the recovery D-Bus call.
                m_helper->saveState();
                QFile snapshot(m_helper->m_stateFilePath);
                QVERIFY(snapshot.open(QIODevice::ReadOnly));
                const QJsonObject root = QJsonDocument::fromJson(snapshot.readAll()).object();
                QCOMPARE(root.value(QStringLiteral("modifiedDevices")).toArray(), QJsonArray{device});
                QCOMPARE(root.value(QStringLiteral("modifiedHidDevices")).toArray(), QJsonArray{hid});
                QCOMPARE(root.value(QStringLiteral("runtimeAccessUids")).toArray(), QJsonArray{54321});
                m_helper->finalizeStoppedUnit(QStringLiteral("couchplay-player1.service"),
                                             QStringLiteral("nested terminal recovery"), true);
            });
            QCoreApplication::processEvents();
        };
    }
    m_helper->loadAndReconcileState();
    QVERIFY(!crashState.isEmpty());
    QFile file(m_helper->m_stateFilePath);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(crashState), qint64(crashState.size()));
    file.close();
    m_helper->m_modifiedDevices.clear();
    m_helper->m_modifiedHidDevices.clear();
    m_helper->m_runtimeAccessSetForUid.clear();
    m_helper->loadAndReconcileState();
    QCOMPARE(m_helper->m_modifiedDevices, QStringList{device});
    QCOMPARE(m_helper->m_modifiedHidDevices, QStringList{hid});
    QVERIFY(m_helper->m_runtimeAccessSetForUid.contains(54321));
}

void TestCouchPlayHelper::testManifestRecoveryRefusesUnverifiableManifest_data()
{
    QTest::addColumn<bool>("missing");
    QTest::newRow("missing-without-cleanup-proof") << true;
    QTest::newRow("wrong-unit") << false;
}

void TestCouchPlayHelper::testManifestRecoveryRefusesUnverifiableManifest()
{
    QFETCH(bool, missing);
    configureLaunchFixture();
    const QString unit = QStringLiteral("couchplay-player1.service");
    const QString token = QStringLiteral("00000000-0000-4000-8000-000000000999");
    m_helper->m_launchTokenByUnitName.insert(unit, token);
    if (!missing) {
        LaunchManifest manifest;
        manifest.unitName = QStringLiteral("unrelated.service");
        m_ops->m_launchManifests.insert(token, manifest);
    }
    QVERIFY(m_helper->saveState());
    m_helper->loadAndReconcileState();
    QCOMPARE(m_helper->m_launchTokenByUnitName.value(unit), token);
    const QDBusReply<qint64> blocked = launchPlayer1();
    QVERIFY(!blocked.isValid());
    if (!missing) {
        QVERIFY(m_ops->m_launchManifests.contains(token));
    }
}

void TestCouchPlayHelper::testFailedLaunchRetryDoesNotStopReplacement_data()
{
    QTest::addColumn<bool>("secondRetry");
    QTest::addColumn<bool>("earlyFailure");
    QTest::newRow("first-callback") << false << false;
    QTest::newRow("nested-callback") << true << false;
    QTest::newRow("early-return") << false << true;
}

void TestCouchPlayHelper::testFailedLaunchRetryDoesNotStopReplacement()
{
    QFETCH(bool, secondRetry);
    QFETCH(bool, earlyFailure);
    configureLaunchFixture();
    const QString unit = QStringLiteral("couchplay-player1.service");
    m_ops->m_transientStartSucceeds = false;
    m_ops->setProcessExitCodeForCommand(QStringLiteral("systemctl"), 1);
    int failedStops = 0;
    int replacementStops = 0;
    const auto resetStopCallback = qScopeGuard([this] {
        m_ops->setOnSystemctlStop([this](const QString &) {
            m_systemdUnit->setActiveState(QStringLiteral("inactive"));
        });
    });
    m_ops->setOnSystemctlStop([&](const QString &) { ++failedStops; });
    const QDBusReply<qint64> failed = launchPlayer1();
    QVERIFY(!failed.isValid());
    QVERIFY(m_helper->m_pendingLaunches.contains(unit));
    if (secondRetry) {
        QTRY_VERIFY_WITH_TIMEOUT(failedStops >= 2, 2000);
    }
    m_ops->setProcessExitCodeForCommand(QStringLiteral("systemctl"), 0);
    m_systemdUnit->setActiveState(QStringLiteral("inactive"));
    m_helper->failPendingLaunch(unit, QStringLiteral("old unit is now terminal"));
    QVERIFY(!m_helper->m_pendingLaunches.contains(unit));
    m_systemdUnit->reset();
    m_systemdUnit->setMainPid(0);
    m_ops->m_transientStartSucceeds = true;
    m_ops->setOnSystemctlStop([&](const QString &) {
        ++replacementStops;
        m_systemdUnit->setActiveState(QStringLiteral("inactive"));
    });
    QSignalSpy readySpy(m_helper, &CouchPlayHelper::instanceReady);
    [&] {
        QTimer replacementReady;
        replacementReady.setSingleShot(true);
        connect(&replacementReady, &QTimer::timeout, &replacementReady, [this]() {
            m_systemdUnit->setMainPid(23456);
            m_systemdUnit->setActiveState(QStringLiteral("active"));
        });
        replacementReady.start(1400);
        m_ops->m_transientStartSucceeds = !earlyFailure;
        const QDBusReply<qint64> replacement = launchPlayer1();
        if (earlyFailure) {
            QVERIFY(!replacement.isValid());
            return;
        }
        QVERIFY2(replacement.isValid(), qPrintable(replacement.error().message()));
        QCOMPARE(replacement.value(), qint64(23456));
        QTRY_COMPARE(readySpy.count(), 1);
        QCOMPARE(replacementStops, 0);
    }();
    if (earlyFailure) {
        // Model the next test's fixture after an early return. A suite-lifetime
        // singleShot would overwrite this fixture and acknowledge the wrong PID.
        m_ops->m_transientStartSucceeds = true;
        m_systemdUnit->reset();
        QTest::qWait(1500);
        const QDBusReply<qint64> next = launchPlayer1();
        QVERIFY2(next.isValid(), qPrintable(next.error().message()));
        QCOMPARE(next.value(), qint64(12345));
        QCOMPARE(readySpy.count(), 0);
    }
}

void TestCouchPlayHelper::testInitialInactiveStateDoesNotReleasePreparingLaunch()
{
    configureLaunchFixture();
    m_systemdUnit->setActiveState(QStringLiteral("active"));
    m_systemdUnit->setNextActiveStateRead(QStringLiteral("inactive"));
    QSignalSpy readySpy(m_helper, &CouchPlayHelper::instanceReady);

    const QDBusReply<qint64> reply = launchPlayer1();
    QVERIFY2(reply.isValid(), qPrintable(reply.error().message()));
    QCOMPARE(reply.value(), qint64(12345));
    QCOMPARE(m_helper->m_usernameToUnitName.value(QStringLiteral("player1")), QStringLiteral("couchplay-player1.service"));
    QTRY_COMPARE(readySpy.count(), 1);
    QCOMPARE(readySpy.first().at(1).toLongLong(), qint64(12345));
    const QDBusReply<bool> stopReply = m_dbusInterface->call(QStringLiteral("StopInstance"), qint64(12345));
    QVERIFY(stopReply.isValid());
    QVERIFY(stopReply.value());
}

void TestCouchPlayHelper::testInitialInactiveAfterPreparingPidReportsStopped()
{
    configureLaunchFixture();
    m_systemdUnit->setActiveState(QStringLiteral("inactive"));
    QSignalSpy readySpy(m_helper, &CouchPlayHelper::instanceReady);
    QSignalSpy stoppedSpy(m_helper, &CouchPlayHelper::instanceStopped);

    const QDBusReply<qint64> reply = launchPlayer1();
    QVERIFY2(reply.isValid(), qPrintable(reply.error().message()));
    QCOMPARE(reply.value(), qint64(12345));
    QTRY_COMPARE(stoppedSpy.count(), 1);
    QCOMPARE(stoppedSpy.first().at(0).toString(), QStringLiteral("player1"));
    QCOMPARE(stoppedSpy.first().at(1).toLongLong(), qint64(12345));
    QVERIFY(stoppedSpy.first().at(2).toString().contains(QStringLiteral("preparation failed")));
    QCOMPARE(readySpy.count(), 0);
    QVERIFY(m_ops->m_launchManifests.isEmpty());
}

void TestCouchPlayHelper::testTerminalUnitStopsWithoutObservedNonterminalState()
{
    configureLaunchFixture();
    QSignalSpy readySpy(m_helper, &CouchPlayHelper::instanceReady);
    QSignalSpy stoppedSpy(m_helper, &CouchPlayHelper::instanceStopped);
    const QDBusReply<qint64> reply = launchPlayer1();
    QVERIFY2(reply.isValid(), qPrintable(reply.error().message()));
    QCOMPARE(reply.value(), qint64(12345));
    QCOMPARE(readySpy.count(), 0);

    m_systemdUnit->setActiveState(QStringLiteral("inactive"));
    QTRY_COMPARE(stoppedSpy.count(), 1);
    QCOMPARE(stoppedSpy.first().at(0).toString(), QStringLiteral("player1"));
    QCOMPARE(stoppedSpy.first().at(1).toLongLong(), qint64(12345));
    QVERIFY(stoppedSpy.first().at(2).toString().contains(QStringLiteral("preparation failed")));
    QCOMPARE(readySpy.count(), 0);
}

void TestCouchPlayHelper::testTerminalStateErrorsRetainOwnedUnit()
{
    configureLaunchFixture();
    QSignalSpy readySpy(m_helper, &CouchPlayHelper::instanceReady);
    QSignalSpy stoppedSpy(m_helper, &CouchPlayHelper::instanceStopped);
    const QDBusReply<qint64> reply = launchPlayer1();
    QVERIFY2(reply.isValid(), qPrintable(reply.error().message()));
    QCOMPARE(reply.value(), qint64(12345));
    m_systemdUnit->setActiveState(QStringLiteral("active"));
    QTRY_COMPARE(readySpy.count(), 1);

    const QString unitName = QStringLiteral("couchplay-player1.service");
    m_systemdUnit->setActiveState(QStringLiteral("deactivating"));
    m_helper->onUnitStopped(unitName, QStringLiteral("deactivating"));
    QTRY_VERIFY(!m_helper->m_terminalStateQueries.contains(unitName));
    QCOMPARE(stoppedSpy.count(), 0);

    m_systemdUnit->setActiveStateGetFailure(true);
    m_helper->onUnitStopped(unitName, QStringLiteral("temporary status query failure"));
    QTRY_VERIFY(!m_helper->m_terminalStateQueries.contains(unitName));
    QCOMPARE(stoppedSpy.count(), 0);
    QCOMPARE(m_helper->m_pidToUnitName.value(12345), unitName);

    m_systemdUnit->setActiveStateGetFailure(false);
    m_systemdUnit->setActiveState(QStringLiteral("inactive"));
    QTRY_COMPARE(stoppedSpy.count(), 1);
}

void TestCouchPlayHelper::testStaleTerminalUnitSignalDoesNotStopActiveUnit()
{
    configureLaunchFixture();
    QSignalSpy readySpy(m_helper, &CouchPlayHelper::instanceReady);
    QSignalSpy stoppedSpy(m_helper, &CouchPlayHelper::instanceStopped);
    const QDBusReply<qint64> reply = launchPlayer1();
    QVERIFY2(reply.isValid(), qPrintable(reply.error().message()));
    QCOMPARE(reply.value(), qint64(12345));
    m_systemdUnit->setActiveState(QStringLiteral("active"));
    QTRY_COMPARE(readySpy.count(), 1);

    m_helper->onUnitStopped(QStringLiteral("couchplay-player1.service"), QStringLiteral("stale terminal signal"));
    QTRY_VERIFY(!m_helper->m_terminalStateQueries.contains(QStringLiteral("couchplay-player1.service")));
    QCOMPARE(stoppedSpy.count(), 0);

    m_systemdUnit->setActiveState(QStringLiteral("inactive"));
    QTRY_COMPARE(stoppedSpy.count(), 1);
    QCOMPARE(stoppedSpy.first().at(1).toLongLong(), qint64(12345));
}

void TestCouchPlayHelper::testStaleJobRemovedForPreviousLaunchIsIgnored()
{
    configureLaunchFixture();
    QSignalSpy readySpy(m_helper, &CouchPlayHelper::instanceReady);
    QSignalSpy stoppedSpy(m_helper, &CouchPlayHelper::instanceStopped);
    const QString staleJobPath = QStringLiteral("/org/freedesktop/systemd1/job/9001");
    m_ops->m_beforeTransientStartReply = [this, staleJobPath]() {
        m_systemdManager->emitJobRemoved(staleJobPath,
                                         QStringLiteral("couchplay-player1.service"),
                                         QStringLiteral("failed"));
        QCoreApplication::processEvents();
    };

    const QDBusReply<qint64> reply = launchPlayer1();
    QVERIFY2(reply.isValid(), qPrintable(reply.error().message()));
    QCOMPARE(reply.value(), qint64(12345));
    QVERIFY(m_ops->m_lastTransientJobPath != staleJobPath);
    QVERIFY(m_helper->m_jobMonitors.contains(QStringLiteral("couchplay-player1.service")));
    QCOMPARE(stoppedSpy.count(), 0);

    m_systemdUnit->setActiveState(QStringLiteral("active"));
    QTRY_COMPARE(readySpy.count(), 1);
    QCOMPARE(stoppedSpy.count(), 0);
    const QDBusReply<bool> stopReply = m_dbusInterface->call(QStringLiteral("StopInstance"), qint64(12345));
    QVERIFY(stopReply.isValid());
    QVERIFY(stopReply.value());
}

void TestCouchPlayHelper::testAuxiliaryInstanceCoexistsWithMainInstance()
{
    configureLaunchFixture();
    QSignalSpy readySpy(m_helper, &CouchPlayHelper::instanceReady);

    const QDBusReply<qint64> mainReply = launchPlayer1();
    QVERIFY2(mainReply.isValid(), qPrintable(mainReply.error().message()));
    QCOMPARE(mainReply.value(), qint64(12345));
    QCOMPARE(m_ops->m_transientServiceName, QStringLiteral("couchplay-player1.service"));

    const QDBusReply<qint64> auxiliaryReply = launchAuxiliaryPlayer1();
    QVERIFY2(auxiliaryReply.isValid(), qPrintable(auxiliaryReply.error().message()));
    QCOMPARE(auxiliaryReply.value(), qint64(12346));
    QCOMPARE(m_helper->m_auxiliaryUnitNameForUsername.value(QStringLiteral("player1")),
             QStringLiteral("couchplay-stream@player1.service"));
    QCOMPARE(m_helper->m_usernameToUnitName.value(QStringLiteral("player1")), QStringLiteral("couchplay-player1.service"));
    QCOMPARE(m_ops->m_launchManifests.size(), 2);
    const auto roleForUnit = [this](const QString &name) {
        for (const LaunchManifest &manifest : m_ops->m_launchManifests) {
            if (manifest.unitName == name) {
                return manifest.unitRole;
            }
        }
        return QString();
    };
    QCOMPARE(roleForUnit(QStringLiteral("couchplay-player1.service")), QStringLiteral("main"));
    QCOMPARE(roleForUnit(QStringLiteral("couchplay-stream@player1.service")), QStringLiteral("stream"));

    m_systemdUnit->setActiveState(QStringLiteral("active"));
    m_auxSystemdUnit->setActiveState(QStringLiteral("active"));
    QTRY_COMPARE(readySpy.count(), 2);
    QCOMPARE(readySpy.at(0).at(1).toLongLong(), qint64(12345));
    QCOMPARE(readySpy.at(1).at(1).toLongLong(), qint64(12346));

    const QDBusReply<bool> auxiliaryStop = m_dbusInterface->call(QStringLiteral("StopInstance"), qint64(12346));
    QVERIFY(auxiliaryStop.isValid());
    QVERIFY(auxiliaryStop.value());
    QCOMPARE(m_helper->m_usernameToUnitName.value(QStringLiteral("player1")), QStringLiteral("couchplay-player1.service"));
    QVERIFY(!m_helper->m_auxiliaryUnitNameForUsername.contains(QStringLiteral("player1")));

    const QDBusReply<bool> mainStop = m_dbusInterface->call(QStringLiteral("StopInstance"), qint64(12345));
    QVERIFY(mainStop.isValid());
    QVERIFY(mainStop.value());
    QVERIFY(!m_helper->m_usernameToUnitName.contains(QStringLiteral("player1")));
}

void TestCouchPlayHelper::testUnitRolesRemainIndependentAcrossUsernames()
{
    configureLaunchFixture();
    const QString username = QStringLiteral("player1-stream");
    const QString mainName = m_helper->generateServiceName(username);
    const QString streamName = m_helper->generateStreamServiceName(username);
    const auto unitPath = [](QString name) {
        return QStringLiteral("/org/freedesktop/systemd1/unit/")
            + name.replace(QLatin1Char('-'), QStringLiteral("_2d"))
                  .replace(QLatin1Char('@'), QStringLiteral("_40"))
                  .replace(QLatin1Char('.'), QStringLiteral("_2e"));
    };
    const QString mainPath = unitPath(mainName);
    const QString streamPath = unitPath(streamName);
    MockSystemdUnit secondMain(mainPath, 12347);
    MockSystemdUnit secondStream(streamPath, 12348);
    const bool distinctMainPath = mainPath != m_auxSystemdUnitPath;
    if (distinctMainPath) {
        QVERIFY(QDBusConnection::sessionBus().registerVirtualObject(mainPath, &secondMain));
    }
    QVERIFY(QDBusConnection::sessionBus().registerVirtualObject(streamPath, &secondStream));
    if (distinctMainPath) {
        m_systemdManager->m_extraUnitPaths.insert(mainName, mainPath);
    }
    m_systemdManager->m_extraUnitPaths.insert(streamName, streamPath);
    const auto unregister = qScopeGuard([&] {
        m_ops->setOnSystemctlStop({});
        m_systemdManager->m_extraUnitPaths.clear();
        if (distinctMainPath) {
            QDBusConnection::sessionBus().unregisterObject(mainPath);
        }
        QDBusConnection::sessionBus().unregisterObject(streamPath);
    });
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {QStringLiteral("player1"), username});
    m_ops->setUserExists(username, true, 1002, 1002, QStringLiteral("/home/player1-stream"));
    m_ops->setDirectoryExists(QStringLiteral("/home/player1-stream"), true);
    m_ops->setDirectoryExists(QStringLiteral("/run/user/1002"), true);
    m_ops->setFileExists(QStringLiteral("/run/user/1002"), true);
    m_ops->setFileExists(QStringLiteral("/run/user/1002/bus"), true);
    QMap<QString, MockSystemdUnit *> units{
        {m_helper->generateServiceName(QStringLiteral("player1")), m_systemdUnit},
        {m_helper->generateStreamServiceName(QStringLiteral("player1")), m_auxSystemdUnit},
        {mainName, &secondMain}, {streamName, &secondStream}};
    m_ops->setOnSystemctlStop([&](const QString &name) {
        if (MockSystemdUnit *unit = units.value(name)) {
            unit->setActiveState(QStringLiteral("inactive"));
        }
    });
    const QDBusReply<qint64> firstMain = launchPlayer1();
    QVERIFY(firstMain.isValid());
    const QDBusReply<qint64> firstStream = launchAuxiliaryPlayer1();
    QVERIFY2(firstStream.isValid(), qPrintable(firstStream.error().message()));
    const QDBusReply<qint64> otherMain = launchPlayer1({}, {}, username);
    QVERIFY2(otherMain.isValid(), qPrintable(otherMain.error().message()));
    const QDBusReply<qint64> otherStream = launchAuxiliaryPlayer1(username);
    QVERIFY2(otherStream.isValid(), qPrintable(otherStream.error().message()));
    QCOMPARE(m_helper->m_unitUsernameByName.size(), 4);
    QSignalSpy ready(m_helper, &CouchPlayHelper::instanceReady);
    for (MockSystemdUnit *unit : units) {
        unit->setActiveState(QStringLiteral("active"));
    }
    QTRY_COMPARE(ready.count(), 4);
    m_helper->loadAndReconcileState();
    QCOMPARE(m_helper->m_unitUsernameByName.size(), 4);
    QCOMPARE(m_helper->m_pidToUnitName.size(), 4);
    const QList<qint64> stopOrder{otherMain.value(), firstStream.value(), firstMain.value(), otherStream.value()};
    int remaining = 4;
    for (qint64 pid : stopOrder) {
        const QDBusReply<bool> stopped = m_dbusInterface->call(QStringLiteral("StopInstance"), pid);
        QVERIFY2(stopped.isValid(), qPrintable(stopped.error().message()));
        QVERIFY(stopped.value());
        QCOMPARE(m_helper->m_unitUsernameByName.size(), --remaining);
        QVERIFY(!m_helper->m_pidToUnitName.contains(pid));
        for (auto it = m_helper->m_pidToUnitName.cbegin(); it != m_helper->m_pidToUnitName.cend(); ++it) {
            QVERIFY(units.contains(it.value()));
        }
    }
    QVERIFY(m_ops->m_launchManifests.isEmpty());
}

void TestCouchPlayHelper::testRecoveryStopsLegacyStreamUnit_data()
{
    QTest::addColumn<bool>("ready");
    QTest::addColumn<bool>("stopFails");
    QTest::newRow("preparing") << false << false;
    QTest::newRow("ready") << true << false;
    QTest::newRow("stop-failure-retry") << true << true;
}

void TestCouchPlayHelper::testRecoveryStopsLegacyStreamUnit()
{
    QFETCH(bool, ready);
    QFETCH(bool, stopFails);
    configureLaunchFixture();
    const QString current = m_helper->generateStreamServiceName(QStringLiteral("player1"));
    const QString legacy = QStringLiteral("couchplay-player1-stream.service");
    const QString legacyPath = QStringLiteral("/org/freedesktop/systemd1/unit/couchplay_2dplayer1_2dstream_2eservice");
    MockSystemdUnit legacyUnit(legacyPath, 12346);
    legacyUnit.setActiveState(QStringLiteral("active"));
    QVERIFY(QDBusConnection::sessionBus().registerVirtualObject(legacyPath, &legacyUnit));
    m_systemdManager->m_extraUnitPaths.insert(legacy, legacyPath);
    const auto unregister = qScopeGuard([&] {
        m_ops->setOnSystemctlStop({});
        m_systemdManager->m_extraUnitPaths.clear();
        QDBusConnection::sessionBus().unregisterObject(legacyPath);
    });
    const QDBusReply<qint64> launch = launchAuxiliaryPlayer1();
    QVERIFY(launch.isValid());
    if (ready) {
        QSignalSpy readySpy(m_helper, &CouchPlayHelper::instanceReady);
        m_auxSystemdUnit->setActiveState(QStringLiteral("active"));
        QTRY_COMPARE(readySpy.count(), 1);
    }
    QVERIFY(m_helper->saveState());
    QFile file(m_helper->m_stateFilePath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QByteArray state = file.readAll();
    file.close();
    state.replace(current.toUtf8(), legacy.toUtf8());
    const QString token = m_helper->m_launchTokenByUnitName.value(current);
    LaunchManifest manifest = m_ops->m_launchManifests.value(token);
    manifest.unitName = legacy;
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(state), qint64(state.size()));
    file.close();
    m_ops->m_launchManifests.insert(token, manifest);
    int legacyStops = 0;
    m_ops->setProcessExitCodeForCommand(QStringLiteral("systemctl"), stopFails ? 1 : 0);
    m_ops->setOnSystemctlStop([&](const QString &name) {
        if (name == legacy) {
            ++legacyStops;
            if (!stopFails) {
                legacyUnit.setActiveState(QStringLiteral("inactive"));
            }
        } else if (name == current) {
            m_auxSystemdUnit->setActiveState(QStringLiteral("inactive"));
        }
    });
    m_helper->loadAndReconcileState();
    QVERIFY(legacyStops > 0);
    if (stopFails) {
        QCOMPARE(m_helper->m_auxiliaryUnitNameForUsername.value(QStringLiteral("player1")), legacy);
        QCOMPARE(m_helper->m_launchTokenByUnitName.value(legacy), token);
        QVERIFY(m_ops->m_launchManifests.contains(token));
        stopFails = false;
        m_ops->setProcessExitCodeForCommand(QStringLiteral("systemctl"), 0);
        m_helper->loadAndReconcileState();
    }
    QVERIFY(!m_helper->m_auxiliaryUnitNameForUsername.contains(QStringLiteral("player1")));
    QVERIFY(!m_helper->m_launchTokenByUnitName.contains(legacy));
    QVERIFY(!m_ops->m_launchManifests.contains(token));
    m_auxSystemdUnit->reset();
    const QDBusReply<qint64> replacement = launchAuxiliaryPlayer1();
    QVERIFY2(replacement.isValid(), qPrintable(replacement.error().message()));
    QCOMPARE(m_helper->m_auxiliaryUnitNameForUsername.value(QStringLiteral("player1")), current);
    const QDBusReply<bool> stopped = m_dbusInterface->call(QStringLiteral("StopInstance"), replacement.value());
    QVERIFY(stopped.isValid() && stopped.value());
}

void TestCouchPlayHelper::testHostTmpSourceIsDeferredToBootstrap()
{
    configureLaunchFixture();
    QSignalSpy stoppedSpy(m_helper, &CouchPlayHelper::instanceStopped);
    const QString hostSource = QStringLiteral("/tmp/couchplay-host-source");
    const QVariantList preparations{DirectoryPreparation{hostSource,
                                                         QStringLiteral("copy"),
                                                         QStringLiteral(".couchplay/copies/host-tmp"),
                                                         {}}
                                       .toVariant()};

    const QDBusReply<qint64> reply = launchPlayer1(preparations);
    QVERIFY2(reply.isValid(), qPrintable(reply.error().message()));
    QCOMPARE(reply.value(), qint64(12345));
    QCOMPARE(m_ops->m_launchManifests.size(), 1);
    QCOMPARE(m_ops->m_launchManifests.cbegin().value().directories.constFirst().source, hostSource);
    QVERIFY(!m_ops->m_directoryQueries.contains(hostSource));
    QVERIFY(!m_ops->m_canonicalQueries.contains(hostSource));
    QVERIFY(!m_ops->m_statQueries.contains(hostSource));

    m_systemdUnit->setFailure(QStringLiteral("Directory source is missing: %1").arg(hostSource), ENOENT);
    QTRY_COMPARE(stoppedSpy.count(), 1);
    QVERIFY(stoppedSpy.first().at(2).toString().contains(hostSource));
    QVERIFY(stoppedSpy.first().at(2).toString().contains(QStringLiteral("2")));
}

void TestCouchPlayHelper::testLaunchInstancePreparingPidTimeout()
{
    configureLaunchFixture();
    m_systemdUnit->setMainPid(0);
    QSignalSpy readySpy(m_helper, &CouchPlayHelper::instanceReady);
    QSignalSpy stoppedSpy(m_helper, &CouchPlayHelper::instanceStopped);

    const QDBusReply<qint64> reply = launchPlayer1();
    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::Failed);
    QCOMPARE(readySpy.count(), 0);
    QCOMPARE(stoppedSpy.count(), 0);
    QVERIFY(m_ops->m_launchManifests.isEmpty());
    QVERIFY(m_helper->m_unitUsernameByName.isEmpty());
    QVERIFY(m_helper->m_pendingLaunches.isEmpty());
}

void TestCouchPlayHelper::testLaunchInstance_validationEmptyUsername()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});

    QDBusReply<qint64> reply = m_dbusInterface->callWithArgumentList(
        QDBus::Block, QStringLiteral("LaunchInstance"), QVariantList{
                                                     QString(),
                                                     QString(),
                                                     QStringList{},
                                                     QStringList{QStringLiteral("steam")},
                                                     QString(),
                                                     QStringList(),
                                                     QStringList{},
                                                     false, QVariantList{}});

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testLaunchInstance_validationNonexistentUser()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});

    QDBusReply<qint64> reply = m_dbusInterface->callWithArgumentList(
        QDBus::Block, QStringLiteral("LaunchInstance"), QVariantList{
                                                     QStringLiteral("nonexistent"),
                                                     QString(),
                                                     QStringList{},
                                                     QStringList{QStringLiteral("steam")},
                                                     QString(),
                                                     QStringList(),
                                                     QStringList{},
                                                     false, QVariantList{}});

    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
}

void TestCouchPlayHelper::testLaunchInstance_rejectsRootTarget()
{
    configureLaunchFixture();
    m_ops->setUserExists(QStringLiteral("player1"), true, 0, 0, QStringLiteral("/root"));

    const QDBusReply<qint64> reply = launchPlayer1();
    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::AccessDenied);
    QVERIFY(m_ops->m_launchManifests.isEmpty());
    QVERIFY(m_ops->m_transientServiceName.isEmpty());
}

void TestCouchPlayHelper::testStopInstance_serviceStop()
{
    configureLaunchFixture();
    const QDBusReply<qint64> launchReply = launchPlayer1();
    QVERIFY2(launchReply.isValid(), qPrintable(launchReply.error().message()));
    const qint64 pid = launchReply.value();
    QVERIFY(pid > 0);

    m_ops->m_processInvocations.clear();
    const QDBusReply<bool> stopReply = m_dbusInterface->call(QStringLiteral("StopInstance"), pid);
    QVERIFY(stopReply.isValid());
    QVERIFY(stopReply.value());

    bool foundStop = false;
    bool foundResetFailed = false;
    for (const auto &invocation : m_ops->m_processInvocations) {
        if (invocation.command == QStringLiteral("systemctl")
            && invocation.args.contains(QStringLiteral("stop"))
            && invocation.args.contains(QStringLiteral("couchplay-player1.service"))) {
            foundStop = true;
        }
        if (invocation.command == QStringLiteral("systemctl")
            && invocation.args.contains(QStringLiteral("reset-failed"))) {
            foundResetFailed = true;
        }
    }
    QVERIFY(foundStop);
    QVERIFY(!foundResetFailed);
}

void TestCouchPlayHelper::testLaunchInstance_rejectsProtectedEnvironment_data()
{
    QTest::addColumn<QString>("assignment");
    QTest::newRow("home") << QStringLiteral("HOME=/root");
    QTest::newRow("runtime") << QStringLiteral("XDG_RUNTIME_DIR=/run/user/0");
    QTest::newRow("audio-server") << QStringLiteral("PULSE_SERVER=unix:/tmp/other-user");
    QTest::newRow("notify") << QStringLiteral("NOTIFY_SOCKET=/run/systemd/notify");
}

void TestCouchPlayHelper::testLaunchInstance_rejectsProtectedEnvironment()
{
    QFETCH(QString, assignment);
    configureLaunchFixture();
    const QDBusReply<qint64> reply = launchPlayer1({}, {assignment});
    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);
    QVERIFY(m_ops->m_launchManifests.isEmpty());
    QVERIFY(m_ops->m_transientServiceName.isEmpty());
}

void TestCouchPlayHelper::testKillInstance_serviceKill()
{
    configureLaunchFixture();
    const QDBusReply<qint64> launchReply = launchPlayer1();
    QVERIFY2(launchReply.isValid(), qPrintable(launchReply.error().message()));
    const qint64 pid = launchReply.value();
    QVERIFY(pid > 0);

    m_ops->m_processInvocations.clear();
    const QDBusReply<bool> killReply = m_dbusInterface->call(QStringLiteral("KillInstance"), pid);
    QVERIFY(killReply.isValid());
    QVERIFY(killReply.value());
    bool sentSigkill = false;
    for (const auto &invocation : m_ops->m_processInvocations) {
        sentSigkill = sentSigkill
            || (invocation.command == QStringLiteral("systemctl")
                && invocation.args.contains(QStringLiteral("kill"))
                && invocation.args.contains(QStringLiteral("--signal=SIGKILL"))
                && invocation.args.contains(QStringLiteral("couchplay-player1.service")));
    }
    QVERIFY(sentSigkill);

    const QDBusReply<qint64> nextLaunch = launchPlayer1();
    QVERIFY2(nextLaunch.isValid(), qPrintable(nextLaunch.error().message()));
    QCOMPARE(nextLaunch.value(), qint64(12345));
}


void TestCouchPlayHelper::testStopInstance_fallbackToDirectKill()
{
    m_ops->clear();
    m_helper->m_pidToUsername.clear();
    m_helper->m_pidOwnerUid.clear();
    m_helper->m_usernameToUnitName.clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001, {});
    m_ops->setGroupExists(QStringLiteral("input"), true, 44, {});
    m_ops->setProcessExitCode(0);
    m_ops->setMockProcessStart(true);

    QDBusReply<bool> reply = m_dbusInterface->call(QStringLiteral("StopInstance"), qint64(99999));
    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::InvalidArgs);

}

void TestCouchPlayHelper::testLaunchInstance_startTransientFailure()
{
    configureLaunchFixture();
    m_ops->m_transientStartSucceeds = false;

    const QDBusReply<qint64> reply = launchPlayer1();
    QVERIFY(!reply.isValid());
    QCOMPARE(reply.error().type(), QDBusError::Failed);
    QVERIFY(m_ops->m_launchManifests.isEmpty());
    QVERIFY(!m_helper->m_usernameToUnitName.contains(QStringLiteral("player1")));
    QCOMPARE(m_ops->m_transientServiceName, QStringLiteral("couchplay-player1.service"));
    QVERIFY(!m_ops->m_transientProperties.isEmpty());
}

void TestCouchPlayHelper::testLaunchInstance_startTransientFailureWhenUnitWasNeverLoaded()
{
    configureLaunchFixture();
    m_ops->m_transientStartSucceeds = false;
    // systemctl stop exits non-zero for a unit systemd never loaded; systemd itself reports it inactive.
    m_ops->setProcessExitCodeForCommand(QStringLiteral("systemctl"), 5);

    const QDBusReply<qint64> failed = launchPlayer1();
    QVERIFY(!failed.isValid());
    QTRY_VERIFY(m_helper->m_pendingLaunches.isEmpty());
    QVERIFY(!m_helper->m_usernameToUnitName.contains(QStringLiteral("player1")));

    m_ops->m_transientStartSucceeds = true;
    m_ops->setProcessExitCodeForCommand(QStringLiteral("systemctl"), 0);
    m_systemdUnit->reset();
    const QDBusReply<qint64> retry = launchPlayer1();
    QVERIFY2(retry.isValid(), qPrintable(retry.error().message()));
}

void TestCouchPlayHelper::testLaunchInstance_preparationFailureReason()
{
    configureLaunchFixture();
    QSignalSpy readySpy(m_helper, &CouchPlayHelper::instanceReady);
    QSignalSpy stoppedSpy(m_helper, &CouchPlayHelper::instanceStopped);
    const QDBusReply<qint64> reply = launchPlayer1();
    QVERIFY2(reply.isValid(), qPrintable(reply.error().message()));
    QCOMPARE(reply.value(), qint64(12345));
    QCOMPARE(readySpy.count(), 0);

    m_systemdUnit->setFailure(QStringLiteral("source is not readable"), EACCES);
    QTRY_COMPARE(stoppedSpy.count(), 1);
    QCOMPARE(stoppedSpy.first().at(0).toString(), QStringLiteral("player1"));
    QCOMPARE(stoppedSpy.first().at(1).toLongLong(), qint64(12345));
    QVERIFY(stoppedSpy.first().at(2).toString().contains(QStringLiteral("source is not readable")));
    QVERIFY(stoppedSpy.first().at(2).toString().contains(QStringLiteral("13")));
    QCOMPARE(readySpy.count(), 0);
}

void TestCouchPlayHelper::testLaunchInstance_collectedUnitPreservesFailureReason()
{
    configureLaunchFixture();
    QSignalSpy stoppedSpy(m_helper, &CouchPlayHelper::instanceStopped);
    const QDBusReply<qint64> reply = launchPlayer1();
    QVERIFY2(reply.isValid(), qPrintable(reply.error().message()));
    QCOMPARE(reply.value(), qint64(12345));

    const QString status = QStringLiteral("Overlay ownership mapping is not supported for /tmp/lower; use Copy files");
    m_systemdUnit->setServiceGetAllFailure(true);
    m_systemdUnit->setFailure(status, EOPNOTSUPP);
    QTRY_COMPARE(stoppedSpy.count(), 1);
    const QString reason = stoppedSpy.first().at(2).toString();
    QVERIFY(reason.contains(status));
    QVERIFY(reason.contains(QString::number(EOPNOTSUPP)));
}

void TestCouchPlayHelper::testListCouchPlayUsers()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001,
                          {QStringLiteral("player1"), QStringLiteral("player2")});
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, QStringLiteral("/home/player1"));
    m_ops->setUserExists(QStringLiteral("player2"), true, 1002, 1001, QStringLiteral("/home/player2"));
    m_ops->setFileExists(QStringLiteral("/home/player1"), true);
    m_ops->setFileExists(QStringLiteral("/home/player2"), true);

    QDBusReply<QStringList> reply = m_dbusInterface->call(QStringLiteral("ListCouchPlayUsers"));

    QVERIFY(reply.isValid());
    const QStringList users = reply.value();
    QCOMPARE(users.size(), 2);
    QVERIFY(users.contains(QStringLiteral("player1\t1001\t1001\t/home/player1\t/bin/bash")));
    QVERIFY(users.contains(QStringLiteral("player2\t1002\t1001\t/home/player2\t/bin/bash")));
}

void TestCouchPlayHelper::testListCouchPlayUsersFilters()
{
    m_ops->clear();
    m_ops->setGroupExists(QStringLiteral("couchplay"), true, 1001,
                          {QStringLiteral("lowuid"), QStringLiteral("nohome")});
    m_ops->setUserExists(QStringLiteral("lowuid"), true, 999, 1001, QStringLiteral("/home/lowuid"));
    m_ops->setFileExists(QStringLiteral("/home/lowuid"), true);
    m_ops->setUserExists(QStringLiteral("nohome"), true, 1003, 1001, QStringLiteral("/home/nohome"));

    QDBusReply<QStringList> reply = m_dbusInterface->call(QStringLiteral("ListCouchPlayUsers"));

    QVERIFY(reply.isValid());
    QVERIFY(reply.value().isEmpty());
}

void TestCouchPlayHelper::testGetUserInfo()
{
    m_ops->clear();
    m_ops->setUserExists(QStringLiteral("player1"), true, 1001, 1001, QStringLiteral("/home/player1"));

    QDBusReply<QVariantMap> reply = m_dbusInterface->call(QStringLiteral("GetUserInfo"), QStringLiteral("player1"));

    QVERIFY(reply.isValid());
    const QVariantMap info = reply.value();
    QCOMPARE(info.value(QStringLiteral("uid")).toUInt(), 1001u);
    QCOMPARE(info.value(QStringLiteral("gid")).toUInt(), 1001u);
    QCOMPARE(info.value(QStringLiteral("home")).toString(), QStringLiteral("/home/player1"));
}

void TestCouchPlayHelper::testGetUserInfoNotFound()
{
    m_ops->clear();

    QDBusReply<QVariantMap> reply = m_dbusInterface->call(QStringLiteral("GetUserInfo"), QStringLiteral("ghost"));

    QVERIFY(reply.isValid());
    QVERIFY(reply.value().isEmpty());
}

QTEST_MAIN(TestCouchPlayHelper)
#include "test_couchplayhelper.moc"
