// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#pragma once

#include <QObject>
#include <QString>

class CommandLineBridge : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    bool requestAccepted() const
    {
        return m_requestAccepted;
    }

    Q_INVOKABLE void setRequestAccepted(bool accepted)
    {
        m_requestAccepted = accepted;
    }

Q_SIGNALS:
    void launchRequested(const QString &profileName, bool start, bool exitAfterSession);

private:
    bool m_requestAccepted = false;
};
