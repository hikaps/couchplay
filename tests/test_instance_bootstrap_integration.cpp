// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include "DirectoryPlan.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPair>
#include <QProcess>
#include <QScopeGuard>
#include <QSet>
#include <QTest>
#include <QVector>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace
{
const QString serviceName = QStringLiteral("io.github.hikaps.CouchPlayHelper");
const QString objectPath = QStringLiteral("/io/github/hikaps/CouchPlayHelper");
const QString interfaceName = QStringLiteral("io.github.hikaps.CouchPlayHelper");
const QString helperUnit = QStringLiteral("couchplay-helper.service");
const QString fixtureRoot = QStringLiteral("/run/couchplay-namespace-test");
const QString fixtureUser = QStringLiteral("couchplay-ns-fixture");
const QString playerOne = QStringLiteral("couchplay-ns-player1");
const QString playerTwo = QStringLiteral("couchplay-ns-player2");
const QString longCopyLeaf(212, QLatin1Char('l'));
const QString polkitRulePath = QStringLiteral("/etc/polkit-1/rules.d/00-couchplay-namespace-test.rules");

struct Account {
    QString username;
    QString declaredHome;
    QString canonicalHome;
    uid_t uid = 0;
    gid_t gid = 0;
};

bool runCommand(const QString &program, const QStringList &arguments, QString *error = nullptr, int timeoutMs = 30000)
{
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForStarted(timeoutMs) || !process.waitForFinished(timeoutMs)) {
        if (error) {
            *error = QStringLiteral("%1 %2 did not finish: %3")
                         .arg(program, arguments.join(QLatin1Char(' ')), process.errorString());
        }
        process.kill();
        process.waitForFinished();
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (error) {
            *error = QStringLiteral("%1 %2 failed (%3): %4")
                         .arg(program, arguments.join(QLatin1Char(' ')))
                         .arg(process.exitCode())
                         .arg(QString::fromLocal8Bit(process.readAllStandardError()).trimmed());
        }
        return false;
    }
    return true;
}

bool writeOwnedFile(const QString &path, const QByteArray &contents, uid_t uid, gid_t gid, mode_t mode)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    if (file.write(contents) != contents.size() || !file.flush()) {
        return false;
    }
    file.close();
    const QByteArray encoded = QFile::encodeName(path);
    return ::chown(encoded.constData(), uid, gid) == 0 && ::chmod(encoded.constData(), mode) == 0;
}

bool createOwnedDirectory(const QString &path, uid_t uid, gid_t gid, mode_t mode)
{
    if (!QDir().mkpath(path)) {
        return false;
    }
    const QByteArray encoded = QFile::encodeName(path);
    return ::chown(encoded.constData(), uid, gid) == 0 && ::chmod(encoded.constData(), mode) == 0;
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

QString mountNamespaceForPid(pid_t pid)
{
    const QByteArray path = QByteArrayLiteral("/proc/") + QByteArray::number(pid) + QByteArrayLiteral("/ns/mnt");
    char target[128] = {};
    const ssize_t length = ::readlink(path.constData(), target, sizeof(target) - 1);
    return length < 0 ? QString() : QString::fromLocal8Bit(target, static_cast<qsizetype>(length));
}

QString mountInfoForPid(pid_t pid)
{
    QFile file(QStringLiteral("/proc/%1/mountinfo").arg(pid));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromLocal8Bit(file.readAll());
}

bool hasMountpoint(const QString &mountInfo, const QString &path)
{
    const QString escaped = QString(path).replace(QStringLiteral(" "), QStringLiteral("\\040"));
    for (const QString &line : mountInfo.split(QLatin1Char('\n'))) {
        const QStringList fields = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (fields.size() > 4 && fields.at(4) == escaped) {
            return true;
        }
    }
    return false;
}

bool hasMountpointPrefix(const QString &mountInfo, const QString &prefix)
{
    for (const QString &line : mountInfo.split(QLatin1Char('\n'))) {
        const QStringList fields = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (fields.size() > 4 && fields.at(4).startsWith(prefix)) {
            return true;
        }
    }
    return false;
}

QJsonObject objectAt(const QJsonArray &array, qsizetype index)
{
    return index >= 0 && index < array.size() ? array.at(index).toObject() : QJsonObject();
}

QJsonObject readResult(const QJsonObject &report, const QString &path)
{
    for (const QJsonValue &value : report.value(QStringLiteral("reads")).toArray()) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("path")).toString() == path) {
            return entry;
        }
    }
    return {};
}

QJsonObject writeResult(const QJsonObject &report, const QString &path)
{
    for (const QJsonValue &value : report.value(QStringLiteral("writes")).toArray()) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("path")).toString() == path) {
            return entry;
        }
    }
    return {};
}

QString expectedSystemdProperty(const QString &unit, const QString &property)
{
    QProcess process;
    process.start(QStringLiteral("systemctl"), {QStringLiteral("show"), unit,
                                                 QStringLiteral("--property=%1").arg(property),
                                                 QStringLiteral("--value")});
    if (!process.waitForStarted(5000) || !process.waitForFinished(10000) || process.exitCode() != 0) {
        return {};
    }
    return QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
}
}

class InstanceBootstrapIntegrationTest final : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanup();
    void cleanupTestCase();
    void testLaunchManifestCollectionLimits();
    void testCopyIntoSgidParent();
    void testAliasedSourceProjection_data();
    void testAliasedSourceProjection();
    void testCopyLongLeaf();
    void testStagedCopyRollbackOnFullTarget();
    void testCopyStagingAndPrivateNamespace();
    void testBindAndAclKeepTheirDistinctVisibility();
    void testOverlayIsolationPersistenceAndBrokerRestart();
    void testPreparationFailureCancellationAndExecFailure();

    void onInstanceReady(const QString &username, qlonglong pid);
    void onInstanceStopped(const QString &username, qlonglong pid, const QString &reason);

private:
    QVariantMap record(const QString &source, const QString &mode,
                       const QString &targetAlias = {}, const QString &stagingPath = {}) const;
    QVariantList plan(const QVariantMap &entry) const;
    QString stagingPath(const Account &target, const QString &source) const;
    QString targetFor(const Account &target, const QString &source) const;
    QString reportPath(const QString &label) const;
    QString releasePath(const QString &label) const;
    QStringList gameCommand(const QString &label, const QList<QPair<QString, QString>> &reads = {},
                            const QList<QPair<QString, QString>> &writes = {},
                            const QStringList &absent = {}, bool hold = true) const;
    QDBusMessage call(const QString &method, const QVariantList &arguments = {});
    bool callAsFixtureCaller(const QString &method, const QVariantList &arguments) const;
    qint64 launch(const Account &target, const QVariantList &directoryPlan, const QStringList &command,
                  const QString &workingDirectory = QString(), bool privateTemporaryDirectories = true,
                  const QStringList &sharedRoots = {}, QString *launchError = nullptr);
    bool stop(qint64 pid);
    bool waitForReport(const QString &path, QJsonObject *report, int timeoutMs = 30000) const;

    bool waitForProcessExit(qint64 pid, int timeoutMs = 15000) const;
    bool waitForSignal(const QSet<qlonglong> &pids, qlonglong pid, int timeoutMs = 30000) const;
    bool installProbeAndRule(QString *error);
    bool createFixtureUsers(QString *error);
    bool createFixtureFiles(QString *error);
    bool mountTmpfs(const QString &path, uid_t uid, gid_t gid, mode_t mode, bool *mounted, QString *error);
    bool assertDroppedCredentials(const QJsonObject &report, const Account &target,
                                  const QString &expectedWorkingDirectory = QString()) const;
    bool assertViewIsPrivate(qint64 pid, const Account &target, const QString &relativeTarget,
                             bool isMount) const;
    QString helperPid() const;
    void releaseOwnedUnits();

    Account m_caller;
    Account m_players[2];
    QDBusConnection m_bus = QDBusConnection(QString());
    QString m_sourceRoot;
    QSet<QString> m_createdUsers;
    QSet<QString> m_createdGroups;
    bool m_probeInstalled = false;
    QString m_externalRoot;
    QString m_overlayStorage = QStringLiteral("/var/lib/couchplay/overlays");
    QString m_probeBuildExecutable;
    QString m_probeExecutable;
    bool m_sourceMounted = false;
    bool m_externalMounted = false;
    bool m_overlayStorageMounted = false;
    bool m_unsupportedLowerMounted = false;
    QString m_unsupportedLower;
    QString m_privateSourceRoot;
    QString m_externalWorkingDirectory;
    QString m_hostTmpSource;
    bool m_ruleInstalled = false;
    QHash<qint64, QString> m_hostMountInfoByPid;
    QHash<qint64, QString> m_ownedPids;
    QSet<qlonglong> m_readyPids;
    QHash<qlonglong, QString> m_stoppedPids;
};

bool InstanceBootstrapIntegrationTest::mountTmpfs(const QString &path, uid_t uid, gid_t gid,
                                                   mode_t mode, bool *mounted, QString *error)
{
    if (!QDir().mkpath(path)) {
        if (error) *error = QStringLiteral("Cannot create tmpfs mountpoint %1").arg(path);
        return false;
    }
    const QByteArray encoded = QFile::encodeName(path);
    const QByteArray options = QByteArrayLiteral("size=128m,mode=") + QByteArray::number(mode, 8);
    if (::mount("tmpfs", encoded.constData(), "tmpfs", MS_NOSUID | MS_NODEV, options.constData()) != 0) {
        if (error) *error = QStringLiteral("Cannot mount fixture tmpfs at %1: %2").arg(path, QString::fromLocal8Bit(strerror(errno)));
        return false;
    }
    *mounted = true;
    if (::chown(encoded.constData(), uid, gid) != 0 || ::chmod(encoded.constData(), mode) != 0) {
        if (error) *error = QStringLiteral("Cannot set fixture tmpfs ownership at %1").arg(path);
        return false;
    }
    return true;
}

bool InstanceBootstrapIntegrationTest::createFixtureUsers(QString *error)
{
    if (!runCommand(QStringLiteral("groupadd"), {QStringLiteral("-f"), QStringLiteral("couchplay")}, error)
        || !runCommand(QStringLiteral("groupadd"), {QStringLiteral("-f"), QStringLiteral("games")}, error)) {
        return false;
    }
    const QStringList groupNames = {QStringLiteral("couchplay-ns-fixture"),
                                    QStringLiteral("couchplay-ns-player1"),
                                    QStringLiteral("couchplay-ns-player2")};
    const QList<uint> gids = {58600, 58601, 58602};
    const QList<uint> uids = {58600, 58601, 58602};
    const QStringList usernames = {fixtureUser, playerOne, playerTwo};
    const QStringList homes = {QStringLiteral("/home/couchplay-ns-fixture"),
                               QStringLiteral("/home/couchplay-ns-player1-alias"),
                               QStringLiteral("/home/couchplay-ns-player2-alias")};
    const QStringList canonicalHomes = {homes.at(0), QStringLiteral("/home/couchplay-ns-player1-real"),
                                        QStringLiteral("/home/couchplay-ns-player2-real")};
    for (qsizetype i = 0; i < usernames.size(); ++i) {
        const QString runtimePath = QStringLiteral("/run/user/%1").arg(uids.at(i));
        if (::getpwnam(usernames.at(i).toLocal8Bit().constData()) || ::getpwuid(uids.at(i))
            || ::getgrnam(groupNames.at(i).toLocal8Bit().constData()) || ::getgrgid(gids.at(i))
            || QFileInfo::exists(homes.at(i)) || QFileInfo::exists(canonicalHomes.at(i))
            || QFileInfo::exists(runtimePath)) {
            if (error) *error = QStringLiteral("Reserved fixture account/group ID is already in use");
            return false;
        }
        if (!runCommand(QStringLiteral("groupadd"), {QStringLiteral("--gid"), QString::number(gids.at(i)),
                                                        groupNames.at(i)}, error)) {
            return false;
        }
        m_createdGroups.insert(groupNames.at(i));
        QStringList args{QStringLiteral("--uid"), QString::number(uids.at(i)),
                         QStringLiteral("--gid"), groupNames.at(i), QStringLiteral("--no-create-home"),
                         QStringLiteral("--home-dir"), homes.at(i), QStringLiteral("--shell"),
                         QStringLiteral("/sbin/nologin")};
        if (i == 0) {
            args << QStringLiteral("--groups") << QStringLiteral("games,couchplay");
        } else {
            args << QStringLiteral("--groups") << QStringLiteral("couchplay");
        }
        args << usernames.at(i);
        if (!runCommand(QStringLiteral("useradd"), args, error)) {
            return false;
        }
        m_createdUsers.insert(usernames.at(i));
        if (!QDir().mkpath(canonicalHomes.at(i))) {
            if (error) *error = QStringLiteral("Cannot create fixture home %1").arg(canonicalHomes.at(i));
            return false;
        }
        const QByteArray canonical = QFile::encodeName(canonicalHomes.at(i));
        if (::chown(canonical.constData(), uids.at(i), gids.at(i)) != 0
            || ::chmod(canonical.constData(), 0700) != 0) {
            if (error) *error = QStringLiteral("Cannot set fixture home ownership for %1").arg(usernames.at(i));
            return false;
        }
        if (i != 0) {
            const QByteArray alias = QFile::encodeName(homes.at(i));
            if (::symlink(canonical.constData(), alias.constData()) != 0) {
                if (error) *error = QStringLiteral("Cannot create declared-home alias for %1").arg(usernames.at(i));
                return false;
            }
        }
        struct passwd *pw = ::getpwnam(usernames.at(i).toLocal8Bit().constData());
        if (!pw) {
            if (error) *error = QStringLiteral("Fixture account %1 is not in passwd after creation").arg(usernames.at(i));
            return false;
        }
        Account account{usernames.at(i), QString::fromLocal8Bit(pw->pw_dir), canonicalHomes.at(i),
                        pw->pw_uid, pw->pw_gid};
        if (i == 0) {
            m_caller = account;
            if (::chmod(QFile::encodeName(m_caller.declaredHome).constData(), 0755) != 0) {
                if (error) *error = QStringLiteral("Cannot make fixture source ancestors traversable");
                return false;
            }
        } else {
            m_players[i - 1] = account;
        }
        const QString runtime = QStringLiteral("/run/user/%1").arg(account.uid);
        if (!QDir().mkpath(runtime)) {
            if (error) *error = QStringLiteral("Cannot create fixture user runtime directory");
            return false;
        }
        const QString runtimeBus = runtime + QStringLiteral("/bus");
        if (::chown(QFile::encodeName(runtime).constData(), account.uid, account.gid) != 0
            || ::chmod(QFile::encodeName(runtime).constData(), 0700) != 0
            || (!QFileInfo::exists(runtimeBus)
                && !writeOwnedFile(runtimeBus, {}, account.uid, account.gid, 0600))) {
            if (error) *error = QStringLiteral("Cannot prepare fixture user runtime directory");
            return false;
        }
    }
    return true;
}

bool InstanceBootstrapIntegrationTest::createFixtureFiles(QString *error)
{
    m_sourceRoot = m_caller.declaredHome + QStringLiteral("/sources");
    if (!mountTmpfs(m_sourceRoot, m_caller.uid, m_caller.gid, 0755, &m_sourceMounted, error)) {
        return false;
    }
    // Linux desktops commonly expose shared home mounts. Detached clones must
    // not retain their peer group when attached in a player namespace.
    const QByteArray sharedSource = QFile::encodeName(m_sourceRoot);
    if (::mount(nullptr, sharedSource.constData(), nullptr, MS_SHARED, nullptr) != 0) {
        if (error) *error = QStringLiteral("Cannot share fixture source mount: %1")
                                .arg(QString::fromLocal8Bit(strerror(errno)));
        return false;
    }
    m_externalRoot = QStringLiteral("/mnt/couchplay-namespace-test");
    if (QFileInfo::exists(m_externalRoot)) {
        if (error) *error = QStringLiteral("External fixture mount path is already in use");
        return false;
    }
    if (!mountTmpfs(m_externalRoot, m_caller.uid, m_caller.gid, 0755, &m_externalMounted, error)) {
        return false;
    }
    if (!QDir().mkpath(QStringLiteral("/var/lib/couchplay"))) {
        if (error) *error = QStringLiteral("Cannot create overlay storage parent");
        return false;
    }
    const QString overlayParent = QStringLiteral("/var/lib/couchplay");
    if (!QDir(overlayParent).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty()) {
        if (error) *error = QStringLiteral("Fixture overlay storage parent is not empty");
        return false;
    }
    if (!mountTmpfs(m_overlayStorage, 0, 0, 0700, &m_overlayStorageMounted, error)) {
        return false;
    }

    const auto makeSource = [&](const QString &name, mode_t directoryMode, const QByteArray &fileContents,
                                mode_t fileMode) {
        const QString directory = m_sourceRoot + QLatin1Char('/') + name;
        return createOwnedDirectory(directory, m_caller.uid, m_caller.gid, directoryMode)
            && writeOwnedFile(directory + QStringLiteral("/Launcher.ini"), fileContents,
                              m_caller.uid, m_caller.gid, fileMode);
    };
    if (!makeSource(QStringLiteral("copy"), 0700, QByteArrayLiteral("COPY-ORIGINAL"), 0600)
        || !writeOwnedFile(m_sourceRoot + QStringLiteral("/copy/Games.ini"), QByteArrayLiteral("GAMES-ORIGINAL"),
                           m_caller.uid, m_caller.gid, 0600)
        || !createOwnedDirectory(m_sourceRoot + QStringLiteral("/copy/Banners"), m_caller.uid, m_caller.gid, 0700)
        || !writeOwnedFile(m_sourceRoot + QStringLiteral("/copy/Banners/banner.png"), QByteArrayLiteral("BANNER"),
                           m_caller.uid, m_caller.gid, 0600)
        || !createOwnedDirectory(m_sourceRoot + QStringLiteral("/copy/icons"), m_caller.uid, m_caller.gid, 0700)
        || !writeOwnedFile(m_sourceRoot + QStringLiteral("/copy/icons/icon.png"), QByteArrayLiteral("ICON"),
                           m_caller.uid, m_caller.gid, 0600)
        || !makeSource(QStringLiteral("bind"), 0555, QByteArrayLiteral("BIND-READONLY"), 0444)
        || !makeSource(QStringLiteral("overlay"), 0700, QByteArrayLiteral("OVERLAY-ORIGINAL"), 0600)
        || !makeSource(QStringLiteral("acl"), 0700, QByteArrayLiteral("ACL-ORIGINAL"), 0600)
        || !makeSource(QStringLiteral("inaccessible"), 0700, QByteArrayLiteral("INACCESSIBLE"), 0600)
        || !makeSource(QStringLiteral("race"), 0700, QByteArrayLiteral("RACE-ORIGINAL"), 0600)) {
        if (error) *error = QStringLiteral("Cannot create source fixtures on tmpfs");
        return false;
    }
    if (!createOwnedDirectory(m_externalRoot + QStringLiteral("/one"), m_caller.uid, m_caller.gid, 0700)
        || !writeOwnedFile(m_externalRoot + QStringLiteral("/one/Launcher.ini"), QByteArrayLiteral("EXTERNAL-ONE"),
                           m_caller.uid, m_caller.gid, 0600)
        || !createOwnedDirectory(m_externalRoot + QStringLiteral("/two"), m_caller.uid, m_caller.gid, 0700)
        || !writeOwnedFile(m_externalRoot + QStringLiteral("/two/Launcher.ini"), QByteArrayLiteral("EXTERNAL-TWO"),
                           m_caller.uid, m_caller.gid, 0600)) {
        if (error) *error = QStringLiteral("Cannot create external copy fixtures on tmpfs");
        return false;
    }
    const QString stageOne = stagingPath(m_players[0], m_sourceRoot + QStringLiteral("/copy"));
    const QString stageOverlay = stagingPath(m_players[0], m_sourceRoot + QStringLiteral("/overlay"));
    const QStringList stageParents{
        m_caller.declaredHome + QStringLiteral("/.local"),
        m_caller.declaredHome + QStringLiteral("/.local/share"),
        m_caller.declaredHome + QStringLiteral("/.local/share/couchplay"),
        m_caller.declaredHome + QStringLiteral("/.local/share/couchplay/player-data"),
        m_caller.declaredHome + QStringLiteral("/.local/share/couchplay/player-data/ns-gate"),
        m_caller.declaredHome + QStringLiteral("/.local/share/couchplay/player-data/ns-gate/") + m_players[0].username};
    for (const QString &path : stageParents) {
        if (!createOwnedDirectory(path, m_caller.uid, m_caller.gid, 0700)) {
            if (error) *error = QStringLiteral("Cannot create caller-owned staging parent %1").arg(path);
            return false;
        }
    }
    if (!createOwnedDirectory(stageOne, m_caller.uid, m_caller.gid, 0700)
        || !writeOwnedFile(stageOne + QStringLiteral("/Launcher.ini"), QByteArrayLiteral("COPY-STAGED"),
                           m_caller.uid, m_caller.gid, 0600)
        || !createOwnedDirectory(stageOverlay, m_caller.uid, m_caller.gid, 0700)
        || !writeOwnedFile(stageOverlay + QStringLiteral("/Staged.ini"), QByteArrayLiteral("OVERLAY-STAGED"),
                           m_caller.uid, m_caller.gid, 0600)) {
        if (error) *error = QStringLiteral("Cannot create caller-owned staging fixtures");
        return false;
    }
    const QString reports = fixtureRoot + QStringLiteral("/reports");
    if (!QDir().mkpath(reports) || ::chmod(QFile::encodeName(fixtureRoot).constData(), 0755) != 0
        || ::chmod(QFile::encodeName(reports).constData(), 0777) != 0) {
        if (error) *error = QStringLiteral("Cannot prepare player-writable probe reports");
        return false;
    }
    const QString releases = fixtureRoot + QStringLiteral("/release");
    if (!QDir().mkpath(releases) || ::chmod(QFile::encodeName(releases).constData(), 0755) != 0) {
        if (error) *error = QStringLiteral("Cannot prepare process release markers");
        return false;
    }
    const QString privateTmpMarker = QStringLiteral("/tmp/couchplay-namespace-host-private-marker");
    if (QFileInfo::exists(privateTmpMarker)) {
        if (error) *error = QStringLiteral("Private-tmp marker path is already in use");
        return false;
    }
    if (!writeOwnedFile(privateTmpMarker, QByteArrayLiteral("host-only"), 0, 0, 0644)) {
        if (error) *error = QStringLiteral("Cannot create private-tmp observer marker");
        return false;
    }
    return true;
}

