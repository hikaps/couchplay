// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#pragma once

#include <QString>
#include <QStringList>

struct CommandLineRequest
{
    QString profileName;
    bool start = false;
    bool exitAfterSession = false;

    bool requested() const
    {
        return !profileName.isEmpty();
    }
};

class CommandLineOptions
{
public:
    static CommandLineRequest parse(const QStringList &arguments, QString *errorMessage = nullptr);
};
