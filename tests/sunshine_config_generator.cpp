// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 CouchPlay Contributors
//
// Standalone generator that emits a real SunshineConfig config set
// (sunshine.conf + apps.json + credentials.json) to a directory and prints the
// sunshine.conf path on stdout. Used by the appium Sunshine integration test to
// feed real `sunshine` the exact output the app produces -- so the test catches
// any drift between SunshineConfig and what Sunshine accepts. Not a unit test.

#include <QCoreApplication>
#include <QTextStream>
#include <QVariantMap>

#include "SunshineConfig.h"

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    QTextStream err(stderr);

    if (argc < 2) {
        err << "usage: sunshine_config_generator <outputDir> [instanceIndex]\n";
        err << "   example: sunshine_config_generator /tmp/sun 0\n";
        return 1;
    }

    const QString dir = QString::fromLocal8Bit(argv[1]);
    const int instanceIndex = (argc >= 3) ? QString::fromLocal8Bit(argv[2]).toInt() : 0;

    QVariantMap config;
    config[QStringLiteral("username")] = QStringLiteral("couchplay");
    config[QStringLiteral("password")] = QStringLiteral("couchplay");
    config[QStringLiteral("streamResolution")] = QStringLiteral("1920x1080");
    config[QStringLiteral("streamBitrate")] = 20000;
    config[QStringLiteral("streamCodec")] = QStringLiteral("h264");
    config[QStringLiteral("outputName")] = QStringLiteral("0");

    const QString path = SunshineConfig::generateConfig(config, instanceIndex, dir);
    if (path.isEmpty()) {
        err << "SunshineConfig::generateConfig failed\n";
        return 2;
    }

    out << path << '\n';
    return 0;
}
