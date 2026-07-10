// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include "VirtualDeviceWatcher.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileSystemWatcher>
#include <QRegularExpression>
#include <QTimer>

VirtualDeviceWatcher::VirtualDeviceWatcher(QObject *parent)
    : QObject(parent)
    , m_watcher(new QFileSystemWatcher(this))
    , m_debounceTimer(new QTimer(this))
{
    m_debounceTimer->setSingleShot(true);
    m_debounceTimer->setInterval(500); // 500ms debounce
    connect(m_debounceTimer, &QTimer::timeout, this, &VirtualDeviceWatcher::onDebounceTimeout);
}

VirtualDeviceWatcher::~VirtualDeviceWatcher()
{
    stopWatching();
}

void VirtualDeviceWatcher::startWatching(const QSet<int> &knownEventNumbers)
{
    if (m_watching) {
        stopWatching();
    }

    m_knownEventNumbers = knownEventNumbers;

    // Watch /dev/input/ directory for changes
    m_watcher->addPath(QStringLiteral("/dev/input"));
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, &VirtualDeviceWatcher::onInputDirectoryChanged);

    m_watching = true;
    qDebug() << "VirtualDeviceWatcher: Started watching /dev/input";
}

void VirtualDeviceWatcher::stopWatching()
{
    if (!m_watching) {
        return;
    }

    m_debounceTimer->stop();
    disconnect(m_watcher, &QFileSystemWatcher::directoryChanged, this, &VirtualDeviceWatcher::onInputDirectoryChanged);

    QStringList watched = m_watcher->directories();
    if (!watched.isEmpty()) {
        m_watcher->removePaths(watched);
    }

    m_knownEventNumbers.clear();
    m_watching = false;

    qDebug() << "VirtualDeviceWatcher: Stopped watching";
}

void VirtualDeviceWatcher::onInputDirectoryChanged()
{
    // Debounce rapid changes
    m_debounceTimer->start();
}

void VirtualDeviceWatcher::onDebounceTimeout()
{
    if (!m_watching) {
        return;
    }
    checkForNewDevices();
}

void VirtualDeviceWatcher::checkForNewDevices()
{
    QDir dir(QStringLiteral("/dev/input"));
    QStringList eventFiles = dir.entryList({QStringLiteral("event*")}, QDir::Files | QDir::System);

    static QRegularExpression eventRegex(QStringLiteral("event(\\d+)"));

    for (const QString &eventFile : eventFiles) {
        QRegularExpressionMatch match = eventRegex.match(eventFile);
        if (!match.hasMatch()) {
            continue;
        }

        int eventNumber = match.captured(1).toInt();

        // Skip if we already know about this device
        if (m_knownEventNumbers.contains(eventNumber)) {
            continue;
        }

        // New device found
        QString devicePath = QStringLiteral("/dev/input/%1").arg(eventFile);
        QString deviceName = getDeviceName(eventNumber);

        if (!isVirtualDevice(eventNumber, deviceName)) {
            m_knownEventNumbers.insert(eventNumber);
            continue;
        }

        qDebug() << "VirtualDeviceWatcher: Virtual device appeared:" << devicePath << deviceName;

        // Add to known set so we don't report it again
        m_knownEventNumbers.insert(eventNumber);

        // Emit signal
        Q_EMIT virtualDeviceAppeared(eventNumber, devicePath, deviceName);
    }
}

QString VirtualDeviceWatcher::getDeviceName(int eventNumber) const
{
    QString sysfsPath = QStringLiteral("/sys/class/input/event%1/device/name").arg(eventNumber);
    QFile nameFile(sysfsPath);
    if (nameFile.open(QIODevice::ReadOnly)) {
        return QString::fromLocal8Bit(nameFile.readAll().trimmed());
    }
    return QString();
}

bool VirtualDeviceWatcher::isVirtualDevice(int eventNumber, const QString &name) const
{
    QString lowerName = name.toLower();

    if (lowerName.contains(QStringLiteral("virtual")) || lowerName.contains(QStringLiteral("xtest"))
        || lowerName.contains(QStringLiteral("uinput"))) {
        return true;
    }

    if (lowerName.contains(QStringLiteral("sunshine")) ||
        lowerName.contains(QStringLiteral("passthrough"))) {
        return true;
    }

    QString physPath = QStringLiteral("/sys/class/input/event%1/device/phys").arg(eventNumber);
    QFile physFile(physPath);
    if (physFile.open(QIODevice::ReadOnly)) {
        QString phys = QString::fromLocal8Bit(physFile.readAll().trimmed()).toLower();
        if (phys.isEmpty() || phys.contains(QStringLiteral("virtual"))) {
            // Check bus type
            QString busPath = QStringLiteral("/sys/class/input/event%1/device/id/bustype").arg(eventNumber);
            QFile busFile(busPath);
            if (busFile.open(QIODevice::ReadOnly)) {
                QString bus = QString::fromLocal8Bit(busFile.readAll().trimmed()).toLower();
                if (bus == QStringLiteral("0005") || bus == QStringLiteral("0003")) {
                    return false;
                }
            }
            return true;
        }
    }

    return false;
}
