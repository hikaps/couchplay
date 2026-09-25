// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QTest>

#include "SteamConfigManager.h"
#include "SteamShortcutsVdf.h"

namespace {

QByteArray stringRecord(const QByteArray &key, const QByteArray &value)
{
    QByteArray result;
    result.append(char(0x01));
    result.append(key);
    result.append('\0');
    result.append(value);
    result.append('\0');
    return result;
}

QByteArray intRecord(const QByteArray &key, quint32 value)
{
    QByteArray result;
    result.append(char(0x02));
    result.append(key);
    result.append('\0');
    result.append(static_cast<char>(value & 0xff));
    result.append(static_cast<char>((value >> 8) & 0xff));
    result.append(static_cast<char>((value >> 16) & 0xff));
    result.append(static_cast<char>((value >> 24) & 0xff));
    return result;
}

QByteArray uint64Record(const QByteArray &key, quint64 value)
{
    QByteArray result;
    result.append(char(0x07));
    result.append(key);
    result.append('\0');
    for (int shift = 0; shift < 64; shift += 8) {
        result.append(static_cast<char>((value >> shift) & 0xff));
    }
    return result;
}

QByteArray objectRecord(const QByteArray &key, const QList<QByteArray> &children)
{
    QByteArray result;
    result.append(char(0x00));
    result.append(key);
    result.append('\0');
    for (const QByteArray &child : children) {
        result += child;
    }
    result.append(char(0x08));
    return result;
}

QByteArray document(const QList<QByteArray> &entries)
{
    QByteArray result = objectRecord(QByteArrayLiteral("shortcuts"), entries);
    result.append(char(0x08));
    return result;
}

QByteArray marker(char suffix)
{
    return QByteArrayLiteral("couchplay://profile/") + QByteArray(64, suffix);
}

SteamShortcut shortcut(const QByteArray &profileMarker)
{
    SteamShortcut result;
    result.appName = QStringLiteral("CouchPlay - Profile");
    result.exe = QStringLiteral("/tmp/couchplay-launcher");
    result.startDir = QStringLiteral("/tmp");
    result.icon = QStringLiteral("/tmp/couchplay.png");
    result.shortcutPath = QString::fromLatin1(profileMarker);
    result.launchOptions = QStringLiteral("--start");
    result.tags = {QStringLiteral("CouchPlay")};
    return result;
}

} // namespace

