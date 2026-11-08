// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>

class QNetworkReply;
class QNetworkRequest;

/**
 * @brief Client for the parts of Sunshine's web API used for Moonlight pairing
 *
 * Talks to https://127.0.0.1:<streamPort + 1> with HTTP Basic auth (user "couchplay") and never
 * sends Origin/Referer, so Sunshine skips CSRF protection. Sunshine serves a self-signed
 * certificate; TLS errors are ignored only when the peer certificate equals the one loaded from
 * the Sunshine state directory (cert.pem). Any other certificate aborts the request.
 *
 * Results arrive by signal; network and certificate problems emit requestFailed().
 */
class SunshineApiClient : public QObject
{
    Q_OBJECT

public:
    /** @param streamPort the Sunshine `port` value (the API listens on streamPort + 1) */
    SunshineApiClient(int streamPort, const QString &secret, const QString &certPath, QObject *parent = nullptr);

    /** True once GET /api/pin answered 404/405 (Sunshine without pairing ids). Defaults to false. */
    bool legacyPairing() const;

    void fetchPendingPairings();
    void submitPin(const QString &pairingId, const QString &pin, const QString &clientName);
    void fetchClients();
    void unpair(const QString &uuid);

Q_SIGNALS:
    /** @param pairings maps {id, name, address}; empty in legacy mode */
    void pairingsFetched(const QVariantList &pairings, bool legacy);
    void pinResult(bool ok, const QString &message);
    /** @param clients maps {name, uuid} */
    void clientsFetched(const QVariantList &clients);
    void unpairResult(bool ok);
    void requestFailed(const QString &message);

private:
    QNetworkRequest makeRequest(const QString &path, bool json) const;
    QNetworkReply *get(const QString &path);
    QNetworkReply *post(const QString &path, const QByteArray &body);
    void onSslErrors(QNetworkReply *reply);
    /** Reports transport/credential errors via requestFailed(); true when the reply is unusable.
     *  With @p bodyAllowed an HTTP error status that carries a body is left to the caller. */
    bool failed(QNetworkReply *reply, bool bodyAllowed);

    QNetworkAccessManager m_network;
    QUrl m_baseUrl;
    QByteArray m_authorization;
    QString m_certPath;
    bool m_legacy = false;
};
