// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors

#include <QDebug>
#include <QObject>
#include <QTest>

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "../src/core/Logging.h"

class TestLogging : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testRotation();
    void testOpenFailureNoCrash();
    void testAppendToExisting();
};

void TestLogging::testRotation()
{
    // Small maxSize forces rotation quickly; "sub/" verifies mkpath of the parent dir.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + QStringLiteral("/sub/couchplay.log");

    RotatingFileLogger logger(path, 200, 3);
    QVERIFY(logger.open());

    // Write well past the 200-byte threshold so multiple rotations occur.
    for (int i = 0; i < 50; ++i) {
        logger.write(QtInfoMsg, {}, QStringLiteral("line %1 padding to fill the 200-byte bucket").arg(i));
    }

    // Active file exists and at least one backup was produced.
    QVERIFY(QFile::exists(path));
    QVERIFY(QFile::exists(path + QStringLiteral(".1")));
    // Cap is enforced: a 4th backup must never appear.
    QVERIFY(!QFile::exists(path + QStringLiteral(".4")));
}

void TestLogging::testOpenFailureNoCrash()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // Create a regular file that will be treated as the parent directory.
    QFile blocker(dir.filePath(QStringLiteral("blocker")));
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    blocker.close();

    // Parent path is a file, so mkpath fails and open() returns false.
    RotatingFileLogger logger(dir.filePath(QStringLiteral("blocker")) + QStringLiteral("/couchplay.log"));
    QVERIFY(!logger.open());

    // Writing to a logger that failed to open must be a safe no-op.
    logger.write(QtInfoMsg, {}, QStringLiteral("this must not crash"));
    QVERIFY(true);
}

void TestLogging::testAppendToExisting()
{
    // A pre-existing log file's size is accounted for, so rotation can still
    // trigger on a session that resumes a near-full file.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + QStringLiteral("/couchplay.log");

    // Seed a 150-byte file (just under the 200-byte cap).
    {
        QFile seed(path);
        QVERIFY(seed.open(QIODevice::WriteOnly));
        seed.write(QByteArray(150, 'x'));
    }

    RotatingFileLogger logger(path, 200, 3);
    QVERIFY(logger.open());

    // One modest write should push it over the cap and trigger a rotation.
    logger.write(QtInfoMsg, {}, QStringLiteral("triggering rotation on top of an existing near-full log file"));

    QVERIFY(QFile::exists(path));
    QVERIFY(QFile::exists(path + QStringLiteral(".1")));
}

QTEST_MAIN(TestLogging)
#include "test_logging.moc"
