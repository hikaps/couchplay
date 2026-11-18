// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 hikaps

#include "HostStatus.h"

#include "HostProcess.h"

#include <QDir>
#include <QMap>

HostStatus::HostStatus(QObject *parent)
    : QObject(parent)
    , m_process(this)
{
    m_process.setProcessChannelMode(QProcess::SeparateChannels);
    connect(&m_process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        if (status != QProcess::NormalExit || exitCode != 0) {
            return;
        }
        const Result result = parse(QString::fromLocal8Bit(m_process.readAllStandardOutput()));
        if (result.missingTools != m_result.missingTools || result.uinputWritable != m_result.uinputWritable
            || result.legacyHelperFound != m_result.legacyHelperFound) {
            m_result = result;
            Q_EMIT changed();
        }
    });
}

QString HostStatus::probeScript()
{
    return QStringLiteral(
        "for c in bwrap dbus-run-session gamescope; do command -v \"$c\" >/dev/null 2>&1 || echo \"missing $c\"; done\n"
        "[ -w /dev/uinput ] || echo nouinput\n"
        // An empty 'couchplay' group is left behind by the migration script; only a group that still has members
        // or accounts using it as their primary group means old player accounts exist.
        "legacy_group() { g=$(getent group couchplay) || return 1; [ -n \"${g##*:}\" ] && return 0;"
        " getent passwd | awk -F: -v gid=\"$(echo \"$g\" | cut -d: -f3)\" '$4 == gid { f = 1 } END { exit !f }'; }\n"
        "if [ -e /etc/systemd/system/couchplay-helper.service ] || [ -x /usr/local/libexec/couchplay-helper ]"
        " || [ -x /usr/libexec/couchplay-helper ] || legacy_group; then echo legacy; fi\n");
}

HostStatus::Result HostStatus::parse(const QString &output)
{
    static const QMap<QString, QString> packages{
        {QStringLiteral("bwrap"), QStringLiteral("bubblewrap")},
        {QStringLiteral("dbus-run-session"), QStringLiteral("dbus")},
        {QStringLiteral("gamescope"), QStringLiteral("gamescope")},
    };

    Result result;
    const QStringList lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line == QLatin1String("nouinput")) {
            result.uinputWritable = false;
        } else if (line == QLatin1String("legacy")) {
            result.legacyHelperFound = true;
        } else if (line.startsWith(QLatin1String("missing "))) {
            const QString tool = line.mid(8).trimmed();
            if (packages.contains(tool)) {
                result.missingTools.append(QStringLiteral("%1 (package: %2)").arg(tool, packages.value(tool)));
            }
        }
    }
    return result;
}

QString HostStatus::migrationCommand() const
{
    QString user = qEnvironmentVariable("USER");
    if (user.isEmpty()) {
        user = QDir::home().dirName();
    }
    QString command = QStringLiteral("sudo bash migrate-from-helper.sh --user %1").arg(user);
    if (HostProcess::runningInFlatpak()) {
        command += QStringLiteral(" --flatpak");
    }
    return command;
}

void HostStatus::refresh()
{
    if (m_process.state() != QProcess::NotRunning) {
        return;
    }
    HostProcess::configure(m_process, QStringLiteral("sh"), {QStringLiteral("-c"), probeScript()});
    m_process.start();
}
