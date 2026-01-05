// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 hikaps

#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include <QVariantMap>
#include <qqmlintegration.h>

#include <KConfig>
#include <KConfigGroup>

class SessionManager;

/**
 * @brief Configuration for a single gamescope instance
 */
struct InstanceConfig {
    Q_GADGET
    Q_PROPERTY(QString username MEMBER username)
    Q_PROPERTY(int monitor MEMBER monitor)
    Q_PROPERTY(int internalWidth MEMBER internalWidth)
    Q_PROPERTY(int internalHeight MEMBER internalHeight)
    Q_PROPERTY(int outputWidth MEMBER outputWidth)
    Q_PROPERTY(int outputHeight MEMBER outputHeight)
    Q_PROPERTY(int refreshRate MEMBER refreshRate)
    Q_PROPERTY(QString scalingMode MEMBER scalingMode)
    Q_PROPERTY(QString filterMode MEMBER filterMode)
    Q_PROPERTY(QList<int> devices MEMBER devices)
    Q_PROPERTY(QString gameCommand MEMBER gameCommand)

public:
    QString username;
    int monitor = 0;
    int internalWidth = 1920;
    int internalHeight = 1080;
    int outputWidth = 960;
    int outputHeight = 1080;
    int refreshRate = 60;
    QString scalingMode = QStringLiteral("fit");
    QString filterMode = QStringLiteral("linear");
    QList<int> devices;
    QString gameCommand;
};

Q_DECLARE_METATYPE(InstanceConfig)

/**
 * @brief A complete session profile
 */
struct SessionProfile {
    Q_GADGET
    Q_PROPERTY(QString name MEMBER name)
    Q_PROPERTY(QString layout MEMBER layout)
    Q_PROPERTY(QString filePath MEMBER filePath)

public:
    QString name;
    QString layout = QStringLiteral("horizontal"); // horizontal, vertical, multi-monitor
    QString filePath;
    QList<InstanceConfig> instances;
};

Q_DECLARE_METATYPE(SessionProfile)

/**
 * @brief Manages session profiles - save, load, and current session state
 */
class SessionManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString currentProfileName READ currentProfileName NOTIFY currentProfileChanged)
    Q_PROPERTY(QString currentLayout READ currentLayout WRITE setCurrentLayout NOTIFY currentLayoutChanged)
    Q_PROPERTY(int instanceCount READ instanceCount WRITE setInstanceCount NOTIFY instanceCountChanged)
    Q_PROPERTY(QVariantList savedProfiles READ savedProfilesAsVariant NOTIFY savedProfilesChanged)
    Q_PROPERTY(QVariantList instances READ instancesAsVariant NOTIFY instancesChanged)

public:
    explicit SessionManager(QObject *parent = nullptr);
    ~SessionManager() override;

    // Profile management
    Q_INVOKABLE bool saveProfile(const QString &name);
    Q_INVOKABLE bool loadProfile(const QString &name);
    Q_INVOKABLE bool deleteProfile(const QString &name);
    Q_INVOKABLE void refreshProfiles();

    // Current session
    Q_INVOKABLE void newSession();
    Q_INVOKABLE QVariantMap getInstanceConfig(int index) const;
    Q_INVOKABLE void setInstanceConfig(int index, const QVariantMap &config);
    Q_INVOKABLE void setInstanceUser(int index, const QString &username);
    Q_INVOKABLE void setInstanceMonitor(int index, int monitor);
    Q_INVOKABLE void setInstanceResolution(int index, int internalW, int internalH, int outputW, int outputH);
    Q_INVOKABLE void setInstanceDevices(int index, const QList<int> &devices);
    Q_INVOKABLE void setInstanceGame(int index, const QString &gameCommand);
    Q_INVOKABLE void recalculateOutputResolutions(int screenWidth, int screenHeight);

    // Property getters/setters
    QString currentProfileName() const { return m_currentProfile.name; }
    QString currentLayout() const { return m_currentProfile.layout; }
    void setCurrentLayout(const QString &layout);
    int instanceCount() const { return m_currentProfile.instances.size(); }
    void setInstanceCount(int count);

    QList<SessionProfile> savedProfiles() const { return m_savedProfiles; }
    QVariantList savedProfilesAsVariant() const;
    QVariantList instancesAsVariant() const;

    const SessionProfile &currentProfile() const { return m_currentProfile; }

Q_SIGNALS:
    void currentProfileChanged();
    void currentLayoutChanged();
    void instanceCountChanged();
    void savedProfilesChanged();
    void instancesChanged();
    void errorOccurred(const QString &message);

private:
    QString profilesDir() const;
    QString profilePath(const QString &name) const;

    SessionProfile m_currentProfile;
    QList<SessionProfile> m_savedProfiles;
};
