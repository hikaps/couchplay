// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 hikaps

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h>

#include "../src/core/PlayerFiles.h"

namespace
{
bool writeFile(const QString &path, const QByteArray &content, QFileDevice::Permissions perms = {})
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(content);
    file.close();
    return perms ? QFile::setPermissions(path, perms) : true;
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool makeLink(const QString &target, const QString &linkPath)
{
    QDir().mkpath(QFileInfo(linkPath).absolutePath());
    return QFile::link(target, linkPath);
}
}

class TestPlayerFiles : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void copyTreeCopiesNestedAndHidden();
    void copyTreeDoesNotWriteThroughDestinationSymlink();
    void hasSymlinkUnderDetectsLinkedComponents();
    void removeTreeDoesNotFollowSymlinksAndHandlesReadOnlyDirs();
    void replaceTreeOverlaysStagingOverReadOnlyDirs();
    void replaceTreeRestoresInterruptedBackupBeforeFailing();
    void copyTreeMergesOverExisting();
    void copyTreePreservesModes();
    void copyTreeKeepsSymlinksAsLinks();
    void copyTreeReplacesFileWithSymlink();
    void copyTreeMissingSourceFails();
    void replaceTreeCreatesDestination();
    void replaceTreeReplacesExistingDestination();
    void replaceTreeOverlaysStaging();
    void replaceTreeIgnoresMissingStaging();
    void replaceTreeKeepsSymlinks();
    void replaceTreeFailureKeepsOldCopy();
    void replaceTreeCleansStaleTemporaries();
    void slugIsStable();
    void slugRelativeToHome();
    void slugOutsideHome();
    void slugIsUniqueForCollidingFlattening();
};

void TestPlayerFiles::copyTreeCopiesNestedAndHidden()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString src = tmp.filePath(QStringLiteral("src"));
    const QString dst = tmp.filePath(QStringLiteral("dst"));
    QVERIFY(writeFile(src + QStringLiteral("/a.txt"), "A"));
    QVERIFY(writeFile(src + QStringLiteral("/.hidden"), "H"));
    QVERIFY(writeFile(src + QStringLiteral("/d1/d2/deep.txt"), "D"));
    QVERIFY(QDir().mkpath(src + QStringLiteral("/empty")));

    QString error;
    QVERIFY2(PlayerFiles::copyTreeContents(src, dst, &error), qPrintable(error));
    QVERIFY(error.isEmpty());
    QCOMPARE(readFile(dst + QStringLiteral("/a.txt")), QByteArray("A"));
    QCOMPARE(readFile(dst + QStringLiteral("/.hidden")), QByteArray("H"));
    QCOMPARE(readFile(dst + QStringLiteral("/d1/d2/deep.txt")), QByteArray("D"));
    QVERIFY(QFileInfo(dst + QStringLiteral("/empty")).isDir());
    // Contents, not the directory itself, are copied.
    QVERIFY(!QFileInfo::exists(dst + QStringLiteral("/src")));
}

void TestPlayerFiles::copyTreeMergesOverExisting()
{
    QTemporaryDir tmp;
    const QString src = tmp.filePath(QStringLiteral("src"));
    const QString dst = tmp.filePath(QStringLiteral("dst"));
    QVERIFY(writeFile(src + QStringLiteral("/shared.txt"), "new"));
    QVERIFY(writeFile(src + QStringLiteral("/sub/n.txt"), "n"));
    QVERIFY(writeFile(dst + QStringLiteral("/shared.txt"), "old"));
    QVERIFY(writeFile(dst + QStringLiteral("/keep.txt"), "keep"));
    QVERIFY(writeFile(dst + QStringLiteral("/sub/existing.txt"), "e"));

    QVERIFY(PlayerFiles::copyTreeContents(src, dst));
    QCOMPARE(readFile(dst + QStringLiteral("/shared.txt")), QByteArray("new"));
    QCOMPARE(readFile(dst + QStringLiteral("/keep.txt")), QByteArray("keep"));
    QCOMPARE(readFile(dst + QStringLiteral("/sub/existing.txt")), QByteArray("e"));
    QCOMPARE(readFile(dst + QStringLiteral("/sub/n.txt")), QByteArray("n"));
}

