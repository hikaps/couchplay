// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include "SteamShortcutManager.h"

#include "SessionManager.h"
#include "SessionRunner.h"
#include "SteamConfigManager.h"
#include "SteamShortcutsVdf.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QPair>
#include <QProcess>
#include <QPointer>
#include <QTimer>
#include <QVariantMap>

#include <algorithm>
#include <memory>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

constexpr int HostTerminationGraceMs = 1000;
constexpr char HostSupervisorScript[] = R"SUPERVISOR(
set -u
child=
termination_requested=0
terminate_host_child() {
    if [[ -z "$child" ]]; then
        termination_requested=1
        return
    fi
    trap '' TERM HUP INT QUIT
    kill -TERM -- "-$child" 2>/dev/null || kill -TERM "$child" 2>/dev/null || true
    for ((attempt = 0; attempt < 15; ++attempt)); do
        kill -0 -- "-$child" 2>/dev/null || break
        sleep 0.05
    done
    kill -KILL -- "-$child" 2>/dev/null || kill -KILL "$child" 2>/dev/null || true
    wait "$child" 2>/dev/null || true
    exit 143
}
# Flatpak HostCommand WATCH_BUS sends SIGINT to the host command process group.
trap terminate_host_child TERM HUP INT QUIT
setsid /bin/bash -c "$1" couchplay-steam-host "${@:2}" &
child=$!
if ((termination_requested)); then
    terminate_host_child
fi
wait "$child"
status=$?
exit "$status"
)SUPERVISOR";

QString shellQuote(const QString &value)
{
    QString quoted(QStringLiteral("'"));
    for (const QChar character : value) {
        if (character == QLatin1Char('\'')) {
            quoted += QStringLiteral("'\\''");
        } else {
            quoted += character;
        }
    }
    quoted += QLatin1Char('\'');
    return quoted;
}

QString vdfQuotePath(const QString &path)
{
    QString value = path;
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QStringLiteral("\"") + value + QStringLiteral("\"");
}

QString markerFor(const QString &path)
{
    const QByteArray hash = QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QStringLiteral("couchplay://profile/") + QString::fromLatin1(hash);
}

struct ProcessIdentity {
    qint64 processGroupId = 0;
    qint64 startTime = 0;
};

ProcessIdentity processIdentity(pid_t processId)
{
#ifdef Q_OS_LINUX
    if (processId <= 0) {
        return {};
    }
    QFile statFile(QStringLiteral("/proc/%1/stat").arg(processId));
    if (!statFile.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray stat = statFile.readAll();
    const qsizetype commandEnd = stat.lastIndexOf(')');
    if (commandEnd < 0) {
        return {};
    }
    const QList<QByteArray> fields = stat.mid(commandEnd + 2).simplified().split(' ');
    if (fields.size() <= 19) {
        return {};
    }
    bool groupOk = false;
    bool startOk = false;
    const qint64 groupId = fields.at(2).toLongLong(&groupOk);
    const qint64 startTime = fields.at(19).toLongLong(&startOk);
    return groupOk && startOk ? ProcessIdentity{groupId, startTime} : ProcessIdentity{};
#else
    Q_UNUSED(processId);
    return {};
#endif
}

qint64 processStartTime(pid_t processId)
{
    return processIdentity(processId).startTime;
}

QList<QPair<qint64, qint64>> processGroupWitnesses(pid_t groupId)
{
    QList<QPair<qint64, qint64>> witnesses;
#ifdef Q_OS_LINUX
    if (groupId > 0) {
        QDirIterator processes(QStringLiteral("/proc"), QDir::Dirs | QDir::NoDotAndDotDot);
        while (processes.hasNext()) {
            processes.next();
            bool ok = false;
            const qint64 pid = processes.fileName().toLongLong(&ok);
            if (!ok || pid == groupId) {
                continue;
            }
            const ProcessIdentity identity = processIdentity(static_cast<pid_t>(pid));
            if (identity.processGroupId == groupId && identity.startTime > 0) {
                witnesses.append({pid, identity.startTime});
            }
        }
    }
#else
    Q_UNUSED(groupId);
#endif
    return witnesses;
}

bool processGroupIdentityMatches(qint64 groupId,
                                 qint64 leaderStartTime,
                                 bool termSignalled,
                                 const QList<QPair<qint64, qint64>> &witnesses)
{
    if (leaderStartTime <= 0) {
#ifdef Q_OS_LINUX
        return false;
#else
        return true;
#endif
    }
    const ProcessIdentity leader = processIdentity(static_cast<pid_t>(groupId));
    if (leader.startTime > 0) {
        return leader.startTime == leaderStartTime;
    }
    if (!termSignalled) {
        return false;
    }
    for (const auto &[pid, startTime] : witnesses) {
        const ProcessIdentity witness = processIdentity(static_cast<pid_t>(pid));
        if (witness.processGroupId == groupId && witness.startTime == startTime) {
            return true;
        }
    }
    return false;
}

} // namespace

SteamShortcutManager::SteamShortcutManager(QObject *parent)
    : QObject(parent)
{
    m_status = QStringLiteral("Ready");
}

