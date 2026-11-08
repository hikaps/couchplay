// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 hikaps

#include "PairingSession.h"

#include "SunshineConfig.h"

namespace
{
constexpr int kPairingDisplayWidth = 1280;
constexpr int kPairingDisplayHeight = 720;
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
        if (ok && index == m_instanceIndex && !m_username.isEmpty()) {
            Q_EMIT pairedChanged(m_username);
        }
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

void PairingSession::setInstanceIndex(int index)
{
    if (m_instanceIndex == index) {
        return;
    }
    m_instanceIndex = index;
    Q_EMIT instanceIndexChanged();
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

void PairingSession::start(int instanceIndex, const QString &username)
{
    if (m_state == Starting || m_state == Ready) {
        qWarning() << "PairingSession::start: a pairing session is already running";
        return;
    }

    setInstanceIndex(instanceIndex);
    setUsername(username);
    setError(QString());

    QString refusal;
    if (username.isEmpty()) {
        refusal = QStringLiteral("Assign a gaming user to this player first");
    } else if (instanceIndex < 0) {
        refusal = QStringLiteral("Invalid player");
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
    m_virtualDisplays->create(instanceIndex, QSize(kPairingDisplayWidth, kPairingDisplayHeight));
}

void PairingSession::stop()
{
    const QString user = m_username;
    setState(Idle);
    teardown();
    setError(QString());
    setInstanceIndex(-1);
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
}

void PairingSession::onDisplayReady(int index, const QString &outputName)
{
    if (m_state != Starting || index != m_instanceIndex) {
        return;
    }

    const int preferred = m_sessionManager ? m_sessionManager->effectiveSunshinePort(index)
                                           : SunshineConfig::calculatePort(index);
    const int port = SunshineConfig::allocatePort(preferred);
    if (port < 0) {
        fail(QStringLiteral("No free port for Sunshine"));
        return;
    }

    StreamSpec spec;
    spec.instanceIndex = index;
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
    if (index != m_instanceIndex) {
        return;
    }
    fail(QStringLiteral("Could not create the virtual display: %1").arg(message));
}

void PairingSession::onStreamStarted(int index)
{
    if (m_state == Starting && index == m_instanceIndex) {
        setState(Ready);
    }
}

void PairingSession::onStreamError(int index, const QString &message)
{
    if (index != m_instanceIndex) {
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
