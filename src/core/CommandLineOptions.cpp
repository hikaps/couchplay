// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include "CommandLineOptions.h"

#include <QRegularExpression>

namespace
{
bool parseGame(const QString &value, GameSelection *game)
{
    const QStringList parts = value.split(QLatin1Char(':'));
    if (parts.constFirst() == QStringLiteral("steam") && parts.size() == 2) {
        static const QRegularExpression digits(QStringLiteral("^[0-9]+$"));
        if (!digits.match(parts.at(1)).hasMatch() || parts.at(1).toULongLong() == 0) {
            return false;
        }
        game->launcherId = QStringLiteral("steam");
        game->backend = QStringLiteral("native");
        game->gameId = parts.at(1);
        return true;
    }
    if (parts.constFirst() == QStringLiteral("heroic") && parts.size() >= 3) {
        static const QStringList runners = {QStringLiteral("legendary"),
                                            QStringLiteral("gog"),
                                            QStringLiteral("nile"),
                                            QStringLiteral("sideload")};
        const QString appName = QStringList(parts.mid(2)).join(QLatin1Char(':'));
        if (!runners.contains(parts.at(1)) || appName.isEmpty()) {
            return false;
        }
        game->launcherId = QStringLiteral("heroic");
        game->backend = parts.at(1);
        game->gameId = appName;
        return true;
    }
    return false;
}
} // namespace

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
        } else if (argument == QStringLiteral("--game")) {
            if (!request.game.isEmpty() || index + 1 >= arguments.size() || arguments.at(index + 1).isEmpty()
                || arguments.at(index + 1).startsWith(QLatin1Char('-'))) {
                fail(QStringLiteral("--game requires exactly one LAUNCHER:ID value"));
                return {};
            }
            const QString value = arguments.at(++index);
            if (!parseGame(value, &request.game)) {
                fail(QStringLiteral("Invalid --game value: %1 (expected steam:APPID or heroic:RUNNER:APPNAME)")
                         .arg(value));
                return {};
            }
        } else if (argument == QStringLiteral("--quick")) {
            bool ok = false;
            const int players = (request.quickPlayers == 0 && index + 1 < arguments.size()
                                 && !arguments.at(index + 1).startsWith(QLatin1Char('-')))
                ? arguments.at(index + 1).toInt(&ok)
                : 0;
            if (!ok || players < 2 || players > 4) {
                fail(QStringLiteral("--quick requires a player count from 2 to 4"));
                return {};
            }
            request.quickPlayers = players;
            ++index;
        } else {
            fail(QStringLiteral("Unknown command-line argument: %1").arg(argument));
            return {};
        }
    }

    if (request.quickPlayers > 0) {
        if (!request.profileName.isEmpty()) {
            fail(QStringLiteral("--quick cannot be combined with --profile"));
            return {};
        }
        if (request.game.isEmpty()) {
            fail(QStringLiteral("--quick requires --game"));
            return {};
        }
        request.start = true; // a quick session always starts; an explicit --start is accepted too
    }

    if (request.start && !request.requested()) {
        fail(QStringLiteral("--start requires --profile or --quick"));
        return {};
    }
    if (request.exitAfterSession && (!request.start || !request.requested())) {
        fail(QStringLiteral("--exit-after-session requires --profile and --start, or --quick"));
        return {};
    }
    if (request.skipPairingCheck && (!request.start || !request.requested())) {
        fail(QStringLiteral("--skip-pairing-check requires --profile and --start, or --quick"));
        return {};
    }
    if (!request.game.isEmpty() && (!request.start || !request.requested())) {
        fail(QStringLiteral("--game requires --profile and --start, or --quick"));
        return {};
    }
    return request;
}
