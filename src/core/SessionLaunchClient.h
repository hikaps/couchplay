// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#pragma once

#include <functional>

#include <QObject>

#include "CommandLineOptions.h"

class QApplication;

class SessionLaunchClient final
{
public:
    static int run(QApplication &application, const CommandLineRequest &request, const QString &serviceName);
    static QObject *watchTermination(QObject *parent, std::function<void()> onTermination);
};