bool InstanceBootstrapIntegrationTest::installProbeAndRule(QString *error)
{
    m_probeBuildExecutable = QCoreApplication::applicationDirPath() + QStringLiteral("/namespace_gamescope_probe");
    m_probeExecutable = QStringLiteral("/usr/local/libexec/couchplay-namespace-probe");
    if (!QFileInfo(m_probeBuildExecutable).isExecutable()) {
        if (error) *error = QStringLiteral("Built native namespace probe is missing: %1").arg(m_probeBuildExecutable);
        return false;
    }
    if (QFileInfo::exists(QStringLiteral("/usr/bin/gamescope"))
        || QFileInfo::exists(QStringLiteral("/usr/local/libexec/couchplay-namespace-probe"))) {
        if (error) *error = QStringLiteral("Disposable container already has a gamescope/probe executable");
        return false;
    }
    if (!runCommand(QStringLiteral("install"), {QStringLiteral("-D"), QStringLiteral("-m"), QStringLiteral("0755"),
                                                  m_probeBuildExecutable, QStringLiteral("/usr/bin/gamescope")}, error)) {
        return false;
    }
    m_probeInstalled = true;
    // Distro Gamescope binaries may carry file capabilities; the game must not inherit them.
    if (!runCommand(QStringLiteral("setcap"), {QStringLiteral("cap_sys_nice=eip"),
                                               QStringLiteral("/usr/bin/gamescope")}, error)) {
        return false;
    }
    if (!runCommand(QStringLiteral("install"), {QStringLiteral("-D"), QStringLiteral("-m"), QStringLiteral("0755"),
                                                  m_probeBuildExecutable, m_probeExecutable}, error)) {
        return false;
    }

    if (QFileInfo::exists(polkitRulePath)) {
        if (error) *error = QStringLiteral("Refusing to overwrite an existing Polkit rule");
        return false;
    }
    const QByteArray rule = QByteArrayLiteral(
        "polkit.addRule(function(action, subject) {\n"
        "  if (subject.user === 'couchplay-ns-fixture' &&\n"
        "      action.id === 'io.github.hikaps.couchplay.manage-session') {\n"
        "    return polkit.Result.YES;\n"
        "  }\n"
        "});\n");
    if (!writeOwnedFile(polkitRulePath, rule, 0, 0, 0644)) {
        if (error) *error = QStringLiteral("Cannot install container-only Polkit fixture rule");
        return false;
    }
    m_ruleInstalled = true;
    QString commandError;
    if (!runCommand(QStringLiteral("systemctl"), {QStringLiteral("restart"), QStringLiteral("polkit.service")},
                    &commandError)) {
        if (error) *error = commandError;
        return false;
    }
    return true;
}

void InstanceBootstrapIntegrationTest::initTestCase()
{
    QVERIFY2(qgetenv("COUCHPLAY_NAMESPACE_CONTAINER") == QByteArrayLiteral("1")
                 && QFileInfo::exists(QStringLiteral("/.dockerenv")),
             "The privileged namespace test may run only inside its disposable Docker container");
    QVERIFY2(::geteuid() == 0, "COUCHPLAY_PRIVILEGED_TESTS requires the disposable test process to run as root");
    QVERIFY2(QFileInfo::exists(QStringLiteral("/run/systemd/system")), "systemd must be PID 1 in the privileged fixture");
    QVERIFY2(QFileInfo::exists(QStringLiteral("/run/dbus/system_bus_socket")), "the system D-Bus must be running");
    QCOMPARE(expectedSystemdProperty(helperUnit, QStringLiteral("ActiveState")), QStringLiteral("active"));
    QCOMPARE(expectedSystemdProperty(helperUnit, QStringLiteral("PrivateTmp")), QStringLiteral("yes"));
    QCOMPARE(expectedSystemdProperty(helperUnit, QStringLiteral("ProtectKernelTunables")), QStringLiteral("yes"));
    QCOMPARE(expectedSystemdProperty(helperUnit, QStringLiteral("ProtectKernelModules")), QStringLiteral("yes"));
    QCOMPARE(expectedSystemdProperty(QStringLiteral("polkit.service"), QStringLiteral("ActiveState")),
             QStringLiteral("active"));
    QVERIFY2(!QFileInfo::exists(fixtureRoot), "The fixed namespace fixture path must be unused");

    QString error;
    QVERIFY2(createFixtureUsers(&error), qPrintable(error));
    QVERIFY2(createFixtureFiles(&error), qPrintable(error));
    QVERIFY2(installProbeAndRule(&error), qPrintable(error));

    const QString previousHelperPid = helperPid();
    QVERIFY2(runCommand(QStringLiteral("systemctl"), {QStringLiteral("restart"), helperUnit}, &error), qPrintable(error));
    QVERIFY2(QFileInfo::exists(QStringLiteral("/run/systemd/system")), "systemd stopped during fixture setup");
    const QString currentHelperPid = helperPid();
    QVERIFY(!currentHelperPid.isEmpty());
    QVERIFY(currentHelperPid != QStringLiteral("0"));
    QVERIFY(currentHelperPid != previousHelperPid);
    QVERIFY(mountNamespaceForPid(currentHelperPid.toLongLong()) != mountNamespaceForPid(::getpid()));

    m_bus = QDBusConnection::connectToBus(QDBusConnection::SystemBus, QStringLiteral("couchplay-namespace-observer"));
    QVERIFY2(m_bus.isConnected(), qPrintable(m_bus.lastError().message()));
    QVERIFY(m_bus.connect(serviceName, objectPath, interfaceName, QStringLiteral("instanceReady"),
                          this, SLOT(onInstanceReady(QString,qlonglong))));
    QVERIFY(m_bus.connect(serviceName, objectPath, interfaceName, QStringLiteral("instanceStopped"),
                          this, SLOT(onInstanceStopped(QString,qlonglong,QString))));

}

void InstanceBootstrapIntegrationTest::cleanup()
{
    releaseOwnedUnits();
}

void InstanceBootstrapIntegrationTest::cleanupTestCase()
{
    releaseOwnedUnits();
    if (m_bus.isConnected()) {
        m_bus.disconnect(serviceName, objectPath, interfaceName, QStringLiteral("instanceReady"),
                         this, SLOT(onInstanceReady(QString,qlonglong)));
        m_bus.disconnect(serviceName, objectPath, interfaceName, QStringLiteral("instanceStopped"),
                         this, SLOT(onInstanceStopped(QString,qlonglong,QString)));
        QDBusConnection::disconnectFromBus(m_bus.name());
    }
    if (m_ruleInstalled) {
        QFile::remove(polkitRulePath);
        QProcess::execute(QStringLiteral("systemctl"), {QStringLiteral("restart"), QStringLiteral("polkit.service")});
        m_ruleInstalled = false;
    }
    if (m_probeInstalled) {
        QFile::remove(QStringLiteral("/usr/bin/gamescope"));
        QFile::remove(QStringLiteral("/usr/local/libexec/couchplay-namespace-probe"));
        m_probeInstalled = false;
    }
    if (m_sourceMounted || m_externalMounted || m_overlayStorageMounted || m_unsupportedLowerMounted) {
        QProcess::execute(QStringLiteral("systemctl"), {QStringLiteral("restart"), helperUnit});
    }
    // A failing isolation regression may have leaked an Overlay onto this
    // fixture-owned source. Uncover it before deleting protected upper data.
    if (m_sourceMounted) {
        const QByteArray leakedSource = QFile::encodeName(m_sourceRoot + QStringLiteral("/overlay"));
        (void)::umount2(leakedSource.constData(), 0);
    }
    if (m_overlayStorageMounted) {
        for (const QString &username : {fixtureUser, playerOne, playerTwo}) {
            QDir(QDir(m_overlayStorage).filePath(username)).removeRecursively();
        }
        ::umount2(QFile::encodeName(m_overlayStorage).constData(), 0);
        m_overlayStorageMounted = false;
    }
    if (m_externalMounted) {
        ::umount2(QFile::encodeName(m_externalRoot).constData(), 0);
        m_externalMounted = false;
    }
    if (m_unsupportedLowerMounted) {
        ::umount2(QFile::encodeName(m_unsupportedLower).constData(), 0);
        m_unsupportedLowerMounted = false;
    }
    if (m_sourceMounted) {
        ::umount2(QFile::encodeName(m_sourceRoot).constData(), 0);
        m_sourceMounted = false;
    }
    QDir(m_overlayStorage).removeRecursively();
    QDir(m_externalRoot).removeRecursively();
    QFile::remove(QStringLiteral("/tmp/couchplay-namespace-host-private-marker"));
    if (!m_privateSourceRoot.isEmpty()) {
        QDir(m_privateSourceRoot).removeRecursively();
    }
    if (!m_externalWorkingDirectory.isEmpty()) {
        QDir(m_externalWorkingDirectory).removeRecursively();
    }
    if (!m_hostTmpSource.isEmpty()) {
        QDir(m_hostTmpSource).removeRecursively();
    }
    QDir(fixtureRoot).removeRecursively();

    const QStringList usernames{fixtureUser, playerOne, playerTwo};
    const QStringList declaredHomes{QStringLiteral("/home/couchplay-ns-fixture"),
                                    QStringLiteral("/home/couchplay-ns-player1-alias"),
                                    QStringLiteral("/home/couchplay-ns-player2-alias")};
    const QStringList canonicalHomes{declaredHomes.at(0), QStringLiteral("/home/couchplay-ns-player1-real"),
                                     QStringLiteral("/home/couchplay-ns-player2-real")};
    const QList<uint> uids{58600, 58601, 58602};
    for (qsizetype i = 0; i < usernames.size(); ++i) {
        if (!m_createdUsers.contains(usernames.at(i))) {
            continue;
        }
        QProcess::execute(QStringLiteral("userdel"), {usernames.at(i)});
        QFile::remove(declaredHomes.at(i));
        QDir(canonicalHomes.at(i)).removeRecursively();
        QDir(QStringLiteral("/run/user/%1").arg(uids.at(i))).removeRecursively();
    }
    for (const QString &group : {QStringLiteral("couchplay-ns-fixture"),
                                 QStringLiteral("couchplay-ns-player1"),
                                 QStringLiteral("couchplay-ns-player2")}) {
        if (m_createdGroups.contains(group) && ::getgrnam(group.toLocal8Bit().constData())) {
            QProcess::execute(QStringLiteral("groupdel"), {group});
        }
    }
}

QVariantMap InstanceBootstrapIntegrationTest::record(const QString &source, const QString &mode,
                                                      const QString &targetAlias, const QString &stagingPath) const
{
    return {{QStringLiteral("source"), source}, {QStringLiteral("mode"), mode},
            {QStringLiteral("targetAlias"), targetAlias}, {QStringLiteral("stagingPath"), stagingPath}};
}

QVariantList InstanceBootstrapIntegrationTest::plan(const QVariantMap &entry) const
{
    return {entry};
}

QString InstanceBootstrapIntegrationTest::stagingPath(const Account &target, const QString &source) const
{
    return m_caller.declaredHome + QStringLiteral("/.local/share/couchplay/player-data/ns-gate/")
        + target.username + QLatin1Char('/') + DirectoryPlan::dataDirectoryStagingSlug(source, m_caller.declaredHome);
}

QString InstanceBootstrapIntegrationTest::targetFor(const Account &target, const QString &source) const
{
    const QString relative = QDir(m_caller.declaredHome).relativeFilePath(source);
    return QDir(target.declaredHome).filePath(relative);
}

QString InstanceBootstrapIntegrationTest::reportPath(const QString &label) const
{
    return fixtureRoot + QStringLiteral("/reports/") + label + QStringLiteral(".json");
}

QString InstanceBootstrapIntegrationTest::releasePath(const QString &label) const
{
    return fixtureRoot + QStringLiteral("/release/") + label;
}

QStringList InstanceBootstrapIntegrationTest::gameCommand(const QString &label,
                                                           const QList<QPair<QString, QString>> &reads,
                                                           const QList<QPair<QString, QString>> &writes,
                                                           const QStringList &absent, bool hold) const
{
    QStringList result{m_probeExecutable, QStringLiteral("--probe-game"), reportPath(label)};
    for (const auto &item : reads) {
        result << QStringLiteral("--read") << item.first << item.second;
    }
    for (const auto &item : writes) {
        result << QStringLiteral("--write") << item.first << item.second;
    }
    for (const QString &path : absent) {
        result << QStringLiteral("--absent") << path;
    }
    if (hold) {
        result << QStringLiteral("--hold") << releasePath(label);
    }
    return result;
}

QDBusMessage InstanceBootstrapIntegrationTest::call(const QString &method, const QVariantList &arguments)
{
    QDBusMessage request = QDBusMessage::createMethodCall(serviceName, objectPath, interfaceName, method);
    request.setArguments(arguments);
    return m_bus.call(request, QDBus::Block, 30000);
}

qint64 InstanceBootstrapIntegrationTest::launch(const Account &target, const QVariantList &directoryPlan,
                                                const QStringList &command, const QString &workingDirectory,
                                                bool privateTemporaryDirectories, const QStringList &sharedRoots,
                                                QString *launchError)
{
    if (launchError) launchError->clear();

    const QString hostMountInfoBefore = mountInfoForPid(::getpid());
    if (hostMountInfoBefore.isEmpty()) {
        return 0;
    }
    const QJsonObject request{{QStringLiteral("method"), QStringLiteral("LaunchInstance")},
                              {QStringLiteral("targetUsername"), target.username},
                              {QStringLiteral("command"), QJsonArray::fromStringList(command)},
                              {QStringLiteral("directoryPreparations"),
                               QJsonArray::fromVariantList(directoryPlan)},
                              {QStringLiteral("workingDirectory"),
                               workingDirectory.isEmpty() ? target.declaredHome : workingDirectory},
                              {QStringLiteral("privateTemporaryDirectories"), privateTemporaryDirectories},
                              {QStringLiteral("sharedRoots"), QJsonArray::fromStringList(sharedRoots)}};
    QProcess client;
    client.start(QCoreApplication::applicationFilePath(),
                 {QStringLiteral("--fixture-dbus-call"), m_caller.username, QString::number(m_caller.uid),
                  QString::number(m_caller.gid),
                  QString::fromLatin1(QJsonDocument(request).toJson(QJsonDocument::Compact).toBase64())});
    if (!client.waitForStarted(5000) || !client.waitForFinished(35000)
        || client.exitStatus() != QProcess::NormalExit || client.exitCode() != 0) {
        const QString failure = QString::fromLocal8Bit(client.readAllStandardError()).trimmed();
        if (launchError) *launchError = failure;
        qWarning().noquote() << "Fixture-caller LaunchInstance failed:" << failure;
        client.kill();
        client.waitForFinished();
        return 0;
    }
    bool validPid = false;

    const qint64 pid = QString::fromLatin1(client.readAllStandardOutput()).trimmed().toLongLong(&validPid);
    if (!validPid) {
        const QString failure = QStringLiteral("Fixture-caller LaunchInstance returned an invalid PID");
        if (launchError) *launchError = failure;
        qWarning().noquote() << failure;
        return 0;
    }

    if (pid > 0) {
        m_ownedPids.insert(pid, target.username);
        m_hostMountInfoByPid.insert(pid, hostMountInfoBefore);
    }
    return pid;
}

bool InstanceBootstrapIntegrationTest::callAsFixtureCaller(const QString &method,
                                                               const QVariantList &arguments) const
{
    const QJsonObject request{{QStringLiteral("method"), method},
                              {QStringLiteral("arguments"), QJsonArray::fromVariantList(arguments)}};
    QProcess client;
    client.start(QCoreApplication::applicationFilePath(),
                 {QStringLiteral("--fixture-dbus-call"), m_caller.username, QString::number(m_caller.uid),
                  QString::number(m_caller.gid),
                  QString::fromLatin1(QJsonDocument(request).toJson(QJsonDocument::Compact).toBase64())});
    if (!client.waitForStarted(5000) || !client.waitForFinished(35000)
        || client.exitStatus() != QProcess::NormalExit || client.exitCode() != 0) {
        qWarning().noquote() << "Fixture-caller D-Bus call failed:"
                             << QString::fromLocal8Bit(client.readAllStandardError()).trimmed();
        client.kill();
        client.waitForFinished();
        return false;
    }
    return QString::fromLatin1(client.readAllStandardOutput()).trimmed() == QStringLiteral("true");
}