void TestPlayerFiles::copyTreePreservesModes()
{
    QTemporaryDir tmp;
    const QString src = tmp.filePath(QStringLiteral("src"));
    const QString dst = tmp.filePath(QStringLiteral("dst"));
    // Qt reports the owner bits twice (Owner and User).
    const QFileDevice::Permissions exec = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
        | QFileDevice::ReadUser | QFileDevice::WriteUser | QFileDevice::ExeUser | QFileDevice::ReadGroup
        | QFileDevice::ExeGroup;
    QVERIFY(writeFile(src + QStringLiteral("/run.sh"), "#!/bin/sh\n", exec));
    const QFileDevice::Permissions dirPerms = exec;
    QVERIFY(writeFile(src + QStringLiteral("/bin/tool"), "x"));
    QVERIFY(QFile::setPermissions(src + QStringLiteral("/bin"), dirPerms));

    QVERIFY(PlayerFiles::copyTreeContents(src, dst));
    QCOMPARE(QFileInfo(dst + QStringLiteral("/run.sh")).permissions(), exec);
    QCOMPARE(QFileInfo(dst + QStringLiteral("/bin")).permissions(), dirPerms);
}

void TestPlayerFiles::copyTreeDoesNotWriteThroughDestinationSymlink()
{
    QTemporaryDir tmp;
    const QString src = tmp.filePath(QStringLiteral("src"));
    const QString dst = tmp.filePath(QStringLiteral("dst"));
    const QString outside = tmp.filePath(QStringLiteral("outside"));
    QVERIFY(writeFile(outside + QStringLiteral("/slot1"), "host"));
    QVERIFY(writeFile(src + QStringLiteral("/saves/slot1"), "player"));
    QVERIFY(QDir().mkpath(dst));
    QVERIFY(makeLink(outside, dst + QStringLiteral("/saves")));

    QVERIFY(PlayerFiles::copyTreeContents(src, dst));
    QFile host(outside + QStringLiteral("/slot1"));
    QVERIFY(host.open(QIODevice::ReadOnly));
    QCOMPARE(host.readAll(), QByteArray("host"));
    QVERIFY(!QFileInfo(dst + QStringLiteral("/saves")).isSymLink());
}

void TestPlayerFiles::hasSymlinkUnderDetectsLinkedComponents()
{
    QTemporaryDir tmp;
    const QString base = tmp.filePath(QStringLiteral("players"));
    const QString outside = tmp.filePath(QStringLiteral("outside"));
    QVERIFY(writeFile(base + QStringLiteral("/p/home/real/file"), "x"));
    QVERIFY(QDir().mkpath(outside));
    QVERIFY(makeLink(outside, base + QStringLiteral("/p/home/escape")));

    QVERIFY(!PlayerFiles::hasSymlinkUnder(base, base + QStringLiteral("/p/home/real/file")));
    QVERIFY(!PlayerFiles::hasSymlinkUnder(base, base + QStringLiteral("/p/home/not/yet/created")));
    QVERIFY(PlayerFiles::hasSymlinkUnder(base, base + QStringLiteral("/p/home/escape/x/y")));
    QVERIFY(PlayerFiles::hasSymlinkUnder(base, outside + QStringLiteral("/file")));
}

void TestPlayerFiles::removeTreeDoesNotFollowSymlinksAndHandlesReadOnlyDirs()
{
    QTemporaryDir tmp;
    const QString tree = tmp.filePath(QStringLiteral("tree"));
    const QString outside = tmp.filePath(QStringLiteral("outside"));
    QVERIFY(writeFile(outside + QStringLiteral("/keep.txt"), "K"));
    QVERIFY(writeFile(tree + QStringLiteral("/ro/file"), "x"));
    QVERIFY(makeLink(outside, tree + QStringLiteral("/link")));
    QVERIFY(QFile::setPermissions(tree + QStringLiteral("/ro"), QFileDevice::ReadOwner | QFileDevice::ExeOwner));

    QVERIFY(PlayerFiles::removeTree(tree));
    QVERIFY(!QFileInfo::exists(tree));
    QVERIFY(QFileInfo::exists(outside + QStringLiteral("/keep.txt")));
}

