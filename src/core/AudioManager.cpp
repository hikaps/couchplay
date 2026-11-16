// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "AudioManager.h"

#include <QDebug>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>

AudioManager::AudioManager(QObject *parent)
    : QObject(parent)
{
    m_runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);

    checkConfiguration();
    refreshOutputDevices();
}

AudioManager::~AudioManager()
{
    if (m_sinkProcess) {
        m_sinkProcess->kill();
        m_sinkProcess->waitForFinished(1000);
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
}


QVariantList AudioManager::parseSinks(const QByteArray &json)
{
    QVariantList devices;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isArray()) {
        return devices;
    }
    for (const QJsonValue &value : doc.array()) {
        const QJsonObject sink = value.toObject();
        const QString name = sink.value(QStringLiteral("name")).toString();
        // couchplay-* are the per-stream null sinks created for Sunshine capture, not real outputs.
        if (name.isEmpty() || name.startsWith(QStringLiteral("couchplay-"))) {
            continue;
        }
        const QString description = sink.value(QStringLiteral("description")).toString();
        devices.append(QVariantMap{{QStringLiteral("name"), name},
                                   {QStringLiteral("description"), description.isEmpty() ? name : description}});
    }
    return devices;
}

void AudioManager::refreshOutputDevices()
{
    if (m_sinkProcess) {
        m_sinkProcess->disconnect();
        m_sinkProcess->kill();
        m_sinkProcess->deleteLater();
    }
    m_sinkProcess = new QProcess(this);
    connect(m_sinkProcess, &QProcess::finished, this, &AudioManager::onSinkListFinished);
    connect(m_sinkProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && !m_outputDevices.isEmpty()) {
            m_outputDevices.clear();
            Q_EMIT outputDevicesChanged();
        }
    });
    m_sinkProcess->start(QStringLiteral("pactl"), {QStringLiteral("--format=json"), QStringLiteral("list"), QStringLiteral("sinks")});
}

void AudioManager::onSinkListFinished(int exitCode)
{
    QVariantList devices;
    if (exitCode == 0) {
        devices = parseSinks(m_sinkProcess->readAllStandardOutput());
    }
    m_sinkProcess->deleteLater();
    m_sinkProcess = nullptr;

    if (devices != m_outputDevices) {
        m_outputDevices = devices;
        Q_EMIT outputDevicesChanged();
    }
}