bool InstanceBootstrapIntegrationTest::stop(qint64 pid)
{
    if (!callAsFixtureCaller(QStringLiteral("StopInstance"), {pid})) {
        qWarning().noquote() << "StopInstance failed for" << pid;
        return false;
    }
    return true;
}

bool InstanceBootstrapIntegrationTest::waitForProcessExit(qint64 pid, int timeoutMs) const
{
    QElapsedTimer timer;
    timer.start();
    const QString procPath = QStringLiteral("/proc/%1").arg(pid);
    const auto hostMountInfoUnchanged = [&] {
        const auto baseline = m_hostMountInfoByPid.constFind(pid);
        return baseline != m_hostMountInfoByPid.cend() && mountInfoForPid(::getpid()) == baseline.value();
    };
    while (timer.elapsed() < timeoutMs) {
        if (!QFileInfo::exists(procPath)) {
            return hostMountInfoUnchanged();
        }
        QCoreApplication::processEvents();
        QTest::qWait(20);
    }
    return !QFileInfo::exists(procPath) && hostMountInfoUnchanged();
}

bool InstanceBootstrapIntegrationTest::waitForReport(const QString &path, QJsonObject *report, int timeoutMs) const
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            QJsonParseError parseError{};
            const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
            if (parseError.error == QJsonParseError::NoError && document.isObject()) {
                *report = document.object();
                return true;
            }
        }
        QCoreApplication::processEvents();
        QTest::qWait(20);
    }
    return false;
}

bool InstanceBootstrapIntegrationTest::waitForSignal(const QSet<qlonglong> &pids, qlonglong pid, int timeoutMs) const
{
    Q_UNUSED(pids)
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (m_readyPids.contains(pid)) {
            return true;
        }
        QCoreApplication::processEvents();
        QTest::qWait(20);
    }
    return m_readyPids.contains(pid);
}

bool InstanceBootstrapIntegrationTest::assertDroppedCredentials(
    const QJsonObject &report, const Account &target, const QString &expectedWorkingDirectory) const
{
    if (report.value(QStringLiteral("uid")).toVariant().toLongLong() != target.uid
        || report.value(QStringLiteral("euid")).toVariant().toLongLong() != target.uid
        || report.value(QStringLiteral("gid")).toVariant().toLongLong() != target.gid
        || report.value(QStringLiteral("egid")).toVariant().toLongLong() != target.gid
        || report.value(QStringLiteral("home")).toString() != target.declaredHome
        || report.value(QStringLiteral("cwd")).toString()
               != (expectedWorkingDirectory.isEmpty() ? target.canonicalHome : expectedWorkingDirectory)
        || report.value(QStringLiteral("mountNamespace")).toString().isEmpty()
        || report.value(QStringLiteral("notifySocketPresent")).toBool()) {
        return false;
    }
    const qint64 probePid = report.value(QStringLiteral("pid")).toVariant().toLongLong();
    const QString processNamespace = report.value(QStringLiteral("mountNamespace")).toString();
    const QString helperPidValue = helperPid();
    if (probePid <= 0 || !m_ownedPids.contains(probePid) || processNamespace.isEmpty() || helperPidValue.isEmpty()) {
        return false;
    }
    const QString hostNamespace = mountNamespaceForPid(::getpid());
    const QString helperNamespace = mountNamespaceForPid(static_cast<pid_t>(helperPidValue.toLongLong()));
    if (hostNamespace.isEmpty() || helperNamespace.isEmpty() || hostNamespace == helperNamespace
        || hostNamespace == processNamespace || helperNamespace == processNamespace) {
        return false;
    }
    char hostUserNamespace[128] = {};
    const ssize_t hostUserNamespaceLength =
        ::readlink("/proc/self/ns/user", hostUserNamespace, sizeof(hostUserNamespace) - 1);
    if (hostUserNamespaceLength < 0
        || report.value(QStringLiteral("userNamespace")).toString()
            != QString::fromLocal8Bit(hostUserNamespace, hostUserNamespaceLength)) {
        return false;
    }
    for (auto it = m_ownedPids.cbegin(); it != m_ownedPids.cend(); ++it) {
        if (it.key() == probePid || !QFileInfo::exists(QStringLiteral("/proc/%1").arg(it.key()))) {
            continue;
        }
        const QString otherNamespace = mountNamespaceForPid(static_cast<pid_t>(it.key()));
        if (!otherNamespace.isEmpty() && otherNamespace == processNamespace) {
            return false;
        }
    }
    for (const QString &capability : {QStringLiteral("CapEff"), QStringLiteral("CapPrm"),
                                      QStringLiteral("CapInh"), QStringLiteral("CapAmb"),
                                      QStringLiteral("CapBnd")}) {
        if (report.value(capability).toString() != QStringLiteral("0000000000000000")) {
            return false;
        }
    }
    if (report.value(QStringLiteral("NoNewPrivs")).toString() != QStringLiteral("1")
        || !report.value(QStringLiteral("inheritedDescriptors")).toArray().isEmpty()
        || !report.value(QStringLiteral("privateTmpLowerPaths")).toArray().isEmpty()) {
        return false;
    }
    const QByteArray username = target.username.toLocal8Bit();
    int expectedCount = 64;
    QVector<gid_t> expectedVector(expectedCount);
    if (::getgrouplist(username.constData(), target.gid, expectedVector.data(), &expectedCount) < 0) {
        return false;
    }
    expectedVector.resize(expectedCount);
    QSet<gid_t> expectedGroups;
    for (gid_t group : expectedVector) {
        expectedGroups.insert(group);
    }
    QSet<gid_t> actualGroups;
    for (const QJsonValue &value : report.value(QStringLiteral("groups")).toArray()) {
        actualGroups.insert(static_cast<gid_t>(value.toVariant().toULongLong()));
    }
    return expectedGroups == actualGroups;
}

bool InstanceBootstrapIntegrationTest::assertViewIsPrivate(qint64 pid, const Account &target,
                                                            const QString &relativeTarget, bool isMount) const
{
    const QString canonicalTarget = QDir(target.canonicalHome).filePath(relativeTarget);
    const QString declaredTarget = QDir(target.declaredHome).filePath(relativeTarget);
    const QStringList targetPaths = canonicalTarget == declaredTarget
        ? QStringList{canonicalTarget} : QStringList{canonicalTarget, declaredTarget};
    const QString hostMountInfo = QString::fromLocal8Bit(readFile(QStringLiteral("/proc/self/mountinfo")));
    const auto launchMountInfo = m_hostMountInfoByPid.constFind(pid);
    if (launchMountInfo == m_hostMountInfoByPid.cend() || hostMountInfo != launchMountInfo.value()) {
        qWarning() << "Host mount table changed during instance startup" << pid;
        return false;
    }
    const QString helperPidValue = helperPid();
    if (helperPidValue.isEmpty()) {
        return false;
    }
    const qint64 helperPidNumber = helperPidValue.toLongLong();
    const QString hostNamespace = mountNamespaceForPid(::getpid());
    const QString helperNamespace = mountNamespaceForPid(static_cast<pid_t>(helperPidNumber));
    const QString processNamespace = mountNamespaceForPid(static_cast<pid_t>(pid));
    if (hostNamespace.isEmpty() || helperNamespace.isEmpty() || processNamespace.isEmpty()
        || hostNamespace == helperNamespace || hostNamespace == processNamespace
        || helperNamespace == processNamespace) {
        qWarning() << "View namespace identity mismatch" << hostNamespace << helperNamespace << processNamespace;
        return false;
    }
    for (auto it = m_ownedPids.cbegin(); it != m_ownedPids.cend(); ++it) {
        if (it.key() == pid || !QFileInfo::exists(QStringLiteral("/proc/%1").arg(it.key()))) {
            continue;
        }
        if (mountNamespaceForPid(static_cast<pid_t>(it.key())) == processNamespace) {
            return false;
        }
    }
    const QString helperMountInfo = mountInfoForPid(helperPidNumber);
    const QString processMountInfo = mountInfoForPid(pid);
    if (processMountInfo.isEmpty()) {
        return false;
    }
    const QString stagingMountPrefix = QStringLiteral("/tmp/.lower-");
    const bool hostHasStagingMount = hasMountpointPrefix(hostMountInfo, stagingMountPrefix);
    const bool helperHasStagingMount = hasMountpointPrefix(helperMountInfo, stagingMountPrefix);
    const bool processHasStagingMount = hasMountpointPrefix(processMountInfo, stagingMountPrefix);
    if (hostHasStagingMount || helperHasStagingMount || processHasStagingMount) {
        qWarning() << "Temporary lower mountpoint remains attached" << hostHasStagingMount
                   << helperHasStagingMount << processHasStagingMount;
        return false;
    }
    bool processHasMount = false;
    for (const QString &path : targetPaths) {
        if (hasMountpoint(hostMountInfo, path) || hasMountpoint(helperMountInfo, path)) {
            qWarning() << "Projected target visible outside player" << path
                       << hasMountpoint(hostMountInfo, path) << hasMountpoint(helperMountInfo, path);
            return false;
        }
        processHasMount |= hasMountpoint(processMountInfo, path);
    }
    if (processHasMount != isMount) {
        qWarning() << "Player projected mount presence mismatch" << isMount << processHasMount << targetPaths;
    }
    return processHasMount == isMount;
}

QString InstanceBootstrapIntegrationTest::helperPid() const
{
    return expectedSystemdProperty(helperUnit, QStringLiteral("MainPID"));
}

void InstanceBootstrapIntegrationTest::releaseOwnedUnits()
{
    for (auto it = m_ownedPids.cbegin(); it != m_ownedPids.cend(); ++it) {
        const qint64 pid = it.key();
        if (::kill(static_cast<pid_t>(pid), 0) == 0 && m_bus.isConnected()) {
            stop(pid);
        }
        runCommand(QStringLiteral("systemctl"), {QStringLiteral("stop"),
                                                   QStringLiteral("couchplay-%1.service").arg(it.value())});
    }
    m_ownedPids.clear();
}

void InstanceBootstrapIntegrationTest::onInstanceReady(const QString &username, qlonglong pid)
{
    Q_UNUSED(username)
    m_readyPids.insert(pid);
}

void InstanceBootstrapIntegrationTest::onInstanceStopped(const QString &username, qlonglong pid, const QString &reason)
{
    Q_UNUSED(username)
    m_stoppedPids.insert(pid, reason);
}

void InstanceBootstrapIntegrationTest::testLaunchManifestCollectionLimits()
{
    LaunchManifest manifest;
    manifest.unitName = QStringLiteral("couchplay-player1.service");
    manifest.unitRole = QStringLiteral("main");
    manifest.callerUid = 1000;
    manifest.callerGid = 1000;
    manifest.targetUsername = QStringLiteral("player1");
    manifest.targetUid = 1001;
    manifest.targetGid = 1001;
    manifest.declaredHome = QStringLiteral("/home/compositor");
    manifest.canonicalHome = manifest.declaredHome;
    manifest.targetDeclaredHome = QStringLiteral("/home/player1");
    manifest.targetCanonicalHome = manifest.targetDeclaredHome;
    manifest.workingDirectory = manifest.targetDeclaredHome;
    for (int index = 0; index < 512; ++index) {
        const QString suffix = QString::number(index);
        manifest.directories.append({QStringLiteral("/source/%1").arg(suffix), QStringLiteral("bind"),
                                     QStringLiteral(".config/item-%1").arg(suffix), {}});
    }
    manifest.gamescopeArgv.append(QStringLiteral("/usr/bin/gamescope"));
    for (int index = 1; index < 4096; ++index) {
        manifest.gamescopeArgv.append(QStringLiteral("--arg-%1").arg(index));
    }
    for (int index = 0; index < 512; ++index) {
        manifest.environment.insert(QStringLiteral("CPLIMIT_%1").arg(index), QStringLiteral("value"));
    }

    QString token;
    QString error;
    const bool written = DirectoryPlan::writeLaunchManifest(manifest, &token, &error);
    if (!written) {
        QFAIL(qPrintable(error));
    }
    LaunchManifest roundTripped;
    const bool readable = DirectoryPlan::readLaunchManifest(token, &roundTripped, &error);
    QString cleanupError;
    const bool removed = DirectoryPlan::unlinkLaunchManifest(token, &cleanupError);
    QVERIFY2(removed, qPrintable(cleanupError));
    QVERIFY2(readable, qPrintable(error));
    QCOMPARE(roundTripped.directories.size(), 512);
    QCOMPARE(roundTripped.gamescopeArgv.size(), 4096);
    QCOMPARE(roundTripped.environment.size(), 512);

    QList<LaunchManifest> overLimit;
    LaunchManifest tooManyDirectories = manifest;
    tooManyDirectories.directories.append({QStringLiteral("/source/extra"), QStringLiteral("bind"),
                                           QStringLiteral(".config/extra"), {}});
    overLimit.append(std::move(tooManyDirectories));
    LaunchManifest tooManyArguments = manifest;
    tooManyArguments.gamescopeArgv.append(QStringLiteral("--extra"));
    overLimit.append(std::move(tooManyArguments));
    LaunchManifest tooManyEnvironment = manifest;
    tooManyEnvironment.environment.insert(QStringLiteral("CPLIMIT_EXTRA"), QStringLiteral("value"));
    overLimit.append(std::move(tooManyEnvironment));

    const QStringList collectionNames{QStringLiteral("directories"), QStringLiteral("argv"),
                                      QStringLiteral("environment")};
    QStringList persistedCollections;
    QStringList unreadableCollections;
    QStringList cleanupFailures;
    for (qsizetype index = 0; index < overLimit.size(); ++index) {
        const QString &name = collectionNames.at(index);
        const LaunchManifest &candidate = overLimit.at(index);
        QString rejectedToken;
        QString writeError;
        const bool persisted = DirectoryPlan::writeLaunchManifest(candidate, &rejectedToken, &writeError);
        if (persisted) {
            persistedCollections.append(name);
            LaunchManifest rejected;
            QString readError;
            if (!DirectoryPlan::readLaunchManifest(rejectedToken, &rejected, &readError)) {
                unreadableCollections.append(name);
            }
            QString unlinkError;
            if (!DirectoryPlan::unlinkLaunchManifest(rejectedToken, &unlinkError)) {
                cleanupFailures.append(name);
            }
        }
    }
    QVERIFY2(cleanupFailures.isEmpty(), qPrintable(cleanupFailures.join(QLatin1Char(','))));
    QVERIFY2(unreadableCollections.isEmpty(), qPrintable(unreadableCollections.join(QLatin1Char(','))));
    QVERIFY2(persistedCollections.isEmpty(), qPrintable(persistedCollections.join(QLatin1Char(','))));
}

void InstanceBootstrapIntegrationTest::testCopyIntoSgidParent()
{
    const Account &target = m_players[0];
    const QString source = m_sourceRoot + QStringLiteral("/sgid-parent-source");
    QVERIFY(createOwnedDirectory(source, m_caller.uid, m_caller.gid, 0700));
    const QString sourceFile = source + QStringLiteral("/Launcher.ini");
    QVERIFY(writeOwnedFile(sourceFile, QByteArrayLiteral("SGID-SOURCE"), m_caller.uid, m_caller.gid, 0600));

    const QString parent = QDir(target.canonicalHome).filePath(QStringLiteral("sgid-parent"));
    QVERIFY(createOwnedDirectory(parent, 0, m_caller.gid, 02711));
    struct stat parentBefore {};
    QVERIFY(::stat(QFile::encodeName(parent).constData(), &parentBefore) == 0);
    QCOMPARE(parentBefore.st_uid, uid_t(0));
    QCOMPARE(parentBefore.st_gid, m_caller.gid);
    QCOMPARE(parentBefore.st_mode & 07777, mode_t(02711));
    const QString targetAlias = QStringLiteral("sgid-parent/copied");
    const QString targetFile = QDir(target.canonicalHome).filePath(targetAlias + QStringLiteral("/Launcher.ini"));
    const QString label = QStringLiteral("copy-into-sgid-parent");
    qint64 runningPid = 0;
    const auto unitCleanup = qScopeGuard([&] {
        if (runningPid > 0) {
            (void)stop(runningPid);
            (void)waitForProcessExit(runningPid);
        }
    });
    runningPid = launch(target, plan(record(source, QStringLiteral("copy"), targetAlias)),
                        gameCommand(label, {{targetFile, QStringLiteral("SGID-SOURCE")}},
                                    {{targetFile, QStringLiteral("PLAYER-UPDATED")}}));
    QVERIFY2(runningPid > 0, "Copying below an SGID target parent was not acknowledged");
    QTRY_VERIFY_WITH_TIMEOUT(m_readyPids.contains(runningPid) || m_stoppedPids.contains(runningPid), 30000);
    QVERIFY2(m_readyPids.contains(runningPid), qPrintable(m_stoppedPids.value(runningPid)));
    QJsonObject report;
    QVERIFY(waitForReport(reportPath(label), &report));
    QVERIFY(assertDroppedCredentials(report, target));
    QCOMPARE(readResult(report, targetFile).value(QStringLiteral("bytes")).toString(),
             QStringLiteral("SGID-SOURCE"));
    QCOMPARE(readResult(report, targetFile).value(QStringLiteral("uid")).toVariant().toLongLong(),
             static_cast<qint64>(target.uid));
    QVERIFY(writeResult(report, targetFile).value(QStringLiteral("written")).toBool());
    QCOMPARE(readFile(targetFile), QByteArrayLiteral("PLAYER-UPDATED"));
    QVERIFY(stop(runningPid));
    QVERIFY(waitForProcessExit(runningPid));
    runningPid = 0;
    QCOMPARE(readFile(sourceFile), QByteArrayLiteral("SGID-SOURCE"));

    struct stat parentAfter {};
    QVERIFY(::stat(QFile::encodeName(parent).constData(), &parentAfter) == 0);
    QCOMPARE(parentAfter.st_dev, parentBefore.st_dev);
    QCOMPARE(parentAfter.st_ino, parentBefore.st_ino);
    QCOMPARE(parentAfter.st_uid, uid_t(0));
    QCOMPARE(parentAfter.st_gid, m_caller.gid);
    QCOMPARE(parentAfter.st_mode & 07777, mode_t(02711));
}

void InstanceBootstrapIntegrationTest::testAliasedSourceProjection_data()
{
    QTest::addColumn<QString>("mode");
    QTest::newRow("copy") << QStringLiteral("copy");
    QTest::newRow("overlay") << QStringLiteral("overlay");
}