SteamShortcutManager::~SteamShortcutManager()
{
    if (m_timeout) {
        m_timeout->stop();
    }
    QProcess *process = m_process;
    if (process) {
        QObject::disconnect(process, nullptr, this, nullptr);
    }

    QList<HostProcessGroup> groups = m_pendingHostProcessGroups;
    if (m_hostProcessGroupId > 0) {
        HostProcessGroup active;
        active.generation = m_hostProcessGeneration;
        active.processGroupId = m_hostProcessGroupId;
        active.leaderStartTime = m_hostProcessGroupStartTime;
        active.process = process;
        groups.append(active);
    } else if (process && process->processId() > 0) {
        HostProcessGroup fallback;
        fallback.generation = m_hostProcessGeneration;
        fallback.processGroupId = process->processId();
        fallback.leaderStartTime = processStartTime(process->processId());
        fallback.process = process;
        groups.append(fallback);
    }

    QList<HostProcessGroup> signalledGroups;
    for (const HostProcessGroup &group : groups) {
        const bool identityMatches = processGroupIdentityMatches(group.processGroupId,
                                                                 group.leaderStartTime,
                                                                 group.termSignalled,
                                                                 group.witnesses);
        const bool groupSignalled = group.processGroupId > 0 && identityMatches
            && ::kill(-static_cast<pid_t>(group.processGroupId), SIGTERM) == 0;
        if (groupSignalled) {
            signalledGroups.append(group);
        } else if (group.process && group.process->state() != QProcess::NotRunning) {
            group.process->terminate();
        }
    }

    for (const HostProcessGroup &group : groups) {
        QProcess *groupProcess = group.process.data();
        if (groupProcess && groupProcess->state() != QProcess::NotRunning) {
            if (!groupProcess->waitForFinished(HostTerminationGraceMs)) {
                const bool identityMatches = processGroupIdentityMatches(group.processGroupId,
                                                                         group.leaderStartTime,
                                                                         group.termSignalled,
                                                                         group.witnesses);
                if (group.processGroupId > 0 && identityMatches) {
                    ::kill(-static_cast<pid_t>(group.processGroupId), SIGKILL);
                }
                groupProcess->kill();
                groupProcess->waitForFinished(HostTerminationGraceMs);
            }
        } else if (std::any_of(signalledGroups.cbegin(), signalledGroups.cend(),
                               [&group](const HostProcessGroup &signalled) {
            return signalled.generation == group.generation
                && signalled.processGroupId == group.processGroupId;
        })) {
            constexpr useconds_t PollIntervalUs = 50'000;
            for (int elapsedMs = 0; elapsedMs < HostTerminationGraceMs; elapsedMs += 50) {
                if (::kill(-static_cast<pid_t>(group.processGroupId), 0) != 0) {
                    break;
                }
                ::usleep(PollIntervalUs);
            }
            const bool identityMatches = processGroupIdentityMatches(group.processGroupId,
                                                                     group.leaderStartTime,
                                                                     group.termSignalled,
                                                                     group.witnesses);
            if (identityMatches) {
                ::kill(-static_cast<pid_t>(group.processGroupId), SIGKILL);
            }
        }
    }
    m_pendingHostProcessGroups.clear();
    m_hostProcessGroupId = 0;
    m_hostProcessGroupStartTime = 0;
    m_process = nullptr;
}

QVariantList SteamShortcutManager::accounts() const
{
    QVariantList result;
    for (const Account &account : m_accounts) {
        result.append(QVariantMap{{QStringLiteral("root"), account.root},
                                  {QStringLiteral("accountId"), account.accountId},
                                  {QStringLiteral("kind"), account.kind},
                                  {QStringLiteral("label"), account.label},
                                  {QStringLiteral("running"), account.running}});
    }
    return result;
}

void SteamShortcutManager::setSessionManager(SessionManager *manager)
{
    if (m_sessionManager == manager) {
        return;
    }
    m_sessionManager = manager;
    Q_EMIT sessionManagerChanged();
}

void SteamShortcutManager::setSessionRunner(SessionRunner *runner)
{
    if (m_sessionRunner == runner) {
        return;
    }
    m_sessionRunner = runner;
    Q_EMIT sessionRunnerChanged();
}

void SteamShortcutManager::setBusy(bool busy)
{
    if (m_busy == busy) {
        return;
    }
    m_busy = busy;
    Q_EMIT busyChanged();
    Q_EMIT cancellableChanged();
}
bool SteamShortcutManager::cancellable() const
{
    return !m_busy || m_phase == Phase::Probing || m_phase == Phase::Prepared || m_phase == Phase::ClosingSteam;
}

void SteamShortcutManager::setGameMode(bool gameMode)
{
    if (m_gameMode == gameMode) {
        return;
    }
    m_gameMode = gameMode;
    Q_EMIT gameModeChanged();
}

void SteamShortcutManager::setStatus(const QString &status)
{
    if (m_status == status) {
        return;
    }
    m_status = status;
    Q_EMIT statusChanged();
}

void SteamShortcutManager::setPhase(Phase phase)
{
    if (m_phase == phase) {
        return;
    }
    m_phase = phase;
    Q_EMIT cancellableChanged();
}

QString SteamShortcutManager::profileName() const
{
    return m_selectedProfile;
}

QString SteamShortcutManager::profileFilePath() const
{
    return m_profilePath;
}

QString SteamShortcutManager::profileIdentity() const
{
    return markerFor(m_profilePath).mid(QStringLiteral("couchplay://profile/").size());
}

