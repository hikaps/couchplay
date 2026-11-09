// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 hikaps

#include "PairingSession.h"

#include "SunshineConfig.h"

namespace
{
constexpr int kPairingDisplayWidth = 1280;
constexpr int kPairingDisplayHeight = 720;
// Player slot used for the pairing display and stream entry; see the class comment
constexpr int kPairingInstanceIndex = 0;
}

PairingSession::PairingSession(QObject *parent)
    : QObject(parent)
    , m_streamManager(new StreamManager(this))
    , m_virtualDisplays(new VirtualDisplayManager(this))
{
    connect(m_virtualDisplays,
            &VirtualDisplayManager::ready,
            this,
            [this](int index, const QString &outputName, const QRect &) {
                onDisplayReady(index, outputName);
            });
    connect(m_virtualDisplays, &VirtualDisplayManager::failed, this, &PairingSession::onDisplayFailed);
    connect(m_streamManager, &StreamManager::streamStarted, this, &PairingSession::onStreamStarted);
    connect(m_streamManager, &StreamManager::streamError, this, &PairingSession::onStreamError);
    connect(m_streamManager, &StreamManager::streamStopped, this, &PairingSession::onStreamStopped);
    connect(m_streamManager, &StreamManager::pinResult, this, [this](int index, bool ok, const QString &) {
        if (ok && index == kPairingInstanceIndex && !m_username.isEmpty()) {
            Q_EMIT pairedChanged(m_username);
        }
    });
    connect(m_streamManager, &StreamManager::unpairResult, this, [this](int index, bool ok, const QString &message) {
        onUnpairResult(m_streamManager, index, ok, message);
    });
}

PairingSession::~PairingSession()
{
    // The StreamManager child terminates its Sunshine processes when it is destroyed
    m_virtualDisplays->destroyAll();
}

void PairingSession::setSessionManager(SessionManager *manager)
{
    if (m_sessionManager == manager) {
        return;
    }
    m_sessionManager = manager;
    Q_EMIT sessionManagerChanged();
}

void PairingSession::setSessionRunner(SessionRunner *runner)
{
    if (m_sessionRunner == runner) {
        return;
    }
    if (m_sessionRunner) {
        disconnect(m_sessionRunner, nullptr, this, nullptr);
        if (m_sessionRunner->streamManager()) {
            disconnect(m_sessionRunner->streamManager(), &StreamManager::unpairResult, this, nullptr);
        }
    }
    m_sessionRunner = runner;
    if (runner) {
        connect(runner, &SessionRunner::activeChanged, this, [this]() {
            // A session needs the virtual display names and Sunshine state of its players
            if (sessionActive() && m_state != Idle) {
                stop();
            }
        });
    }
    if (runner && runner->streamManager()) {
        StreamManager *live = runner->streamManager();
        connect(live, &StreamManager::unpairResult, this, [this, live](int index, bool ok, const QString &message) {
            onUnpairResult(live, index, ok, message);
        });
    }
    Q_EMIT sessionRunnerChanged();
}

void PairingSession::setSettingsManager(SettingsManager *manager)
{
    if (m_settingsManager == manager) {
        return;
    }
    m_settingsManager = manager;
    Q_EMIT settingsManagerChanged();
}

void PairingSession::setVirtualDisplayProgramOverride(const QString &program)
{
    m_virtualDisplays->setProgramOverride(program);
}

void PairingSession::setScreenLookup(VirtualDisplayManager::ScreenLookup lookup)
{
    m_virtualDisplays->setScreenLookup(std::move(lookup));
}

void PairingSession::setVirtualDisplayTimeoutMs(int timeoutMs)
{
    m_virtualDisplays->setTimeoutMs(timeoutMs);
}

void PairingSession::setSunshineProgramOverride(const QString &program)
{
    m_streamManager->setProgramOverride(program);
}

bool PairingSession::sessionActive() const
{
    return m_sessionRunner && m_sessionRunner->isActive();
}

int PairingSession::pairedClientCount(const QString &username) const
{
    return SunshineConfig::pairedClientCount(username);
}

QVariantList PairingSession::pairedClients(const QString &username) const
{
    return SunshineConfig::pairedClients(username);
}

QString PairingSession::hostPort(const QString &username) const
{
    const int port = SunshineConfig::userPort(username);
    return port > 0 ? QString::number(port) : QString();
}

void PairingSession::setState(State state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    Q_EMIT stateChanged();
    updateBusy();
}

void PairingSession::setError(const QString &error)
{
    if (m_error == error) {
        return;
    }
    m_error = error;
    Q_EMIT errorChanged();
}

void PairingSession::setUsername(const QString &username)
{
    if (m_username == username) {
        return;
    }
    m_username = username;
    Q_EMIT usernameChanged();
}

void PairingSession::updateBusy()
{
    const bool current = busy();
    if (m_lastBusy == current) {
        return;
    }
    m_lastBusy = current;
    Q_EMIT busyChanged();
}

void PairingSession::start(const QString &username)
{
    if (m_state == Starting || m_state == Ready) {
        qWarning() << "PairingSession::start: a pairing session is already running";
        return;
    }

    setUsername(username);
    setError(QString());

    QString refusal;
    if (username.isEmpty()) {
        refusal = QStringLiteral("Choose a gaming user first");
    } else if (sessionActive()) {
        refusal = QStringLiteral("Pairing is not available while a session is running");
    } else if (m_stopping) {
        refusal = QStringLiteral("The previous pairing session is still shutting down; try again in a moment");
    }
    if (!refusal.isEmpty()) {
        setError(refusal);
        setState(Failed);
        return;
    }

    setState(Starting);
    m_virtualDisplays->create(kPairingInstanceIndex, QSize(kPairingDisplayWidth, kPairingDisplayHeight));
}

