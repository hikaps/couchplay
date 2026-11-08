// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include "CommandLineOptions.h"

CommandLineRequest CommandLineOptions::parse(const QStringList &arguments, QString *errorMessage)
{
    CommandLineRequest request;
    if (errorMessage) {
        errorMessage->clear();
    }

    auto fail = [errorMessage](const QString &message) {
        if (errorMessage) {
            *errorMessage = message;
        }
    };

    int index = 0;
    if (!arguments.isEmpty() && !arguments.constFirst().startsWith(QLatin1Char('-'))) {
        index = 1;
    }
    for (; index < arguments.size(); ++index) {
        const QString argument = arguments.at(index);
        if (argument == QStringLiteral("--profile") || argument == QStringLiteral("-p")) {
            if (!request.profileName.isEmpty() || index + 1 >= arguments.size()
                || arguments.at(index + 1).isEmpty()
                || arguments.at(index + 1).startsWith(QLatin1Char('-'))) {
                fail(QStringLiteral("--profile requires exactly one profile name"));
                return {};
            }
            request.profileName = arguments.at(++index);
        } else if (argument == QStringLiteral("--start")) {
            if (request.start) {
                fail(QStringLiteral("--start was specified more than once"));
                return {};
            }
            request.start = true;
        } else if (argument == QStringLiteral("--exit-after-session")) {
            if (request.exitAfterSession) {
                fail(QStringLiteral("--exit-after-session was specified more than once"));
                return {};
            }
            request.exitAfterSession = true;
        } else if (argument == QStringLiteral("--skip-pairing-check")) {
            if (request.skipPairingCheck) {
                fail(QStringLiteral("--skip-pairing-check was specified more than once"));
                return {};
            }
            request.skipPairingCheck = true;
        } else {
            fail(QStringLiteral("Unknown command-line argument: %1").arg(argument));
            return {};
        }
    }

    if (request.start && request.profileName.isEmpty()) {
        fail(QStringLiteral("--start requires --profile"));
        return {};
    }
    if (request.exitAfterSession && (!request.start || request.profileName.isEmpty())) {
        fail(QStringLiteral("--exit-after-session requires --profile and --start"));
        return {};
    }
    if (request.skipPairingCheck && (!request.start || request.profileName.isEmpty())) {
        fail(QStringLiteral("--skip-pairing-check requires --profile and --start"));
        return {};
    }
    return request;
}
