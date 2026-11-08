// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "SunshineConfig.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTcpServer>
#include <QUdpSocket>

namespace
{
constexpr int kSecretLength = 32;
constexpr QFileDevice::Permissions kOwnerFile = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
constexpr QFileDevice::Permissions kOwnerDir = kOwnerFile | QFileDevice::ExeOwner;

QString sanitizeIdentity(const QString &identityKey)
{
    QString result;
    result.reserve(identityKey.size());
    for (const QChar ch : identityKey) {
        const bool allowed = (ch >= QLatin1Char('a') && ch <= QLatin1Char('z'))
            || (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z')) || (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
            || ch == QLatin1Char('.') || ch == QLatin1Char('_') || ch == QLatin1Char('-');
        result.append(allowed ? ch : QLatin1Char('_'));
    }
    // "." and ".." must never name the directory itself
    if (result.isEmpty() || result == QLatin1String(".") || result == QLatin1String("..")) {
        return QStringLiteral("_desktop");
    }
    return result;
}

// A conf value ends at the end of its line; strip anything that could start another one.
QString confValue(QString value)
{
    value.remove(QLatin1Char('\n'));
    value.remove(QLatin1Char('\r'));
    return value.trimmed();
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "SunshineConfig: failed to open" << path << file.errorString();
        return false;
    }
    if (file.write(data) != data.size()) {
        qWarning() << "SunshineConfig: failed to write" << path << file.errorString();
        return false;
    }
    return true;
}

bool portSetFree(int port)
{
    for (const int offset : {-5, 0, 1, 21}) {
        QTcpServer server;
        if (!server.listen(QHostAddress::Any, static_cast<quint16>(port + offset))) {
            return false;
        }
    }
    for (const int offset : {9, 10, 11, 13}) {
        QUdpSocket socket;
        if (!socket.bind(QHostAddress::Any, static_cast<quint16>(port + offset))) {
            return false;
        }
    }
    return true;
}
}

QString SunshineConfig::apiUser()
{
    return QStringLiteral("couchplay");
}

QString SunshineConfig::displayName(const StreamSpec &spec)
{
    if (!spec.displayName.isEmpty()) {
        return spec.displayName;
    }
    return QStringLiteral("CouchPlay – Player %1 (%2)")
        .arg(spec.instanceIndex + 1)
        .arg(spec.identityKey.isEmpty() ? QStringLiteral("_desktop") : spec.identityKey);
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

QString SunshineConfig::stateDirectory(const QString &identityKey)
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/sunshine/")
        + sanitizeIdentity(identityKey);
}

QString SunshineConfig::ensureStateDirectory(const QString &identityKey)
{
    const QString dir = stateDirectory(identityKey);
    if (!QDir().mkpath(dir)) {
        qWarning() << "SunshineConfig: failed to create" << dir;
        return {};
    }
    if (!QFile::setPermissions(dir, kOwnerDir)) {
        qWarning() << "SunshineConfig: failed to restrict permissions of" << dir;
        return {};
    }
    return dir;
}

QString SunshineConfig::secret(const QString &stateDir)
{
    if (stateDir.isEmpty()) {
        return {};
    }

    const QString path = stateDir + QStringLiteral("/couchplay.secret");
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        const QString existing = QString::fromUtf8(file.readAll()).trimmed();
        file.close();
        if (!existing.isEmpty()) {
            return existing;
        }
    }

    static const QString alphabet =
        QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");
    QString generated;
    generated.reserve(kSecretLength);
    for (int i = 0; i < kSecretLength; ++i) {
        generated.append(alphabet.at(static_cast<int>(QRandomGenerator::system()->bounded(alphabet.size()))));
    }

    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "SunshineConfig: failed to create" << path << file.errorString();
        return {};
    }
    file.setPermissions(kOwnerFile);
    const QByteArray data = generated.toUtf8();
    if (file.write(data) != data.size()) {
        qWarning() << "SunshineConfig: failed to write" << path;
        return {};
    }
    return generated;
}

QString SunshineConfig::generateConfig(const StreamSpec &spec, const QString &stateDir)
{
    if (stateDir.isEmpty()) {
        qWarning() << "SunshineConfig::generateConfig: stateDir is empty";
        return {};
    }

    const QString name = displayName(spec);

    QStringList lines;
    lines << QStringLiteral("sunshine_name = %1").arg(confValue(name));
    lines << QStringLiteral("port = %1").arg(spec.port);
    lines << QStringLiteral("capture = kwin");
    lines << QStringLiteral("output_name = %1").arg(confValue(spec.outputName));
    if (!spec.sinkName.isEmpty()) {
        lines << QStringLiteral("audio_sink = %1").arg(confValue(spec.sinkName));
    }
    lines << QStringLiteral("file_state = %1/sunshine_state.json").arg(stateDir);
    lines << QStringLiteral("credentials_file = %1/credentials.json").arg(stateDir);
    lines << QStringLiteral("pkey = %1/pkey.pem").arg(stateDir);
    lines << QStringLiteral("cert = %1/cert.pem").arg(stateDir);
    lines << QStringLiteral("file_apps = %1/apps.json").arg(stateDir);
    lines << QStringLiteral("log_path = %1/sunshine.log").arg(stateDir);
    lines << QStringLiteral("keyboard = enabled");
    lines << QStringLiteral("mouse = enabled");
    lines << QStringLiteral("controller = enabled");
    lines << QStringLiteral("max_bitrate = %1").arg(spec.bitrate);
    lines << QStringLiteral("origin_web_ui_allowed = pc");
    lines << QStringLiteral("upnp = disabled");

    // 1 disables a codec; "auto" leaves Sunshine's own detection alone.
    if (spec.codec == QLatin1String("h264")) {
        lines << QStringLiteral("hevc_mode = 1");
        lines << QStringLiteral("av1_mode = 1");
    } else if (spec.codec == QLatin1String("hevc")) {
        lines << QStringLiteral("av1_mode = 1");
    }

    const QString confPath = stateDir + QStringLiteral("/sunshine.conf");
    if (!writeFile(confPath, (lines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8())) {
        return {};
    }

    const QByteArray apps =
        R"({"env":{},"apps":[{"name":"CouchPlay","image-path":"desktop.png"}]})"
        "\n";
    if (!writeFile(stateDir + QStringLiteral("/apps.json"), apps)) {
        return {};
    }

    return confPath;
}

QStringList SunshineConfig::credentialArguments(const QString &confPath, const QString &secret)
{
    return {confPath, QStringLiteral("--creds"), apiUser(), secret};
}

int SunshineConfig::allocatePort(int preferred, int attempts)
{
    for (int i = 0; i < attempts; ++i) {
        const int candidate = preferred + i * PORT_SPACING;
        // The highest port in the set is candidate + 21
        if (candidate < MIN_PORT + 5 || candidate > 65535 - 21) {
            continue;
        }
        if (portSetFree(candidate)) {
            return candidate;
        }
    }
    return -1;
}