void InstanceBootstrapIntegrationTest::testAliasedSourceProjection()
{
    QFETCH(QString, mode);
    const Account &target = m_players[0];
    const QString canonicalSource = m_externalRoot + QStringLiteral("/one");
    const QString sourceAlias = m_sourceRoot + QStringLiteral("/external-source-alias");
    QVERIFY(!QFileInfo::exists(sourceAlias));
    QVERIFY(::symlink(QFile::encodeName(canonicalSource).constData(), QFile::encodeName(sourceAlias).constData()) == 0);
    const auto aliasCleanup = qScopeGuard([&] { (void)::unlink(QFile::encodeName(sourceAlias).constData()); });
    QVERIFY(::lchown(QFile::encodeName(sourceAlias).constData(), m_caller.uid, m_caller.gid) == 0);

    const QString targetAlias = QStringLiteral(".couchplay/source-alias");
    const QString targetFile = QDir(target.canonicalHome).filePath(targetAlias + QStringLiteral("/Launcher.ini"));
    const QString label = QStringLiteral("source-alias-%1").arg(mode);
    QString launchError;
    qint64 runningPid = 0;
    const auto unitCleanup = qScopeGuard([&] {
        if (runningPid > 0) {
            (void)stop(runningPid);
            (void)waitForProcessExit(runningPid);
        }
    });
    runningPid = launch(target, plan(record(sourceAlias, mode, targetAlias)),
                        gameCommand(label, {{targetFile, QStringLiteral("EXTERNAL-ONE")}},
                                    {{targetFile, QStringLiteral("PRIVATE-ALIAS-WRITE")}}),
                        QString(), true, {}, &launchError);
    QVERIFY2(runningPid > 0, qPrintable(launchError));
    QTRY_VERIFY_WITH_TIMEOUT(m_readyPids.contains(runningPid) || m_stoppedPids.contains(runningPid), 30000);
    QVERIFY2(m_readyPids.contains(runningPid), qPrintable(m_stoppedPids.value(runningPid)));
    QJsonObject report;
    QVERIFY(waitForReport(reportPath(label), &report));
    QVERIFY(assertDroppedCredentials(report, target));
    QCOMPARE(readResult(report, targetFile).value(QStringLiteral("bytes")).toString(),
             QStringLiteral("EXTERNAL-ONE"));
    QCOMPARE(readResult(report, targetFile).value(QStringLiteral("uid")).toVariant().toLongLong(),
             static_cast<qint64>(target.uid));
    QVERIFY(writeResult(report, targetFile).value(QStringLiteral("written")).toBool());
    QVERIFY(assertViewIsPrivate(runningPid, target, targetAlias, mode == QStringLiteral("overlay")));
    QCOMPARE(readFile(canonicalSource + QStringLiteral("/Launcher.ini")), QByteArrayLiteral("EXTERNAL-ONE"));
    QVERIFY(stop(runningPid));
    QVERIFY(waitForProcessExit(runningPid));
    runningPid = 0;
}

void InstanceBootstrapIntegrationTest::testCopyLongLeaf()
{
    const Account &target = m_players[0];
    const QString source = m_sourceRoot + QStringLiteral("/long-leaf");
    QVERIFY(createOwnedDirectory(source, m_caller.uid, m_caller.gid, 0700));
    const QString sourceFile = source + QStringLiteral("/Launcher.ini");
    QVERIFY(writeOwnedFile(sourceFile, QByteArrayLiteral("LONG-LEAF-COPY"), m_caller.uid, m_caller.gid, 0600));

    QVERIFY(longCopyLeaf.toUtf8().size() >= 212);
    const QString destination = QDir(target.canonicalHome).filePath(longCopyLeaf);
    const QString targetFile = destination + QStringLiteral("/Launcher.ini");
    const QString label = QStringLiteral("copy-long-leaf");
    QString launchError;
    qint64 runningPid = 0;
    const auto unitCleanup = qScopeGuard([&] {
        if (runningPid > 0) {
            (void)stop(runningPid);
            (void)waitForProcessExit(runningPid);
        }
    });
    runningPid = launch(target, plan(record(source, QStringLiteral("copy"), longCopyLeaf)),
                        gameCommand(label, {{targetFile, QStringLiteral("LONG-LEAF-COPY")}}),
                        QString(), true, {}, &launchError);
    QVERIFY2(runningPid > 0, qPrintable(launchError));
    QTRY_VERIFY_WITH_TIMEOUT(m_readyPids.contains(runningPid) || m_stoppedPids.contains(runningPid), 30000);
    QVERIFY2(m_readyPids.contains(runningPid), qPrintable(m_stoppedPids.value(runningPid)));
    QJsonObject report;
    QVERIFY(waitForReport(reportPath(label), &report));
    QVERIFY(assertDroppedCredentials(report, target));
    QCOMPARE(readResult(report, targetFile).value(QStringLiteral("bytes")).toString(),
             QStringLiteral("LONG-LEAF-COPY"));
    QCOMPARE(readResult(report, targetFile).value(QStringLiteral("uid")).toVariant().toLongLong(),
             static_cast<qint64>(target.uid));
    QCOMPARE(readFile(sourceFile), QByteArrayLiteral("LONG-LEAF-COPY"));
    QVERIFY(stop(runningPid));
    QVERIFY(waitForProcessExit(runningPid));
    runningPid = 0;
}

void InstanceBootstrapIntegrationTest::testStagedCopyRollbackOnFullTarget()
{
    const Account &target = m_players[0];
    const QString source = m_sourceRoot + QStringLiteral("/copy");
    const QString staged = stagingPath(target, source);
    const QString oversizedStageFile = staged + QStringLiteral("/oversized-staging-file");
    QVERIFY(!QFileInfo::exists(oversizedStageFile));
    const auto stagingCleanup = qScopeGuard([&] { (void)QFile::remove(oversizedStageFile); });
    QFile sparseFile(oversizedStageFile);
    QVERIFY(sparseFile.open(QIODevice::WriteOnly));
    constexpr qint64 sparseSize = 2 * 1024 * 1024;
    QVERIFY(sparseFile.resize(sparseSize));
    QVERIFY(sparseFile.flush());
    sparseFile.close();
    const QByteArray sparsePath = QFile::encodeName(oversizedStageFile);
    QVERIFY(::chown(sparsePath.constData(), m_caller.uid, m_caller.gid) == 0);
    QVERIFY(::chmod(sparsePath.constData(), 0600) == 0);

    const QString mountPath = QDir(target.canonicalHome).filePath(QStringLiteral(".couchplay/staging-full"));
    QVERIFY(!QFileInfo::exists(mountPath));
    bool mounted = false;
    const auto mountCleanup = qScopeGuard([&] {
        const QByteArray path = QFile::encodeName(mountPath);
        if (mounted && ::umount2(path.constData(), MNT_DETACH) != 0) return;
        (void)QDir(mountPath).removeRecursively();
    });
    QVERIFY(QDir().mkpath(mountPath));
    const QByteArray mountBytes = QFile::encodeName(mountPath);
    const QByteArray mountOptions = QByteArrayLiteral("size=1m,mode=0700");
    QVERIFY2(::mount("tmpfs", mountBytes.constData(), "tmpfs", MS_NOSUID | MS_NODEV,
                     mountOptions.constData()) == 0,
             qPrintable(QString::fromLocal8Bit(strerror(errno))));
    mounted = true;
    QVERIFY2(::mount(nullptr, mountBytes.constData(), nullptr, MS_SHARED, nullptr) == 0,
             qPrintable(QString::fromLocal8Bit(strerror(errno))));
    QVERIFY(::chown(mountBytes.constData(), target.uid, target.gid) == 0);
    QVERIFY(::chmod(mountBytes.constData(), 0700) == 0);

    const QString targetAlias = QStringLiteral(".couchplay/staging-full/destination");
    const QString destination = QDir(target.canonicalHome).filePath(targetAlias);
    QVERIFY(createOwnedDirectory(destination, target.uid, target.gid, 0700));
    const QString destinationFile = destination + QStringLiteral("/Launcher.ini");
    QVERIFY(writeOwnedFile(destinationFile, QByteArrayLiteral("ORIGINAL-DESTINATION"),
                          target.uid, target.gid, 0600));
    struct stat destinationBefore {};
    QVERIFY(::stat(QFile::encodeName(destination).constData(), &destinationBefore) == 0);
    const QStringList scratchBefore = QDir(mountPath).entryList(
        {QStringLiteral(".cptmp-*")}, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);

    const QString label = QStringLiteral("staged-copy-full-target");
    QString launchError;
    QString failureReason;
    qint64 preparingPid = 0;
    const auto processCleanup = qScopeGuard([&] {
        if (preparingPid > 0) {
            (void)stop(preparingPid);
            (void)waitForProcessExit(preparingPid);
        }
    });
    preparingPid = launch(target, plan(record(source, QStringLiteral("copy"), targetAlias, staged)),
                          gameCommand(label), QString(), true, {}, &launchError);
    if (preparingPid > 0) {
        QTRY_VERIFY_WITH_TIMEOUT(m_stoppedPids.contains(preparingPid) || m_readyPids.contains(preparingPid), 30000);
        QVERIFY2(!m_readyPids.contains(preparingPid), "A staged copy unexpectedly fit on the full target");
        QVERIFY(m_stoppedPids.contains(preparingPid));
        failureReason = m_stoppedPids.value(preparingPid);
        QVERIFY2(failureReason.contains(source), qPrintable(failureReason));
        QVERIFY(waitForProcessExit(preparingPid));
        preparingPid = 0;
    } else {
        failureReason = launchError;
        QVERIFY2(failureReason.contains(source), qPrintable(failureReason));
    }
    QVERIFY2(failureReason.contains(QStringLiteral("No space left on device")), qPrintable(failureReason));

    struct stat destinationAfter {};
    QVERIFY(::stat(QFile::encodeName(destination).constData(), &destinationAfter) == 0);
    QCOMPARE(destinationAfter.st_dev, destinationBefore.st_dev);
    QCOMPARE(destinationAfter.st_ino, destinationBefore.st_ino);
    QCOMPARE(readFile(destinationFile), QByteArrayLiteral("ORIGINAL-DESTINATION"));
    QCOMPARE(QDir(mountPath).entryList({QStringLiteral(".cptmp-*")},
                                      QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot),
             scratchBefore);
    QCOMPARE(readFile(source + QStringLiteral("/Launcher.ini")), QByteArrayLiteral("COPY-ORIGINAL"));
    QVERIFY(!QFileInfo::exists(reportPath(label)));
}

void InstanceBootstrapIntegrationTest::testCopyStagingAndPrivateNamespace()
{
    const Account &target = m_players[0];
    // A searchable parent need not be readable by the player.
    QVERIFY(createOwnedDirectory(target.canonicalHome + QStringLiteral("/sources"), 0, 0, 0711));
    const QString source = m_sourceRoot + QStringLiteral("/copy");
    const QString staged = stagingPath(target, source);
    const QString destination = targetFor(target, source);
    const QString targetFile = destination + QStringLiteral("/Launcher.ini");
    const QString report = reportPath(QStringLiteral("copy-stage"));
    const QString hostTmpMarker = QStringLiteral("/tmp/couchplay-namespace-host-private-marker");
    const QStringList command = gameCommand(QStringLiteral("copy-stage"),
                                             {{targetFile, QStringLiteral("COPY-STAGED")},
                                              {destination + QStringLiteral("/Games.ini"), QStringLiteral("GAMES-ORIGINAL")},
                                              {destination + QStringLiteral("/Banners/banner.png"), QStringLiteral("BANNER")},
                                              {destination + QStringLiteral("/icons/icon.png"), QStringLiteral("ICON")}},
                                             {{targetFile, QStringLiteral("COPY-PLAYER-WRITE")},
                                              {destination + QStringLiteral("/player-only.txt"),
                                               QStringLiteral("PLAYER-ONLY")}},
                                             {hostTmpMarker}, true);
    const qint64 pid = launch(target, plan(record(source, QStringLiteral("copy"), {}, staged)), command);
    QVERIFY2(pid > 0, "The helper did not acknowledge the copy instance PID");
    QVERIFY2(waitForSignal({}, pid), "The copy instance never reported preparation readiness");
    QJsonObject result;
    QVERIFY2(waitForReport(report, &result), "The target-UID probe did not write its report");
    QVERIFY(assertDroppedCredentials(result, target));
    const QJsonObject read = readResult(result, targetFile);
    QCOMPARE(read.value(QStringLiteral("bytes")).toString(), QStringLiteral("COPY-STAGED"));
    QCOMPARE(readResult(result, destination + QStringLiteral("/Games.ini")).value(QStringLiteral("bytes")).toString(),
             QStringLiteral("GAMES-ORIGINAL"));
    QCOMPARE(readResult(result, destination + QStringLiteral("/Banners/banner.png")).value(QStringLiteral("bytes")).toString(),
             QStringLiteral("BANNER"));
    QCOMPARE(readResult(result, destination + QStringLiteral("/icons/icon.png")).value(QStringLiteral("bytes")).toString(),
             QStringLiteral("ICON"));
    QCOMPARE(read.value(QStringLiteral("expected")).toString(), QStringLiteral("COPY-STAGED"));
    QCOMPARE(read.value(QStringLiteral("uid")).toVariant().toLongLong(), static_cast<qint64>(target.uid));
    QCOMPARE(read.value(QStringLiteral("gid")).toVariant().toLongLong(), static_cast<qint64>(target.gid));
    QVERIFY(writeResult(result, targetFile).value(QStringLiteral("written")).toBool());
    QCOMPARE(objectAt(result.value(QStringLiteral("absent")).toArray(), 0).value(QStringLiteral("exists")).toBool(), false);
    QVERIFY(m_readyPids.contains(pid));
    QVERIFY(assertViewIsPrivate(pid, target, QStringLiteral("sources/copy"), false));
    QCOMPARE(mountNamespaceForPid(pid), result.value(QStringLiteral("mountNamespace")).toString());
    QVERIFY(mountNamespaceForPid(pid) != mountNamespaceForPid(::getpid()));
    QVERIFY(mountNamespaceForPid(pid) != mountNamespaceForPid(helperPid().toLongLong()));
    QCOMPARE(readFile(targetFile), QByteArrayLiteral("COPY-PLAYER-WRITE"));
    QCOMPARE(readFile(source + QStringLiteral("/Launcher.ini")), QByteArrayLiteral("COPY-ORIGINAL"));
    struct stat sourceStat {};
    QVERIFY(::stat(QFile::encodeName(source + QStringLiteral("/Launcher.ini")).constData(), &sourceStat) == 0);
    QCOMPARE(sourceStat.st_uid, m_caller.uid);
    QCOMPARE(sourceStat.st_gid, m_caller.gid);
    QCOMPARE(sourceStat.st_mode & 07777, mode_t(0600));


    const QString firstPlayerOnly = destination + QStringLiteral("/player-only.txt");
    const QJsonObject firstPlayerOnlyWrite = writeResult(result, firstPlayerOnly);
    QVERIFY2(firstPlayerOnlyWrite.value(QStringLiteral("written")).toBool(),
             qPrintable(QStringLiteral("errno %1").arg(
                 firstPlayerOnlyWrite.value(QStringLiteral("errno")).toInt())));
    QCOMPARE(readFile(firstPlayerOnly), QByteArrayLiteral("PLAYER-ONLY"));
    QVERIFY(stop(pid));
    QVERIFY(waitForProcessExit(pid));

    const QString playerOnly = destination + QStringLiteral("/player-only.txt");
    QVERIFY(writeOwnedFile(source + QStringLiteral("/Launcher.ini"), QByteArrayLiteral("COPY-UPDATED"),
                           m_caller.uid, m_caller.gid, 0600));
    QVERIFY(writeOwnedFile(source + QStringLiteral("/Games.ini"), QByteArrayLiteral("GAMES-UPDATED"),
                           m_caller.uid, m_caller.gid, 0600));
    QVERIFY(writeOwnedFile(staged + QStringLiteral("/Launcher.ini"), QByteArrayLiteral("COPY-STAGED-SECOND"),
                           m_caller.uid, m_caller.gid, 0600));
    const QString replacementLabel = QStringLiteral("copy-replacement");
    const qint64 replacementPid = launch(target, plan(record(source, QStringLiteral("copy"), {}, staged)),
                                         gameCommand(replacementLabel,
                                                     {{targetFile, QStringLiteral("COPY-STAGED-SECOND")},
                                                      {destination + QStringLiteral("/Games.ini"),
                                                       QStringLiteral("GAMES-UPDATED")}}, {},
                                                     {hostTmpMarker, playerOnly}, true));
    QVERIFY2(replacementPid > 0, "The second copy launch was not acknowledged");
    QVERIFY(waitForSignal({}, replacementPid));
    QJsonObject replacementReport;
    QVERIFY(waitForReport(reportPath(replacementLabel), &replacementReport));
    QVERIFY(assertDroppedCredentials(replacementReport, target));
    QCOMPARE(readResult(replacementReport, targetFile).value(QStringLiteral("bytes")).toString(),
             QStringLiteral("COPY-STAGED-SECOND"));
    QCOMPARE(readResult(replacementReport, destination + QStringLiteral("/Games.ini"))
                 .value(QStringLiteral("bytes")).toString(), QStringLiteral("GAMES-UPDATED"));
    QCOMPARE(objectAt(replacementReport.value(QStringLiteral("absent")).toArray(), 0)
                 .value(QStringLiteral("exists")).toBool(), false);
    QCOMPARE(objectAt(replacementReport.value(QStringLiteral("absent")).toArray(), 1)
                 .value(QStringLiteral("exists")).toBool(), false);
    QCOMPARE(readFile(targetFile), QByteArrayLiteral("COPY-STAGED-SECOND"));
    QVERIFY(!QFileInfo::exists(playerOnly));
    QVERIFY(stop(replacementPid));
    QVERIFY(waitForProcessExit(replacementPid));

    QCOMPARE(readFile(source + QStringLiteral("/Launcher.ini")), QByteArrayLiteral("COPY-UPDATED"));
    const QString secondPlayerDestination = targetFor(m_players[1], source);
    const QString secondPlayerFile = secondPlayerDestination + QStringLiteral("/Launcher.ini");
    const qint64 secondPlayerCopyPid = launch(
        m_players[1], plan(record(source, QStringLiteral("copy"))),
        gameCommand(QStringLiteral("copy-player2"), {{secondPlayerFile, QStringLiteral("COPY-UPDATED")}},
                    {{secondPlayerFile, QStringLiteral("PLAYER2-COPY-WRITE")}}));
    QVERIFY2(secondPlayerCopyPid > 0, "The second player copy instance was not acknowledged");
    QVERIFY(waitForSignal({}, secondPlayerCopyPid));
    QJsonObject secondPlayerCopyReport;
    QVERIFY(waitForReport(reportPath(QStringLiteral("copy-player2")), &secondPlayerCopyReport));
    QVERIFY(assertDroppedCredentials(secondPlayerCopyReport, m_players[1]));
    QCOMPARE(readResult(secondPlayerCopyReport, secondPlayerFile).value(QStringLiteral("bytes")).toString(),
             QStringLiteral("COPY-UPDATED"));
    QCOMPARE(readResult(secondPlayerCopyReport, secondPlayerFile).value(QStringLiteral("uid")).toVariant().toLongLong(),
             static_cast<qint64>(m_players[1].uid));
    QVERIFY(writeResult(secondPlayerCopyReport, secondPlayerFile).value(QStringLiteral("written")).toBool());
    QVERIFY(assertViewIsPrivate(secondPlayerCopyPid, m_players[1], QStringLiteral("sources/copy"), false));
    QCOMPARE(readFile(source + QStringLiteral("/Launcher.ini")), QByteArrayLiteral("COPY-UPDATED"));
    QCOMPARE(readFile(secondPlayerFile), QByteArrayLiteral("PLAYER2-COPY-WRITE"));
    QVERIFY(stop(secondPlayerCopyPid));
    QVERIFY(waitForProcessExit(secondPlayerCopyPid));


    const QString externalOne = m_externalRoot + QStringLiteral("/one");
    const QString externalTwo = m_externalRoot + QStringLiteral("/two");
    const QString externalTargetOne = QDir(target.declaredHome).filePath(
        QStringLiteral(".couchplay/copies/") + DirectoryPlan::dataDirectoryStagingSlug(externalOne, m_caller.declaredHome));
    const QString externalTargetTwo = QDir(target.declaredHome).filePath(
        QStringLiteral(".couchplay/copies/") + DirectoryPlan::dataDirectoryStagingSlug(externalTwo, m_caller.declaredHome));
    QVERIFY(externalTargetOne != externalTargetTwo);
    const qint64 externalPid = launch(target, plan(record(externalOne, QStringLiteral("copy"))),
                                      gameCommand(QStringLiteral("external-copy"),
                                                  {{externalTargetOne + QStringLiteral("/Launcher.ini"),
                                                    QStringLiteral("EXTERNAL-ONE")}}, {}, {}, true));
    QVERIFY2(externalPid > 0, "An external-prefix copy did not start");
    QVERIFY2(waitForSignal({}, externalPid), "The external copy did not reach READY");
    QJsonObject externalReport;
    QVERIFY(waitForReport(reportPath(QStringLiteral("external-copy")), &externalReport));
    QCOMPARE(readResult(externalReport, externalTargetOne + QStringLiteral("/Launcher.ini"))
                 .value(QStringLiteral("bytes")).toString(), QStringLiteral("EXTERNAL-ONE"));
    QVERIFY(assertDroppedCredentials(externalReport, target));
    QVERIFY(stop(externalPid));
    QVERIFY(waitForProcessExit(externalPid));
    const qint64 secondExternalPid = launch(target, plan(record(externalTwo, QStringLiteral("copy"))),
                                            gameCommand(QStringLiteral("external-copy-two"),
                                                        {{externalTargetTwo + QStringLiteral("/Launcher.ini"),
                                                          QStringLiteral("EXTERNAL-TWO")}}, {}, {}, true));
    QVERIFY(secondExternalPid > 0);
    QVERIFY(waitForSignal({}, secondExternalPid));
    QJsonObject secondExternalReport;
    QVERIFY(waitForReport(reportPath(QStringLiteral("external-copy-two")), &secondExternalReport));
    QCOMPARE(readResult(secondExternalReport, externalTargetTwo + QStringLiteral("/Launcher.ini"))
                 .value(QStringLiteral("bytes")).toString(), QStringLiteral("EXTERNAL-TWO"));
    QVERIFY(stop(secondExternalPid));
    QVERIFY(waitForProcessExit(secondExternalPid));
    QCOMPARE(readFile(externalTargetOne + QStringLiteral("/Launcher.ini")), QByteArrayLiteral("EXTERNAL-ONE"));
    QCOMPARE(readFile(externalTargetTwo + QStringLiteral("/Launcher.ini")), QByteArrayLiteral("EXTERNAL-TWO"));
    const QString hostTmpSource = QStringLiteral("/tmp/couchplay-namespace-host-source");
    QVERIFY2(!QFileInfo::exists(hostTmpSource), "The host-/tmp fixture path must be unused");
    m_hostTmpSource = hostTmpSource;
    QVERIFY(createOwnedDirectory(hostTmpSource, m_caller.uid, m_caller.gid, 0700));
    const QString hostTmpSourceFile = hostTmpSource + QStringLiteral("/Launcher.ini");
    QVERIFY(writeOwnedFile(hostTmpSourceFile, QByteArrayLiteral("HOST-TMP-SOURCE"),
                           m_caller.uid, m_caller.gid, 0600));
    const QString hostTmpTarget = QDir(target.declaredHome).filePath(
        QStringLiteral(".couchplay/copies/")
        + DirectoryPlan::dataDirectoryStagingSlug(hostTmpSource, m_caller.declaredHome));
    const QString hostTmpTargetFile = hostTmpTarget + QStringLiteral("/Launcher.ini");
    const QString hostTmpLabel = QStringLiteral("host-tmp-source");
    const qint64 hostTmpPid = launch(target, plan(record(hostTmpSource, QStringLiteral("copy"))),
                                     gameCommand(hostTmpLabel,
                                                 {{hostTmpTargetFile, QStringLiteral("HOST-TMP-SOURCE")}}, {},
                                                 {hostTmpSourceFile}, true));
    QVERIFY2(hostTmpPid > 0, "The host-/tmp source launch was not acknowledged");
    QVERIFY(waitForSignal({}, hostTmpPid));
    QCOMPARE(expectedSystemdProperty(QStringLiteral("couchplay-%1.service").arg(target.username),
                                     QStringLiteral("PrivateTmp")), QStringLiteral("yes"));
    const QString activeUnit = QStringLiteral("couchplay-%1.service").arg(target.username);
    QCOMPARE(expectedSystemdProperty(activeUnit, QStringLiteral("Type")), QStringLiteral("notify"));
    QCOMPARE(expectedSystemdProperty(activeUnit, QStringLiteral("NotifyAccess")), QStringLiteral("main"));
    QCOMPARE(expectedSystemdProperty(activeUnit, QStringLiteral("CollectMode")),
             QStringLiteral("inactive-or-failed"));
    QCOMPARE(expectedSystemdProperty(activeUnit, QStringLiteral("Delegate")), QStringLiteral("yes"));
    QCOMPARE(expectedSystemdProperty(activeUnit, QStringLiteral("KillMode")), QStringLiteral("control-group"));
    QCOMPARE(expectedSystemdProperty(activeUnit, QStringLiteral("SendSIGKILL")), QStringLiteral("yes"));
    QCOMPARE(expectedSystemdProperty(activeUnit, QStringLiteral("PrivateMounts")), QStringLiteral("yes"));
    QCOMPARE(expectedSystemdProperty(activeUnit, QStringLiteral("User")), target.username);
    QCOMPARE(expectedSystemdProperty(activeUnit, QStringLiteral("WorkingDirectory")), QStringLiteral("/"));
    const QString timeoutStart = expectedSystemdProperty(activeUnit, QStringLiteral("TimeoutStartUSec"));
    QVERIFY2(timeoutStart == QStringLiteral("1min") || timeoutStart == QStringLiteral("60s")
                 || timeoutStart == QStringLiteral("60000000"),
             qPrintable(timeoutStart));
    QJsonObject hostTmpReport;
    QVERIFY(waitForReport(reportPath(hostTmpLabel), &hostTmpReport));
    QVERIFY(assertDroppedCredentials(hostTmpReport, target));
    QCOMPARE(readResult(hostTmpReport, hostTmpTargetFile).value(QStringLiteral("bytes")).toString(),
             QStringLiteral("HOST-TMP-SOURCE"));
    QCOMPARE(objectAt(hostTmpReport.value(QStringLiteral("absent")).toArray(), 0)
                 .value(QStringLiteral("exists")).toBool(), false);
    QVERIFY(assertViewIsPrivate(hostTmpPid, target,
                                QStringLiteral(".couchplay/copies/")
                                    + DirectoryPlan::dataDirectoryStagingSlug(hostTmpSource,
                                                                             m_caller.declaredHome),
                                false));
    QCOMPARE(readFile(hostTmpSourceFile), QByteArrayLiteral("HOST-TMP-SOURCE"));
    QVERIFY(stop(hostTmpPid));
    QVERIFY(waitForProcessExit(hostTmpPid));
    QCOMPARE(readFile(hostTmpSourceFile), QByteArrayLiteral("HOST-TMP-SOURCE"));
}