class TestSteamShortcutsVdf : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testPreservesForeignBytesAndUnknownFields()
    {
        const QByteArray unknown = objectRecord(QByteArrayLiteral("unknown-property"),
                                                {uint64Record(QByteArrayLiteral("uint64"), 0x0123456789abcdefULL),
                                                 objectRecord(QByteArrayLiteral("nested"),
                                                              {stringRecord(QByteArrayLiteral("opaque"), "value")})});
        const QByteArray foreign = objectRecord(QByteArrayLiteral("0"),
                                                {stringRecord(QByteArrayLiteral("AppName"), "Foreign game"),
                                                 stringRecord(QByteArrayLiteral("exe"), "foreign-exe"), unknown});
        const QByteArray input = document({foreign});

        QByteArray output;
        QString error;
        QVERIFY2(SteamShortcutsVdf::upsert(input, shortcut(marker('a')), &output, &error), qPrintable(error));
        QVERIFY(output.contains(foreign));
        QVERIFY(output.contains(unknown));
    }

    void testReplacesOwnedShortcutIdempotently()
    {
        const QByteArray profileMarker = marker('b');
        const QByteArray unknown = objectRecord(QByteArrayLiteral("opaque"),
                                                {uint64Record(QByteArrayLiteral("new-type"), 0xfeedfaceULL),
                                                 objectRecord(QByteArrayLiteral("deep"),
                                                              {stringRecord(QByteArrayLiteral("field"), "untouched")})});
        const QByteArray owned = objectRecord(QByteArrayLiteral("0"),
                                              {intRecord(QByteArrayLiteral("appid"), 1234),
                                               stringRecord(QByteArrayLiteral("AppName"), "old name"),
                                               stringRecord(QByteArrayLiteral("exe"), "old-exe"),
                                               stringRecord(QByteArrayLiteral("StartDir"), "old-dir"),
                                               stringRecord(QByteArrayLiteral("icon"), "user-artwork"),
                                               stringRecord(QByteArrayLiteral("ShortcutPath"), profileMarker),
                                               stringRecord(QByteArrayLiteral("LaunchOptions"), "old-options"), unknown});
        const SteamShortcut input = shortcut(profileMarker);
        const QByteArray source = document({owned});

        QByteArray first;
        QString error;
        QVERIFY2(SteamShortcutsVdf::upsert(source, input, &first, &error), qPrintable(error));
        QVERIFY(first.contains(unknown));
        QVERIFY(first.contains(stringRecord(QByteArrayLiteral("icon"), "user-artwork")));
        QVERIFY(!first.contains(stringRecord(QByteArrayLiteral("AppName"), "old name")));
        QVERIFY(!first.contains(stringRecord(QByteArrayLiteral("exe"), "old-exe")));

        QByteArray second;
        QVERIFY2(SteamShortcutsVdf::upsert(first, input, &second, &error), qPrintable(error));
        QCOMPARE(second, first);
    }

    void testSameNameForeignShortcutIsNotOverridden()
    {
        const SteamShortcut input = shortcut(marker('c'));
        const QByteArray foreign = objectRecord(QByteArrayLiteral("0"),
                                                {stringRecord(QByteArrayLiteral("AppName"), input.appName.toUtf8()),
                                                 stringRecord(QByteArrayLiteral("exe"), "foreign-exe"),
                                                 stringRecord(QByteArrayLiteral("ShortcutPath"), "steam://foreign")});
        const QByteArray source = document({foreign});

        QByteArray output;
        QString error;
        QVERIFY2(SteamShortcutsVdf::upsert(source, input, &output, &error), qPrintable(error));
        QVERIFY(output.contains(foreign));
        QCOMPARE(output.count(stringRecord(QByteArrayLiteral("AppName"), input.appName.toUtf8())), 2);
        QVERIFY(output.contains(stringRecord(QByteArrayLiteral("exe"), "foreign-exe")));
        QVERIFY(output.contains(stringRecord(QByteArrayLiteral("exe"), input.exe.toUtf8())));
    }

    void testRejectsMalformedAndUnboundedDocuments()
    {
        const SteamShortcut input = shortcut(marker('d'));
        QByteArray output = QByteArrayLiteral("sentinel");
        QString error;

        QVERIFY(!SteamShortcutsVdf::upsert({}, input, &output, &error));
        QCOMPARE(output, QByteArrayLiteral("sentinel"));
        QVERIFY(!error.isEmpty());

        const QByteArray truncated = QByteArray::fromHex("0073686f72746375747300");
        QVERIFY(!SteamShortcutsVdf::upsert(truncated, input, &output, &error));
        QCOMPARE(output, QByteArrayLiteral("sentinel"));

        QByteArray oversized(16 * 1024 * 1024 + 1, '\0');
        QVERIFY(!SteamShortcutsVdf::upsert(oversized, input, &output, &error));
        QCOMPARE(output, QByteArrayLiteral("sentinel"));

        QByteArray deeplyNested = stringRecord(QByteArrayLiteral("leaf"), "value");
        for (int depth = 0; depth < 40; ++depth) {
            deeplyNested = objectRecord(QByteArrayLiteral("nested"), {deeplyNested});
        }
        QVERIFY(!SteamShortcutsVdf::upsert(document({objectRecord(QByteArrayLiteral("0"), {deeplyNested})}),
                                           input,
                                           &output,
                                           &error));
        QCOMPARE(output, QByteArrayLiteral("sentinel"));
    }

    void testRejectsAmbiguousShortcutIdentities()
    {
        const QByteArray owned = objectRecord(QByteArrayLiteral("0"),
                                              {intRecord(QByteArrayLiteral("appid"), 1234),
                                               stringRecord(QByteArrayLiteral("ShortcutPath"), marker('f'))});
        const QByteArray duplicate = objectRecord(QByteArrayLiteral("1"),
                                                  {intRecord(QByteArrayLiteral("appid"), 1234)});
        const QByteArray invalidType = objectRecord(QByteArrayLiteral("0"),
                                                    {stringRecord(QByteArrayLiteral("appid"), "1234")});
        const QByteArray leadingZero = objectRecord(QByteArrayLiteral("00"), {});
        QByteArray output = QByteArrayLiteral("unchanged");
        QString error;
        const SteamShortcut input = shortcut(marker('f'));
        QVERIFY(!SteamShortcutsVdf::upsert(document({owned, duplicate}), input, &output, &error));
        QCOMPARE(output, QByteArrayLiteral("unchanged"));
        QVERIFY(!SteamShortcutsVdf::upsert(document({invalidType}), input, &output, &error));
        QCOMPARE(output, QByteArrayLiteral("unchanged"));
        QVERIFY(!SteamShortcutsVdf::upsert(document({leadingZero}), input, &output, &error));
        QCOMPARE(output, QByteArrayLiteral("unchanged"));
        QByteArray wideString;
        wideString.append(char(0x05));
        wideString.append("opaque\0", 7);
        wideString.append("x\0\0\0", 4);
        QVERIFY(!SteamShortcutsVdf::upsert(document({objectRecord(QByteArrayLiteral("0"), {wideString})}),
                                           input, &output, &error));
        QCOMPARE(output, QByteArrayLiteral("unchanged"));
    }

    void testUpsertIntoEmptyDocument()
    {
        QByteArray output;
        QString error;
        const SteamShortcut input = shortcut(marker('e'));
        QVERIFY2(SteamShortcutsVdf::upsert(SteamShortcutsVdf::emptyDocument(), input, &output, &error), qPrintable(error));
        QVERIFY(output.startsWith(QByteArray::fromHex("0073686f72746375747300")));
        QVERIFY(output.contains(stringRecord(QByteArrayLiteral("ShortcutPath"), input.shortcutPath.toUtf8())));
    }
};

QTEST_MAIN(TestSteamShortcutsVdf)
#include "test_steamshortcutmanager.moc"
