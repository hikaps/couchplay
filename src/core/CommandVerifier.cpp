// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "CommandVerifier.h"
#include "Logging.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <pwd.h>
#include <unistd.h>

CommandVerificationResult CommandVerifier::verifyCommand(const QString &command)
{
    CommandVerificationResult result;
    result.isValid = false;
    result.isFlatpak = false;
    result.isFlatpakAvailable = false;
    result.isAccessibleToOtherUsers = false;
    result.isAbsolutePath = false;

    if (command.isEmpty()) {
        result.errorMessage = QStringLiteral("Empty command");
        result.commandType = QStringLiteral("invalid");
        return result;
    }

    result.commandType = detectCommandType(command);

    if (result.commandType == QStringLiteral("invalid")) {
        result.isValid = false;
        result.errorMessage = QStringLiteral("Invalid command format");
        return result;
    }

    if (result.commandType == QStringLiteral("flatpak")) {
        result.isFlatpak = true;
        result.isAbsolutePath = false;
        result.isFlatpakAvailable = isFlatpakAvailable();

        if (!result.isFlatpakAvailable) {
            result.isValid = false;
            result.errorMessage = QStringLiteral("Flatpak not installed or not accessible");
            return result;
        }

        // Extract the app ID from "flatpak run [--options] <app-id> [args]":
        // exported desktop entries commonly insert options between "run" and
        // the app ID, so a fixed token position misses every exported launcher
        const QString appID = extractFlatpakAppId(command);
        if (appID.isEmpty() || !isValidFlatpakAppId(appID)) {
            result.isValid = false;
            result.errorMessage = QStringLiteral("Invalid Flatpak command format");
            return result;
        }

        result.isValid = isFlatpakAppInstalled(appID);
        if (!result.isValid) {
            result.errorMessage =
                QString(QStringLiteral("Flatpak app '%1' not installed or not accessible to all users")).arg(appID);
        }
        result.isAccessibleToOtherUsers = result.isValid; // Flatpak apps are generally accessible
    } else if (result.commandType == QStringLiteral("absolute")) {
        result.isFlatpak = false;
        result.isAbsolutePath = true;

        QFileInfo fileInfo(command);
        result.isAccessibleToOtherUsers = isAccessibleToOtherUsersPath(command);
        result.isValid = fileInfo.exists() && fileInfo.isExecutable();
        if (!result.isValid) {
            result.errorMessage = QString(QStringLiteral("Command not found at path: %1")).arg(command);
            // Don't completely fail validation for desktop file commands - they might be installed
            result.isValid = true; // Allow desktop file commands even if executable doesn't exist yet
        }
    } else if (result.commandType == QStringLiteral("path")) {
        result.isFlatpak = false;
        result.isAbsolutePath = false;

        QString resolvedPath = resolveCommandPath(command);
        if (resolvedPath.isEmpty()) {
            result.isValid = false;
            result.errorMessage = QString(QStringLiteral("Command not found in PATH: %1")).arg(command);
        } else {
            QFileInfo fileInfo(resolvedPath);
            result.resolvedPath = resolvedPath;
            result.isAccessibleToOtherUsers = isAccessibleToOtherUsersPath(resolvedPath);
            result.isValid = fileInfo.exists() && fileInfo.isExecutable();
            if (!result.isValid) {
                result.errorMessage = QString(QStringLiteral("Command is not executable: %1")).arg(command);
            }
        }
    }

    return result;
}

QString CommandVerifier::detectCommandType(const QString &command)
{
    if (command.isEmpty()) {
        return QStringLiteral("invalid");
    }

    if (command.startsWith(QStringLiteral("flatpak run "))) {
        return QStringLiteral("flatpak");
    }

    if (command.startsWith(QStringLiteral("/"))) {
        // Check if it's an absolute path to an executable
        // Extract the command part (without arguments)
        QString commandName = command.split(QStringLiteral(" ")).first();
        QFileInfo fileInfo(commandName);
        if (fileInfo.exists() && fileInfo.isExecutable()) {
            return QStringLiteral("absolute");
        } else {
            // Allow absolute paths even if they don't exist - might be installed later
            return QStringLiteral("absolute");
        }
    }

    // Check if it's a command that can be found in PATH
    QString commandName = command.split(QStringLiteral(" ")).first();
    if (!commandName.isEmpty()) {
        return QStringLiteral("path");
    }

    return QStringLiteral("invalid");
}

