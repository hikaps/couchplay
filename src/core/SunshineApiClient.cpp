// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "SunshineApiClient.h"

#include "SunshineConfig.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>
#include <QVariantMap>

namespace
{
constexpr int kTransferTimeoutMs = 10000;
constexpr char kCertMismatchProperty[] = "certMismatch";

int httpStatus(QNetworkReply *reply)
{
    return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

QJsonObject parseObject(const QByteArray &body)
{
    return QJsonDocument::fromJson(body).object();
}
}

SunshineApiClient::SunshineApiClient(int streamPort, const QString &secret, const QString &certPath, QObject *parent)
    : QObject(parent)
    , m_network(this)
    , m_certPath(certPath)
{
    m_baseUrl.setScheme(QStringLiteral("https"));
    m_baseUrl.setHost(QStringLiteral("127.0.0.1"));
    m_baseUrl.setPort(streamPort + 1);

    const QByteArray credentials = (SunshineConfig::apiUser() + QLatin1Char(':') + secret).toUtf8();
    m_authorization = "Basic " + credentials.toBase64();
}

bool SunshineApiClient::legacyPairing() const
{
    return m_legacy;
}

QNetworkRequest SunshineApiClient::makeRequest(const QString &path, bool json) const
{
    QUrl url = m_baseUrl;
    url.setPath(path);
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", m_authorization);
    request.setRawHeader("Accept", "application/json");
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setTransferTimeout(kTransferTimeoutMs);
    if (json) {
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    }
    return request;
}

QNetworkReply *SunshineApiClient::get(const QString &path)
{
    QNetworkReply *reply = m_network.get(makeRequest(path, false));
    connect(reply, &QNetworkReply::sslErrors, this, [this, reply]() {
        onSslErrors(reply);
    });
    return reply;
}

QNetworkReply *SunshineApiClient::post(const QString &path, const QByteArray &body)
{
    QNetworkReply *reply = m_network.post(makeRequest(path, true), body);
    connect(reply, &QNetworkReply::sslErrors, this, [this, reply]() {
        onSslErrors(reply);
    });
    return reply;
}

void SunshineApiClient::onSslErrors(QNetworkReply *reply)
{
    const QSslCertificate peer = reply->sslConfiguration().peerCertificate();
    const QList<QSslCertificate> pinned = QSslCertificate::fromPath(m_certPath);
    if (!peer.isNull() && !pinned.isEmpty() && pinned.constFirst() == peer) {
        reply->ignoreSslErrors();
        return;
    }
    reply->setProperty(kCertMismatchProperty, true);
    reply->abort();
}

bool SunshineApiClient::failed(QNetworkReply *reply, bool bodyAllowed)
{
    if (reply->error() == QNetworkReply::NoError) {
        return false;
    }

    const int status = httpStatus(reply);
    QString message;
    if (reply->property(kCertMismatchProperty).toBool()) {
        message = QStringLiteral("The Sunshine certificate does not match the one stored for this player");
    } else if (status == 401 || status == 403) {
        message = QStringLiteral("Sunshine rejected the CouchPlay credentials");
    } else if (bodyAllowed && status != 0 && reply->bytesAvailable() > 0) {
        return false;
    } else {
        message = reply->errorString();
    }
    Q_EMIT requestFailed(message);
    return true;
}

void SunshineApiClient::fetchPendingPairings()
{
    QNetworkReply *reply = get(QStringLiteral("/api/pin"));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        const int status = httpStatus(reply);
        if (status == 404 || status == 405) {
            m_legacy = true;
            Q_EMIT pairingsFetched({}, true);
            return;
        }
        if (failed(reply, false)) {
            return;
        }

        m_legacy = false;
        QVariantList pairings;
        const QJsonArray entries = parseObject(reply->readAll()).value(QLatin1String("pairings")).toArray();
        for (const QJsonValue &value : entries) {
            const QJsonObject object = value.toObject();
            QVariantMap pairing;
            pairing[QStringLiteral("id")] = object.value(QLatin1String("id")).toVariant().toString();
            pairing[QStringLiteral("name")] = object.value(QLatin1String("name")).toString();
            pairing[QStringLiteral("address")] = object.value(QLatin1String("address")).toString();
            pairings.append(pairing);
        }
        Q_EMIT pairingsFetched(pairings, false);
    });
}

void SunshineApiClient::submitPin(const QString &pairingId, const QString &pin, const QString &clientName)
{
    QJsonObject body;
    if (!m_legacy) {
        body.insert(QStringLiteral("pairing_id"), pairingId);
    }
    body.insert(QStringLiteral("pin"), pin);
    body.insert(QStringLiteral("name"), clientName);

    QNetworkReply *reply = post(QStringLiteral("/api/pin"), QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (failed(reply, true)) {
            return;
        }

        const QJsonObject object = parseObject(reply->readAll());
        const bool ok = object.value(QLatin1String("status")).toBool(false);
        QString message;
        if (!ok) {
            message = object.value(QLatin1String("error")).toString();
            if (message.isEmpty()) {
                message = object.value(QLatin1String("message")).toString();
            }
            if (message.isEmpty()) {
                message = QStringLiteral("Sunshine rejected the PIN");
            }
        }
        Q_EMIT pinResult(ok, message);
    });
}

void SunshineApiClient::fetchClients()
{
    QNetworkReply *reply = get(QStringLiteral("/api/clients/list"));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (failed(reply, false)) {
            return;
        }

        QVariantList clients;
        const QJsonArray entries = parseObject(reply->readAll()).value(QLatin1String("named_certs")).toArray();
        for (const QJsonValue &value : entries) {
            const QJsonObject object = value.toObject();
            QVariantMap client;
            client[QStringLiteral("name")] = object.value(QLatin1String("name")).toString();
            client[QStringLiteral("uuid")] = object.value(QLatin1String("uuid")).toString();
            clients.append(client);
        }
        Q_EMIT clientsFetched(clients);
    });
}

void SunshineApiClient::unpair(const QString &uuid)
{
    QJsonObject body;
    body.insert(QStringLiteral("uuid"), uuid);

    QNetworkReply *reply = post(QStringLiteral("/api/clients/unpair"), QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (failed(reply, true)) {
            return;
        }
        Q_EMIT unpairResult(parseObject(reply->readAll()).value(QLatin1String("status")).toBool(false));
    });
}
