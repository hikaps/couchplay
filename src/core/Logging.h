// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QFile>
#include <QMessageLogContext>
#include <QMutex>
#include <QLoggingCategory>

/**
 * CouchPlay Logging Categories
 *
 * Usage:
 *   #include "Logging.h"
 *   qCDebug(couchplaySteam) << "Message";
 *   qCWarning(couchplayCore) << "Warning message";
 *
 * Enable via environment variable:
 *   QT_LOGGING_RULES="couchplay.*=true" ./build/bin/couchplay
 *
 * Or use the run-debug.sh script:
 *   ./run-debug.sh
 */

// Core session management (SessionRunner, SessionManager)
Q_DECLARE_LOGGING_CATEGORY(couchplayCore)

// Steam configuration sync (SteamConfigManager)
Q_DECLARE_LOGGING_CATEGORY(couchplaySteam)

// D-Bus helper client communication
Q_DECLARE_LOGGING_CATEGORY(couchplayHelper)

// Gamescope instance management
Q_DECLARE_LOGGING_CATEGORY(couchplayGamescope)

// Device management (input devices)
Q_DECLARE_LOGGING_CATEGORY(couchplayDevices)

// Directory sharing
Q_DECLARE_LOGGING_CATEGORY(couchplaySharing)

/**
 * RotatingFileLogger — dependency-free rotating file sink for beta logging.
 *
 * Writes formatted log lines to a file; when the file exceeds maxSizeBytes it
 * is rotated to .1, .1 to .2, ..., up to maxBackups files (oldest beyond the cap
 * is dropped). Thread-safe via an internal mutex. Best-effort: write/rotation
 * failures are silently ignored so logging never crashes the app.
 *
 * Precondition: maxBackups >= 1. Callers passing < 1 are unsupported.
 */
class RotatingFileLogger {
public:
    // maxSizeBytes default 5 MiB; maxBackups default 3 (couchplay.log + .1/.2/.3 = 20 MiB ceiling).
    explicit RotatingFileLogger(QString filePath, qint64 maxSizeBytes = 5 * 1024 * 1024, int maxBackups = 3);
    ~RotatingFileLogger();
    bool open();  // mkpath parent dir, open file append; return false on any failure (app continues, no file logging)
    void write(QtMsgType type, const QMessageLogContext &context, const QString &msg); // thread-safe; rotates when threshold exceeded
    const QString &filePath() const { return m_filePath; }

private:
    void rotate();  // called under m_mutex
    QString m_filePath;
    qint64 m_maxSizeBytes;
    int m_maxBackups;
    QFile m_file;
    qint64 m_written = 0; // logical bytes written to current file (drives rotation; survives buffering)
    QMutex m_mutex;
};
