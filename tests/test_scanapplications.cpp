// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <QTest>
#include <QSignalSpy>
#include <QDebug>

#include "PresetManager.h"

class TestScanApplications : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testScanApplications();
};

void TestScanApplications::testScanApplications()
{
    PresetManager manager;

    qDebug() << "\n=== Before scan ===";
    qDebug() << "Available apps:" << manager.availableApplications().length();

    QSignalSpy spy(&manager, &PresetManager::applicationsChanged);
    manager.scanApplications();

    qDebug() << "\n=== After scan ===";
    qDebug() << "Available apps:" << manager.availableApplications().length();
    qDebug() << "Signal emitted:" << (spy.count() > 0);

    if (manager.availableApplications().isEmpty()) {
        qWarning() << "No applications found! This might be expected on some systems.";
        qWarning() << "Checking common directories...";
        QStringList paths = {
            QStringLiteral("/usr/share/applications"),
            QStringLiteral("/usr/local/share/applications"),
            QDir::homePath() + QStringLiteral("/.local/share/applications"),
        };

        for (const QString &path : paths) {
            QDir dir(path);
            if (dir.exists()) {
                qDebug() << path << "exists with" << dir.entryList({QStringLiteral("*.desktop")}, QDir::Files).length() << ".desktop files";
            } else {
                qDebug() << path << "does not exist";
            }
        }
    } else {
        qDebug() << "\n=== Found applications ===";
        int count = std::min(10, static_cast<int>(manager.availableApplications().length()));
        for (int i = 0; i < count; ++i) {
            const auto app = manager.availableApplications()[i];
            qDebug() << i << ":" << app.name << "(" << app.command << ")";
        }
        if (manager.availableApplications().length() > 10) {
            qDebug() << "... and" << (manager.availableApplications().length() - 10) << "more";
        }
    }

    QVERIFY(spy.count() >= 1);  // Should emit at least once
}

QTEST_MAIN(TestScanApplications)
#include "test_scanapplications.moc"