void SteamShortcutManager::detachHostProcessGroup(quint64 generation,
                                                   qint64 processGroupId,
                                                   QProcess *process)
{
    if (processGroupId <= 0) {
        return;
    }

    HostProcessGroup group;
    group.generation = generation;
    group.processGroupId = processGroupId;
    group.leaderStartTime = m_hostProcessGroupStartTime;
    if (group.leaderStartTime == 0) {
        group.leaderStartTime = processStartTime(static_cast<pid_t>(processGroupId));
    }
    group.process = process;
    if (m_hostProcessGeneration == generation && m_hostProcessGroupId == processGroupId
        && m_hostProcessGroupStartTime > 0) {
        group.leaderStartTime = m_hostProcessGroupStartTime;
    }

    for (HostProcessGroup &pending : m_pendingHostProcessGroups) {
        if (pending.generation == generation && pending.processGroupId == processGroupId) {
            if (pending.leaderStartTime == 0) {
                pending.leaderStartTime = group.leaderStartTime;
            }
            if (!pending.process) {
                pending.process = process;
            }
            if (m_hostProcessGeneration == generation && m_hostProcessGroupId == processGroupId) {
                m_hostProcessGroupId = 0;
                m_hostProcessGroupStartTime = 0;
            }
            return;
        }
    }

    m_pendingHostProcessGroups.append(group);
    if (m_hostProcessGeneration == generation && m_hostProcessGroupId == processGroupId) {
        m_hostProcessGroupId = 0;
        m_hostProcessGroupStartTime = 0;
    }
}

void SteamShortcutManager::releaseHostProcessGroup(quint64 generation, qint64 processGroupId)
{
    if (m_hostProcessGeneration == generation && m_hostProcessGroupId == processGroupId) {
        m_hostProcessGroupId = 0;
        m_hostProcessGroupStartTime = 0;
    }
    for (int index = m_pendingHostProcessGroups.size() - 1; index >= 0; --index) {
        if (m_pendingHostProcessGroups.at(index).generation == generation
            && m_pendingHostProcessGroups.at(index).processGroupId == processGroupId) {
            m_pendingHostProcessGroups.removeAt(index);
        }
    }
}

bool SteamShortcutManager::findHostProcessGroup(quint64 generation,
                                                qint64 processGroupId,
                                                HostProcessGroup *group) const
{
    if (m_hostProcessGeneration == generation && m_hostProcessGroupId == processGroupId
        && processGroupId > 0) {
        if (group) {
            group->generation = generation;
            group->processGroupId = processGroupId;
            group->leaderStartTime = m_hostProcessGroupStartTime;
            group->process = m_process;
        }
        return true;
    }
    for (const HostProcessGroup &pending : m_pendingHostProcessGroups) {
        if (pending.generation == generation && pending.processGroupId == processGroupId) {
            if (group) {
                *group = pending;
            }
            return true;
        }
    }
    return false;
}

bool SteamShortcutManager::hostProcessGroupIdentityMatches(quint64 generation, qint64 processGroupId) const
{
    HostProcessGroup group;
    if (!findHostProcessGroup(generation, processGroupId, &group)) {
        return false;
    }
    return processGroupIdentityMatches(processGroupId,
                                       group.leaderStartTime,
                                       group.termSignalled,
                                       group.witnesses);
}

bool SteamShortcutManager::signalOwnedHostProcessGroup(quint64 generation,
                                                        qint64 processGroupId,
                                                        int signal)
{
    if (processGroupId <= 0 || !findHostProcessGroup(generation, processGroupId)
        || !hostProcessGroupIdentityMatches(generation, processGroupId)) {
        if (processGroupId > 0 && findHostProcessGroup(generation, processGroupId)) {
            releaseHostProcessGroup(generation, processGroupId);
        }
        return false;
    }
    return ::kill(-static_cast<pid_t>(processGroupId), signal) == 0;
}

void SteamShortcutManager::scheduleHostProcessGroupEscalation(quint64 generation,
                                                               qint64 processGroupId,
                                                               QProcess *process,
                                                               bool termSignalled,
                                                               const QList<QPair<qint64, qint64>> &witnesses)
{
    if (termSignalled) {
        for (HostProcessGroup &group : m_pendingHostProcessGroups) {
            if (group.generation == generation && group.processGroupId == processGroupId) {
                group.termSignalled = true;
                group.witnesses = witnesses;
                break;
            }
        }
    }
    const QPointer<QProcess> guardedProcess(process);
    QTimer::singleShot(HostTerminationGraceMs, this,
                       [this, generation, processGroupId, guardedProcess] {
        if (processGroupId > 0) {
            const bool owned = findHostProcessGroup(generation, processGroupId)
                && hostProcessGroupIdentityMatches(generation, processGroupId);
            const bool groupSignalled = owned && signalOwnedHostProcessGroup(generation, processGroupId, SIGKILL);
            if (!groupSignalled && owned && guardedProcess && guardedProcess->state() != QProcess::NotRunning) {
                guardedProcess->kill();
            }
            releaseHostProcessGroup(generation, processGroupId);
        } else if (guardedProcess && guardedProcess->state() != QProcess::NotRunning) {
            guardedProcess->kill();
        }
    });
}
void SteamShortcutManager::fail(const QString &message)
{
    stopSteamPoll();
    m_shutdownDeadline = QDeadlineTimer();
    if (m_cancelled) {
        finishCancelled();
        return;
    }
    if (!m_shutdownConfirmed || !m_wasRunning || m_reopenAttempted || m_root.isEmpty()) {
        setPhase(Phase::Idle);
        setBusy(false);
        setStatus(message);
        Q_EMIT errorOccurred(message);
        return;
    }

    m_reopenAttempted = true;
    setPhase(Phase::ReopeningSteam);
    setStatus(QStringLiteral("Reopening Steam"));
    runHost(QStringLiteral("start"), {m_root}, QByteArray(),
            [this, message](int exitCode, const QByteArray &, const QString &recoveryError) {
        m_shutdownConfirmed = false;
        setPhase(Phase::Idle);
        setBusy(false);
        QString finalMessage = message;
        if (exitCode != 0) {
            const QString reason = recoveryError.isEmpty()
                ? QStringLiteral("Steam could not be reopened")
                : QStringLiteral("Steam could not be reopened: %1").arg(recoveryError);
            finalMessage += QStringLiteral("; ") + reason;
        }
        setStatus(finalMessage);
        Q_EMIT errorOccurred(finalMessage);
    });
}

