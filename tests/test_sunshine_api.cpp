// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "SunshineApiClient.h"

#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>
#include <QTcpServer>
#include <QTest>

namespace
{
const QString kSecret = QStringLiteral("s3cret");

struct RecordedRequest {
    QByteArray method;
    QByteArray path;
    QMap<QByteArray, QByteArray> headers; ///< lower-case names
    QByteArray body;
};

struct CannedResponse {
    int status = 200;
    QByteArray body;
};

QString dataFile(const char *name)
{
    return QStringLiteral(TEST_DATA_DIR "/") + QLatin1String(name);
}

/// Minimal HTTPS server answering canned responses and recording requests.
class FakeSunshine
{
public:
    explicit FakeSunshine(const char *certName, const char *keyName)
    {
        QFile keyFile(dataFile(keyName));
        QVERIFY2(keyFile.open(QIODevice::ReadOnly), "cannot open key fixture");
        const QSslKey key(&keyFile, QSsl::Rsa);
        const QList<QSslCertificate> certs = QSslCertificate::fromPath(dataFile(certName));
        QVERIFY(!certs.isEmpty());
        QVERIFY(!key.isNull());

        QSslConfiguration config = QSslConfiguration::defaultConfiguration();
        config.setLocalCertificate(certs.constFirst());
        config.setPrivateKey(key);
        config.setPeerVerifyMode(QSslSocket::VerifyNone);
        m_server.setSslConfiguration(config);

        // Pick a free port N so the client can use streamPort = N - 1
        quint16 port = 0;
        {
            QTcpServer probe;
            QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
            port = probe.serverPort();
        }
        QVERIFY(m_server.listen(QHostAddress::LocalHost, port));

        QObject::connect(&m_server, &QTcpServer::pendingConnectionAvailable, &m_server, [this]() {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                handle(socket);
            }
        });
    }

    int clientStreamPort() const
    {
        return m_server.serverPort() - 1;
    }

    void enqueue(int status, const QByteArray &body)
    {
        m_responses.append({status, body});
    }

    const QList<RecordedRequest> &requests() const
    {
        return m_requests;
    }

private:
    void handle(QTcpSocket *socket)
    {
        socket->setParent(&m_server);
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
            QByteArray &buffer = m_buffers[socket];
            buffer.append(socket->readAll());

            const int headerEnd = buffer.indexOf("\r\n\r\n");
            if (headerEnd < 0) {
                return;
            }

            RecordedRequest request;
            const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
            const QList<QByteArray> requestLine = lines.constFirst().trimmed().split(' ');
            request.method = requestLine.value(0);
            request.path = requestLine.value(1);
            for (int i = 1; i < lines.size(); ++i) {
                const int colon = lines.at(i).indexOf(':');
                if (colon > 0) {
                    request.headers.insert(lines.at(i).left(colon).trimmed().toLower(),
                                           lines.at(i).mid(colon + 1).trimmed());
                }
            }

            const int contentLength = request.headers.value("content-length", "0").toInt();
            const int bodyStart = headerEnd + 4;
            if (buffer.size() - bodyStart < contentLength) {
                return;
            }
            request.body = buffer.mid(bodyStart, contentLength);
            m_buffers.remove(socket);
            m_requests.append(request);

            const CannedResponse response = m_responses.isEmpty() ? CannedResponse{500, "{}"} : m_responses.takeFirst();
            QByteArray reply = "HTTP/1.1 " + QByteArray::number(response.status) + " Status\r\n";
            reply += "Content-Type: application/json\r\n";
            reply += "Content-Length: " + QByteArray::number(response.body.size()) + "\r\n";
            reply += "Connection: close\r\n\r\n";
            reply += response.body;
            socket->write(reply);
            socket->disconnectFromHost();
        });
    }

    QSslServer m_server;
    QList<CannedResponse> m_responses;
    QList<RecordedRequest> m_requests;
    QMap<QTcpSocket *, QByteArray> m_buffers;
};

QJsonObject bodyObject(const RecordedRequest &request)
{
    return QJsonDocument::fromJson(request.body).object();
}
}

class TestSunshineApi : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void fetchesPairingsFromPinnedCertificate();
    void rejectsDifferentCertificate();
    void sendsBasicAuthWithoutOriginOrReferer();
    void submitPinBodyModern();
    void submitPinReportsRejection();
    void notFoundSwitchesToLegacy();
    void fetchesClients();
    void unpairBody();
};

