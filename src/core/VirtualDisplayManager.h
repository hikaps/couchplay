// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 hikaps

#pragma once

#include <QHash>
#include <QObject>
#include <QRect>
#include <QSet>
#include <QSize>
#include <QString>
#include <QTimer>
#include <functional>
#include <memory>

class QProcess;

/**
 * @brief Creates KWin virtual outputs, one `couchplay-vdisplay` process per output
 *
 * Each process holds a zkde_screencast virtual output open; terminating it removes the
 * output. Processes are started on the host as the desktop user through HostProcess.
 */
class VirtualDisplayManager : public QObject
{
    Q_OBJECT

public:
    /// Resolves a newly created output: fills the name and geometry, returns false while it is not visible yet.
    using ScreenLookup = std::function<bool(const QString &expectedName,
                                            const QSize &size,
                                            const QSet<QString> &knownBefore,
                                            QString *name,
                                            QRect *geometry)>;

    explicit VirtualDisplayManager(QObject *parent = nullptr);
    ~VirtualDisplayManager() override;

    /// Start a virtual output of @p size for @p instanceIndex; reports through ready()/failed().
    void create(int instanceIndex, const QSize &size);

    /// Terminate every process (kill after a short grace) and forget all outputs.
    void destroyAll();

    bool hasDisplays() const { return !m_displays.isEmpty(); }
    QString outputName(int instanceIndex) const;

    /// KWin's name for the output created with `--name couchplay-p<index+1>`.
    static QString expectedOutputName(int instanceIndex);

    // Test seams
    void setProgramOverride(const QString &program) { m_programOverride = program; }
    void setScreenLookup(ScreenLookup lookup) { m_screenLookup = std::move(lookup); }
    void setTimeoutMs(int ms) { m_timeoutMs = ms; }

Q_SIGNALS:
    void ready(int instanceIndex, const QString &outputName, const QRect &geometry);
    void failed(int instanceIndex, const QString &message);

private:
    struct Display {
        QProcess *process = nullptr;
        QSize size;
        QSet<QString> knownBefore;
        QByteArray buffer;
        QString stderrText;
        QString outputName;
        bool nodeReported = false;
        bool finished = false; // ready or failed already emitted
        QTimer *pollTimer = nullptr;
        QTimer *timeoutTimer = nullptr;
    };

    void onStdout(int index);
    void poll(int index);
    void fail(int index, const QString &message);
    void finish(Display &display);

    QHash<int, Display> m_displays;
    QString m_programOverride;
    ScreenLookup m_screenLookup;
    int m_timeoutMs = 10000;
};
