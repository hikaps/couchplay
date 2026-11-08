// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#pragma once

#include <QList>
#include <QMap>
#include <QString>

/**
 * @brief Per-player PulseAudio/PipeWire null sinks that Sunshine captures
 *
 * Runs pactl on the host (via HostProcess) as the desktop user. prepare() records the current
 * default sink and loads one null sink per streaming player; teardown() unloads them and restores
 * the default sink (Sunshine switches it while a client streams).
 */
class StreamAudio
{
public:
    /** Records the current default sink and loads one null sink per streaming player. */
    bool prepare(const QList<int> &streamingIndices, QString *error = nullptr);

    /** "couchplay-stream-p<index+1>" for prepared indices, else empty. */
    QString streamSink(int instanceIndex) const;

    /** The recorded default sink (may be empty). */
    QString defaultSink() const;

    /** Unload modules and restore the default sink; idempotent. */
    void teardown();

    /** Reloads any prepared sink that is no longer present (pipewire-pulse restarted). */
    void ensureSinks();

    bool isActive() const;

    /** Test seam: replaces the "pactl" executable. */
    void setProgramOverride(const QString &program);

private:
    bool runPactl(const QStringList &args, QString *output) const;
    bool loadSink(int index, uint *moduleIndex);

    QString m_programOverride;
    QString m_defaultSink;
    QMap<int, QString> m_sinks; ///< instance index -> sink name
    QMap<int, uint> m_modules; ///< instance index -> loaded module index
    bool m_active = false;
};