bool CommandVerifier::isFlatpakCommand(const QString &command)
{
    return detectCommandType(command) == QStringLiteral("flatpak");
}

bool CommandVerifier::isAbsolutePath(const QString &command)
{
    // Simply check if it starts with "/" - that's what makes it an absolute path
    return command.startsWith(QStringLiteral("/"));
}

bool CommandVerifier::commandExistsInPath(const QString &commandName)
{
    QProcess process;
    process.setProgram(QStringLiteral("which"));
    process.setArguments({commandName});
    process.start();

    if (process.waitForFinished(2000)) {
        return process.exitCode() == 0;
    }

    return false;
}

bool CommandVerifier::isCommandExecutable(const QString &path)
{
    QFileInfo fileInfo(path);
    return fileInfo.exists() && fileInfo.isExecutable();
}

bool CommandVerifier::isUserLocalCommand(const QString &command)
{
    if (isFlatpakCommand(command)) {
        return false; // Flatpak apps are generally system-wide
    }

    QString commandName = command.split(QStringLiteral(" ")).first();
    if (commandName.isEmpty()) {
        return false;
    }

    // If it's an absolute path, check if it's user-local
    if (commandName.startsWith(QStringLiteral("/"))) {
        return isUserLocalPath(commandName);
    }

    // Check if the command resolves to a user-local path
    QString resolvedPath = resolveCommandPath(commandName);
    if (!resolvedPath.isEmpty()) {
        return isUserLocalPath(resolvedPath);
    }

    return false;
}

bool CommandVerifier::isFlatpakAvailable()
{
    QProcess process;
    process.setProgram(QStringLiteral("flatpak"));
    process.setArguments({QStringLiteral("--version")});
    process.start();

    if (process.waitForFinished(2000)) {
        return process.exitCode() == 0;
    }

    return false;
}

bool CommandVerifier::isFlatpakAppInstalled(const QString &appID)
{
    if (!isValidFlatpakAppId(appID)) {
        return false;
    }

    QProcess process;
    process.setProgram(QStringLiteral("flatpak"));
    process.setArguments({QStringLiteral("list"), QStringLiteral("--app"), QStringLiteral("--columns=application")});
    process.start();

    if (process.waitForFinished(2000)) {
        if (process.exitCode() == 0) {
            QByteArray output = process.readAllStandardOutput();
            QStringList installedApps = QString::fromUtf8(output).split(QStringLiteral("\n"));
            return installedApps.contains(appID);
        }
    }

    return false;
}

QString CommandVerifier::resolveCommandPath(const QString &commandName)
{
    QProcess process;
    process.setProgram(QStringLiteral("which"));
    process.setArguments({commandName});
    process.start();

    if (process.waitForFinished(2000)) {
        if (process.exitCode() == 0) {
            QByteArray output = process.readAllStandardOutput();
            QString path = QString::fromUtf8(output).trimmed();
            if (!path.isEmpty()) {
                QFileInfo fileInfo(path);
                if (fileInfo.exists() && fileInfo.isExecutable()) {
                    return path;
                }
            }
        }
    }

    return QString();
}

bool CommandVerifier::isDirectoryReadable(const QString &path)
{
    QDir dir(path);
    return dir.exists() && dir.isReadable();
}

bool CommandVerifier::isUserLocalPath(const QString &path)
{
    if (path.isEmpty()) {
        return false;
    }

    QString canonical = QFileInfo(path).canonicalFilePath();
    if (canonical.isEmpty()) {
        canonical = path;
    }

    QString homePath = QDir::homePath();
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir) {
        homePath = QString::fromLocal8Bit(pw->pw_dir);
    }

    if (homePath.isEmpty() || homePath == QStringLiteral("/")) {
        return false;
    }

    if (canonical.startsWith(homePath)) {
        return true;
    }

    QStringList userLocalPatterns = {
        QStringLiteral("/.local/"),
        QStringLiteral("/.config/"),
        QStringLiteral("/.steam/"),
    };

    for (const QString &pattern : userLocalPatterns) {
        if (canonical.contains(pattern)) {
            return true;
        }
    }

    return false;
}