void TestSunshineApi::fetchesPairingsFromPinnedCertificate()
{
    FakeSunshine server("sunshine-test-cert.pem", "sunshine-test-key.pem");
    server.enqueue(200, R"({"pairings":[{"id":"abc","name":"Phone","address":"192.168.1.5"}]})");

    SunshineApiClient client(server.clientStreamPort(), kSecret, dataFile("sunshine-test-cert.pem"));
    QSignalSpy pairings(&client, &SunshineApiClient::pairingsFetched);
    QSignalSpy failures(&client, &SunshineApiClient::requestFailed);

    client.fetchPendingPairings();

    QTRY_COMPARE(pairings.count(), 1);
    QCOMPARE(failures.count(), 0);
    const QVariantList list = pairings.at(0).at(0).toList();
    QCOMPARE(list.size(), 1);
    const QVariantMap entry = list.at(0).toMap();
    QCOMPARE(entry.value(QStringLiteral("id")).toString(), QStringLiteral("abc"));
    QCOMPARE(entry.value(QStringLiteral("name")).toString(), QStringLiteral("Phone"));
    QCOMPARE(entry.value(QStringLiteral("address")).toString(), QStringLiteral("192.168.1.5"));
    QCOMPARE(pairings.at(0).at(1).toBool(), false);
    QVERIFY(!client.legacyPairing());

    QCOMPARE(server.requests().size(), 1);
    QCOMPARE(server.requests().at(0).method, QByteArray("GET"));
    QCOMPARE(server.requests().at(0).path, QByteArray("/api/pin"));
}

void TestSunshineApi::rejectsDifferentCertificate()
{
    // The server presents a certificate other than the one pinned by the client
    FakeSunshine server("sunshine-test-other-cert.pem", "sunshine-test-other-key.pem");
    server.enqueue(200, R"({"pairings":[{"id":"abc","name":"Phone","address":"x"}]})");

    SunshineApiClient client(server.clientStreamPort(), kSecret, dataFile("sunshine-test-cert.pem"));
    QSignalSpy pairings(&client, &SunshineApiClient::pairingsFetched);
    QSignalSpy failures(&client, &SunshineApiClient::requestFailed);

    client.fetchPendingPairings();

    QTRY_COMPARE(failures.count(), 1);
    QCOMPARE(pairings.count(), 0);
    QVERIFY(server.requests().isEmpty());
}

void TestSunshineApi::sendsBasicAuthWithoutOriginOrReferer()
{
    FakeSunshine server("sunshine-test-cert.pem", "sunshine-test-key.pem");
    server.enqueue(200, R"({"pairings":[]})");

    SunshineApiClient client(server.clientStreamPort(), kSecret, dataFile("sunshine-test-cert.pem"));
    QSignalSpy pairings(&client, &SunshineApiClient::pairingsFetched);
    client.fetchPendingPairings();
    QTRY_COMPARE(pairings.count(), 1);

    const RecordedRequest request = server.requests().at(0);
    const QByteArray expected = "Basic " + QByteArray("couchplay:s3cret").toBase64();
    QCOMPARE(request.headers.value("authorization"), expected);
    QVERIFY(!request.headers.contains("origin"));
    QVERIFY(!request.headers.contains("referer"));
}

void TestSunshineApi::submitPinBodyModern()
{
    FakeSunshine server("sunshine-test-cert.pem", "sunshine-test-key.pem");
    server.enqueue(200, R"({"status":true})");

    SunshineApiClient client(server.clientStreamPort(), kSecret, dataFile("sunshine-test-cert.pem"));
    QSignalSpy results(&client, &SunshineApiClient::pinResult);
    client.submitPin(QStringLiteral("abc"), QStringLiteral("1234"), QStringLiteral("Phone"));

    QTRY_COMPARE(results.count(), 1);
    QCOMPARE(results.at(0).at(0).toBool(), true);

    const RecordedRequest request = server.requests().at(0);
    QCOMPARE(request.method, QByteArray("POST"));
    QCOMPARE(request.path, QByteArray("/api/pin"));
    QVERIFY(request.headers.value("content-type").startsWith("application/json"));
    const QJsonObject body = bodyObject(request);
    QCOMPARE(body.size(), 3);
    QCOMPARE(body.value(QStringLiteral("pairing_id")).toString(), QStringLiteral("abc"));
    QCOMPARE(body.value(QStringLiteral("pin")).toString(), QStringLiteral("1234"));
    QCOMPARE(body.value(QStringLiteral("name")).toString(), QStringLiteral("Phone"));
}

