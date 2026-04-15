// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <qqmlintegration.h>

class AudioManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool multiUserConfigured READ isMultiUserConfigured NOTIFY configurationChanged)
    Q_PROPERTY(QString audioServer READ audioServer NOTIFY audioServerChanged)

public:
    explicit AudioManager(QObject *parent = nullptr);
    ~AudioManager() override;

    bool isMultiUserConfigured() const { return m_multiUserConfigured; }
    QString audioServer() const { return m_audioServer; }

    Q_INVOKABLE void checkConfiguration();

Q_SIGNALS:
    void configurationChanged();
    void audioServerChanged();
    void errorOccurred(const QString &message);

private Q_SLOTS:
    void onPactlFinished(int exitCode);

private:
    bool checkSocketAcl(const QString &socketPath);

    bool m_multiUserConfigured = false;
    QString m_audioServer;
    QString m_runtimeDir;
    QProcess *m_pactlProcess = nullptr;
};