void SteamShortcutManager::runHost(const QString &operation,
                                   const QStringList &arguments,
                                   const QByteArray &input,
                                   std::function<void(int, const QByteArray &, const QString &)> callback,
                                   int timeoutMs)
{
    if (m_process) {
        fail(QStringLiteral("Another Steam operation is already running"));
        return;
    }

    QFile scriptFile(QStringLiteral(":/couchplay/steam-host.sh"));
    if (!scriptFile.open(QIODevice::ReadOnly)) {
        callback(-1, QByteArray(), QStringLiteral("Steam host transport is unavailable"));
        return;
    }
    const QString script = QString::fromUtf8(scriptFile.readAll());
    auto *process = new QProcess(this);
    const quint64 generation = ++m_hostOperationGeneration;
    m_process = process;
    m_hostProcessGeneration = generation;
    m_hostProcessGroupId = 0;
    m_hostProcessGroupStartTime = 0;
    m_hostCallback = std::move(callback);
    auto completed = std::make_shared<bool>(false);
    auto timedOut = std::make_shared<bool>(false);
    auto finish = [this, process, generation, completed, timedOut](int exitCode,
                                                                    const QByteArray &output,
                                                                    const QString &error) {
        if (*completed) {
            return;
        }
        *completed = true;
        if (m_timeout) {
            m_timeout->stop();
        }
        const qint64 processGroupId = m_hostProcessGeneration == generation ? m_hostProcessGroupId : 0;
        if (m_process == process) {
            m_process = nullptr;
        }
        const bool retainGroup = *timedOut
            || (m_cancelled && m_phase != Phase::ClosingSteam && m_phase != Phase::ReopeningSteam);
        if (processGroupId > 0) {
            if (retainGroup) {
                detachHostProcessGroup(generation, processGroupId, process);
            } else {
                releaseHostProcessGroup(generation, processGroupId);
            }
        }
        auto callback = std::move(m_hostCallback);
        process->deleteLater();
        if (m_cancelled && m_phase != Phase::ClosingSteam && m_phase != Phase::ReopeningSteam) {
            finishCancelled();
            return;
        }
        if (callback) {
            if (*timedOut) {
                callback(-1, output, QStringLiteral("Steam host operation timed out"));
            } else {
                callback(exitCode, output, error);
            }
        }
    };

    connect(process, &QProcess::started, this, [this, process, generation, timedOut] {
        m_hostProcessGeneration = generation;
        m_hostProcessGroupId = process->processId();
        m_hostProcessGroupStartTime = processStartTime(process->processId());
        if (!*timedOut && (!m_cancelled || m_phase == Phase::ClosingSteam || m_phase == Phase::ReopeningSteam)) {
            return;
        }

        const qint64 processGroupId = m_hostProcessGroupId;
        if (processGroupId > 0) {
            detachHostProcessGroup(generation, processGroupId, process);
        }
        const auto witnesses = processGroupWitnesses(static_cast<pid_t>(processGroupId));
        const bool termSignalled = processGroupId > 0
            && signalOwnedHostProcessGroup(generation, processGroupId, SIGTERM);
        if (!termSignalled) {
            process->terminate();
        }
        scheduleHostProcessGroupEscalation(generation, processGroupId, process,
                                           termSignalled, witnesses);
    });
    connect(process, &QProcess::finished, this, [finish, process](int exitCode, QProcess::ExitStatus status) {
        finish(status == QProcess::NormalExit ? exitCode : -1,
               process->readAllStandardOutput(),
               QString::fromLocal8Bit(process->readAllStandardError()));
    });
    connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            finish(-1, QByteArray(), QStringLiteral("Steam host transport failed to start"));
        }
    });

    if (!m_timeout) {
        m_timeout = new QTimer(this);
        m_timeout->setSingleShot(true);
    }
    m_timeout->disconnect();
    connect(m_timeout, &QTimer::timeout, this, [this, process, generation, timedOut] {
        *timedOut = true;
        qint64 processGroupId = m_hostProcessGeneration == generation ? m_hostProcessGroupId : 0;
        if (processGroupId <= 0) {
            processGroupId = process->processId();
        }
        if (processGroupId > 0) {
            if (m_hostProcessGeneration == generation) {
                m_hostProcessGroupId = processGroupId;
            }
            detachHostProcessGroup(generation, processGroupId, process);
        }
        const auto witnesses = processGroupWitnesses(static_cast<pid_t>(processGroupId));
        const bool termSignalled = processGroupId > 0
            && signalOwnedHostProcessGroup(generation, processGroupId, SIGTERM);
        if (!termSignalled) {
            process->terminate();
        }
        scheduleHostProcessGroupEscalation(generation, processGroupId, process,
                                           termSignalled, witnesses);
    });

    const QString hostSupervisor = QString::fromLatin1(HostSupervisorScript);
    QStringList processArguments;
    if (qEnvironmentVariableIsSet("FLATPAK_ID")) {
        process->setProgram(QStringLiteral("/usr/bin/flatpak-spawn"));
        processArguments = {QStringLiteral("--host"), QStringLiteral("--watch-bus"), QStringLiteral("/bin/bash"),
                            QStringLiteral("-c"), hostSupervisor, QStringLiteral("couchplay-supervisor"), script, operation};
    } else {
        process->setProgram(QStringLiteral("/bin/bash"));
        processArguments = {QStringLiteral("-c"), hostSupervisor, QStringLiteral("couchplay-supervisor"), script, operation};
    }
    processArguments.append(arguments);
    process->setArguments(processArguments);
    process->setProcessChannelMode(QProcess::SeparateChannels);
    process->setChildProcessModifier([] {
        if (::setpgid(0, 0) != 0) {
            ::_exit(127);
        }
    });
    const int defaultTimeoutMs = operation == QStringLiteral("start") ? 15000 : 10000;
    m_timeout->start(timeoutMs > 0 ? timeoutMs : defaultTimeoutMs);
    process->start();
    if (!input.isEmpty()) {
        process->write(input);
    }
    process->closeWriteChannel();
}

