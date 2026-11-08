// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include "SignalBridge.h"

#include <QSocketNotifier>
#include <QtDebug>
#include <cerrno>

#include <csignal>
#include <sys/socket.h>
#include <unistd.h>

namespace {
constexpr int handledSignals[] = {SIGTERM, SIGINT, SIGHUP};
constexpr int handledSignalCount = sizeof(handledSignals) / sizeof(handledSignals[0]);

int s_socketPair[2] = {-1, -1};
struct sigaction s_previousActions[handledSignalCount];
bool s_installed = false;

void forwardSignal(int signalNumber)
{
    // write() is async-signal-safe; a full socket buffer just drops a repeated signal.
    const int savedErrno = errno;
    [[maybe_unused]] const ssize_t written = ::write(s_socketPair[0], &signalNumber, sizeof(signalNumber));
    errno = savedErrno;
}
}

SignalBridge::SignalBridge(QObject *parent)
    : QObject(parent)
{
    if (s_installed) {
        qWarning() << "SignalBridge: already installed; this instance will not receive signals";
        return;
    }
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, s_socketPair) != 0) {
        qWarning() << "SignalBridge: socketpair failed";
        return;
    }

    struct sigaction action {};
    action.sa_handler = forwardSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    for (int i = 0; i < handledSignalCount; ++i) {
        ::sigaction(handledSignals[i], &action, &s_previousActions[i]);
    }
    s_installed = true;
    m_active = true;

    m_notifier = new QSocketNotifier(s_socketPair[1], QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &SignalBridge::onSocketActivated);
}

SignalBridge::~SignalBridge()
{
    if (!m_active) {
        return;
    }
    for (int i = 0; i < handledSignalCount; ++i) {
        ::sigaction(handledSignals[i], &s_previousActions[i], nullptr);
    }
    delete m_notifier;
    m_notifier = nullptr;
    ::close(s_socketPair[0]);
    ::close(s_socketPair[1]);
    s_socketPair[0] = s_socketPair[1] = -1;
    s_installed = false;
}

void SignalBridge::onSocketActivated()
{
    m_notifier->setEnabled(false);
    int signalNumber = 0;
    while (::read(s_socketPair[1], &signalNumber, sizeof(signalNumber)) == static_cast<ssize_t>(sizeof(signalNumber))) {
        Q_EMIT terminationRequested(signalNumber);
    }
    m_notifier->setEnabled(true);
}
