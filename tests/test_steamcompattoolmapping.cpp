// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include <QTest>

#include "../helper/SteamCompatToolMapping.h"

using SteamCompatToolMapping::Changes;
using SteamCompatToolMapping::Tool;

namespace
{

// Shape and whitespace as Steam writes config/config.vdf
const QByteArray kConfig = QByteArrayLiteral(
    "\"InstallConfigStore\"\n"
    "{\n"
    "\t\"Software\"\n"
    "\t{\n"
    "\t\t\"Valve\"\n"
    "\t\t{\n"
    "\t\t\t\"Steam\"\n"
    "\t\t\t{\n"
    "\t\t\t\t\"AutoUpdateWindowEnabled\"\t\t\"0\"\n"
    "\t\t\t\t\"CompatToolMapping\"\n"
    "\t\t\t\t{\n"
    "\t\t\t\t\t\"0\"\n"
    "\t\t\t\t\t{\n"
    "\t\t\t\t\t\t\"name\"\t\t\"proton_9\"\n"
    "\t\t\t\t\t\t\"config\"\t\t\"\"\n"
    "\t\t\t\t\t\t\"priority\"\t\t\"75\"\n"
    "\t\t\t\t\t}\n"
    "\t\t\t\t\t\"49520\"\n"
    "\t\t\t\t\t{\n"
    "\t\t\t\t\t\t\"name\"\t\t\"proton_9\"\n"
    "\t\t\t\t\t\t\"config\"\t\t\"\"\n"
    "\t\t\t\t\t\t\"priority\"\t\t\"250\"\n"
    "\t\t\t\t\t}\n"
    "\t\t\t\t}\n"
    "\t\t\t\t\"ConnectCache\"\n"
    "\t\t\t\t{\n"
    "\t\t\t\t\t\"7a1b\"\t\t\"secret-token\"\n"
    "\t\t\t\t}\n"
    "\t\t\t}\n"
    "\t\t}\n"
    "\t}\n"
    "}\n");

// A player's config before they ever picked a tool
const QByteArray kConfigWithoutMapping = QByteArrayLiteral(
    "\"InstallConfigStore\"\n"
    "{\n"
    "\t\"Software\"\n"
    "\t{\n"
    "\t\t\"valve\"\n"
    "\t\t{\n"
    "\t\t\t\"steam\"\n"
    "\t\t\t{\n"
    "\t\t\t\t\"ConnectCache\"\n"
    "\t\t\t\t{\n"
    "\t\t\t\t\t\"7a1b\"\t\t\"secret-token\"\n"
    "\t\t\t\t}\n"
    "\t\t\t}\n"
    "\t\t}\n"
    "\t}\n"
    "}\n");

Tool tool(const QString &name, const QString &priority = QStringLiteral("250"))
{
    return Tool{name, QString(), priority};
}

}

