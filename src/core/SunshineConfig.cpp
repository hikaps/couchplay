// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "SunshineConfig.h"

#include <QCryptographicHash>
#include <algorithm>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>

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

    const QString username = instanceConfig.value(QStringLiteral("username"), QStringLiteral("couchplay")).toString();
    const QString password = instanceConfig.value(QStringLiteral("password"), QStringLiteral("couchplay")).toString();
    if (!writeCredentialsFile(configDir, username, password)) {
        return {};
    }

    return configFilePath;
}

int SunshineConfig::calculatePort(int instanceIndex, int basePort)
{
    int port = basePort + (instanceIndex * PORT_SPACING);
    if (port > MAX_PORT) {
        return MAX_PORT;
    }
    if (port < MIN_PORT) {
        return MIN_PORT;
    }
    return port;
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

bool SunshineConfig::writeCredentialsFile(const QString &configDir, const QString &username, const QString &password)
{
    const QString chars = QStringLiteral("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
    QString salt;
    salt.reserve(16);
    for (int i = 0; i < 16; ++i) {
        salt.append(chars[QRandomGenerator::global()->bounded(chars.size())]);
    }

    QByteArray hash = QCryptographicHash::hash((password + salt).toUtf8(), QCryptographicHash::Sha256);
    std::reverse(hash.begin(), hash.end());
    const QString hashHex = QString::fromLatin1(hash.toHex()).toUpper();

    QJsonObject creds;
    creds[QStringLiteral("username")] = username;
    creds[QStringLiteral("salt")] = salt;
    creds[QStringLiteral("password")] = hashHex;

    QJsonDocument doc(creds);

    const QString credsPath = configDir + QStringLiteral("/credentials.json");
    QFile file(credsPath);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "SunshineConfig: Failed to open credentials file" << credsPath;
        return false;
    }
    file.write(doc.toJson(QJsonDocument::Compact));
    file.close();
    return true;
}

QString SunshineConfig::buildConfigContent(const QVariantMap &instanceConfig, int instanceIndex, const QString &configDir)
{
    const int port = instanceConfig.value(QStringLiteral("sunshinePort")).toInt();
    const int actualPort = (port > 0)
        ? qBound(MIN_PORT, port, MAX_PORT)
        : calculatePort(instanceIndex);

    const QString outputName = instanceConfig.value(QStringLiteral("outputName")).toString();
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

    content.append(QStringLiteral("\nfile_apps = %1/apps.json\n").arg(configDir));
    content.append(QStringLiteral("credentials_file = %1/credentials.json\n").arg(configDir));
    content.append(QStringLiteral("log_path = %1/sunshine.log\n").arg(configDir));

    const QString sink = instanceConfig.value(QStringLiteral("sink")).toString();
    if (!sink.isEmpty()) {
        content.append(QStringLiteral("sink = %1\n").arg(sink));
    }

    content.append(QStringLiteral("\ngamepad = auto\n"));
    content.append(QStringLiteral("controller = enabled\n"));
    content.append(QStringLiteral("keyboard = enabled\n"));
    content.append(QStringLiteral("mouse = enabled\n"));

    content.append(QStringLiteral("sunshine_name = CouchPlay Player %1\n").arg(instanceIndex));

    return content;
}
