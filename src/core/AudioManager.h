// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QObject>
#include <QProcess>
#include <qqmlintegration.h>
#include <QString>
#include <QVariantList>

class AudioManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString audioServer READ audioServer NOTIFY audioServerChanged)
    Q_PROPERTY(QVariantList outputDevices READ outputDevices NOTIFY outputDevicesChanged)

public:
    explicit AudioManager(QObject *parent = nullptr);
    ~AudioManager() override;

    QString audioServer() const
    {
        return m_audioServer;
    }
    // Playback sinks as a list of {name, description} maps; refreshed by refreshOutputDevices().
    QVariantList outputDevices() const
    {
        return m_outputDevices;
    }

    // Parses `pactl --format=json list sinks`; CouchPlay's own null sinks are left out.
    static QVariantList parseSinks(const QByteArray &json);

    Q_INVOKABLE void checkConfiguration();
    Q_INVOKABLE void refreshOutputDevices();

Q_SIGNALS:
    void audioServerChanged();
    void outputDevicesChanged();
    void errorOccurred(const QString &message);

private Q_SLOTS:
    void onSinkListFinished(int exitCode);

private:
    QString m_audioServer;
    QString m_runtimeDir;
    QVariantList m_outputDevices;
    QProcess *m_sinkProcess = nullptr;
};