bool SteamShortcutManager::prepare(const QString &requestedProfile)
{
    if (m_busy) {
        return false;
    }
    if (!m_sessionManager || !SessionManager::isValidProfileName(requestedProfile)) {
        fail(QStringLiteral("Invalid saved profile"));
        return false;
    }
    if (m_sessionRunner && m_sessionRunner->isActive()) {
        fail(QStringLiteral("Stop the active CouchPlay session before adding a profile to Steam"));
        return false;
    }

    const auto profiles = m_sessionManager->savedProfiles();
    const auto profile = std::find_if(profiles.cbegin(), profiles.cend(), [&](const SessionProfile &candidate) {
        return candidate.name == requestedProfile;
    });
    if (profile == profiles.cend() || profile->filePath.isEmpty()) {
        fail(QStringLiteral("Saved profile not found"));
        return false;
    }

    stopSteamPoll();
    m_cancelled = false;
    m_shutdownConfirmed = false;
    m_shutdownDeadline = QDeadlineTimer();
    m_selectedProfile = profile->name;
    m_profilePath = profile->filePath;
    m_identity = profileIdentity();
    m_accounts.clear();
    setPhase(Phase::Probing);
    setBusy(true);
    setStatus(QStringLiteral("Detecting Steam installations"));
    runHost(QStringLiteral("probe"), {}, QByteArray(), [this](int exitCode, const QByteArray &output, const QString &error) {
        if (exitCode != 0) {
            fail(error.isEmpty() ? QStringLiteral("Steam was not detected") : error);
            return;
        }
        parseProbe(output);
        setBusy(false);
        setPhase(Phase::Prepared);
        setStatus(m_gameMode ? QStringLiteral("Switch to Desktop Mode to add this profile to Steam")
                             : QStringLiteral("Select a Steam account"));
        Q_EMIT prepared();
    });
    return true;
}

void SteamShortcutManager::parseProbe(const QByteArray &output)
{
    const QList<QByteArray> fields = output.split('\0');
    QHash<QString, QString> kinds;
    QHash<QString, bool> running;
    m_accounts.clear();
    for (qsizetype i = 0; i < fields.size();) {
        const QByteArray type = fields.at(i++);
        if (type == "H") {
            i += 3;
        } else if (type == "G") {
            if (i < fields.size()) {
                setGameMode(fields.at(i++) == "1");
            }
        } else if (type == "I" && i + 3 < fields.size()) {
            const QString root = QString::fromUtf8(fields.at(i++));
            kinds[root] = QString::fromUtf8(fields.at(i++));
            ++i; // host command path
            running[root] = fields.at(i++) == "1";
        } else if (type == "A" && i + 1 < fields.size()) {
            Account account;
            account.root = QString::fromUtf8(fields.at(i++));
            account.accountId = QString::fromUtf8(fields.at(i++));
            account.kind = kinds.value(account.root);
            account.running = running.value(account.root);
            account.label = QStringLiteral("%1 · %2").arg(account.kind.isEmpty() ? QStringLiteral("Steam") : account.kind,
                                                        account.accountId);
            m_accounts.append(account);
        } else {
            break;
        }
    }
    std::sort(m_accounts.begin(), m_accounts.end(), [](const Account &left, const Account &right) {
        return left.root == right.root ? left.accountId < right.accountId : left.root < right.root;
    });
    Q_EMIT accountsChanged();
}
bool SteamShortcutManager::selectedAccountRunning() const
{
    return std::any_of(m_accounts.cbegin(), m_accounts.cend(), [&](const Account &account) {
        return account.root == m_root && account.accountId == m_accountId && account.running;
    });
}