void TestSunshineApi::submitPinReportsRejection()
{
    FakeSunshine server("sunshine-test-cert.pem", "sunshine-test-key.pem");
    server.enqueue(200, R"({"status":false})");

    SunshineApiClient client(server.clientStreamPort(), kSecret, dataFile("sunshine-test-cert.pem"));
    QSignalSpy results(&client, &SunshineApiClient::pinResult);
    client.submitPin(QStringLiteral("abc"), QStringLiteral("0000"), QStringLiteral("Phone"));

    QTRY_COMPARE(results.count(), 1);
    QCOMPARE(results.at(0).at(0).toBool(), false);
    QVERIFY(!results.at(0).at(1).toString().isEmpty());
}

void TestSunshineApi::notFoundSwitchesToLegacy()
{
    FakeSunshine server("sunshine-test-cert.pem", "sunshine-test-key.pem");
    server.enqueue(404, "{}");
    server.enqueue(200, R"({"status":true})");

    SunshineApiClient client(server.clientStreamPort(), kSecret, dataFile("sunshine-test-cert.pem"));
    QSignalSpy pairings(&client, &SunshineApiClient::pairingsFetched);
    QSignalSpy failures(&client, &SunshineApiClient::requestFailed);

    client.fetchPendingPairings();
    QTRY_COMPARE(pairings.count(), 1);
    QCOMPARE(failures.count(), 0);
    QVERIFY(pairings.at(0).at(0).toList().isEmpty());
    QCOMPARE(pairings.at(0).at(1).toBool(), true);
    QVERIFY(client.legacyPairing());

    QSignalSpy results(&client, &SunshineApiClient::pinResult);
    client.submitPin(QStringLiteral("ignored"), QStringLiteral("1234"), QStringLiteral("Phone"));
    QTRY_COMPARE(results.count(), 1);
    QCOMPARE(results.at(0).at(0).toBool(), true);

    const QJsonObject body = bodyObject(server.requests().at(1));
    QCOMPARE(body.size(), 2);
    QVERIFY(!body.contains(QStringLiteral("pairing_id")));
    QCOMPARE(body.value(QStringLiteral("pin")).toString(), QStringLiteral("1234"));
    QCOMPARE(body.value(QStringLiteral("name")).toString(), QStringLiteral("Phone"));
}

void TestSunshineApi::fetchesClients()
{
    FakeSunshine server("sunshine-test-cert.pem", "sunshine-test-key.pem");
    server.enqueue(200, R"({"named_certs":[{"name":"Phone","uuid":"u-1"},{"name":"Tablet","uuid":"u-2"}]})");

    SunshineApiClient client(server.clientStreamPort(), kSecret, dataFile("sunshine-test-cert.pem"));
    QSignalSpy clients(&client, &SunshineApiClient::clientsFetched);
    client.fetchClients();

    QTRY_COMPARE(clients.count(), 1);
    const QVariantList list = clients.at(0).at(0).toList();
    QCOMPARE(list.size(), 2);
    QCOMPARE(list.at(1).toMap().value(QStringLiteral("name")).toString(), QStringLiteral("Tablet"));
    QCOMPARE(list.at(1).toMap().value(QStringLiteral("uuid")).toString(), QStringLiteral("u-2"));
    QCOMPARE(server.requests().at(0).path, QByteArray("/api/clients/list"));
}

void TestSunshineApi::unpairBody()
{
    FakeSunshine server("sunshine-test-cert.pem", "sunshine-test-key.pem");
    server.enqueue(200, R"({"status":true})");

    SunshineApiClient client(server.clientStreamPort(), kSecret, dataFile("sunshine-test-cert.pem"));
    QSignalSpy results(&client, &SunshineApiClient::unpairResult);
    client.unpair(QStringLiteral("u-1"));

    QTRY_COMPARE(results.count(), 1);
    QCOMPARE(results.at(0).at(0).toBool(), true);

    const RecordedRequest request = server.requests().at(0);
    QCOMPARE(request.method, QByteArray("POST"));
    QCOMPARE(request.path, QByteArray("/api/clients/unpair"));
    const QJsonObject body = bodyObject(request);
    QCOMPARE(body.size(), 1);
    QCOMPARE(body.value(QStringLiteral("uuid")).toString(), QStringLiteral("u-1"));
}

QTEST_MAIN(TestSunshineApi)
#include "test_sunshine_api.moc"