void TestPlayerFiles::replaceTreeOverlaysStagingOverReadOnlyDirs()
{
    QTemporaryDir tmp;
    const QString src = tmp.filePath(QStringLiteral("src"));
    const QString staging = tmp.filePath(QStringLiteral("staging"));
    const QString dest = tmp.filePath(QStringLiteral("dest"));
    QVERIFY(writeFile(src + QStringLiteral("/ro/a"), "a"));
    QVERIFY(QFile::setPermissions(src + QStringLiteral("/ro"), QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    QVERIFY(writeFile(staging + QStringLiteral("/ro/b"), "b"));

    QString error;
    QVERIFY2(PlayerFiles::replaceTree(src, dest, staging, &error), qPrintable(error));
    QVERIFY2(PlayerFiles::replaceTree(src, dest, staging, &error), qPrintable(error));
    QVERIFY(QFileInfo::exists(dest + QStringLiteral("/ro/b")));
    QVERIFY(!QFileInfo::exists(dest + QStringLiteral(".couchplay-old")));
    QFile::setPermissions(src + QStringLiteral("/ro"), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}

void TestPlayerFiles::replaceTreeRestoresInterruptedBackupBeforeFailing()
{
    QTemporaryDir tmp;
    const QString dest = tmp.filePath(QStringLiteral("dest"));
    // A previous run died between its two renames: the only copy of the player's data sits in "old"
    QVERIFY(writeFile(dest + QStringLiteral(".couchplay-old/save.dat"), "player data"));

    QString error;
    QVERIFY(!PlayerFiles::replaceTree(tmp.filePath(QStringLiteral("missing")), dest, QString(), &error));
    QCOMPARE(readFile(dest + QStringLiteral("/save.dat")), QByteArray("player data"));
}

void TestPlayerFiles::copyTreeKeepsSymlinksAsLinks()
{
    QTemporaryDir tmp;
    const QString src = tmp.filePath(QStringLiteral("src"));
    const QString dst = tmp.filePath(QStringLiteral("dst"));
    const QString outside = tmp.filePath(QStringLiteral("outside"));
    QVERIFY(writeFile(outside + QStringLiteral("/secret.txt"), "S"));
    QVERIFY(writeFile(src + QStringLiteral("/real.txt"), "R"));
    QVERIFY(makeLink(QStringLiteral("real.txt"), src + QStringLiteral("/rel-link")));
    QVERIFY(makeLink(outside, src + QStringLiteral("/dir-link")));
    QVERIFY(makeLink(QStringLiteral("/nonexistent/target"), src + QStringLiteral("/dangling")));
    QVERIFY(makeLink(QStringLiteral("../real.txt"), src + QStringLiteral("/nested/inner-link")));

    QString error;
    QVERIFY2(PlayerFiles::copyTreeContents(src, dst, &error), qPrintable(error));

    const QFileInfo rel(dst + QStringLiteral("/rel-link"));
    QVERIFY(rel.isSymLink());
    QCOMPARE(rel.symLinkTarget(), dst + QStringLiteral("/real.txt"));
    QCOMPARE(QFile::symLinkTarget(rel.filePath()), dst + QStringLiteral("/real.txt"));

    // A link to a directory is a link in the copy, not a copied directory.
    const QFileInfo dirLink(dst + QStringLiteral("/dir-link"));
    QVERIFY(dirLink.isSymLink());
    QCOMPARE(dirLink.symLinkTarget(), outside);
    QVERIFY(!QFileInfo::exists(dst + QStringLiteral("/dir-link.couchplay")));

    // Dangling links survive with their literal target.
    QVERIFY(QFileInfo(dst + QStringLiteral("/dangling")).isSymLink());
    QCOMPARE(QFile::symLinkTarget(dst + QStringLiteral("/dangling")), QStringLiteral("/nonexistent/target"));

    QVERIFY(QFileInfo(dst + QStringLiteral("/nested/inner-link")).isSymLink());
}

void TestPlayerFiles::copyTreeReplacesFileWithSymlink()
{
    QTemporaryDir tmp;
    const QString src = tmp.filePath(QStringLiteral("src"));
    const QString dst = tmp.filePath(QStringLiteral("dst"));
    QVERIFY(writeFile(src + QStringLiteral("/real.txt"), "R"));
    QVERIFY(makeLink(QStringLiteral("real.txt"), src + QStringLiteral("/f")));
    QVERIFY(writeFile(dst + QStringLiteral("/f"), "old regular file"));

    QVERIFY(PlayerFiles::copyTreeContents(src, dst));
    QVERIFY(QFileInfo(dst + QStringLiteral("/f")).isSymLink());
}

void TestPlayerFiles::copyTreeMissingSourceFails()
{
    QTemporaryDir tmp;
    const QString dst = tmp.filePath(QStringLiteral("dst"));
    QString error;
    QVERIFY(!PlayerFiles::copyTreeContents(tmp.filePath(QStringLiteral("missing")), dst, &error));
    QVERIFY(error.contains(QStringLiteral("missing")));
    QVERIFY(!QFileInfo::exists(dst));
}

void TestPlayerFiles::replaceTreeCreatesDestination()
{
    QTemporaryDir tmp;
    const QString src = tmp.filePath(QStringLiteral("src"));
    const QString dest = tmp.filePath(QStringLiteral("home/Games/foo"));
    QVERIFY(writeFile(src + QStringLiteral("/a.txt"), "A"));

    QString error;
    QVERIFY2(PlayerFiles::replaceTree(src, dest, QString(), &error), qPrintable(error));
    QCOMPARE(readFile(dest + QStringLiteral("/a.txt")), QByteArray("A"));
    QVERIFY(!QFileInfo::exists(dest + QStringLiteral(".couchplay-new")));
    QVERIFY(!QFileInfo::exists(dest + QStringLiteral(".couchplay-old")));
}

void TestPlayerFiles::replaceTreeReplacesExistingDestination()
{
    QTemporaryDir tmp;
    const QString src = tmp.filePath(QStringLiteral("src"));
    const QString dest = tmp.filePath(QStringLiteral("dest"));
    QVERIFY(writeFile(src + QStringLiteral("/new.txt"), "new"));
    QVERIFY(writeFile(src + QStringLiteral("/both.txt"), "from source"));
    QVERIFY(writeFile(dest + QStringLiteral("/stale.txt"), "stale"));
    QVERIFY(writeFile(dest + QStringLiteral("/both.txt"), "from old dest"));

    QVERIFY(PlayerFiles::replaceTree(src, dest, QString()));
    QCOMPARE(readFile(dest + QStringLiteral("/new.txt")), QByteArray("new"));
    QCOMPARE(readFile(dest + QStringLiteral("/both.txt")), QByteArray("from source"));
    // Replace, not merge: files only in the old copy are gone.
    QVERIFY(!QFileInfo::exists(dest + QStringLiteral("/stale.txt")));
    QVERIFY(!QFileInfo::exists(dest + QStringLiteral(".couchplay-old")));
    QVERIFY(!QFileInfo::exists(dest + QStringLiteral(".couchplay-new")));
}

void TestPlayerFiles::replaceTreeOverlaysStaging()
{
    QTemporaryDir tmp;
    const QString src = tmp.filePath(QStringLiteral("src"));
    const QString staging = tmp.filePath(QStringLiteral("staging"));
    const QString dest = tmp.filePath(QStringLiteral("dest"));
    QVERIFY(writeFile(src + QStringLiteral("/config.ini"), "source config"));
    QVERIFY(writeFile(src + QStringLiteral("/untouched.txt"), "u"));
    QVERIFY(writeFile(staging + QStringLiteral("/config.ini"), "staged config"));
    QVERIFY(writeFile(staging + QStringLiteral("/extra/only-staged.txt"), "x"));

    QVERIFY(PlayerFiles::replaceTree(src, dest, staging));
    QCOMPARE(readFile(dest + QStringLiteral("/config.ini")), QByteArray("staged config"));
    QCOMPARE(readFile(dest + QStringLiteral("/untouched.txt")), QByteArray("u"));
    QCOMPARE(readFile(dest + QStringLiteral("/extra/only-staged.txt")), QByteArray("x"));
    // The staging directory itself is only read.
    QCOMPARE(readFile(staging + QStringLiteral("/config.ini")), QByteArray("staged config"));
    // The source is never modified.
    QCOMPARE(readFile(src + QStringLiteral("/config.ini")), QByteArray("source config"));
}

void TestPlayerFiles::replaceTreeIgnoresMissingStaging()
{
    QTemporaryDir tmp;
    const QString src = tmp.filePath(QStringLiteral("src"));
    const QString dest = tmp.filePath(QStringLiteral("dest"));
    QVERIFY(writeFile(src + QStringLiteral("/a.txt"), "A"));

    QVERIFY(PlayerFiles::replaceTree(src, dest, tmp.filePath(QStringLiteral("no-such-staging"))));
    QCOMPARE(readFile(dest + QStringLiteral("/a.txt")), QByteArray("A"));
}

void TestPlayerFiles::replaceTreeKeepsSymlinks()
{
    QTemporaryDir tmp;
    const QString src = tmp.filePath(QStringLiteral("src"));
    const QString dest = tmp.filePath(QStringLiteral("dest"));
    const QString outside = tmp.filePath(QStringLiteral("outside"));
    QVERIFY(writeFile(outside + QStringLiteral("/x.txt"), "x"));
    QVERIFY(writeFile(src + QStringLiteral("/a.txt"), "A"));
    QVERIFY(makeLink(outside, src + QStringLiteral("/out")));

    QVERIFY(PlayerFiles::replaceTree(src, dest, QString()));
    QVERIFY(QFileInfo(dest + QStringLiteral("/out")).isSymLink());
    QCOMPARE(QFileInfo(dest + QStringLiteral("/out")).symLinkTarget(), outside);
}

void TestPlayerFiles::replaceTreeFailureKeepsOldCopy()
{
    if (::geteuid() == 0) {
        QSKIP("root can read files regardless of mode");
    }
    QTemporaryDir tmp;
    const QString src = tmp.filePath(QStringLiteral("src"));
    const QString dest = tmp.filePath(QStringLiteral("dest"));
    QVERIFY(writeFile(src + QStringLiteral("/ok.txt"), "ok"));
    const QString unreadable = src + QStringLiteral("/secret.txt");
    QVERIFY(writeFile(unreadable, "nope", QFileDevice::WriteOwner));
    QVERIFY(writeFile(dest + QStringLiteral("/previous.txt"), "previous"));

    QString error;
    QVERIFY(!PlayerFiles::replaceTree(src, dest, QString(), &error));
    QVERIFY(error.contains(QStringLiteral("secret.txt")));

    // The previous copy is intact and no half-built tree is left behind.
    QCOMPARE(readFile(dest + QStringLiteral("/previous.txt")), QByteArray("previous"));
    QVERIFY(!QFileInfo::exists(dest + QStringLiteral("/ok.txt")));
    QVERIFY(!QFileInfo::exists(dest + QStringLiteral(".couchplay-new")));
    QVERIFY(!QFileInfo::exists(dest + QStringLiteral(".couchplay-old")));

    QFile::setPermissions(unreadable, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

void TestPlayerFiles::replaceTreeCleansStaleTemporaries()
{
    QTemporaryDir tmp;
    const QString src = tmp.filePath(QStringLiteral("src"));
    const QString dest = tmp.filePath(QStringLiteral("dest"));
    QVERIFY(writeFile(src + QStringLiteral("/a.txt"), "A"));
    // Leftovers from an interrupted earlier run must not leak into the result.
    QVERIFY(writeFile(dest + QStringLiteral(".couchplay-new/leak.txt"), "leak"));
    QVERIFY(writeFile(dest + QStringLiteral(".couchplay-old/leak.txt"), "leak"));

    QVERIFY(PlayerFiles::replaceTree(src, dest, QString()));
    QVERIFY(!QFileInfo::exists(dest + QStringLiteral("/leak.txt")));
    QVERIFY(!QFileInfo::exists(dest + QStringLiteral(".couchplay-new")));
    QVERIFY(!QFileInfo::exists(dest + QStringLiteral(".couchplay-old")));
}

void TestPlayerFiles::slugIsStable()
{
    const QString home = QStringLiteral("/home/u");
    const QString dir = QStringLiteral("/home/u/.local/share/Foo");
    const QString slug = PlayerFiles::dataDirectoryStagingSlug(dir, home);
    QCOMPARE(PlayerFiles::dataDirectoryStagingSlug(dir, home), slug);
    QVERIFY(!slug.contains(QLatin1Char('/')));
    QVERIFY(QRegularExpression(QStringLiteral("-[0-9a-f]{8}$")).match(slug).hasMatch());
}

void TestPlayerFiles::slugRelativeToHome()
{
    const QString slug =
        PlayerFiles::dataDirectoryStagingSlug(QStringLiteral("/home/u/.local/share/Foo"), QStringLiteral("/home/u"));
    QVERIFY(slug.startsWith(QStringLiteral(".local_share_Foo-")));
}

void TestPlayerFiles::slugOutsideHome()
{
    const QString slug =
        PlayerFiles::dataDirectoryStagingSlug(QStringLiteral("/mnt/games/saves"), QStringLiteral("/home/u"));
    QVERIFY(slug.startsWith(QStringLiteral("mnt_games_saves-")));
    // A sibling path sharing the home prefix is not "inside" home.
    const QString sibling =
        PlayerFiles::dataDirectoryStagingSlug(QStringLiteral("/home/uother/x"), QStringLiteral("/home/u"));
    QVERIFY(sibling.startsWith(QStringLiteral("home_uother_x-")));
}

void TestPlayerFiles::slugIsUniqueForCollidingFlattening()
{
    const QString home = QStringLiteral("/home/u");
    const QString a = PlayerFiles::dataDirectoryStagingSlug(QStringLiteral("/home/u/a/b_c"), home);
    const QString b = PlayerFiles::dataDirectoryStagingSlug(QStringLiteral("/home/u/a_b/c"), home);
    QVERIFY(a != b);
    QVERIFY(a.startsWith(QStringLiteral("a_b_c-")));
    QVERIFY(b.startsWith(QStringLiteral("a_b_c-")));
}

QTEST_MAIN(TestPlayerFiles)
#include "test_playerfiles.moc"