void InstanceBootstrapIntegrationTest::testBindAndAclKeepTheirDistinctVisibility()
{
    const Account &target = m_players[0];
    const QString bindSource = m_sourceRoot + QStringLiteral("/bind");
    const QString bindDestination = targetFor(target, bindSource);
    const QString bindFile = bindDestination + QStringLiteral("/Launcher.ini");
    const qint64 bindPid = launch(target, plan(record(bindSource, QStringLiteral("bind"))),
                                  gameCommand(QStringLiteral("bind"), {{bindFile, QStringLiteral("BIND-READONLY")}},
                                              {{bindFile, QStringLiteral("MUST-NOT-WRITE")}}, {}, true));
    QVERIFY2(bindPid > 0, "The bind instance was not acknowledged");
    QVERIFY(waitForSignal({}, bindPid));
    QJsonObject bindReport;
    QVERIFY(waitForReport(reportPath(QStringLiteral("bind")), &bindReport));
    QVERIFY(assertDroppedCredentials(bindReport, target));
    const QJsonObject bindRead = readResult(bindReport, bindFile);
    QCOMPARE(bindRead.value(QStringLiteral("bytes")).toString(), QStringLiteral("BIND-READONLY"));
    QCOMPARE(bindRead.value(QStringLiteral("uid")).toVariant().toLongLong(), static_cast<qint64>(m_caller.uid));
    QCOMPARE(bindRead.value(QStringLiteral("mode")).toVariant().toLongLong(), qint64(0444));
    const QJsonObject bindWrite = writeResult(bindReport, bindFile);
    QVERIFY(!bindWrite.value(QStringLiteral("written")).toBool());
    QVERIFY(bindWrite.value(QStringLiteral("errno")).toInt() == EACCES || bindWrite.value(QStringLiteral("errno")).toInt() == EPERM);
    QVERIFY(assertViewIsPrivate(bindPid, target, QStringLiteral("sources/bind"), true));
    QCOMPARE(readFile(bindSource + QStringLiteral("/Launcher.ini")), QByteArrayLiteral("BIND-READONLY"));
    QVERIFY(stop(bindPid));
    QVERIFY(waitForProcessExit(bindPid));

    const Account &secondTarget = m_players[1];
    const QString secondBindDestination = targetFor(secondTarget, bindSource);
    const QString secondBindFile = secondBindDestination + QStringLiteral("/Launcher.ini");
    const qint64 secondBindPid = launch(
        secondTarget, plan(record(bindSource, QStringLiteral("bind"))),
        gameCommand(QStringLiteral("bind-player2"), {{secondBindFile, QStringLiteral("BIND-READONLY")}},
                    {{secondBindFile, QStringLiteral("MUST-NOT-WRITE")}}, {}, true));
    QVERIFY2(secondBindPid > 0, "The second player's bind instance was not acknowledged");
    QVERIFY(waitForSignal({}, secondBindPid));
    QJsonObject secondBindReport;
    QVERIFY(waitForReport(reportPath(QStringLiteral("bind-player2")), &secondBindReport));
    QVERIFY(assertDroppedCredentials(secondBindReport, secondTarget));
    QCOMPARE(readResult(secondBindReport, secondBindFile).value(QStringLiteral("bytes")).toString(),
             QStringLiteral("BIND-READONLY"));
    QCOMPARE(readResult(secondBindReport, secondBindFile).value(QStringLiteral("uid")).toVariant().toLongLong(),
             static_cast<qint64>(m_caller.uid));
    QVERIFY(!writeResult(secondBindReport, secondBindFile).value(QStringLiteral("written")).toBool());
    QVERIFY(assertViewIsPrivate(secondBindPid, secondTarget, QStringLiteral("sources/bind"), true));
    QVERIFY(stop(secondBindPid));
    QVERIFY(waitForProcessExit(secondBindPid));

    const QString aclSource = m_sourceRoot + QStringLiteral("/acl");
    QVERIFY2(callAsFixtureCaller(QStringLiteral("SetPathAclWithParents"), {aclSource, target.username}),
             "The fixture caller could not install original-path traversal ACLs");
    const QString aclFile = aclSource + QStringLiteral("/Launcher.ini");
    const QString noProjection = targetFor(target, aclSource);
    const qint64 aclPid = launch(target, plan(record(aclSource, QStringLiteral("acl"))),
                                 gameCommand(QStringLiteral("acl-original"),
                                             {{aclFile, QStringLiteral("ACL-ORIGINAL")}},
                                             {{aclFile, QStringLiteral("ACL-ATTEMPTED-WRITE")}},
                                             {noProjection}, true));
    QVERIFY2(aclPid > 0, "The ACL-only instance was not acknowledged");
    QVERIFY(waitForSignal({}, aclPid));
    QJsonObject aclReport;
    QVERIFY(waitForReport(reportPath(QStringLiteral("acl-original")), &aclReport));
    QVERIFY(assertDroppedCredentials(aclReport, target));
    QCOMPARE(readResult(aclReport, aclFile).value(QStringLiteral("bytes")).toString(), QStringLiteral("ACL-ORIGINAL"));
    QVERIFY(!writeResult(aclReport, aclFile).value(QStringLiteral("written")).toBool());
    QCOMPARE(objectAt(aclReport.value(QStringLiteral("absent")).toArray(), 0)
                 .value(QStringLiteral("exists")).toBool(), false);
    QVERIFY(assertViewIsPrivate(aclPid, target, QStringLiteral("sources/acl"), false));
    QCOMPARE(readFile(aclFile), QByteArrayLiteral("ACL-ORIGINAL"));
    QVERIFY(stop(aclPid));
    QVERIFY(waitForProcessExit(aclPid));
}

