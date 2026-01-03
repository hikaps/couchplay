// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

#include "AudioManager.h"

#include <QFile>
#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QDebug>

AudioManager::AudioManager(QObject *parent)
    : QObject(parent)
{
    // Detect audio server
    if (QFile::exists(QStringLiteral("/run/user/1000/pipewire-0"))) {
        m_audioServer = QStringLiteral("pipewire");
    } else {
        m_audioServer = QStringLiteral("pulseaudio");
    }

    checkConfiguration();
}

AudioManager::~AudioManager() = default;

void AudioManager::checkConfiguration()
{
    m_multiUserConfigured = false;

    if (m_audioServer == QStringLiteral("pipewire")) {
        // Check if PipeWire TCP module is configured
        // Look for module-native-protocol-tcp in pipewire config
        
        QStringList configPaths = {
            QDir::homePath() + QStringLiteral("/.config/pipewire/pipewire.conf.d/"),
            QStringLiteral("/etc/pipewire/pipewire.conf.d/")
        };

        for (const QString &path : configPaths) {
            QDir dir(path);
            if (!dir.exists()) continue;

            QStringList files = dir.entryList({QStringLiteral("*.conf")}, QDir::Files);
            for (const QString &fileName : files) {
                QFile file(dir.absoluteFilePath(fileName));
                if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    QString content = QString::fromUtf8(file.readAll());
                    if (content.contains(QStringLiteral("module-native-protocol-tcp")) ||
                        content.contains(QStringLiteral("module-protocol-pulse"))) {
                        m_multiUserConfigured = true;
                        break;
                    }
                }
            }
            if (m_multiUserConfigured) break;
        }
    } else {
        // PulseAudio - check for TCP module
        QProcess pactl;
        pactl.start(QStringLiteral("pactl"), {QStringLiteral("list"), QStringLiteral("modules")});
        if (pactl.waitForFinished(3000)) {
            QString output = QString::fromUtf8(pactl.readAllStandardOutput());
            m_multiUserConfigured = output.contains(QStringLiteral("module-native-protocol-tcp"));
        }
    }

    Q_EMIT configurationChanged();
}

bool AudioManager::configureMultiUser()
{
    if (m_audioServer == QStringLiteral("pipewire")) {
        // Create PipeWire configuration for TCP module
        QString configDir = QDir::homePath() + QStringLiteral("/.config/pipewire/pipewire-pulse.conf.d/");
        QDir().mkpath(configDir);

        QString configPath = configDir + QStringLiteral("10-couchplay-tcp.conf");
        
        QString configContent = QStringLiteral(R"(
# CouchPlay: Enable TCP protocol for multi-user audio sharing
context.modules = [
    {
        name = libpipewire-module-protocol-pulse
        args = {
            server.address = [
                "unix:native"
                "tcp:127.0.0.1:4713"
            ]
        }
    }
]
)");

        QFile file(configPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            Q_EMIT errorOccurred(QStringLiteral("Failed to create PipeWire configuration"));
            return false;
        }

        file.write(configContent.toUtf8());
        file.close();

        // Restart PipeWire to apply
        QProcess::execute(QStringLiteral("systemctl"), 
                          {QStringLiteral("--user"), QStringLiteral("restart"), QStringLiteral("pipewire-pulse")});

        checkConfiguration();
        return true;

    } else {
        // PulseAudio - load TCP module
        QProcess pactl;
        pactl.start(QStringLiteral("pactl"), {
            QStringLiteral("load-module"),
            QStringLiteral("module-native-protocol-tcp"),
            QStringLiteral("auth-ip-acl=127.0.0.1"),
            QStringLiteral("port=4713")
        });

        if (!pactl.waitForFinished(5000)) {
            Q_EMIT errorOccurred(QStringLiteral("Failed to load PulseAudio TCP module"));
            return false;
        }

        if (pactl.exitCode() != 0) {
            Q_EMIT errorOccurred(QString::fromUtf8(pactl.readAllStandardError()));
            return false;
        }

        // Make persistent by adding to default.pa
        QString paConfigDir = QDir::homePath() + QStringLiteral("/.config/pulse/");
        QDir().mkpath(paConfigDir);

        QString defaultPa = paConfigDir + QStringLiteral("default.pa");
        QFile file(defaultPa);
        
        // Append to existing or create new
        if (file.open(QIODevice::Append | QIODevice::Text)) {
            file.write("\n# CouchPlay: Enable TCP for multi-user audio\n");
            file.write("load-module module-native-protocol-tcp auth-ip-acl=127.0.0.1 port=4713\n");
            file.close();
        }

        checkConfiguration();
        return true;
    }
}

QString AudioManager::getAudioServerAddress() const
{
    return QStringLiteral("tcp:127.0.0.1:4713");
}
