// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "AudioManager.h"

#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QDebug>

AudioManager::AudioManager(QObject *parent)
    : QObject(parent)
{
    m_runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);

    checkConfiguration();
}

AudioManager::~AudioManager()
{
    if (m_pactlProcess) {
        m_pactlProcess->kill();
        m_pactlProcess->waitForFinished(1000);
    }
}

void AudioManager::checkConfiguration()
{
    const QString oldServer = m_audioServer;
    if (QFile::exists(m_runtimeDir + QStringLiteral("/pipewire-0"))) {
        m_audioServer = QStringLiteral("pipewire");
    } else {
        m_audioServer = QStringLiteral("pulseaudio");
    }

    if (oldServer != m_audioServer) {
        Q_EMIT audioServerChanged();
    }

    const bool wasConfigured = m_multiUserConfigured;
    m_multiUserConfigured = false;

    if (m_audioServer == QStringLiteral("pipewire")) {
        const QString socketPath = m_runtimeDir + QStringLiteral("/pipewire-0");
        m_multiUserConfigured = QFile::exists(socketPath) && checkSocketAcl(socketPath);
    } else {
        if (m_pactlProcess) {
            m_pactlProcess->disconnect();
            m_pactlProcess->kill();
            m_pactlProcess->deleteLater();
        }

        m_pactlProcess = new QProcess(this);
        connect(m_pactlProcess, &QProcess::finished, this, &AudioManager::onPactlFinished);
        m_pactlProcess->start(QStringLiteral("pactl"), {QStringLiteral("list"), QStringLiteral("modules")});
        return;
    }

    if (wasConfigured != m_multiUserConfigured) {
        Q_EMIT configurationChanged();
    }
}

void AudioManager::onPactlFinished(int exitCode)
{
    const bool wasConfigured = m_multiUserConfigured;

    if (exitCode != 0) {
        Q_EMIT errorOccurred(QStringLiteral("pactl failed with exit code %1").arg(exitCode));
    } else {
        QString output = QString::fromUtf8(m_pactlProcess->readAllStandardOutput());
        m_multiUserConfigured = output.contains(QStringLiteral("module-native-protocol-tcp"));
    }

    m_pactlProcess->deleteLater();
    m_pactlProcess = nullptr;

    if (wasConfigured != m_multiUserConfigured) {
        Q_EMIT configurationChanged();
    }
}

bool AudioManager::checkSocketAcl(const QString &socketPath)
{
    if (!QFile::exists(socketPath)) {
        return false;
    }

    QProcess getfacl;
    getfacl.start(QStringLiteral("getfacl"), {QStringLiteral("-p"), QStringLiteral("-q"), socketPath});
    if (!getfacl.waitForFinished(3000)) {
        Q_EMIT errorOccurred(QStringLiteral("getfacl timed out for %1").arg(socketPath));
        return false;
    }

    if (getfacl.exitCode() != 0) {
        Q_EMIT errorOccurred(QStringLiteral("getfacl failed for %1").arg(socketPath));
        return false;
    }

    QString output = QString::fromUtf8(getfacl.readAllStandardOutput());
    return output.contains(QStringLiteral("group:couchplay:rw"));
}