void PairingSession::stop()
{
    if (!m_unpairUuid.isEmpty()) {
        const QString pendingUser = m_unpairUser;
        m_unpairUuid.clear();
        m_unpairUser.clear();
        m_unpairTemporary = false;
        m_unpairManager.clear();
        Q_EMIT unpairFinished(pendingUser, false, QStringLiteral("Pairing was stopped"));
    }
    const QString user = m_username;
    setState(Idle);
    teardown();
    setError(QString());
    setUsername(QString());
    if (!user.isEmpty()) {
        Q_EMIT pairedChanged(user);
    }
}

void PairingSession::teardown()
{
    m_streamManager->stopAll();
    m_virtualDisplays->destroyAll();
    m_stopping = m_streamManager->hasOwnedPids();
    updateBusy();
}

void PairingSession::fail(const QString &message)
{
    if (m_state != Starting && m_state != Ready) {
        return;
    }
    qWarning() << "PairingSession:" << message;
    setError(message);
    setState(Failed);
    teardown();
    if (!m_unpairUuid.isEmpty()) {
        finishUnpair(false, message);
    }
}

void PairingSession::onDisplayReady(int index, const QString &outputName)
{
    if (m_state != Starting || index != kPairingInstanceIndex) {
        return;
    }

    // The user's stable port; allocatePort() still bumps it when something else holds it right now
    const int preferred = SunshineConfig::userPort(m_username);
    const int port = SunshineConfig::allocatePort(preferred > 0 ? preferred : SunshineConfig::BASE_PORT);
    if (port < 0) {
        fail(QStringLiteral("No free port for Sunshine"));
        return;
    }

    StreamSpec spec;
    spec.instanceIndex = kPairingInstanceIndex;
    spec.identityKey = m_username;
    spec.outputName = outputName;
    spec.port = port;

    if (m_settingsManager) {
        m_streamManager->setSunshinePath(m_settingsManager->resolvedSunshinePath());
    }
    if (!m_streamManager->startStream(index, spec)) {
        // StreamManager reports its own failures through streamError; this covers any that stay silent
        fail(QStringLiteral("Could not start Sunshine"));
    }
}

void PairingSession::onDisplayFailed(int index, const QString &message)
{
    if (index != kPairingInstanceIndex) {
        return;
    }
    fail(QStringLiteral("Could not create the virtual display: %1").arg(message));
}

void PairingSession::onStreamStarted(int index)
{
    if (m_state == Starting && index == kPairingInstanceIndex) {
        setState(Ready);
        if (!m_unpairUuid.isEmpty() && m_unpairTemporary) {
            m_streamManager->unpairClient(kPairingInstanceIndex, m_unpairUuid);
        }
    }
}

void PairingSession::onStreamError(int index, const QString &message)
{
    if (index != kPairingInstanceIndex) {
        return;
    }
    fail(message);
}

void PairingSession::onStreamStopped(int)
{
    if (m_stopping && !m_streamManager->hasOwnedPids()) {
        m_stopping = false;
        updateBusy();
    }
}

void PairingSession::unpairClient(const QString &username, const QString &uuid)
{
    QString refusal;
    StreamManager *manager = nullptr;
    int index = kPairingInstanceIndex;
    bool temporary = false;

    if (username.isEmpty() || uuid.isEmpty()) {
        refusal = QStringLiteral("Choose a paired client first");
    } else if (!m_unpairUuid.isEmpty()) {
        refusal = QStringLiteral("Another unpair is still running");
    } else if (sessionActive()) {
        index = m_sessionRunner->streamingInstanceForUser(username);
        manager = m_sessionRunner->streamManager();
        if (index < 0 || !manager || manager->streamState(index) != StreamManager::Streaming) {
            refusal = QStringLiteral("Start this player's stream first, or stop the session");
        }
    } else if (m_state == Ready && m_username == username) {
        manager = m_streamManager;
    } else if (m_state == Starting || m_state == Ready || m_stopping) {
        refusal = QStringLiteral("Pairing is busy; try again in a moment");
    } else {
        manager = m_streamManager;
        temporary = true;
    }

    if (!refusal.isEmpty()) {
        Q_EMIT unpairFinished(username, false, refusal);
        return;
    }

    m_unpairUuid = uuid;
    m_unpairUser = username;
    m_unpairTemporary = temporary;
    m_unpairIndex = index;
    m_unpairManager = manager;

    if (temporary) {
        start(username);
        if (m_state == Failed) {
            finishUnpair(false, m_error);
        }
        return;
    }
    manager->unpairClient(index, uuid);
}

void PairingSession::onUnpairResult(StreamManager *manager, int index, bool ok, const QString &message)
{
    if (m_unpairUuid.isEmpty()) {
        // Unpaired from the pairing sheet
        if (ok && manager == m_streamManager && !m_username.isEmpty()) {
            Q_EMIT pairedChanged(m_username);
        }
        return;
    }
    if (manager != m_unpairManager || index != m_unpairIndex) {
        return;
    }
    finishUnpair(ok, message);
}

void PairingSession::finishUnpair(bool ok, const QString &message)
{
    if (m_unpairUuid.isEmpty()) {
        return;
    }
    const QString user = m_unpairUser;
    const bool temporary = m_unpairTemporary;
    m_unpairUuid.clear();
    m_unpairUser.clear();
    m_unpairTemporary = false;
    m_unpairManager.clear();

    if (temporary) {
        stop();
    } else if (ok) {
        Q_EMIT pairedChanged(user);
    }
    Q_EMIT unpairFinished(user, ok, ok ? QString() : message);
}