void SteamShortcutManager::addToSteam(int accountIndex, bool allowRestart)
{
    if (m_phase != Phase::Prepared || m_busy || accountIndex < 0 || accountIndex >= m_accounts.size()) {
        return;
    }
    if (m_gameMode) {
        fail(QStringLiteral("Switch to Desktop Mode to add this profile to Steam"));
        return;
    }
    if (m_sessionRunner && m_sessionRunner->isActive()) {
        fail(QStringLiteral("Stop the active CouchPlay session first"));
        return;
    }

    const auto profiles = m_sessionManager ? m_sessionManager->savedProfiles() : QList<SessionProfile>();
    const auto profile = std::find_if(profiles.cbegin(), profiles.cend(), [&](const SessionProfile &candidate) {
        return candidate.name == m_selectedProfile && candidate.filePath == m_profilePath;
    });
    if (profile == profiles.cend()) {
        fail(QStringLiteral("Saved profile changed or was deleted"));
        return;
    }

    const Account &account = m_accounts.at(accountIndex);
    m_root = account.root;
    m_accountId = account.accountId;
    m_kind = account.kind;
    m_wasRunning = account.running;
    if (m_wasRunning && !allowRestart) {
        setStatus(QStringLiteral("Steam must be closed before the shortcut can be added"));
        Q_EMIT errorOccurred(m_status);
        return;
    }

    stopSteamPoll();
    m_cancelled = false;
    m_shutdownConfirmed = false;
    m_shutdownDeadline = QDeadlineTimer();
    m_reopenAttempted = false;
    setBusy(true);
    auto beginRegistration = [this] {
        if (m_wasRunning) {
            setPhase(Phase::ClosingSteam);
            m_shutdownDeadline = QDeadlineTimer(30000, Qt::PreciseTimer);
            setStatus(QStringLiteral("Closing Steam"));
            runHost(QStringLiteral("shutdown"), {m_root}, QByteArray(), [this](int exitCode, const QByteArray &, const QString &error) {
                if (exitCode != 0 && !m_cancelled) {
                    const QString shutdownError = error.isEmpty() ? QStringLiteral("Steam could not be closed") : error;
                    if (!m_shutdownDeadline.isForever() && m_shutdownDeadline.hasExpired()) {
                        finishShutdownTimeout();
                        return;
                    }
                    const qint64 remainingMs = m_shutdownDeadline.isForever() ? 10000 : m_shutdownDeadline.remainingTime();
                    if (!m_shutdownDeadline.isForever() && remainingMs <= HostTerminationGraceMs) {
                        finishShutdownTimeout();
                        return;
                    }
                    const qint64 probeBudgetMs = m_shutdownDeadline.isForever()
                        ? 10000
                        : remainingMs - HostTerminationGraceMs;
                    const int probeTimeoutMs = static_cast<int>(std::min<qint64>(10000, probeBudgetMs));
                    runHost(QStringLiteral("probe"), {}, QByteArray(),
                            [this, shutdownError](int probeExitCode, const QByteArray &output, const QString &) {
                        if (!m_shutdownDeadline.isForever() && m_shutdownDeadline.hasExpired()) {
                            finishShutdownTimeout();
                            return;
                        }
                        if (probeExitCode == 0) {
                            parseProbe(output);
                            m_shutdownConfirmed = m_wasRunning && !selectedAccountRunning();
                        }
                        fail(shutdownError);
                    }, probeTimeoutMs);
                    return;
                }
                pollSteamStopped();
            });
        } else {
            beginWrite();
        }
    };
    if (m_kind == QStringLiteral("flatpak")) {
        setStatus(QStringLiteral("Checking Flatpak Steam permissions"));
        runHost(QStringLiteral("flatpak-steam-preflight"), {}, QByteArray(), [this, beginRegistration](int exitCode, const QByteArray &, const QString &) {
            if (exitCode != 0) {
                fail(QStringLiteral("Flatpak Steam must allow host launching before this profile can be added"));
                return;
            }
            beginRegistration();
        });
    } else {
        beginRegistration();
    }
}

void SteamShortcutManager::stopSteamPoll()
{
    ++m_pollGeneration;
    if (!m_pollTimer) {
        return;
    }
    m_pollTimer->stop();
    m_pollTimer->deleteLater();
    m_pollTimer = nullptr;
}

void SteamShortcutManager::scheduleSteamPoll()
{
    stopSteamPoll();
    const quint64 generation = m_pollGeneration;
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    m_pollTimer = timer;
    connect(timer, &QTimer::timeout, this, [this, timer, generation] {
        if (m_pollTimer == timer) {
            m_pollTimer = nullptr;
        }
        timer->deleteLater();
        if (generation != m_pollGeneration || m_phase != Phase::ClosingSteam) {
            return;
        }
        pollSteamStopped();
    });
    timer->start(250);
}

void SteamShortcutManager::pollSteamStopped()
{
    if (m_phase != Phase::ClosingSteam || !m_busy) {
        return;
    }
    if (!m_shutdownDeadline.isForever() && m_shutdownDeadline.hasExpired()) {
        finishShutdownTimeout();
        return;
    }

    const qint64 remainingMs = m_shutdownDeadline.isForever() ? 10000 : m_shutdownDeadline.remainingTime();
    if (!m_shutdownDeadline.isForever() && remainingMs <= HostTerminationGraceMs) {
        finishShutdownTimeout();
        return;
    }
    const qint64 probeBudgetMs = m_shutdownDeadline.isForever() ? 10000 : remainingMs - HostTerminationGraceMs;
    const int probeTimeoutMs = static_cast<int>(std::min<qint64>(10000, probeBudgetMs));
    runHost(QStringLiteral("probe"), {}, QByteArray(),
            [this](int exitCode, const QByteArray &output, const QString &error) {
        if (!m_shutdownDeadline.isForever() && m_shutdownDeadline.hasExpired()) {
            finishShutdownTimeout();
            return;
        }
        if (exitCode != 0) {
            if (m_cancelled) {
                scheduleSteamPoll();
            } else {
                fail(error.isEmpty() ? QStringLiteral("Steam state could not be checked") : error);
            }
            return;
        }
        parseProbe(output);
        if (selectedAccountRunning()) {
            scheduleSteamPoll();
            return;
        }
        m_shutdownDeadline = QDeadlineTimer();
        m_shutdownConfirmed = m_wasRunning;
        if (m_cancelled) {
            finishCancelled();
        } else {
            beginWrite();
        }
    }, probeTimeoutMs);
}

