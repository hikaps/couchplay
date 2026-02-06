// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QString>
#include <QStringList>
#include <functional>

/**
 * @brief Command verification result structure
 */
struct CommandVerificationResult {
    bool isValid;
    QString errorMessage;
    bool isFlatpak;
    bool isFlatpakAvailable; // Flatpak is installed and accessible
    QString resolvedPath;
    bool isAccessibleToOtherUsers;
    bool isAbsolutePath;
    QString commandType; // "flatpak", "absolute", "path", "invalid"
};

/**
 * @brief Interface for path resolution dependency injection
 */
using PathResolverFunc = std::function<QString(const QString&)>;

/**
 * @brief Command verification utility
 * 
 * Provides methods to verify commands work properly for different users,
 * detecting Flatpak vs native commands, PATH resolution, and user-local commands.
 */
class CommandVerifier
{
public:
    /**
     * @brief Set a custom path resolver function for testing
     * @param resolver Function that takes a command name and returns absolute path
     */
    static void setPathResolver(PathResolverFunc resolver);

    /**
     * @brief Verify a command will work for gaming users
     * @param command The command string to verify
     * @return Verification result with detailed information
     */
    static CommandVerificationResult verifyCommand(const QString &command);

    /**
     * @brief Detect the type of command
     * @param command The command to analyze
     * @return Type string: "flatpak", "absolute", "path", or "invalid"
     */
    static QString detectCommandType(const QString &command);

    /**
     * @brief Check if command is a Flatpak command
     * @param command Command to check
     * @return true if it's a Flatpak command
     */
    static bool isFlatpakCommand(const QString &command);

    /**
     * @brief Check if command is an absolute path
     * @param command Command to check
     * @return true if it's an absolute path
     */
    static bool isAbsolutePath(const QString &command);

    /**
     * @brief Check if command exists in PATH
     * @param commandName Command name to search for
     * @return true if command exists and is executable
     */
    static bool commandExistsInPath(const QString &commandName);

    /**
     * @brief Check if file at path is executable
     * @param path File path to check
     * @return true if file exists and is executable
     */
    static bool isCommandExecutable(const QString &path);

    /**
     * @brief Check if command is user-local (won't work for other users)
     * @param command Command to check
     * @return true if command is likely user-local
     */
    static bool isUserLocalCommand(const QString &command);

    /**
     * @brief Check if Flatpak is installed and accessible
     * @return true if Flatpak is available
     */
    static bool isFlatpakAvailable();

    /**
     * @brief Validate Flatpak app ID and check if installed
     * @param appID Flatpak app ID (e.g., "com.heroicgameslauncher.hgl")
     * @return true if Flatpak app is installed and accessible
     */
    static bool isFlatpakAppInstalled(const QString &appID);

    /**
     * @brief Resolve a PATH-based command to absolute path
     * @param commandName Command name to resolve
     * @return Resolved path, or empty if not found
     */
    static QString resolveCommandPath(const QString &commandName);

    /**
     * @brief Validate Flatpak app ID format
     * @param appID Flatpak app ID to validate
     * @return true if app ID format is valid
     */
    static bool isValidFlatpakAppId(const QString &appID);

private:
    /**
     * @brief Check if a directory is readable
     * @param path Directory path
     * @return true if directory exists and is readable
     */
    static bool isDirectoryReadable(const QString &path);

    /**
     * @brief Check if path points to a user-local directory
     * @param path Path to check
     * @return true if path is under user's home directory
     */
    static bool isUserLocalPath(const QString &path);

    /**
     * @brief Check if command path is accessible to other users
     * @param path Command path to check
     * @return true if accessible to other users
     */
    static bool isAccessibleToOtherUsersPath(const QString &path);
};
