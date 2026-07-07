// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "Logging.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>

Q_LOGGING_CATEGORY(couchplayCore, "couchplay.core", QtWarningMsg)
Q_LOGGING_CATEGORY(couchplaySteam, "couchplay.steam", QtWarningMsg)
Q_LOGGING_CATEGORY(couchplayHelper, "couchplay.helper", QtWarningMsg)
Q_LOGGING_CATEGORY(couchplayGamescope, "couchplay.gamescope", QtWarningMsg)
Q_LOGGING_CATEGORY(couchplayDevices, "couchplay.devices", QtWarningMsg)
Q_LOGGING_CATEGORY(couchplaySharing, "couchplay.sharing", QtWarningMsg)

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
    if (m_written >= m_maxSizeBytes) {
        m_file.flush();
        rotate();
    }
}

void RotatingFileLogger::rotate()
{
    // Held under m_mutex. Shift .1->.2->...->maxBackups, dropping the oldest.
    m_file.close();
    for (int i = m_maxBackups; i > 1; --i) {
        const QString older = m_filePath + QLatin1Char('.') + QString::number(i);
        const QString newer = m_filePath + QLatin1Char('.') + QString::number(i - 1);
        if (i == m_maxBackups) {
            QFile::remove(older); // drop the oldest slot beyond the cap
        }
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

