// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include "SteamShortcutManager.h"

#include "SessionManager.h"
#include "SessionRunner.h"
#include "SteamConfigManager.h"
#include "SteamShortcutsVdf.h"

#include <QCoreApplication>
#include <QHash>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTimer>
#include <QVariantMap>

#include <algorithm>
#include <memory>

namespace {

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

} // namespace

SteamShortcutManager::SteamShortcutManager(QObject *parent)
    : QObject(parent)
{
    m_status = QStringLiteral("Ready");
}

SteamShortcutManager::~SteamShortcutManager()
{
    if (m_process) {
        m_process->kill();
    }
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

void SteamShortcutManager::fail(const QString &message)
{
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
    runHost(QStringLiteral("start"), {m_root}, QByteArray(), [this, message](int, const QByteArray &, const QString &) {
        m_shutdownConfirmed = false;
        setPhase(Phase::Idle);
        setBusy(false);
        setStatus(message);
        Q_EMIT errorOccurred(message);
    });
}

void SteamShortcutManager::runHost(const QString &operation,
                                   const QStringList &arguments,
                                   const QByteArray &input,
                                   std::function<void(int, const QByteArray &, const QString &)> callback)
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
    m_process = process;
    m_hostCallback = std::move(callback);
    auto completed = std::make_shared<bool>(false);
    auto finish = [this, process, completed](int exitCode, const QByteArray &output, const QString &error) {
        if (*completed) {
            return;
        }
        *completed = true;
        if (m_timeout) {
            m_timeout->stop();
        }
        if (m_process == process) {
            m_process = nullptr;
        }
        auto callback = std::move(m_hostCallback);
        process->deleteLater();
        if (m_cancelled && m_phase != Phase::ClosingSteam && m_phase != Phase::ReopeningSteam) {
            finishCancelled();
            return;
        }
        if (callback) {
            callback(exitCode, output, error);
        }
    };

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
    connect(m_timeout, &QTimer::timeout, this, [process, finish] {
        process->kill();
        finish(-1, QByteArray(), QStringLiteral("Steam host operation timed out"));
    });

    QStringList processArguments;
    if (qEnvironmentVariableIsSet("FLATPAK_ID")) {
        process->setProgram(QStringLiteral("/usr/bin/flatpak-spawn"));
        processArguments = {QStringLiteral("--host"), QStringLiteral("--watch-bus"), QStringLiteral("/bin/bash"),
                            QStringLiteral("-c"), script, QStringLiteral("couchplay-steam-host"), operation};
    } else {
        process->setProgram(QStringLiteral("/bin/bash"));
        processArguments = {QStringLiteral("-c"), script, QStringLiteral("couchplay-steam-host"), operation};
    }
    processArguments.append(arguments);
    process->setArguments(processArguments);
    process->setProcessChannelMode(QProcess::SeparateChannels);
    process->start();
    if (!input.isEmpty()) {
        process->write(input);
    }
    process->closeWriteChannel();
    m_timeout->start(10000);
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

    m_cancelled = false;
    m_shutdownConfirmed = false;
    m_pollAttempts = 0;
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

    m_cancelled = false;
    m_shutdownConfirmed = false;
    m_pollAttempts = 0;
    m_reopenAttempted = false;
    setBusy(true);
    auto beginRegistration = [this] {
        if (m_wasRunning) {
            setPhase(Phase::ClosingSteam);
            setStatus(QStringLiteral("Closing Steam"));
            runHost(QStringLiteral("shutdown"), {m_root}, QByteArray(), [this](int exitCode, const QByteArray &, const QString &error) {
                if (exitCode != 0 && !m_cancelled) {
                    const QString shutdownError = error.isEmpty() ? QStringLiteral("Steam could not be closed") : error;
                    runHost(QStringLiteral("probe"), {}, QByteArray(),
                            [this, shutdownError](int probeExitCode, const QByteArray &output, const QString &) {
                        if (probeExitCode == 0) {
                            parseProbe(output);
                            m_shutdownConfirmed = m_wasRunning && !selectedAccountRunning();
                        }
                        fail(shutdownError);
                    });
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

void SteamShortcutManager::pollSteamStopped()
{
    if (++m_pollAttempts > 120) {
        m_pollAttempts = 0;
        if (m_cancelled) {
            finishCancelled();
        } else {
            fail(QStringLiteral("Steam did not close within 30 seconds"));
        }
        return;
    }
    runHost(QStringLiteral("probe"), {}, QByteArray(), [this](int exitCode, const QByteArray &output, const QString &error) {
        if (exitCode != 0) {
            m_pollAttempts = 0;
            if (m_cancelled) {
                finishCancelled();
            } else {
                fail(error.isEmpty() ? QStringLiteral("Steam state could not be checked") : error);
            }
            return;
        }
        parseProbe(output);
        const bool running = selectedAccountRunning();
        if (running) {
            if (m_cancelled) {
                m_pollAttempts = 0;
                finishCancelled();
            } else {
                QTimer::singleShot(250, this, &SteamShortcutManager::pollSteamStopped);
            }
        } else {
            m_pollAttempts = 0;
            m_shutdownConfirmed = m_wasRunning;
            if (m_cancelled) {
                finishCancelled();
            } else {
                beginWrite();
            }
        }
    });
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
    m_shutdownConfirmed = false;
    m_pollAttempts = 0;
    m_cancelled = false;
    setPhase(Phase::Idle);
    setBusy(false);
    setStatus(steamReopened || !m_wasRunning ? QStringLiteral("Steam shortcut added")
                                               : QStringLiteral("Steam shortcut saved; Steam could not be reopened"));
    Q_EMIT registrationFinished(m_selectedProfile, updated, steamReopened);
}

void SteamShortcutManager::finishCancelled()
{
    if (m_shutdownConfirmed && m_wasRunning && !m_reopenAttempted && !m_root.isEmpty()) {
        m_reopenAttempted = true;
        setPhase(Phase::ReopeningSteam);
        setStatus(QStringLiteral("Reopening Steam after cancellation"));
        runHost(QStringLiteral("start"), {m_root}, QByteArray(), [this](int exitCode, const QByteArray &, const QString &) {
            m_shutdownConfirmed = false;
            m_pollAttempts = 0;
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
    m_pollAttempts = 0;
    m_cancelled = false;
    setPhase(Phase::Idle);
    setBusy(false);
    setStatus(QStringLiteral("Steam registration cancelled"));
}

void SteamShortcutManager::cancel()
{
    if (!m_busy || !cancellable()) {
        return;
    }
    m_cancelled = true;
    setStatus(QStringLiteral("Cancelling Steam registration"));
    if (m_phase == Phase::ClosingSteam) {
        return;
    }
    if (m_process) {
        m_process->kill();
    } else {
        finishCancelled();
    }
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