void SteamShortcutManager::beginWrite()
{
    setPhase(Phase::Writing);
    setStatus(QStringLiteral("Reading Steam shortcuts"));
    readShortcuts();
}

void SteamShortcutManager::readShortcuts()
{
    runHost(QStringLiteral("read"), {m_root, m_accountId}, QByteArray(), [this](int exitCode, const QByteArray &output, const QString &error) {
        if (exitCode == 3) {
            m_shortcuts = SteamShortcutsVdf::emptyDocument();
            m_expectedHash = QStringLiteral("missing");
        } else if (exitCode == 0) {
            m_shortcuts = output;
            m_expectedHash = QString::fromLatin1(QCryptographicHash::hash(m_shortcuts, QCryptographicHash::Sha256).toHex());
        } else {
            fail(error.isEmpty() ? QStringLiteral("Steam shortcuts could not be read") : error);
            return;
        }
        exportGameMode();
    });
}

void SteamShortcutManager::exportGameMode()
{
    QFile file(QStringLiteral(":/couchplay/gamemode.sh"));
    if (!file.open(QIODevice::ReadOnly)) {
        fail(QStringLiteral("Game Mode launcher resource is unavailable"));
        return;
    }
    runHost(QStringLiteral("export"), {m_identity, QStringLiteral("gamemode")}, file.readAll(),
            [this](int exitCode, const QByteArray &output, const QString &error) {
        if (exitCode != 0) {
            fail(error.isEmpty() ? QStringLiteral("Game Mode launcher could not be exported") : error);
            return;
        }
        m_gameModePath = QString::fromUtf8(output).trimmed();
        exportIcon();
    });
}

void SteamShortcutManager::exportIcon()
{
    QFile file(QStringLiteral(":/couchplay/icon.png"));
    if (!file.open(QIODevice::ReadOnly)) {
        fail(QStringLiteral("CouchPlay icon resource is unavailable"));
        return;
    }
    runHost(QStringLiteral("export"), {m_identity, QStringLiteral("icon")}, file.readAll(),
            [this](int exitCode, const QByteArray &output, const QString &error) {
        if (exitCode != 0) {
            fail(error.isEmpty() ? QStringLiteral("CouchPlay icon could not be exported") : error);
            return;
        }
        m_iconPath = QString::fromUtf8(output).trimmed();
        exportLauncher();
    });
}

QString SteamShortcutManager::launcherContents(const QString &gameModePath) const
{
    const QString route = qEnvironmentVariableIsSet("FLATPAK_ID")
        ? QStringLiteral("--couchplay-flatpak --")
        : QStringLiteral("--couchplay-native %1 --").arg(shellQuote(QCoreApplication::applicationFilePath()));
    return QStringLiteral("#!/bin/bash\nset -e\nSCRIPT_DIR=$(cd -- \"$(dirname -- \"${BASH_SOURCE[0]}\")\" && pwd)\nexec %1 %2 %3 --start --exit-after-session\n")
        .arg(shellQuote(gameModePath), route, shellQuote(QStringLiteral("--profile=") + m_selectedProfile));
}

void SteamShortcutManager::exportLauncher()
{
    runHost(QStringLiteral("export"), {m_identity, QStringLiteral("launcher")}, launcherContents(m_gameModePath).toUtf8(),
            [this](int exitCode, const QByteArray &output, const QString &error) {
        if (exitCode != 0) {
            fail(error.isEmpty() ? QStringLiteral("Profile launcher could not be exported") : error);
            return;
        }
        m_launcherPath = QString::fromUtf8(output).trimmed();
        commitShortcut();
    });
}

void SteamShortcutManager::commitShortcut()
{
    SteamShortcut shortcut;
    shortcut.appName = QStringLiteral("CouchPlay - ") + m_selectedProfile;
    shortcut.exe = m_kind == QStringLiteral("flatpak") ? vdfQuotePath(QStringLiteral("/usr/bin/flatpak-spawn"))
                                                        : vdfQuotePath(m_launcherPath);
    shortcut.startDir = vdfQuotePath(QFileInfo(m_launcherPath).absolutePath());
    shortcut.icon = m_iconPath;
    shortcut.shortcutPath = markerFor(m_profilePath);
    shortcut.launchOptions = m_kind == QStringLiteral("flatpak")
        ? QStringLiteral("--host ") + vdfQuotePath(m_launcherPath)
        : QString();
    shortcut.tags = {QStringLiteral("CouchPlay")};

    QString error;
    if (!SteamShortcutsVdf::upsert(m_shortcuts, shortcut, &m_newShortcuts, &error)) {
        fail(QStringLiteral("Steam shortcuts are invalid: %1").arg(error));
        return;
    }
    setStatus(QStringLiteral("Saving Steam shortcut"));
    runHost(QStringLiteral("commit"), {m_root, m_accountId, m_expectedHash}, m_newShortcuts,
            [this](int exitCode, const QByteArray &, const QString &errorMessage) {
        if (exitCode != 0) {
            fail(errorMessage.isEmpty() ? QStringLiteral("Steam shortcut could not be saved") : errorMessage);
            return;
        }
        reopenSteam(true, m_wasRunning);
    });
}