class TestSteamCompatToolMapping : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testReadReturnsPerAppTools()
    {
        const auto mapping = SteamCompatToolMapping::read(kConfig);
        QVERIFY(mapping.has_value());
        QCOMPARE(mapping->size(), 2);
        QCOMPARE(mapping->value(QStringLiteral("49520")), tool(QStringLiteral("proton_9")));
        QCOMPARE(mapping->value(QStringLiteral("0")), tool(QStringLiteral("proton_9"), QStringLiteral("75")));
    }

    void testReadWithoutMappingIsEmpty()
    {
        const auto mapping = SteamCompatToolMapping::read(kConfigWithoutMapping);
        QVERIFY(mapping.has_value());
        QVERIFY(mapping->isEmpty());
    }

    void testReadRejectsMalformedDocument()
    {
        QVERIFY(!SteamCompatToolMapping::read(QByteArrayLiteral("\"InstallConfigStore\"\n{\n\t\"Software\"\n\t{\n"))
                     .has_value());
        QVERIFY(!SteamCompatToolMapping::read(QByteArrayLiteral("\"InstallConfigStore\"\n{\n\"unterminated")).has_value());
    }

    void testApplyAddsMappingBlockWhenMissing()
    {
        const Changes changes{{QStringLiteral("49520"), tool(QStringLiteral("proton_9"))}};
        const auto updated = SteamCompatToolMapping::apply(kConfigWithoutMapping, changes);
        QVERIFY(updated.has_value());

        const auto mapping = SteamCompatToolMapping::read(*updated);
        QVERIFY(mapping.has_value());
        QCOMPARE(mapping->value(QStringLiteral("49520")), tool(QStringLiteral("proton_9")));
        QVERIFY(updated->contains("\"7a1b\"\t\t\"secret-token\""));
    }

    void testApplyReplacesAndRemovesOnlyChangedApps()
    {
        const Changes changes{{QStringLiteral("49520"), std::nullopt},
                              {QStringLiteral("345180"), tool(QStringLiteral("proton_experimental"))}};
        const auto updated = SteamCompatToolMapping::apply(kConfig, changes);
        QVERIFY(updated.has_value());

        const auto mapping = SteamCompatToolMapping::read(*updated);
        QVERIFY(mapping.has_value());
        QVERIFY(!mapping->contains(QStringLiteral("49520")));
        QCOMPARE(mapping->value(QStringLiteral("345180")), tool(QStringLiteral("proton_experimental")));
        // Apps outside the change set keep their entry
        QCOMPARE(mapping->value(QStringLiteral("0")), tool(QStringLiteral("proton_9"), QStringLiteral("75")));
    }

    void testApplyLeavesUnrelatedBytesUntouched()
    {
        const Changes changes{{QStringLiteral("49520"), tool(QStringLiteral("proton_experimental"))}};
        const auto updated = SteamCompatToolMapping::apply(kConfig, changes);
        QVERIFY(updated.has_value());

        const qsizetype mappingStart = kConfig.indexOf("\t\t\t\t\"CompatToolMapping\"");
        const qsizetype mappingEnd = kConfig.indexOf("\t\t\t\t\"ConnectCache\"");
        QVERIFY(updated->startsWith(kConfig.left(mappingStart)));
        QVERIFY(updated->endsWith(kConfig.mid(mappingEnd)));
    }

    void testApplyWithMatchingEntriesIsByteIdentical()
    {
        const Changes changes{{QStringLiteral("49520"), tool(QStringLiteral("proton_9"))},
                              {QStringLiteral("1794680"), std::nullopt}};
        QCOMPARE(SteamCompatToolMapping::apply(kConfig, changes), std::optional<QByteArray>(kConfig));
    }

    void testApplyEscapesQuotesInValues()
    {
        const Tool quoted{QStringLiteral("GE-Proton \"custom\""), QStringLiteral("a\\b"), QStringLiteral("250")};
        const auto updated = SteamCompatToolMapping::apply(kConfig, Changes{{QStringLiteral("49520"), quoted}});
        QVERIFY(updated.has_value());
        QCOMPARE(SteamCompatToolMapping::read(*updated)->value(QStringLiteral("49520")), quoted);
    }

    void testApplyRejectsDocumentWithoutSteamSection()
    {
        const Changes changes{{QStringLiteral("49520"), tool(QStringLiteral("proton_9"))}};
        QVERIFY(!SteamCompatToolMapping::apply(QByteArrayLiteral("\"InstallConfigStore\"\n{\n}\n"), changes)
                     .has_value());
        QVERIFY(!SteamCompatToolMapping::apply(QByteArrayLiteral("\"InstallConfigStore\"\n{\n"), changes).has_value());
    }

    void testVariantMapRoundTrip()
    {
        const Changes changes{{QStringLiteral("49520"), tool(QStringLiteral("proton_9"))},
                              {QStringLiteral("345180"), std::nullopt}};
        QCOMPARE(SteamCompatToolMapping::fromVariantMap(SteamCompatToolMapping::toVariantMap(changes)),
                 std::optional<Changes>(changes));
    }

    void testFromVariantMapRejectsUntrustedInput()
    {
        const QStringList valid{QStringLiteral("proton_9"), QString(), QStringLiteral("250")};
        QVERIFY(!SteamCompatToolMapping::fromVariantMap({{QStringLiteral("../x"), valid}}).has_value());
        QVERIFY(!SteamCompatToolMapping::fromVariantMap({{QString(), valid}}).has_value());
        QVERIFY(!SteamCompatToolMapping::fromVariantMap(
                     {{QStringLiteral("49520"), QStringList{QStringLiteral("proton_9")}}})
                     .has_value());
        QVERIFY(!SteamCompatToolMapping::fromVariantMap(
                     {{QStringLiteral("49520"),
                       QStringList{QStringLiteral("proton\n\"x\""), QString(), QStringLiteral("250")}}})
                     .has_value());
        QVERIFY(!SteamCompatToolMapping::fromVariantMap({{QStringLiteral("49520"), QStringLiteral("proton_9")}})
                     .has_value());
    }
};

QTEST_MAIN(TestSteamCompatToolMapping)
#include "test_steamcompattoolmapping.moc"
