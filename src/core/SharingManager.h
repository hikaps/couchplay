// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <qqmlintegration.h>

/**
 * SharedDirectory - Data structure for a shared directory entry
 */
struct SharedDirectory {
    Q_GADGET
    Q_PROPERTY(QString sourcePath MEMBER sourcePath)
    Q_PROPERTY(QString mountAlias MEMBER mountAlias)

public:
    QString sourcePath;   // Absolute path to source directory
    QString mountAlias;   // Mount alias (relative to user home), empty for home-relative paths
    
    bool operator==(const SharedDirectory &other) const {
        return sourcePath == other.sourcePath;
    }
};

Q_DECLARE_METATYPE(SharedDirectory)

/**
 * SharingManager - Manages shared directories for CouchPlay sessions
 * 
 * Allows users to configure directories that should be bind-mounted into
 * gaming users' home directories during sessions. Commonly used for:
 * - Steam game libraries
 * - Proton prefixes
 * - Game save directories
 */
class SharingManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    
    Q_PROPERTY(QVariantList sharedDirectories READ sharedDirectoriesAsVariant NOTIFY sharedDirectoriesChanged)
    Q_PROPERTY(QString steamLibraryPath READ steamLibraryPath CONSTANT)

public:
    explicit SharingManager(QObject *parent = nullptr);
    ~SharingManager() override = default;

    /**
     * Add a directory to the sharing list
     * 
     * @param path Absolute path to the directory
     * @param alias Mount alias (where it appears in user homes). Empty for home-relative paths.
     */
    Q_INVOKABLE void addDirectory(const QString &path, const QString &alias = QString());

    /**
     * Remove a directory from the sharing list
     * 
     * @param path Source path of the directory to remove
     */
    Q_INVOKABLE void removeDirectory(const QString &path);

    /**
     * Check if a path is outside the current user's home directory
     * 
     * @param path Path to check
     * @return true if path is outside home
     */
    Q_INVOKABLE bool isOutsideHome(const QString &path) const;

    /**
     * Get the directory list in "source|alias" format for the helper
     * 
     * @return List of directory specifications
     */
    QStringList directoryList() const;

    /**
     * Get shared directories as QVariantList for QML binding
     */
    QVariantList sharedDirectoriesAsVariant() const;

    /**
     * Get the default Steam library path if it exists
     */
    QString steamLibraryPath() const;

Q_SIGNALS:
    void sharedDirectoriesChanged();
    void errorOccurred(const QString &message);

private:
    void loadFromConfig();
    void saveToConfig();
    bool isValidPath(const QString &path) const;
    bool isBlacklisted(const QString &path) const;
    bool isDuplicate(const QString &path) const;
    QString currentUserHome() const;

    QList<SharedDirectory> m_directories;
    QString m_userHome;
};