bool CommandVerifier::isAccessibleToOtherUsersPath(const QString &path)
{
    if (isUserLocalPath(path)) {
        return false;
    }

    QFileInfo fileInfo(path);
    QFileDevice::Permissions permissions = fileInfo.permissions();
    bool hasGroupAccess = permissions.testFlag(QFileDevice::ReadGroup) && permissions.testFlag(QFileDevice::ExeGroup);
    bool hasOtherAccess = permissions.testFlag(QFileDevice::ReadOther) && permissions.testFlag(QFileDevice::ExeOther);

    return hasGroupAccess || hasOtherAccess;
}

QString CommandVerifier::extractFlatpakAppId(const QString &command)
{
    // "flatpak run [--opt value | --opt=value]… [--] <app-id> [args…]":
    // exported desktop entries commonly insert options between "run" and the
    // ID. Skip value-taking options in both forms (a bare scan for the first
    // dotted non-flag token mistook e.g. "--runtime org.freedesktop.Platform"
    // for the app), honor the "--" terminator, then take the first positional
    // as the application reference.
    static const QStringList valueTakingOptions = {
        QStringLiteral("arch"),
        QStringLiteral("branch"),
        QStringLiteral("command"),
        QStringLiteral("cwd"),
        QStringLiteral("runtime"),
        QStringLiteral("runtime-version"),
        QStringLiteral("extra-arguments"),
        QStringLiteral("env"),
        QStringLiteral("env-fd"),
        QStringLiteral("commit"),
        QStringLiteral("runtime-commit"),
        QStringLiteral("parent-pid"),
        QStringLiteral("parent-window"),
        QStringLiteral("own-name"),
        QStringLiteral("talk-name"),
        QStringLiteral("system-own-name"),
        QStringLiteral("system-talk-name"),
        QStringLiteral("system-call"),
        QStringLiteral("call"),
        QStringLiteral("socket"),
        QStringLiteral("device"),
        QStringLiteral("filesystem"),
        QStringLiteral("share"),
        QStringLiteral("unshare"),
        QStringLiteral("inter-share"),
        QStringLiteral("persist"),
        QStringLiteral("subpath"),
        QStringLiteral("bind"),
        QStringLiteral("bind-mount"),
        QStringLiteral("ro-bind"),
        QStringLiteral("add-policy"),
        QStringLiteral("remove-policy"),
        QStringLiteral("app-path"),
        QStringLiteral("usr-path"),
        QStringLiteral("log-session-bus"),
        QStringLiteral("log-system-bus"),
        QStringLiteral("log-a11y-bus"),
    };

    const QStringList tokens = command.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (tokens.size() < 3 || tokens.at(0) != QStringLiteral("flatpak") || tokens.at(1) != QStringLiteral("run")) {
        return QString();
    }
    QString positional;
    for (int i = 2; i < tokens.size() && positional.isEmpty(); ++i) {
        const QString &token = tokens.at(i);
        if (token == QStringLiteral("--")) {
            // Positional section begins: the application reference is next
            if (i + 1 < tokens.size()) {
                positional = tokens.at(i + 1);
            }
            break;
        }
        if (!token.startsWith(QLatin1Char('-'))) {
            positional = token; // first positional is the application reference
            break;
        }
        if (token.contains(QLatin1Char('='))) {
            continue; // self-contained --opt=value
        }
        QString option = token;
        while (option.startsWith(QLatin1Char('-'))) {
            option.remove(0, 1);
        }
        if (valueTakingOptions.contains(option)) {
            ++i; // consume the option's space-separated value
        }
    }
    return isValidFlatpakAppId(positional) ? positional : QString();
}

bool CommandVerifier::isValidFlatpakAppId(const QString &appID)
{
    // Basic validation for Flatpak app ID format
    // Should be in format: com.example.AppName
    if (appID.isEmpty()) {
        return false;
    }

    QStringList parts = appID.split(QStringLiteral("."));
    if (parts.size() < 3) {
        return false;
    }

    // Check that each part is valid
    for (const QString &part : parts) {
        if (part.isEmpty()) {
            return false;
        }

        // Check that part contains only valid characters
        for (QChar ch : part) {
            if (!ch.isLetterOrNumber() && ch != QLatin1Char('-')) {
                return false;
            }
        }
    }

    return true;
}