void InstanceBootstrapIntegrationTest::testOverlayIsolationPersistenceAndBrokerRestart()
{
    const QString source = m_sourceRoot + QStringLiteral("/overlay");
    const QString destinationOne = targetFor(m_players[0], source);
    const QString destinationTwo = targetFor(m_players[1], source);
    const QString configOne = destinationOne + QStringLiteral("/Launcher.ini");
    const QString configTwo = destinationTwo + QStringLiteral("/Launcher.ini");
    const QString stagedOne = stagingPath(m_players[0], source);
    const QString originalBytes = QStringLiteral("OVERLAY-ORIGINAL");
    struct stat sourceBefore {};
    QVERIFY(::stat(QFile::encodeName(source + QStringLiteral("/Launcher.ini")).constData(), &sourceBefore) == 0);
    struct stat directoryBefore {};
    QVERIFY(::stat(QFile::encodeName(source).constData(), &directoryBefore) == 0);
    const QByteArray hostMountsBefore = readFile(QStringLiteral("/proc/1/mountinfo"));
    QVERIFY(!hostMountsBefore.isEmpty());
    const auto sourceUnchanged = [&]() {
        struct stat directoryAfter {};
        struct stat fileAfter {};
        const auto sameStat = [](const struct stat &before, const struct stat &after) {
            return before.st_dev == after.st_dev && before.st_ino == after.st_ino
                && before.st_uid == after.st_uid && before.st_gid == after.st_gid
                && (before.st_mode & 07777) == (after.st_mode & 07777);
        };
        return ::stat(QFile::encodeName(source).constData(), &directoryAfter) == 0
            && ::stat(QFile::encodeName(source + QStringLiteral("/Launcher.ini")).constData(), &fileAfter) == 0
            && sameStat(directoryBefore, directoryAfter) && sameStat(sourceBefore, fileAfter)
            && readFile(source + QStringLiteral("/Launcher.ini")) == QByteArrayLiteral("OVERLAY-ORIGINAL")
            && readFile(QStringLiteral("/proc/1/mountinfo")) == hostMountsBefore;
    };
    const QString pidOneLabel = QStringLiteral("overlay-player1-first");
    const qint64 pidOne = launch(m_players[0], plan(record(source, QStringLiteral("overlay"), {}, stagedOne)),
                                 gameCommand(pidOneLabel,
                                             {{configOne, QStringLiteral("OVERLAY-ORIGINAL")},
                                              {destinationOne + QStringLiteral("/Staged.ini"),
                                               QStringLiteral("OVERLAY-STAGED")}},
                                             {{configOne, QStringLiteral("PLAYER1-PRIVATE")}}, {}, true));
    QVERIFY2(pidOne > 0, "Player one overlay instance was not acknowledged");
    QVERIFY(waitForSignal({}, pidOne));
    QJsonObject first;
    QVERIFY(waitForReport(reportPath(pidOneLabel), &first));
    QVERIFY(assertDroppedCredentials(first, m_players[0]));
    QCOMPARE(readResult(first, configOne).value(QStringLiteral("bytes")).toString(), originalBytes);
    QCOMPARE(readResult(first, destinationOne + QStringLiteral("/Staged.ini"))
                 .value(QStringLiteral("bytes")).toString(), QStringLiteral("OVERLAY-STAGED"));
    QVERIFY(writeResult(first, configOne).value(QStringLiteral("written")).toBool());
    QCOMPARE(readResult(first, configOne).value(QStringLiteral("uid")).toVariant().toLongLong(),
             static_cast<qint64>(m_players[0].uid));
    QVERIFY2(sourceUnchanged(), "Player one Overlay changed the caller source or host mount table");
    QVERIFY(assertViewIsPrivate(pidOne, m_players[0], QStringLiteral("sources/overlay"), true));

    const QString pidTwoLabel = QStringLiteral("overlay-player2-first");
    const qint64 pidTwo = launch(m_players[1], plan(record(source, QStringLiteral("overlay"))),
                                 gameCommand(pidTwoLabel,
                                             {{configTwo, QStringLiteral("OVERLAY-ORIGINAL")}},
                                             {{configTwo, QStringLiteral("PLAYER2-PRIVATE")}}, {}, true));
    QVERIFY2(pidTwo > 0, "Player two could not independently use the same overlay lower");
    QVERIFY(waitForSignal({}, pidTwo));
    QJsonObject second;
    QVERIFY(waitForReport(reportPath(pidTwoLabel), &second));
    QVERIFY(assertDroppedCredentials(second, m_players[1]));
    QCOMPARE(readResult(second, configTwo).value(QStringLiteral("bytes")).toString(), originalBytes);
    QVERIFY(writeResult(second, configTwo).value(QStringLiteral("written")).toBool());
    QVERIFY2(sourceUnchanged(), "Two player Overlays changed the caller source or host mount table");
    QVERIFY(assertViewIsPrivate(pidTwo, m_players[1], QStringLiteral("sources/overlay"), true));
    QVERIFY(mountNamespaceForPid(pidOne) != mountNamespaceForPid(pidTwo));
    QVERIFY(mountNamespaceForPid(pidOne) != mountNamespaceForPid(::getpid()));
    QVERIFY(mountNamespaceForPid(pidOne) != mountNamespaceForPid(helperPid().toLongLong()));
    QVERIFY(mountNamespaceForPid(pidTwo) != mountNamespaceForPid(::getpid()));
    QVERIFY(mountNamespaceForPid(pidTwo) != mountNamespaceForPid(helperPid().toLongLong()));
    QCOMPARE(readFile(source + QStringLiteral("/Launcher.ini")), QByteArrayLiteral("OVERLAY-ORIGINAL"));
    struct stat sourceAfter {};
    QVERIFY(::stat(QFile::encodeName(source + QStringLiteral("/Launcher.ini")).constData(), &sourceAfter) == 0);
    QCOMPARE(sourceAfter.st_uid, sourceBefore.st_uid);
    QCOMPARE(sourceAfter.st_gid, sourceBefore.st_gid);
    QCOMPARE(sourceAfter.st_mode & 07777, sourceBefore.st_mode & 07777);

    const QString oldHelperPid = helperPid();
    QString error;
    QVERIFY2(runCommand(QStringLiteral("systemctl"), {QStringLiteral("restart"), helperUnit}, &error), qPrintable(error));
    const QString newHelperPid = helperPid();
    QVERIFY(!newHelperPid.isEmpty());
    QVERIFY(newHelperPid != oldHelperPid);
    QVERIFY(::kill(static_cast<pid_t>(pidOne), 0) == 0);
    QVERIFY(::kill(static_cast<pid_t>(pidTwo), 0) == 0);
    QVERIFY(mountNamespaceForPid(pidOne) != mountNamespaceForPid(newHelperPid.toLongLong()));
    QVERIFY(assertViewIsPrivate(pidOne, m_players[0], QStringLiteral("sources/overlay"), true));

    const qint64 duplicate = launch(m_players[0], plan(record(source, QStringLiteral("overlay"))),
                                    gameCommand(QStringLiteral("overlay-duplicate")));
    QVERIFY2(duplicate == 0, "A second active unit reused the same player's leased overlay upper/work pair");

    QVERIFY(stop(pidOne));
    QVERIFY(waitForProcessExit(pidOne));
    QVERIFY(sourceUnchanged());
    QVERIFY(stop(pidTwo));
    QVERIFY(waitForProcessExit(pidTwo));
    QVERIFY(sourceUnchanged());
    const QString relaunchLabel = QStringLiteral("overlay-player1-relaunch");
    const qint64 relaunch = launch(m_players[0], plan(record(source, QStringLiteral("overlay"))),
                                   gameCommand(relaunchLabel,
                                               {{configOne, QStringLiteral("PLAYER1-PRIVATE")}}, {}, {}, true));
    QVERIFY(relaunch > 0);
    QVERIFY(waitForSignal({}, relaunch));
    QJsonObject persisted;
    QVERIFY(waitForReport(reportPath(relaunchLabel), &persisted));
    QCOMPARE(readResult(persisted, configOne).value(QStringLiteral("bytes")).toString(),
             QStringLiteral("PLAYER1-PRIVATE"));
    QVERIFY(assertDroppedCredentials(persisted, m_players[0]));
    QCOMPARE(readFile(source + QStringLiteral("/Launcher.ini")), QByteArrayLiteral("OVERLAY-ORIGINAL"));
    QVERIFY(stop(relaunch));
    QVERIFY(waitForProcessExit(relaunch));
    QVERIFY(sourceUnchanged());
    QDir hostTmp(QStringLiteral("/tmp"));
    const QStringList lowerScratchBefore = hostTmp.entryList(
        {QStringLiteral(".lower-*")}, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    const QString privateTmpFalseLabel = QStringLiteral("overlay-private-tmp-false-early-kill");
    const QString privateTmpFalseBytes = QStringLiteral("PLAYER1-PRIVATE-TMP-FALSE");
    const qint64 privateTmpFalsePid = launch(
        m_players[0], plan(record(source, QStringLiteral("overlay"))),
        gameCommand(privateTmpFalseLabel,
                    {{configOne, QStringLiteral("PLAYER1-PRIVATE")}},
                    {{configOne, privateTmpFalseBytes}}, {}, true), {}, false);
    QVERIFY2(privateTmpFalsePid > 0, "PrivateTmp=false overlay launch was not acknowledged");
    QVERIFY(waitForSignal({}, privateTmpFalsePid));
    QJsonObject privateTmpFalseReport;
    QVERIFY(waitForReport(reportPath(privateTmpFalseLabel), &privateTmpFalseReport));
    QVERIFY(assertDroppedCredentials(privateTmpFalseReport, m_players[0]));
    QCOMPARE(readResult(privateTmpFalseReport, configOne).value(QStringLiteral("bytes")).toString(),
             QStringLiteral("PLAYER1-PRIVATE"));
    QVERIFY(writeResult(privateTmpFalseReport, configOne).value(QStringLiteral("written")).toBool());
    QVERIFY(assertViewIsPrivate(privateTmpFalsePid, m_players[0], QStringLiteral("sources/overlay"), true));
    QCOMPARE(expectedSystemdProperty(QStringLiteral("couchplay-%1.service").arg(m_players[0].username),
                                     QStringLiteral("PrivateTmp")), QStringLiteral("no"));
    QCOMPARE(hostTmp.entryList({QStringLiteral(".lower-*")},
                               QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden), lowerScratchBefore);
    QVERIFY(::kill(static_cast<pid_t>(privateTmpFalsePid), SIGKILL) == 0);
    QTRY_VERIFY_WITH_TIMEOUT(m_stoppedPids.contains(privateTmpFalsePid), 30000);
    QVERIFY(waitForProcessExit(privateTmpFalsePid));
    QVERIFY(sourceUnchanged());
    QCOMPARE(readFile(source + QStringLiteral("/Launcher.ini")), QByteArrayLiteral("OVERLAY-ORIGINAL"));
    QCOMPARE(hostTmp.entryList({QStringLiteral(".lower-*")},
                               QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden), lowerScratchBefore);
    const auto privateTmpFalseUnitInactive = [&] {
        const QString state = expectedSystemdProperty(
            QStringLiteral("couchplay-%1.service").arg(m_players[0].username), QStringLiteral("ActiveState"));
        return state.isEmpty() || state == QStringLiteral("inactive") || state == QStringLiteral("failed");
    };
    QTRY_VERIFY_WITH_TIMEOUT(privateTmpFalseUnitInactive(), 30000);

    const QString earlyKillRelaunchLabel = QStringLiteral("overlay-private-tmp-false-relaunch");
    const qint64 earlyKillRelaunch = launch(
        m_players[0], plan(record(source, QStringLiteral("overlay"))),
        gameCommand(earlyKillRelaunchLabel, {{configOne, privateTmpFalseBytes}}, {}, {}, true));
    QVERIFY(earlyKillRelaunch > 0);
    QVERIFY(waitForSignal({}, earlyKillRelaunch));
    QJsonObject earlyKillRelaunchReport;
    QVERIFY(waitForReport(reportPath(earlyKillRelaunchLabel), &earlyKillRelaunchReport));
    QCOMPARE(readResult(earlyKillRelaunchReport, configOne).value(QStringLiteral("bytes")).toString(),
             privateTmpFalseBytes);
    QVERIFY(assertDroppedCredentials(earlyKillRelaunchReport, m_players[0]));
    QVERIFY(stop(earlyKillRelaunch));
    QVERIFY(waitForProcessExit(earlyKillRelaunch));
    QCOMPARE(hostTmp.entryList({QStringLiteral(".lower-*")},
                               QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden), lowerScratchBefore);
    QVERIFY(sourceUnchanged());

    // The view was prepared before the game executable failed; failure must
    // still leave the shared caller mount and source untouched.
    const qint64 failedExecPid = launch(m_players[0], plan(record(source, QStringLiteral("overlay"))),
                                        {QStringLiteral("/no/such/couchplay-namespace-game")});
    QVERIFY(failedExecPid > 0);
    QTRY_VERIFY_WITH_TIMEOUT(m_readyPids.contains(failedExecPid), 30000);
    QTRY_VERIFY_WITH_TIMEOUT(m_stoppedPids.contains(failedExecPid), 30000);
    QVERIFY(waitForProcessExit(failedExecPid));
    QVERIFY(sourceUnchanged());
}

void InstanceBootstrapIntegrationTest::testPreparationFailureCancellationAndExecFailure()
{
    const Account &target = m_players[0];
    m_unsupportedLower = m_sourceRoot + QStringLiteral("/unsupported-idmap");
    QVERIFY(createOwnedDirectory(m_unsupportedLower, m_caller.uid, m_caller.gid, 0700));
    const QByteArray unsupportedPath = QFile::encodeName(m_unsupportedLower);
    QVERIFY2(::mount("ramfs", unsupportedPath.constData(), "ramfs", MS_NOSUID | MS_NODEV, nullptr) == 0,
             qPrintable(QString::fromLocal8Bit(strerror(errno))));
    m_unsupportedLowerMounted = true;
    QVERIFY(::chown(unsupportedPath.constData(), m_caller.uid, m_caller.gid) == 0);
    QVERIFY(::chmod(unsupportedPath.constData(), 0700) == 0);
    const QString unsupportedFile = m_unsupportedLower + QStringLiteral("/Launcher.ini");
    QVERIFY(writeOwnedFile(unsupportedFile, QByteArrayLiteral("RAMFS-ORIGINAL"), m_caller.uid, m_caller.gid, 0600));
    struct stat unsupportedBefore {};
    QVERIFY(::stat(QFile::encodeName(unsupportedFile).constData(), &unsupportedBefore) == 0);
    const auto assertUnsupportedIdmapFailure = [&](const QString &label) {
        const QByteArray hostMountsBefore = readFile(QStringLiteral("/proc/1/mountinfo"));
        const qint64 pid = launch(target, plan(record(m_unsupportedLower, QStringLiteral("overlay"))),
                                  gameCommand(label));
        if (pid <= 0) {
            QCOMPARE(readFile(QStringLiteral("/proc/1/mountinfo")), hostMountsBefore);
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(m_stoppedPids.contains(pid), 30000);
        QVERIFY(!m_readyPids.contains(pid));
        QVERIFY(waitForProcessExit(pid));
        QCOMPARE(readFile(QStringLiteral("/proc/1/mountinfo")), hostMountsBefore);
    };

    // Exercise second-player preparation failure while the first player's real view is active.
    const QString firstPlayerSource = m_sourceRoot + QStringLiteral("/overlay");
    const QString firstPlayerTarget = targetFor(m_players[0], firstPlayerSource);
    const qint64 firstPlayerPid = launch(m_players[0],
                                         plan(record(firstPlayerSource, QStringLiteral("overlay"))),
                                         gameCommand(QStringLiteral("second-player-failure-first"),
                                                     {{firstPlayerTarget + QStringLiteral("/Launcher.ini"),
                                                       QStringLiteral("OVERLAY-ORIGINAL")}}, {}, {}, true));
    QVERIFY2(firstPlayerPid > 0, "The first player did not start before the second-player failure");
    QVERIFY(waitForSignal({}, firstPlayerPid));
    QJsonObject firstPlayerReport;
    QVERIFY(waitForReport(reportPath(QStringLiteral("second-player-failure-first")), &firstPlayerReport));
    QVERIFY(assertDroppedCredentials(firstPlayerReport, m_players[0]));
    QVERIFY(assertViewIsPrivate(firstPlayerPid, m_players[0], QStringLiteral("sources/overlay"), true));

    QString secondPlayerError;
    const qint64 secondPlayerPid = launch(m_players[1],
                                          plan(record(m_unsupportedLower, QStringLiteral("overlay"))),
                                          gameCommand(QStringLiteral("second-player-failure")), QString(), true, {},
                                          &secondPlayerError);
    if (secondPlayerPid > 0) {
        QTRY_VERIFY_WITH_TIMEOUT(m_stoppedPids.contains(secondPlayerPid), 30000);
        QVERIFY(!m_readyPids.contains(secondPlayerPid));
        const QString secondPlayerReason = m_stoppedPids.value(secondPlayerPid);
        QVERIFY2(secondPlayerReason.contains(QStringLiteral("Overlay ownership mapping is not supported")),
                 qPrintable(secondPlayerReason));
    } else {
        QVERIFY2(secondPlayerError.contains(QStringLiteral("Overlay ownership mapping is not supported")),
                 qPrintable(secondPlayerError));
    }
    QVERIFY(::kill(static_cast<pid_t>(firstPlayerPid), 0) == 0);


    // The owning caller must stop earlier instances once a later player fails preparation.
    QVERIFY(stop(firstPlayerPid));
    QVERIFY(waitForProcessExit(firstPlayerPid));
    const auto firstUnitInactive = [&] {
        const QString state = expectedSystemdProperty(
            QStringLiteral("couchplay-%1.service").arg(m_players[0].username), QStringLiteral("ActiveState"));
        return state.isEmpty() || state == QStringLiteral("inactive") || state == QStringLiteral("failed");
    };
    QTRY_VERIFY_WITH_TIMEOUT(firstUnitInactive(), 30000);

    assertUnsupportedIdmapFailure(QStringLiteral("unsupported-idmap"));
    assertUnsupportedIdmapFailure(QStringLiteral("unsupported-idmap-retry"));
    QCOMPARE(readFile(unsupportedFile), QByteArrayLiteral("RAMFS-ORIGINAL"));
    struct stat unsupportedAfter {};
    QVERIFY(::stat(QFile::encodeName(unsupportedFile).constData(), &unsupportedAfter) == 0);
    QCOMPARE(unsupportedAfter.st_uid, unsupportedBefore.st_uid);
    QCOMPARE(unsupportedAfter.st_gid, unsupportedBefore.st_gid);
    QCOMPARE(unsupportedAfter.st_mode & 07777, unsupportedBefore.st_mode & 07777);
    const QString privateRoot = QStringLiteral("/srv/private");
    QVERIFY2(!QFileInfo::exists(privateRoot), "The private-source fixture path must be unused");
    m_privateSourceRoot = privateRoot;
    QVERIFY(createOwnedDirectory(privateRoot, 0, 0, 0700));
    const QString privateCopySource = privateRoot + QStringLiteral("/data");
    QVERIFY(createOwnedDirectory(privateCopySource, m_caller.uid, m_caller.gid, 0700));
    const QString privateSourceFile = privateCopySource + QStringLiteral("/Launcher.ini");
    QVERIFY(writeOwnedFile(privateSourceFile, QByteArrayLiteral("PRIVATE-SOURCE"),
                           m_caller.uid, m_caller.gid, 0644));
    const QString privateCopyTarget = QDir(target.declaredHome).filePath(
        QStringLiteral(".couchplay/copies/")
        + DirectoryPlan::dataDirectoryStagingSlug(privateCopySource, m_caller.declaredHome));
    const QString privateMountTarget = QDir(target.canonicalHome).filePath(
        QStringLiteral(".couchplay/mounts/srv/private/data"));
    for (const QString &mode : {QStringLiteral("copy"), QStringLiteral("bind"), QStringLiteral("overlay")}) {
        const QString label = QStringLiteral("caller-inaccessible-") + mode;
        const QString targetPath = mode == QStringLiteral("copy") ? privateCopyTarget : privateMountTarget;
        QString launchError;
        const qint64 pid = launch(target, plan(record(privateCopySource, mode)), gameCommand(label),
                                  QString(), true, {}, &launchError);
        if (pid > 0) {
            QTRY_VERIFY_WITH_TIMEOUT(m_stoppedPids.contains(pid), 30000);
            QVERIFY2(!m_readyPids.contains(pid),
                     qPrintable(QStringLiteral("%1 unexpectedly reported READY").arg(mode)));
            QVERIFY(waitForProcessExit(pid));
            const QString reason = m_stoppedPids.value(pid);
            QVERIFY2(reason.contains(QStringLiteral("authenticated caller"))
                         && reason.contains(privateCopySource),
                     qPrintable(reason));
        } else {
            QVERIFY2(launchError.contains(QStringLiteral("authenticated caller"))
                         && launchError.contains(privateCopySource),
                     qPrintable(launchError));
        }
        QVERIFY2(!QFileInfo::exists(reportPath(label)), qPrintable(QStringLiteral("%1 ran despite inaccessible ancestor")
                                                                    .arg(mode)));
        QVERIFY2(!QFileInfo::exists(targetPath), qPrintable(QStringLiteral("%1 created a projected target")
                                                               .arg(mode)));
    }



    const QString externalCwd = QStringLiteral("/opt/game");
    QVERIFY2(!QFileInfo::exists(externalCwd), "The external working-directory fixture path must be unused");
    m_externalWorkingDirectory = externalCwd;
    QVERIFY(createOwnedDirectory(externalCwd, 0, 0, 0755));
    const QString cwdMarker = externalCwd + QStringLiteral("/Launcher.ini");
    QVERIFY(writeOwnedFile(cwdMarker, QByteArrayLiteral("DIRECT-WORKING-DIRECTORY"), 0, 0, 0644));
    const QString cwdLabel = QStringLiteral("external-working-directory");
    const qint64 cwdPid = launch(target, {},
                                 gameCommand(cwdLabel, {{cwdMarker, QStringLiteral("DIRECT-WORKING-DIRECTORY")}},
                                             {}, {}, true),
                                 externalCwd, true, {externalCwd});
    QVERIFY2(cwdPid > 0, "The permitted external working directory was not acknowledged");
    QVERIFY(waitForSignal({}, cwdPid));
    QJsonObject cwdReport;
    QVERIFY(waitForReport(reportPath(cwdLabel), &cwdReport));
    QVERIFY(assertDroppedCredentials(cwdReport, target, externalCwd));
    QCOMPARE(readResult(cwdReport, cwdMarker).value(QStringLiteral("bytes")).toString(),
             QStringLiteral("DIRECT-WORKING-DIRECTORY"));
    QVERIFY(stop(cwdPid));
    QVERIFY(waitForProcessExit(cwdPid));

    const QString callerHomeBase = m_caller.declaredHome + QStringLiteral("/.local");
    const QString callerHomeShare = callerHomeBase + QStringLiteral("/share");
    const QString callerHomeCwd = callerHomeShare + QStringLiteral("/Example Launcher");
    QVERIFY(createOwnedDirectory(callerHomeBase, m_caller.uid, m_caller.gid, 0700));
    QVERIFY(createOwnedDirectory(callerHomeShare, m_caller.uid, m_caller.gid, 0700));
    QVERIFY(createOwnedDirectory(callerHomeCwd, m_caller.uid, m_caller.gid, 0700));
    const QString callerCwdMarker = callerHomeCwd + QStringLiteral("/Launcher.ini");
    QVERIFY(writeOwnedFile(callerCwdMarker, QByteArrayLiteral("CALLER-HOME-CWD"),
                           m_caller.uid, m_caller.gid, 0600));
    QVERIFY2(callAsFixtureCaller(QStringLiteral("SetPathAclWithParents"), {callerHomeCwd, target.username}),
             "The fixture caller could not grant original-path cwd traversal ACLs");
    const QString projectedCwd = targetFor(target, callerHomeCwd);
    QVERIFY(!QFileInfo::exists(projectedCwd));
    const QString callerCwdLabel = QStringLiteral("caller-home-original-cwd");
    const qint64 callerCwdPid = launch(target, plan(record(callerHomeCwd, QStringLiteral("acl"))),
                                       gameCommand(callerCwdLabel,
                                                   {{callerCwdMarker, QStringLiteral("CALLER-HOME-CWD")}}, {}, {}, true),
                                       callerHomeCwd, true, {callerHomeCwd});
    QVERIFY2(callerCwdPid > 0, "The caller-home working-directory launch was not acknowledged");
    QVERIFY(waitForSignal({}, callerCwdPid));
    QJsonObject callerCwdReport;
    QVERIFY(waitForReport(reportPath(callerCwdLabel), &callerCwdReport));
    QVERIFY(assertDroppedCredentials(callerCwdReport, target, callerHomeCwd));
    QCOMPARE(readResult(callerCwdReport, callerCwdMarker).value(QStringLiteral("bytes")).toString(),
             QStringLiteral("CALLER-HOME-CWD"));
    QVERIFY(!QFileInfo::exists(projectedCwd));
    QVERIFY(stop(callerCwdPid));
    QVERIFY(waitForProcessExit(callerCwdPid));

    const QString missingSource = m_sourceRoot + QStringLiteral("/does-not-exist");
    const qint64 missing = launch(target, plan(record(missingSource, QStringLiteral("copy"))),
                                  gameCommand(QStringLiteral("missing-source")));
    QVERIFY2(missing == 0, "A missing directory source was accepted for launch");

    const QString inaccessible = m_sourceRoot + QStringLiteral("/inaccessible");
    const QString inaccessibleDestination = targetFor(target, inaccessible);
    const QString inaccessibleReport = reportPath(QStringLiteral("inaccessible-source"));
    QString inaccessibleError;
    const qint64 inaccessiblePid = launch(target, plan(record(inaccessible, QStringLiteral("bind"))),
                                          gameCommand(QStringLiteral("inaccessible-source"),
                                                      {{inaccessibleDestination + QStringLiteral("/Launcher.ini"),
                                                        QStringLiteral("INACCESSIBLE")}}),
                                          QString(), true, {}, &inaccessibleError);
    if (inaccessiblePid > 0) {
        QTRY_VERIFY_WITH_TIMEOUT(m_stoppedPids.contains(inaccessiblePid) || m_readyPids.contains(inaccessiblePid), 30000);
        QVERIFY2(!m_readyPids.contains(inaccessiblePid), "An inaccessible bind view incorrectly reported READY");
        QVERIFY(m_stoppedPids.contains(inaccessiblePid));
        QVERIFY(waitForProcessExit(inaccessiblePid));
    } else {
        QVERIFY(!inaccessibleError.isEmpty());
    }
    QVERIFY(!QFileInfo::exists(inaccessibleReport));

    const QString unsafeSource = m_sourceRoot + QStringLiteral("/source-symlink");
    QVERIFY(::symlink(QFile::encodeName(m_sourceRoot + QStringLiteral("/copy")).constData(),
                      QFile::encodeName(unsafeSource).constData()) == 0);
    QString symlinkError;
    const qint64 symlinkPid = launch(target, plan(record(unsafeSource, QStringLiteral("copy"))),
                                     gameCommand(QStringLiteral("source-symlink")), QString(), true, {},
                                     &symlinkError);
    if (symlinkPid > 0) {
        QElapsedTimer stoppedTimer;
        stoppedTimer.start();
        while (!m_stoppedPids.contains(symlinkPid) && !m_readyPids.contains(symlinkPid)
               && stoppedTimer.elapsed() < 30000) {

            QCoreApplication::processEvents();
            QTest::qWait(20);
        }
        QVERIFY2(!m_readyPids.contains(symlinkPid), "A source-child symlink unexpectedly reported READY");
        if (!m_stoppedPids.contains(symlinkPid)) {

            const QString unitName = QStringLiteral("couchplay-%1.service").arg(target.username);
            const QStringList unitProperties = {
                QStringLiteral("ActiveState"), QStringLiteral("SubState"), QStringLiteral("MainPID"),
                QStringLiteral("Result"), QStringLiteral("StatusText"), QStringLiteral("StatusErrno")
            };
            QStringList showArguments{QStringLiteral("show"), unitName};
            for (const QString &property : unitProperties) {
                showArguments << QStringLiteral("-p") << property;
            }
            QProcess showUnit;
            showUnit.start(QStringLiteral("systemctl"), showArguments);
            const bool showFinished = showUnit.waitForStarted(2000) && showUnit.waitForFinished(3000);
            qWarning().noquote() << "SYMLINK-UNIT-STATE" << symlinkPid << m_readyPids.contains(symlinkPid)
                                 << m_stoppedPids.keys()
                                 << (showFinished ? QString::fromLocal8Bit(showUnit.readAllStandardOutput()).trimmed()
                                                  : showUnit.errorString());
            QFile stateFile(QStringLiteral("/run/couchplay/state.json"));
            QJsonObject state;
            if (stateFile.open(QIODevice::ReadOnly)) {
                const QJsonDocument document = QJsonDocument::fromJson(stateFile.readAll());
                if (document.isObject()) state = document.object();
            }
            QJsonObject ownership;
            for (const QString &key : {QStringLiteral("activeUnits"), QStringLiteral("pidToUnitName"),
                                       QStringLiteral("pidToUsername"), QStringLiteral("pidOwnerUid"),
                                       QStringLiteral("readyUnits"), QStringLiteral("unitOwnerUid"),
                                       QStringLiteral("targetIdentityByUnit")}) {
                ownership.insert(key, state.value(key));
            }
            qWarning().noquote() << "SYMLINK-BROKER-MAPS"
                                 << QJsonDocument(ownership).toJson(QJsonDocument::Compact);
            QProcess unitJournal;
            unitJournal.start(QStringLiteral("journalctl"),
                              {QStringLiteral("--no-pager"), QStringLiteral("--since"), QStringLiteral("5 minutes ago"),
                               QStringLiteral("-u"), unitName, QStringLiteral("-o"), QStringLiteral("short-iso")});
            if (unitJournal.waitForStarted(2000) && unitJournal.waitForFinished(3000)) {
                qWarning().noquote() << "SYMLINK-UNIT-JOURNAL"
                                     << QString::fromLocal8Bit(unitJournal.readAllStandardOutput()).trimmed();
            }
        }
        QVERIFY2(m_stoppedPids.contains(symlinkPid), "The acknowledged symlink failure emitted no stopped signal");
        QVERIFY(waitForProcessExit(symlinkPid));
        const QString reason = m_stoppedPids.value(symlinkPid);
        QVERIFY2(reason.contains(unsafeSource) || reason.contains(QStringLiteral("symlink"), Qt::CaseInsensitive),
                 qPrintable(reason));
    } else {
        QVERIFY2(symlinkError.contains(unsafeSource)
                     || symlinkError.contains(QStringLiteral("symlink"), Qt::CaseInsensitive),
                 qPrintable(symlinkError));
    }
    QVERIFY(!QFileInfo::exists(targetFor(target, unsafeSource)));

    QVERIFY(::unlink(QFile::encodeName(unsafeSource).constData()) == 0);

    const QString stagingSource = m_sourceRoot + QStringLiteral("/staging-symlink-source");
    const QString stagingAttacker = m_sourceRoot + QStringLiteral("/staging-attacker");
    QVERIFY(createOwnedDirectory(stagingSource, m_caller.uid, m_caller.gid, 0700));
    QVERIFY(writeOwnedFile(stagingSource + QStringLiteral("/Launcher.ini"), QByteArrayLiteral("STAGING-SOURCE"),
                           m_caller.uid, m_caller.gid, 0600));
    QVERIFY(createOwnedDirectory(stagingAttacker, m_caller.uid, m_caller.gid, 0700));
    QVERIFY(writeOwnedFile(stagingAttacker + QStringLiteral("/Launcher.ini"), QByteArrayLiteral("STAGING-ATTACKER"),
                           m_caller.uid, m_caller.gid, 0600));
    const QString stagingLeaf = stagingPath(target, stagingSource);
    QVERIFY(!QFileInfo::exists(stagingLeaf));
    QVERIFY(::symlink(QFile::encodeName(stagingAttacker).constData(),
                      QFile::encodeName(stagingLeaf).constData()) == 0);
    QString stagingSymlinkError;
    const qint64 stagingSymlinkPid = launch(target, plan(record(stagingSource, QStringLiteral("copy"), {}, stagingLeaf)),
                                            gameCommand(QStringLiteral("staging-symlink")), QString(), true, {},
                                            &stagingSymlinkError);
    if (stagingSymlinkPid > 0) {
        QTRY_VERIFY_WITH_TIMEOUT(m_stoppedPids.contains(stagingSymlinkPid)
                                     || m_readyPids.contains(stagingSymlinkPid), 30000);
        QVERIFY2(!m_readyPids.contains(stagingSymlinkPid), "A symlinked staging directory unexpectedly reported READY");
        QVERIFY(m_stoppedPids.contains(stagingSymlinkPid));
        QVERIFY(waitForProcessExit(stagingSymlinkPid));
    } else {
        QVERIFY2(!stagingSymlinkError.isEmpty(), "A staging-symlink launch failed without a D-Bus error");
    }
    QVERIFY(!QFileInfo::exists(reportPath(QStringLiteral("staging-symlink"))));
    QVERIFY(!QFileInfo::exists(targetFor(target, stagingSource)));
    QVERIFY(::unlink(QFile::encodeName(stagingLeaf).constData()) == 0);
    const QString raceSource = m_sourceRoot + QStringLiteral("/race");
    const QString raceBackup = m_sourceRoot + QStringLiteral("/race-original");
    const QString attackerSource = m_sourceRoot + QStringLiteral("/attacker");
    QVERIFY(createOwnedDirectory(attackerSource, m_caller.uid, m_caller.gid, 0700));
    QVERIFY(writeOwnedFile(attackerSource + QStringLiteral("/Launcher.ini"), QByteArrayLiteral("ATTACKER"),
                           m_caller.uid, m_caller.gid, 0600));
    for (int i = 0; i < 8192; ++i) {
        const QString path = raceSource + QStringLiteral("/race-file-%1").arg(i, 5, 10, QLatin1Char('0'));
        if (!writeOwnedFile(path, QByteArrayLiteral("RACE-FILE"), m_caller.uid, m_caller.gid, 0600)) {
            QFAIL("Could not create the tmpfs source-swap window fixture");
        }
    }
    const QString raceDestination = targetFor(target, raceSource) + QStringLiteral("/Launcher.ini");
    const QString raceStagedDestination = targetFor(target, raceSource) + QStringLiteral("/Staged.ini");
    const QString raceStaging = stagingPath(target, raceSource);
    const QString raceStagingBackup = raceStaging + QStringLiteral(".original");
    const QString attackerStaging = m_sourceRoot + QStringLiteral("/staging-race-attacker");
    QVERIFY(!QFileInfo::exists(raceStaging));
    QVERIFY(createOwnedDirectory(raceStaging, m_caller.uid, m_caller.gid, 0700));
    QVERIFY(writeOwnedFile(raceStaging + QStringLiteral("/Staged.ini"), QByteArrayLiteral("STAGING-ORIGINAL"),
                           m_caller.uid, m_caller.gid, 0600));
    QVERIFY(createOwnedDirectory(attackerStaging, m_caller.uid, m_caller.gid, 0700));
    QVERIFY(writeOwnedFile(attackerStaging + QStringLiteral("/Staged.ini"), QByteArrayLiteral("STAGING-ATTACKER"),
                           m_caller.uid, m_caller.gid, 0600));
    const qint64 racePid = launch(
        target, plan(record(raceSource, QStringLiteral("copy"), {}, raceStaging)),
        gameCommand(QStringLiteral("source-and-staging-swap"),
                    {{raceDestination, QStringLiteral("RACE-ORIGINAL")},
                     {raceStagedDestination, QStringLiteral("STAGING-ORIGINAL")}}, {}, {}, true));
    QVERIFY(racePid > 0);
    QCoreApplication::processEvents();
    QVERIFY2(!m_readyPids.contains(racePid) && !m_stoppedPids.contains(racePid),
             "The source-swap fixture completed before its path replacement");
    QVERIFY(::rename(QFile::encodeName(raceSource).constData(), QFile::encodeName(raceBackup).constData()) == 0);
    QVERIFY(::symlink(QFile::encodeName(attackerSource).constData(), QFile::encodeName(raceSource).constData()) == 0);
    QVERIFY(::rename(QFile::encodeName(raceStaging).constData(), QFile::encodeName(raceStagingBackup).constData()) == 0);
    QVERIFY(::symlink(QFile::encodeName(attackerStaging).constData(), QFile::encodeName(raceStaging).constData()) == 0);
    QTRY_VERIFY_WITH_TIMEOUT(m_readyPids.contains(racePid) || m_stoppedPids.contains(racePid), 30000);
    if (m_readyPids.contains(racePid)) {
        QJsonObject raceReport;
        QVERIFY(waitForReport(reportPath(QStringLiteral("source-and-staging-swap")), &raceReport));
        QCOMPARE(readResult(raceReport, raceDestination).value(QStringLiteral("bytes")).toString(),
                 QStringLiteral("RACE-ORIGINAL"));
        QCOMPARE(readResult(raceReport, raceStagedDestination).value(QStringLiteral("bytes")).toString(),
                 QStringLiteral("STAGING-ORIGINAL"));
        QVERIFY(stop(racePid));
        QVERIFY(waitForProcessExit(racePid));
    } else {
        QVERIFY(m_stoppedPids.contains(racePid));
        QVERIFY(!QFileInfo::exists(reportPath(QStringLiteral("source-and-staging-swap"))));
    }
    QVERIFY(::unlink(QFile::encodeName(raceSource).constData()) == 0);
    QVERIFY(::rename(QFile::encodeName(raceBackup).constData(), QFile::encodeName(raceSource).constData()) == 0);
    QVERIFY(::unlink(QFile::encodeName(raceStaging).constData()) == 0);
    QVERIFY(::rename(QFile::encodeName(raceStagingBackup).constData(), QFile::encodeName(raceStaging).constData()) == 0);
    QVERIFY(!QFileInfo::exists(QStringLiteral("/etc/Launcher.ini")));
    const QString targetSwapAlias = QStringLiteral(".couchplay/ns-target-swap");
    const QString targetSwapPath = QDir(target.canonicalHome).filePath(targetSwapAlias);
    const QString targetSwapBackup = targetSwapPath + QStringLiteral(".original");
    QVERIFY(!QFileInfo::exists(targetSwapPath));
    QVERIFY(!QFileInfo::exists(targetSwapBackup));
    QVERIFY(QDir().mkpath(QFileInfo(targetSwapPath).path()));
    const QString targetSwapFile = targetSwapPath + QStringLiteral("/Launcher.ini");
    QString targetSwapError;
    const qint64 targetSwapPid = launch(
        target, plan(record(raceSource, QStringLiteral("copy"), targetSwapAlias)),
        gameCommand(QStringLiteral("target-symlink-swap"),
                    {{targetSwapFile, QStringLiteral("RACE-ORIGINAL")}}, {}, {}, true),
        QString(), true, {}, &targetSwapError);
    QVERIFY2(targetSwapPid > 0, qPrintable(targetSwapError));
    QCoreApplication::processEvents();
    QVERIFY2(!m_readyPids.contains(targetSwapPid) && !m_stoppedPids.contains(targetSwapPid),
             "The target-swap fixture completed before its path replacement");
    struct stat targetSwapInfo {};
    const QByteArray targetSwapPathBytes = QFile::encodeName(targetSwapPath);
    const int targetSwapStat = ::lstat(targetSwapPathBytes.constData(), &targetSwapInfo);
    const int targetSwapStatError = errno;
    const bool hadTargetBeforeSwap = targetSwapStat == 0;
    QVERIFY2(hadTargetBeforeSwap || targetSwapStatError == ENOENT,
             "The target-swap path could not be inspected safely");
    if (hadTargetBeforeSwap) {
        QVERIFY(S_ISDIR(targetSwapInfo.st_mode));
        QVERIFY(::rename(QFile::encodeName(targetSwapPath).constData(),
                         QFile::encodeName(targetSwapBackup).constData()) == 0);
    }
    QVERIFY(::symlink("/etc", QFile::encodeName(targetSwapPath).constData()) == 0);
    QTRY_VERIFY_WITH_TIMEOUT(m_stoppedPids.contains(targetSwapPid) || m_readyPids.contains(targetSwapPid), 30000);
    QVERIFY2(!m_readyPids.contains(targetSwapPid), "A target symlink swap unexpectedly reported READY");
    QVERIFY(m_stoppedPids.contains(targetSwapPid));
    QVERIFY(waitForProcessExit(targetSwapPid));
    QVERIFY(!QFileInfo::exists(reportPath(QStringLiteral("target-symlink-swap"))));
    QVERIFY(QFileInfo(targetSwapPath).isSymLink());
    QVERIFY(!QFileInfo::exists(QStringLiteral("/etc/Launcher.ini")));
    QVERIFY(::unlink(QFile::encodeName(targetSwapPath).constData()) == 0);
    if (hadTargetBeforeSwap) {
        QVERIFY(::rename(QFile::encodeName(targetSwapBackup).constData(),
                         QFile::encodeName(targetSwapPath).constData()) == 0);
    }

    const QString escape = QDir(target.canonicalHome).filePath(QStringLiteral(".couchplay/ns-escape"));
    QVERIFY(QDir().mkpath(QFileInfo(escape).path()));
    QVERIFY(::symlink("/etc", QFile::encodeName(escape).constData()) == 0);
    QString targetSymlinkError;
    const qint64 unsafeTarget = launch(target,
                                       plan(record(m_sourceRoot + QStringLiteral("/copy"), QStringLiteral("copy"),
                                                   QStringLiteral(".couchplay/ns-escape"))),
                                       gameCommand(QStringLiteral("target-symlink")), QString(), true, {},
                                       &targetSymlinkError);
    if (unsafeTarget > 0) {
        QElapsedTimer targetStoppedTimer;
        targetStoppedTimer.start();
        while (!m_stoppedPids.contains(unsafeTarget) && !m_readyPids.contains(unsafeTarget)
               && targetStoppedTimer.elapsed() < 30000) {
            QCoreApplication::processEvents();
            QTest::qWait(20);
        }
        QVERIFY2(!m_readyPids.contains(unsafeTarget), "A symlinked target ancestor unexpectedly reported READY");
        if (!m_stoppedPids.contains(unsafeTarget)) {
            const QString unitName = QStringLiteral("couchplay-%1.service").arg(target.username);
            QProcess showUnit;
            showUnit.start(QStringLiteral("systemctl"),
                           {QStringLiteral("show"), unitName, QStringLiteral("-p"), QStringLiteral("ActiveState"),
                            QStringLiteral("-p"), QStringLiteral("SubState"), QStringLiteral("-p"),
                            QStringLiteral("MainPID"), QStringLiteral("-p"), QStringLiteral("Result"),
                            QStringLiteral("-p"), QStringLiteral("StatusText"), QStringLiteral("-p"),
                            QStringLiteral("StatusErrno")});
            const bool showFinished = showUnit.waitForStarted(2000) && showUnit.waitForFinished(3000);
            qWarning().noquote() << "TARGET-SYMLINK-UNIT-STATE" << unsafeTarget << m_stoppedPids.keys()
                                 << (showFinished ? QString::fromLocal8Bit(showUnit.readAllStandardOutput()).trimmed()
                                                  : showUnit.errorString());
            QFile stateFile(QStringLiteral("/run/couchplay/state.json"));
            if (stateFile.open(QIODevice::ReadOnly)) {
                const QJsonDocument document = QJsonDocument::fromJson(stateFile.readAll());
                qWarning().noquote() << "TARGET-SYMLINK-BROKER-STATE"
                                     << document.toJson(QJsonDocument::Compact);
            }
            QProcess unitJournal;
            unitJournal.start(QStringLiteral("journalctl"),
                              {QStringLiteral("--no-pager"), QStringLiteral("--since"),
                               QStringLiteral("5 minutes ago"), QStringLiteral("-u"), unitName,
                               QStringLiteral("-o"), QStringLiteral("short-iso")});
            if (unitJournal.waitForStarted(2000) && unitJournal.waitForFinished(3000)) {
                qWarning().noquote() << "TARGET-SYMLINK-UNIT-JOURNAL"
                                     << QString::fromLocal8Bit(unitJournal.readAllStandardOutput()).trimmed();
            }
        }
        QVERIFY2(m_stoppedPids.contains(unsafeTarget), "The acknowledged target-symlink failure emitted no stopped signal");
        QVERIFY(waitForProcessExit(unsafeTarget));
    } else {
        QVERIFY2(!targetSymlinkError.isEmpty(), "A target-symlink launch failed without a D-Bus error");
    }
    QVERIFY(!QFileInfo::exists(QStringLiteral("/etc/Launcher.ini")));
    QVERIFY(!QFileInfo::exists(reportPath(QStringLiteral("target-symlink"))));
    QVERIFY(QFileInfo(escape).isSymLink());
    QVERIFY(::unlink(QFile::encodeName(escape).constData()) == 0);

    const QString largeSource = m_sourceRoot + QStringLiteral("/large");
    QVERIFY(createOwnedDirectory(largeSource, m_caller.uid, m_caller.gid, 0700));
    for (int i = 0; i < 8192; ++i) {
        const QString path = largeSource + QStringLiteral("/file-%1").arg(i, 5, 10, QLatin1Char('0'));
        if (!writeOwnedFile(path, QByteArrayLiteral("cancel-during-copy"), m_caller.uid, m_caller.gid, 0600)) {
            QFAIL("Could not create the tmpfs copy-cancellation fixture");
        }
    }
    const QString cancelledCopyTarget = QDir(target.canonicalHome).filePath(longCopyLeaf);
    const QString cancelledCopyParent = QFileInfo(cancelledCopyTarget).path();
    QVERIFY(QDir().mkpath(cancelledCopyParent));
    QVERIFY(createOwnedDirectory(cancelledCopyTarget, target.uid, target.gid, 0700));
    const QString originalTargetFile = cancelledCopyTarget + QStringLiteral("/Original.ini");
    QVERIFY(writeOwnedFile(originalTargetFile, QByteArrayLiteral("ORIGINAL-DESTINATION"),
                           target.uid, target.gid, 0600));
    struct stat originalTargetStat {};
    QVERIFY(::stat(QFile::encodeName(cancelledCopyTarget).constData(), &originalTargetStat) == 0);
    const QString decoyScratchName =
        QStringLiteral(".cptmp-00000000-0000-4000-8000-000000000000-0000000000000000");
    const QString decoyScratchPath = QDir(cancelledCopyParent).filePath(decoyScratchName);
    QVERIFY(createOwnedDirectory(decoyScratchPath, target.uid, target.gid, 0700));
    const QString decoyScratchFile = decoyScratchPath + QStringLiteral("/User.ini");
    QVERIFY(writeOwnedFile(decoyScratchFile, QByteArrayLiteral("USER-OWNED"), target.uid, target.gid, 0600));
    const QStringList copyScratchPattern{QStringLiteral(".cptmp-*")};
    const auto copyScratchEntries = [&]() {
        QStringList entries = QDir(cancelledCopyParent).entryList(
            copyScratchPattern, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
        entries.removeAll(decoyScratchName);
        return entries;
    };
    const qint64 killedCopyPid = launch(target, plan(record(largeSource, QStringLiteral("copy"), longCopyLeaf)),
                                        gameCommand(QStringLiteral("kernel-killed-copy"), {}, {}, {}, false));
    QVERIFY2(killedCopyPid > 0, "The kernel-killed copy fixture was not acknowledged");
    QTRY_VERIFY_WITH_TIMEOUT(!copyScratchEntries().isEmpty(), 30000);
    QVERIFY(!m_readyPids.contains(killedCopyPid));
    QVERIFY(::kill(static_cast<pid_t>(killedCopyPid), SIGKILL) == 0);
    QVERIFY(waitForProcessExit(killedCopyPid));
    QTRY_VERIFY_WITH_TIMEOUT(m_stoppedPids.contains(killedCopyPid), 30000);
    QTRY_VERIFY_WITH_TIMEOUT(copyScratchEntries().isEmpty(), 30000);
    QVERIFY(QFileInfo::exists(decoyScratchPath));
    QCOMPARE(readFile(decoyScratchFile), QByteArrayLiteral("USER-OWNED"));
    struct stat targetAfterKillStat {};
    QVERIFY(::stat(QFile::encodeName(cancelledCopyTarget).constData(), &targetAfterKillStat) == 0);
    QCOMPARE(targetAfterKillStat.st_dev, originalTargetStat.st_dev);
    QCOMPARE(targetAfterKillStat.st_ino, originalTargetStat.st_ino);
    QCOMPARE(readFile(originalTargetFile), QByteArrayLiteral("ORIGINAL-DESTINATION"));
    QCOMPARE(readFile(cancelledCopyTarget + QStringLiteral("/Launcher.ini")), QByteArrayLiteral("LONG-LEAF-COPY"));
    QVERIFY(!QFileInfo::exists(reportPath(QStringLiteral("kernel-killed-copy"))));
    QCoreApplication::processEvents();
    const qint64 restartPreparingPid = launch(m_players[1], plan(record(largeSource, QStringLiteral("copy"))),
                                              gameCommand(QStringLiteral("restart-during-copy"), {}, {}, {}, true));
    QVERIFY2(restartPreparingPid > 0, "The preparing-copy restart fixture was not acknowledged");
    QCoreApplication::processEvents();
    QVERIFY2(!m_readyPids.contains(restartPreparingPid), "The slow copy unexpectedly reached READY before broker restart");
    const QString restartUnitName = QStringLiteral("couchplay-%1.service").arg(m_players[1].username);
    const auto captureCommand = [](const QString &program, const QStringList &arguments) {
        QProcess process;
        process.start(program, arguments);
        if (!process.waitForStarted(2000) || !process.waitForFinished(3000)) {
            return process.errorString();
        }
        return QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
    };
    const QString unitStateBeforeRestart = captureCommand(
        QStringLiteral("systemctl"), {QStringLiteral("show"), restartUnitName, QStringLiteral("-p"),
                                       QStringLiteral("ActiveState"), QStringLiteral("-p"),
                                       QStringLiteral("MainPID"), QStringLiteral("-p"),
                                       QStringLiteral("CollectMode")});
    const QString playerUnitsBeforeRestart = captureCommand(
        QStringLiteral("systemctl"), {QStringLiteral("list-units"), QStringLiteral("--all"),
                                       QStringLiteral("--no-legend"), QStringLiteral("couchplay-*.service")});
    const QString helperJournalBeforeRestart = captureCommand(
        QStringLiteral("journalctl"), {QStringLiteral("--no-pager"), QStringLiteral("-n"),
                                       QStringLiteral("30"), QStringLiteral("-u"), helperUnit,
                                       QStringLiteral("-o"), QStringLiteral("short-iso")});
    QString stateBeforeRestart = QStringLiteral("<unavailable>");
    QFile stateFileBeforeRestart(QStringLiteral("/run/couchplay/state.json"));
    if (stateFileBeforeRestart.open(QIODevice::ReadOnly)) {
        stateBeforeRestart = QString::fromUtf8(stateFileBeforeRestart.readAll()).trimmed();
    }
    const QStringList manifestsBeforeRestart = QDir(QStringLiteral("/run/couchplay/launch-plans"))
                                                   .entryList(QDir::Files | QDir::NoDotAndDotDot);
    const QString helperBeforePreparationRestart = helperPid();
    QString restartError;
    QVERIFY2(runCommand(QStringLiteral("systemctl"), {QStringLiteral("restart"), helperUnit}, &restartError),
             qPrintable(restartError));
    QTRY_VERIFY_WITH_TIMEOUT(!helperPid().isEmpty() && helperPid() != helperBeforePreparationRestart, 30000);
    if (!waitForSignal({}, restartPreparingPid, 55000)) {
        const QString unitName = QStringLiteral("couchplay-%1.service").arg(m_players[1].username);
        QProcess showUnit;
        showUnit.start(QStringLiteral("systemctl"),
                       {QStringLiteral("show"), unitName, QStringLiteral("-p"), QStringLiteral("ActiveState"),
                        QStringLiteral("-p"), QStringLiteral("SubState"), QStringLiteral("-p"),
                        QStringLiteral("MainPID"), QStringLiteral("-p"), QStringLiteral("Result"),
                        QStringLiteral("-p"), QStringLiteral("StatusText"), QStringLiteral("-p"),
                        QStringLiteral("StatusErrno")});
        const bool showFinished = showUnit.waitForStarted(2000) && showUnit.waitForFinished(3000);
        qWarning().noquote() << "PREPARING-BEFORE-RESTART-UNIT" << unitStateBeforeRestart;
        qWarning().noquote() << "PREPARING-BEFORE-RESTART-UNITS" << playerUnitsBeforeRestart;
        qWarning().noquote() << "PREPARING-BEFORE-RESTART-STATE" << stateBeforeRestart;
        qWarning().noquote() << "PREPARING-BEFORE-RESTART-MANIFESTS" << manifestsBeforeRestart;
        qWarning().noquote() << "PREPARING-BEFORE-RESTART-JOURNAL" << helperJournalBeforeRestart;
        qWarning().noquote() << "PREPARING-RESTART-UNIT" << restartPreparingPid << helperPid()
                             << (showFinished ? QString::fromLocal8Bit(showUnit.readAllStandardOutput()).trimmed()
                                              : showUnit.errorString());
        QFile stateFile(QStringLiteral("/run/couchplay/state.json"));
        if (stateFile.open(QIODevice::ReadOnly)) {
            const QJsonDocument document = QJsonDocument::fromJson(stateFile.readAll());
            qWarning().noquote() << "PREPARING-RESTART-STATE" << document.toJson(QJsonDocument::Compact);
        } else {
            qWarning() << "PREPARING-RESTART-STATE-ERROR" << stateFile.errorString();
        }
        qWarning().noquote() << "PREPARING-RESTART-MANIFESTS"
                             << QDir(QStringLiteral("/run/couchplay/launch-plans"))
                                    .entryList(QDir::Files | QDir::NoDotAndDotDot);
        qWarning().noquote() << "PREPARING-RESTART-STOPPED" << m_stoppedPids.value(restartPreparingPid);
        QProcess unitJournal;
        unitJournal.start(QStringLiteral("journalctl"),
                          {QStringLiteral("--no-pager"), QStringLiteral("-n"), QStringLiteral("60"),
                           QStringLiteral("-u"), unitName, QStringLiteral("-u"), helperUnit,
                           QStringLiteral("-o"), QStringLiteral("short-iso")});
        if (unitJournal.waitForStarted(2000) && unitJournal.waitForFinished(3000)) {
            qWarning().noquote() << "PREPARING-RESTART-JOURNAL"
                                 << QString::fromLocal8Bit(unitJournal.readAllStandardOutput()).trimmed();
        }
    }
    QVERIFY2(m_readyPids.contains(restartPreparingPid), "The restarted broker did not recover preparing-unit readiness");
    QJsonObject restartReport;
    QVERIFY(waitForReport(reportPath(QStringLiteral("restart-during-copy")), &restartReport));
    QVERIFY(assertDroppedCredentials(restartReport, m_players[1]));
    QVERIFY(stop(restartPreparingPid));
    QVERIFY(waitForProcessExit(restartPreparingPid));

    const qint64 timeoutPid = launch(target, plan(record(largeSource, QStringLiteral("copy"))),
                                      gameCommand(QStringLiteral("timeout-preparing")));
    QVERIFY(timeoutPid > 0);
    QCoreApplication::processEvents();
    QVERIFY2(!m_readyPids.contains(timeoutPid), "The slow-copy timeout fixture reached READY before it was frozen");
    QVERIFY(::kill(static_cast<pid_t>(timeoutPid), SIGSTOP) == 0);
    QTRY_VERIFY_WITH_TIMEOUT(m_stoppedPids.contains(timeoutPid), 160000);
    QVERIFY(!m_readyPids.contains(timeoutPid));
    QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(QStringLiteral("/proc/%1").arg(timeoutPid)), 120000);
    const QString timeoutState = expectedSystemdProperty(
        QStringLiteral("couchplay-%1.service").arg(target.username), QStringLiteral("ActiveState"));
    QVERIFY(timeoutState.isEmpty() || timeoutState == QStringLiteral("inactive")
            || timeoutState == QStringLiteral("failed"));

    const qint64 preparingPid = launch(target, plan(record(largeSource, QStringLiteral("copy"))),
                                       gameCommand(QStringLiteral("cancel-preparing")));
    QVERIFY(preparingPid > 0);
    QCoreApplication::processEvents();
    const bool wasReadyBeforeStop = m_readyPids.contains(preparingPid);
    QVERIFY(stop(preparingPid));
    QVERIFY(waitForProcessExit(preparingPid));
    QVERIFY2(!wasReadyBeforeStop, "The copy preparation completed before the cancellation assertion could run");

    const QStringList naturalCommand = gameCommand(QStringLiteral("natural-exit"), {}, {}, {}, false)
        + QStringList{QStringLiteral("--sleep-ms"), QStringLiteral("500")};
    const qint64 naturalExit = launch(target, {}, naturalCommand);
    QVERIFY(naturalExit > 0);
    QVERIFY(waitForSignal({}, naturalExit));
    QJsonObject naturalReport;
    QVERIFY(waitForReport(reportPath(QStringLiteral("natural-exit")), &naturalReport));
    QVERIFY(assertDroppedCredentials(naturalReport, target));
    QTRY_VERIFY_WITH_TIMEOUT(m_stoppedPids.contains(naturalExit), 30000);

    const qint64 execFailure = launch(target, {},
                                      {QStringLiteral("/no/such/couchplay-namespace-game")});
    QVERIFY(execFailure > 0);
    QVERIFY(waitForSignal({}, execFailure));
    QTRY_VERIFY_WITH_TIMEOUT(m_stoppedPids.contains(execFailure), 30000);
    QVERIFY(waitForProcessExit(execFailure));
}

int runFixtureCallerLaunch(int argc, char **argv)
{
    if (argc != 6 || std::strcmp(argv[1], "--fixture-dbus-call") != 0) {
        return 2;
    }
    bool uidValid = false;
    bool gidValid = false;
    const uid_t uid = static_cast<uid_t>(QString::fromLocal8Bit(argv[3]).toUInt(&uidValid));
    const gid_t gid = static_cast<gid_t>(QString::fromLocal8Bit(argv[4]).toUInt(&gidValid));
    const QString username = QString::fromLocal8Bit(argv[2]);
    const QByteArray userNameBytes = username.toLocal8Bit();
    if (::geteuid() != 0 || !uidValid || !gidValid
        || ::initgroups(userNameBytes.constData(), gid) != 0
        || ::setresgid(gid, gid, gid) != 0 || ::setresuid(uid, uid, uid) != 0) {
        std::perror("fixture caller credential drop");
        return 126;
    }
    struct passwd *account = ::getpwuid(uid);
    if (!account) {
        std::fprintf(stderr, "fixture caller is missing from passwd after credential drop\n");
        return 126;
    }
    qputenv("HOME", QByteArray(account->pw_dir));
    QByteArray runtimeDirectory = QByteArrayLiteral("/run/user/");
    runtimeDirectory.append(QByteArray::number(uid));
    qputenv("XDG_RUNTIME_DIR", runtimeDirectory);

    QCoreApplication application(argc, argv);
    const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromBase64(argv[5]));
    if (!document.isObject()) {
        std::fprintf(stderr, "invalid fixture-caller launch payload\n");
        return 2;
    }
    const QJsonObject payload = document.object();
    const QString method = payload.value(QStringLiteral("method")).toString();
    QVariantList arguments;
    if (method == QStringLiteral("LaunchInstance")) {
        const QJsonArray commandArray = payload.value(QStringLiteral("command")).toArray();
        QStringList command;
        for (const QJsonValue &value : commandArray) {
            if (!value.isString()) {
                std::fprintf(stderr, "invalid fixture-caller command argument\n");
                return 2;
            }
            command.append(value.toString());
        }
        const QJsonValue preparationValue = payload.value(QStringLiteral("directoryPreparations"));
        if (!preparationValue.isArray()) {
            std::fprintf(stderr, "invalid fixture-caller directory preparation list\n");
            return 2;
        }
        const QString targetUsername = payload.value(QStringLiteral("targetUsername")).toString();
        if (targetUsername.isEmpty()) {
            std::fprintf(stderr, "invalid fixture-caller target username\n");
            return 2;
        }
        const QJsonValue sharedRootsValue = payload.value(QStringLiteral("sharedRoots"));
        if (!sharedRootsValue.isArray()) {
            std::fprintf(stderr, "invalid fixture-caller shared roots\n");
            return 2;
        }
        QStringList sharedRoots;
        for (const QJsonValue &value : sharedRootsValue.toArray()) {
            if (!value.isString()) {
                std::fprintf(stderr, "invalid fixture-caller shared root\n");
                return 2;
            }
            sharedRoots.append(value.toString());
        }
        const QJsonValue privateTmpValue = payload.value(QStringLiteral("privateTemporaryDirectories"));
        if (!privateTmpValue.isBool()) {
            std::fprintf(stderr, "invalid fixture-caller private temporary directory flag\n");
            return 2;
        }
        const QStringList environment{QStringLiteral("XDG_CURRENT_DESKTOP=KDE"),
                                      QStringLiteral("PULSE_SINK=couchplay-fixture-sink")};
        arguments << targetUsername << QString() << QStringList{} << command
                  << payload.value(QStringLiteral("workingDirectory")).toString()
                  << sharedRoots << environment << privateTmpValue.toBool();
        arguments.append(QVariant::fromValue(preparationValue.toArray().toVariantList()));
    } else if (method == QStringLiteral("SetPathAclWithParents")) {
        const QJsonArray values = payload.value(QStringLiteral("arguments")).toArray();
        if (values.size() != 2 || !values.at(0).isString() || !values.at(1).isString()) {
            std::fprintf(stderr, "invalid fixture-caller ACL arguments\n");
            return 2;
        }
        arguments << values.at(0).toString() << values.at(1).toString();
    } else if (method == QStringLiteral("StopInstance")) {
        const QJsonArray values = payload.value(QStringLiteral("arguments")).toArray();
        if (values.size() != 1 || !values.at(0).isDouble()) {
            std::fprintf(stderr, "invalid fixture-caller stop arguments\n");
            return 2;
        }
        arguments << static_cast<qint64>(values.at(0).toDouble());
    } else {
        std::fprintf(stderr, "unsupported fixture-caller D-Bus method\n");
        return 2;
    }
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        std::fprintf(stderr, "fixture caller could not connect to the system bus\n");
        return 1;
    }
    QDBusMessage request = QDBusMessage::createMethodCall(serviceName, objectPath, interfaceName, method);
    request.setArguments(arguments);
    const QDBusMessage reply = bus.call(request, QDBus::Block, 30000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        std::fprintf(stderr, "fixture caller D-Bus call failed: %s %s\n",
                     qPrintable(reply.errorName()), qPrintable(reply.errorMessage()));
        return 1;
    }
    if (method == QStringLiteral("LaunchInstance")) {
        std::printf("%lld\n", static_cast<long long>(reply.arguments().first().toLongLong()));
    } else {
        std::printf("%s\n", reply.arguments().first().toBool() ? "true" : "false");
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--fixture-dbus-call") == 0) {
        return runFixtureCallerLaunch(argc, argv);
    }
    QCoreApplication application(argc, argv);
    InstanceBootstrapIntegrationTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_instance_bootstrap_integration.moc"
