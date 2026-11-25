// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "Logging.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSysInfo>

Q_LOGGING_CATEGORY(couchplayCore, "couchplay.core", QtWarningMsg)
Q_LOGGING_CATEGORY(couchplaySteam, "couchplay.steam", QtWarningMsg)
Q_LOGGING_CATEGORY(couchplayGamescope, "couchplay.gamescope", QtWarningMsg)
Q_LOGGING_CATEGORY(couchplayDevices, "couchplay.devices", QtWarningMsg)
Q_LOGGING_CATEGORY(couchplaySharing, "couchplay.sharing", QtWarningMsg)

namespace LogRedactor {
namespace {
struct Rule {
    QRegularExpression regex;
    QString replacement;
};

QMutex s_playersMutex;
QList<Rule> s_playerRules;

// Whole-name match: player names may contain '-' and '_', so \b would split them.
QRegularExpression wholeWord(const QString &literal)
{
    return QRegularExpression(QStringLiteral("(?<![A-Za-z0-9_-])") + QRegularExpression::escape(literal)
                              + QStringLiteral("(?![A-Za-z0-9_-])"));
}

// The desktop user and host never change while the app runs
const QList<Rule> &identityRules()
{
    static const QList<Rule> rules = [] {
        QList<Rule> list;
        const auto add = [&list](const QString &literal, const QString &replacement) {
            if (literal.size() >= 3 && literal != QLatin1String("localhost")) {
                list.append({wholeWord(literal), replacement});
            }
        };
        add(QSysInfo::machineHostName(), QStringLiteral("<host>"));
        add(qEnvironmentVariable("USER"), QStringLiteral("<user>"));
        // Only a home like /home/<name> or /var/home/<name> names the user; a bare /home (isolated runs) does not
        QDir home = QDir::home();
        const QString homeName = home.dirName();
        home.cdUp();
        if (home.dirName() == QLatin1String("home") || home.dirName() == QLatin1String("Users")) {
            add(homeName, QStringLiteral("<user>"));
        }
        return list;
    }();
    return rules;
}

const QList<Rule> &patternRules()
{
    static const QList<Rule> rules = {
        // SteamID64, and the 32-bit account id Steam uses in userdata/<id> and in our own "Steam ID <id>" lines
        {QRegularExpression(QStringLiteral("\\b7656119\\d{10}\\b")), QStringLiteral("<steamid>")},
        {QRegularExpression(QStringLiteral("(?<=userdata/)\\d+")), QStringLiteral("<steamid>")},
        {QRegularExpression(QStringLiteral("(Steam ID:?\\s*\"?)\\d+")), QStringLiteral("\\1<steamid>")},
        // A controller's stable id is its serial or Bluetooth address
        {QRegularExpression(QStringLiteral("(stableId:?\\s*)(?:\"[^\"]*\"|[^\\s)]+)")),
         QStringLiteral("\\1<device>")},
        {QRegularExpression(QStringLiteral("\\b(?:[0-9A-Fa-f]{1,4}:){7}[0-9A-Fa-f]{1,4}\\b")), QStringLiteral("<ip>")},
        {QRegularExpression(QStringLiteral("(?<![0-9A-Fa-f:])(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}(?![0-9A-Fa-f:])")),
         QStringLiteral("<mac>")},
        {QRegularExpression(QStringLiteral("\\b(?!127\\.|0\\.0\\.0\\.0\\b)(?:\\d{1,3}\\.){3}\\d{1,3}\\b")),
         QStringLiteral("<ip>")},
    };
    return rules;
}
}

QString redact(const QString &message)
{
    QString text = message;
    for (const Rule &rule : identityRules()) {
        text.replace(rule.regex, rule.replacement);
    }
    {
        QMutexLocker lock(&s_playersMutex);
        for (const Rule &rule : std::as_const(s_playerRules)) {
            text.replace(rule.regex, rule.replacement);
        }
    }
    for (const Rule &rule : patternRules()) {
        text.replace(rule.regex, rule.replacement);
    }
    return text;
}

void setPlayerNames(const QStringList &names)
{
    QList<Rule> rules;
    for (const QString &name : names) {
        if (name.size() >= 3) {
            const QByteArray digest = QCryptographicHash::hash(name.toUtf8(), QCryptographicHash::Sha256);
            rules.append({wholeWord(name), QStringLiteral("player-") + QString::fromLatin1(digest.toHex().left(4))});
        }
    }
    QMutexLocker lock(&s_playersMutex);
    s_playerRules = std::move(rules);
}
}

RotatingFileLogger::RotatingFileLogger(QString filePath, qint64 maxSizeBytes, int maxBackups)
    : m_filePath(std::move(filePath))
    , m_maxSizeBytes(maxSizeBytes)
    , m_maxBackups(maxBackups)
{
}

RotatingFileLogger::~RotatingFileLogger()
{
    QMutexLocker lock(&m_mutex);
    m_file.close();
}

bool RotatingFileLogger::open()
{
    QMutexLocker lock(&m_mutex);
    const QString parentDir = QFileInfo(m_filePath).absolutePath();
    if (!QDir().mkpath(parentDir)) {
        return false;
    }
    m_file.setFileName(m_filePath);
    if (!m_file.open(QIODevice::Append | QIODevice::Text)) {
        return false;
    }
    m_written = m_file.size(); // account for an existing log file's size
    return true;
}

void RotatingFileLogger::write(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    QMutexLocker lock(&m_mutex);
    if (!m_file.isOpen()) {
        return;
    }
    const QString line = qFormatLogMessage(type, context, msg) + QLatin1Char('\n');
    const qint64 n = m_file.write(line.toUtf8());
    if (n > 0) {
        m_written += n;
    }
    // Flush every message: the file is for diagnosing crashes and hangs, and a buffered tail would be lost with them.
    m_file.flush();
    if (m_written >= m_maxSizeBytes) {
        rotate();
    }
}

void RotatingFileLogger::rotate()
{
    // Held under m_mutex. Vacate the oldest slot first, then shift .1->.2->...->maxBackups,
    // and move the active file into the vacated .1 slot. Vacating before filling keeps
    // QFile::rename reliable (it never overwrites) and the cap correct even for maxBackups==1
    // (where the shift loop is a no-op and the single .1 backup is simply replaced each rotation).
    m_file.close();
    QFile::remove(m_filePath + QLatin1Char('.') + QString::number(m_maxBackups)); // drop the oldest slot
    for (int i = m_maxBackups; i > 1; --i) {
        const QString older = m_filePath + QLatin1Char('.') + QString::number(i);
        const QString newer = m_filePath + QLatin1Char('.') + QString::number(i - 1);
        QFile::rename(newer, older);
    }
    // Active file -> .1, then reopen fresh.
    QFile::rename(m_filePath, m_filePath + QLatin1String(".1"));
    m_file.setFileName(m_filePath);
    m_written = 0;
    if (!m_file.open(QIODevice::Append | QIODevice::Text)) {
        // Reopen failed (e.g. external deletion race); leave closed — subsequent writes are safe no-ops.
        return;
    }
}

