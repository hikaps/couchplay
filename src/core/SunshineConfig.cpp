// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "SunshineConfig.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>

SunshineConfig::SunshineConfig(QObject *parent)
    : QObject(parent)
{
}

SunshineConfig::~SunshineConfig() = default;

QString SunshineConfig::generateConfig(const QVariantMap &instanceConfig, int instanceIndex, const QString &configDir)
{
    if (configDir.isEmpty()) {
        qWarning() << "SunshineConfig::generateConfig: configDir is empty";
        return {};
    }

    if (instanceIndex < 0) {
        qWarning() << "SunshineConfig::generateConfig: invalid instanceIndex" << instanceIndex;
        return {};
    }

    if (!ensureConfigDirectory(configDir)) {
        return {};
    }

    const QString configContent = buildConfigContent(instanceConfig, instanceIndex, configDir);

    const QString configFilePath = configDir + QStringLiteral("/sunshine.conf");
    QFile configFile(configFilePath);
    if (!configFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "SunshineConfig::generateConfig: failed to open" << configFilePath << configFile.errorString();
        return {};
    }

    if (configFile.write(configContent.toUtf8()) == -1) {
        qWarning() << "SunshineConfig::generateConfig: failed to write" << configFilePath;
        return {};
    }
    configFile.close();

    if (!writeAppsJson(configDir)) {
        return {};
    }

    return configFilePath;
}

int SunshineConfig::calculatePort(int instanceIndex, int basePort)
{
    return basePort + (instanceIndex * PORT_SPACING);
}

QString SunshineConfig::defaultConfigDir(int instanceIndex)
{
    return QStringLiteral("/tmp/couchplay-sunshine-%1").arg(instanceIndex);
}

bool SunshineConfig::ensureConfigDirectory(const QString &configDir)
{
    QDir dir(configDir);
    if (!dir.exists()) {
        if (!dir.mkpath(QStringLiteral("."))) {
            qWarning() << "SunshineConfig: failed to create directory" << configDir;
            return false;
        }
    }

    QFile::setPermissions(configDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                                     | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                                     | QFileDevice::ReadOther | QFileDevice::ExeOther);
    return true;
}

bool SunshineConfig::writeAppsJson(const QString &configDir)
{
    const QString appsPath = configDir + QStringLiteral("/apps.json");
    QFile appsFile(appsPath);
    if (!appsFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "SunshineConfig: failed to write apps.json to" << appsPath;
        return false;
    }

    const QByteArray appsContent = R"({
  "env": {},
  "apps": [
    {
      "name": "Desktop",
      "image-path": "desktop.png"
    }
  ]
}
)";

    if (appsFile.write(appsContent) == -1) {
        qWarning() << "SunshineConfig: failed to write apps.json";
        return false;
    }
    return true;
}

QString SunshineConfig::buildConfigContent(const QVariantMap &instanceConfig, int instanceIndex, const QString &configDir)
{
    const int port = instanceConfig.value(QStringLiteral("sunshinePort")).toInt();
    const int actualPort = (port > 0) ? port : calculatePort(instanceIndex);

    const QString outputName = instanceConfig.value(QStringLiteral("outputName")).toString();
    const QString username = instanceConfig.value(QStringLiteral("username"), QStringLiteral("couchplay")).toString();
    const QString password = instanceConfig.value(QStringLiteral("password"), QStringLiteral("couchplay")).toString();
    const int bitrate = instanceConfig.value(QStringLiteral("streamBitrate")).toInt();

    QString content;
    content.reserve(512);

    content.append(QStringLiteral("port = %1\n").arg(actualPort));
    content.append(QStringLiteral("address_family = ipv4\n"));
    content.append(QStringLiteral("origin_web_ui_allowed = pc\n"));

    if (!outputName.isEmpty()) {
        content.append(QStringLiteral("output_name = %1\n").arg(outputName));
    }

    if (bitrate > 0) {
        content.append(QStringLiteral("max_bitrate = %1\n").arg(bitrate));
    }

    const QString codec = instanceConfig.value(QStringLiteral("streamCodec")).toString().toLower();
    if (!codec.isEmpty()) {
        content.append(QStringLiteral("encoder = %1\n").arg(codec));
    }

    content.append(QStringLiteral("\nusername = %1\n").arg(username));
    content.append(QStringLiteral("password = %1\n").arg(password));

    content.append(QStringLiteral("\nfile_apps = %1/apps.json\n").arg(configDir));
    content.append(QStringLiteral("credentials_file = %1/credentials.json\n").arg(configDir));
    content.append(QStringLiteral("log_file = %1/sunshine.log\n").arg(configDir));

    content.append(QStringLiteral("\ngamepad = auto\n"));
    content.append(QStringLiteral("controller = enabled\n"));
    content.append(QStringLiteral("keyboard = enabled\n"));
    content.append(QStringLiteral("mouse = enabled\n"));

    content.append(QStringLiteral("sunshine_name = CouchPlay Player %1\n").arg(instanceIndex));

    return content;
}