void SteamShortcutManager::reopenSteam(bool success, bool updated)
{
    if (!success) {
        finishRegistration(false, false);
        return;
    }
    if (!m_wasRunning || m_reopenAttempted) {
        finishRegistration(updated, false);
        return;
    }
    m_reopenAttempted = true;
    setPhase(Phase::ReopeningSteam);
    setStatus(QStringLiteral("Reopening Steam"));
    runHost(QStringLiteral("start"), {m_root}, QByteArray(), [this, updated](int exitCode, const QByteArray &, const QString &) {
        finishRegistration(updated, exitCode == 0);
    });
}

void SteamShortcutManager::finishRegistration(bool updated, bool steamReopened)
{
    stopSteamPoll();
    m_shutdownConfirmed = false;
    m_shutdownDeadline = QDeadlineTimer();
    m_cancelled = false;
    setPhase(Phase::Idle);
    setBusy(false);
    setStatus(steamReopened || !m_wasRunning ? QStringLiteral("Steam shortcut added")
                                               : QStringLiteral("Steam shortcut saved; Steam could not be reopened"));
    Q_EMIT registrationFinished(m_selectedProfile, updated, steamReopened);
    if (m_wasRunning && !steamReopened) {
        Q_EMIT errorOccurred(m_status);
    }
}

void SteamShortcutManager::finishCancelled()
{
    stopSteamPoll();
    m_shutdownDeadline = QDeadlineTimer();
    if (m_shutdownConfirmed && m_wasRunning && !m_reopenAttempted && !m_root.isEmpty()) {
        m_reopenAttempted = true;
        setPhase(Phase::ReopeningSteam);
        setStatus(QStringLiteral("Reopening Steam after cancellation"));
        runHost(QStringLiteral("start"), {m_root}, QByteArray(), [this](int exitCode, const QByteArray &, const QString &) {
            m_shutdownConfirmed = false;
            m_cancelled = false;
            setPhase(Phase::Idle);
            setBusy(false);
            setStatus(exitCode == 0 ? QStringLiteral("Steam registration cancelled")
                                    : QStringLiteral("Steam could not be reopened after cancellation"));
            if (exitCode != 0) {
                Q_EMIT errorOccurred(m_status);
            }
        });
        return;
    }

    m_shutdownConfirmed = false;
    m_cancelled = false;
    setPhase(Phase::Idle);
    setBusy(false);
    setStatus(QStringLiteral("Steam registration cancelled"));
}

void SteamShortcutManager::finishShutdownTimeout()
{
    stopSteamPoll();
    m_shutdownDeadline = QDeadlineTimer();
    m_shutdownConfirmed = false;
    if (m_cancelled) {
        m_cancelled = false;
        setPhase(Phase::Idle);
        setBusy(false);
        setStatus(QStringLiteral("Cancellation timed out; Steam may still be running"));
        Q_EMIT errorOccurred(m_status);
        return;
    }
    fail(QStringLiteral("Steam did not close within 30 seconds"));
}
void SteamShortcutManager::cancel()
{
    if (!m_busy || !cancellable()) {
        return;
    }
    m_cancelled = true;
    setStatus(QStringLiteral("Cancelling Steam registration"));
    if (m_phase == Phase::ClosingSteam) {
        if (!m_process) {
            stopSteamPoll();
            pollSteamStopped();
        }
        return;
    }
    if (!m_process) {
        finishCancelled();
        return;
    }

    const quint64 generation = m_hostProcessGeneration;
    qint64 processGroupId = m_hostProcessGroupId;
    if (processGroupId <= 0) {
        processGroupId = m_process->processId();
    }
    if (processGroupId > 0) {
        detachHostProcessGroup(generation, processGroupId, m_process);
    }
    const auto witnesses = processGroupWitnesses(static_cast<pid_t>(processGroupId));
    const bool termSignalled = processGroupId > 0
        && signalOwnedHostProcessGroup(generation, processGroupId, SIGTERM);
    if (!termSignalled) {
        m_process->terminate();
    }
    scheduleHostProcessGroupEscalation(generation, processGroupId, m_process,
                                       termSignalled, witnesses);
}

void SteamShortcutManager::openSteam(int accountIndex)
{
    if (m_busy || accountIndex < 0 || accountIndex >= m_accounts.size() || m_gameMode) {
        return;
    }
    const Account &account = m_accounts.at(accountIndex);
    setBusy(true);
    setStatus(QStringLiteral("Opening Steam"));
    runHost(QStringLiteral("start"), {account.root}, QByteArray(), [this](int exitCode, const QByteArray &, const QString &error) {
        setPhase(Phase::Idle);
        setBusy(false);
        if (exitCode != 0) {
            fail(error.isEmpty() ? QStringLiteral("Steam could not be opened") : error);
        } else {
            setStatus(QStringLiteral("Steam started"));
        }
    });
}
