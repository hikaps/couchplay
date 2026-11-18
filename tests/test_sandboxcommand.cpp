// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 hikaps

#include <QTest>

#include "../src/core/SandboxCommand.h"

namespace
{
QString q(const char *s)
{
    return QString::fromLatin1(s);
}
QString q(const QString &s)
{
    return s;
}

// Index of the first occurrence of the consecutive tokens, or -1.
int searchSeq(const QStringList &argv, const QStringList &seq)
{
    for (int i = 0; i + seq.size() <= argv.size(); ++i) {
        if (argv.mid(i, seq.size()) == seq) {
            return i;
        }
    }
    return -1;
}

template<typename... Args>
int at(const QStringList &argv, const Args &...tokens)
{
    return searchSeq(argv, QStringList{q(tokens)...});
}

SandboxSpec baseSpec()
{
    SandboxSpec spec;
    spec.home = QStringLiteral("/home/desk");
    spec.playerHome = QStringLiteral("/home/desk/.local/share/couchplay/players/p1/home");
    spec.runtimeDir = QStringLiteral("/run/user/1000");
    spec.padNode = QStringLiteral("/dev/input/event90");
    spec.token = QStringLiteral("tok-123");
    spec.gamescopeArgs = {QStringLiteral("-W"), QStringLiteral("1920")};
    spec.gameCommand = {QStringLiteral("steam"), QStringLiteral("-tenfoot")};
    spec.uid = 1000;
    spec.gid = 1001;
    return spec;
}
}

class TestSandboxCommand : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void bwrapIsLauncher();
    void identityAndPidNamespace();
    void devIsRebuiltBeforeDeviceBinds();
    void inputNodes();
    void noPadNodeBindsNoInputNode();
    void extraDeviceNodes();
    void procAndTmp();
    void playerHomeBindAndFlatpakRoBind();
    void noPlayerNoHomeBind();
    void mountKinds();
    void overlayStacksExtraLowerLayersBelowSource();
    void mountsFollowHomeBindAndPrecedeBusMask();
    void busMask();
    void environment();
    void chdirDefaultsToHome();
    void chdirUsesWorkingDirectory();
    void trailingCommand();
    void noGameCommandOmitsSeparator();
};

void TestSandboxCommand::bwrapIsLauncher()
{
    const QStringList argv = buildSandboxArgv(baseSpec());
    QVERIFY(argv.size() > 5);
    QCOMPARE(argv.at(0), QStringLiteral("bwrap"));
    QVERIFY(!argv.contains(QStringLiteral("pasta")));
    QVERIFY(!argv.contains(QStringLiteral("--unshare-net")));
}

void TestSandboxCommand::identityAndPidNamespace()
{
    const QStringList argv = buildSandboxArgv(baseSpec());
    QVERIFY(at(argv, "--unshare-user", "--uid", "1000", "--gid", "1001") > 0);
    QVERIFY(argv.contains(QStringLiteral("--unshare-pid")));
    QVERIFY(argv.contains(QStringLiteral("--die-with-parent")));
}

void TestSandboxCommand::devIsRebuiltBeforeDeviceBinds()
{
    const QStringList argv = buildSandboxArgv(baseSpec());
    const int rootBind = at(argv, "--dev-bind", "/", "/");
    const int dev = at(argv, "--dev", "/dev");
    const int dri = at(argv, "--dev-bind", "/dev/dri", "/dev/dri");
    QVERIFY(rootBind >= 0);
    QVERIFY(dev > rootBind);
    QVERIFY(dri > dev);
    QVERIFY(at(argv, "--tmpfs", "/dev/shm") > dev);
    QVERIFY(at(argv, "--dev-bind-try", "/dev/snd", "/dev/snd") > dev);
    QVERIFY(at(argv, "--dev-bind-try", "/dev/fuse", "/dev/fuse") > dev);
    QVERIFY(at(argv, "--dev-bind-try", "/dev/ntsync", "/dev/ntsync") > dev);
}

void TestSandboxCommand::inputNodes()
{
    SandboxSpec spec = baseSpec();
    spec.directDevices = {QStringLiteral("/dev/input/event7"), QStringLiteral("/dev/input/event8")};
    const QStringList argv = buildSandboxArgv(spec);

    const int dev = at(argv, "--dev", "/dev");
    const int dir = at(argv, "--dir", "/dev/input");
    const int pad = at(argv, "--dev-bind", "/dev/input/event90", "/dev/input/event90");
    const int d1 = at(argv, "--dev-bind", "/dev/input/event7", "/dev/input/event7");
    const int d2 = at(argv, "--dev-bind", "/dev/input/event8", "/dev/input/event8");
    QVERIFY(dir > dev);
    QVERIFY(pad > dir);
    QVERIFY(d1 > dir);
    QVERIFY(d2 > d1);

    // Raw uinput / hidraw never appear in the command, so they are absent from the fresh /dev.
    QVERIFY(!argv.contains(QStringLiteral("/dev/uinput")));
    for (const QString &token : argv) {
        QVERIFY2(!token.startsWith(QStringLiteral("/dev/hidraw")), qPrintable(token));
    }
}

void TestSandboxCommand::noPadNodeBindsNoInputNode()
{
    SandboxSpec spec = baseSpec();
    spec.padNode.clear();
    const QStringList argv = buildSandboxArgv(spec);
    QVERIFY(at(argv, "--dir", "/dev/input") > 0);
    for (const QString &token : argv) {
        QVERIFY2(!token.startsWith(QStringLiteral("/dev/input/event")), qPrintable(token));
    }
}

void TestSandboxCommand::extraDeviceNodes()
{
    SandboxSpec spec = baseSpec();
    spec.extraDeviceNodes = {QStringLiteral("/dev/nvidia0"), QStringLiteral("/dev/nvidiactl")};
    const QStringList argv = buildSandboxArgv(spec);
    const int dev = at(argv, "--dev", "/dev");
    QVERIFY(at(argv, "--dev-bind", "/dev/nvidia0", "/dev/nvidia0") > dev);
    QVERIFY(at(argv, "--dev-bind", "/dev/nvidiactl", "/dev/nvidiactl") > dev);
}

void TestSandboxCommand::procAndTmp()
{
    const QStringList argv = buildSandboxArgv(baseSpec());
    QVERIFY(at(argv, "--proc", "/proc") > 0);
    QVERIFY(at(argv, "--tmpfs", "/tmp") > 0);
}

void TestSandboxCommand::playerHomeBindAndFlatpakRoBind()
{
    const SandboxSpec spec = baseSpec();
    const QStringList argv = buildSandboxArgv(spec);
    const int bind = at(argv, "--bind", spec.playerHome, spec.home);
    const QString flatpak = spec.home + QStringLiteral("/.local/share/flatpak");
    const int ro = at(argv, "--ro-bind-try", flatpak, flatpak);
    QVERIFY(bind > 0);
    QVERIFY(ro > bind);
}

void TestSandboxCommand::noPlayerNoHomeBind()
{
    SandboxSpec spec = baseSpec();
    spec.playerHome.clear();
    const QStringList argv = buildSandboxArgv(spec);
    QVERIFY(!argv.contains(QStringLiteral("--bind")));
    QVERIFY(!argv.contains(QStringLiteral("--ro-bind-try")));
    QVERIFY(!argv.contains(spec.home + QStringLiteral("/.local/share/flatpak")));
    // The sandbox still reports the desktop home as $HOME.
    QVERIFY(at(argv, "--setenv", "HOME", spec.home) > 0);
}

void TestSandboxCommand::mountKinds()
{
    SandboxSpec spec = baseSpec();
    SandboxMount bind;
    bind.kind = SandboxMount::Bind;
    bind.source = QStringLiteral("/src/b");
    bind.target = QStringLiteral("/tgt/b");
    SandboxMount ro;
    ro.kind = SandboxMount::RoBind;
    ro.source = QStringLiteral("/src/r");
    ro.target = QStringLiteral("/tgt/r");
    SandboxMount ov;
    ov.kind = SandboxMount::Overlay;
    ov.source = QStringLiteral("/src/o");
    ov.target = QStringLiteral("/tgt/o");
    ov.upper = QStringLiteral("/layer/upper");
    ov.work = QStringLiteral("/layer/work");
    spec.mounts = {bind, ro, ov};

    const QStringList argv = buildSandboxArgv(spec);
    const int b = at(argv, "--bind", "/src/b", "/tgt/b");
    const int r = at(argv, "--ro-bind", "/src/r", "/tgt/r");
    const int o = at(argv, "--overlay-src", "/src/o", "--overlay", "/layer/upper", "/layer/work", "/tgt/o");
    QVERIFY(b > 0);
    QVERIFY(r > b);
    QVERIFY(o > r);
}

void TestSandboxCommand::overlayStacksExtraLowerLayersBelowSource()
{
    SandboxSpec spec = baseSpec();
    SandboxMount ov;
    ov.kind = SandboxMount::Overlay;
    ov.source = QStringLiteral("/src/top");
    ov.lowerBelow = {QStringLiteral("/src/bottom")};
    ov.target = QStringLiteral("/tgt/o");
    ov.upper = QStringLiteral("/layer/upper");
    ov.work = QStringLiteral("/layer/work");
    spec.mounts = {ov};

    // bwrap puts the last --overlay-src on top, so the lower layer must be listed first.
    const QStringList argv = buildSandboxArgv(spec);
    QVERIFY(at(argv, "--overlay-src", "/src/bottom", "--overlay-src", "/src/top", "--overlay", "/layer/upper",
               "/layer/work", "/tgt/o")
            > 0);
}

void TestSandboxCommand::mountsFollowHomeBindAndPrecedeBusMask()
{
    SandboxSpec spec = baseSpec();
    SandboxMount mount;
    mount.kind = SandboxMount::RoBind;
    mount.source = spec.home + QStringLiteral("/Games");
    mount.target = spec.home + QStringLiteral("/Games");
    spec.mounts = {mount};

    const QStringList argv = buildSandboxArgv(spec);
    const int homeBind = at(argv, "--bind", spec.playerHome, spec.home);
    const int games = at(argv, "--ro-bind", mount.source, mount.target);
    const int bus = at(argv, "--ro-bind", "/dev/null", "/run/user/1000/bus");
    QVERIFY(homeBind >= 0);
    QVERIFY(games > homeBind);
    QVERIFY(bus > games);
}

void TestSandboxCommand::busMask()
{
    SandboxSpec spec = baseSpec();
    QVERIFY(at(buildSandboxArgv(spec), "--ro-bind", "/dev/null", "/run/user/1000/bus") > 0);

    spec.runtimeDir.clear();
    QCOMPARE(at(buildSandboxArgv(spec), "--ro-bind", "/dev/null"), -1);
}

void TestSandboxCommand::environment()
{
    SandboxSpec spec = baseSpec();
    spec.env.insert(QStringLiteral("PULSE_SINK"), QStringLiteral("sink1"));
    spec.env.insert(QStringLiteral("FOO"), QStringLiteral("bar baz"));
    const QStringList argv = buildSandboxArgv(spec);

    const int home = at(argv, "--setenv", "HOME", spec.home);
    const int sink = at(argv, "--setenv", "PULSE_SINK", "sink1");
    const int foo = at(argv, "--setenv", "FOO", "bar baz");
    const int token = at(argv, "--setenv", "COUCHPLAY_SANDBOX_ID", "tok-123");
    const int unset = at(argv, "--unsetenv", "DBUS_SESSION_BUS_ADDRESS");
    QVERIFY(home > 0);
    QVERIFY(sink > home);
    QVERIFY(foo > home);
    QVERIFY(token > sink);
    QVERIFY(token > foo);
    QVERIFY(unset > token);
}

void TestSandboxCommand::chdirDefaultsToHome()
{
    const SandboxSpec spec = baseSpec();
    QVERIFY(at(buildSandboxArgv(spec), "--chdir", spec.home, "--") > 0);
}

void TestSandboxCommand::chdirUsesWorkingDirectory()
{
    SandboxSpec spec = baseSpec();
    spec.workingDirectory = QStringLiteral("/games/foo");
    QVERIFY(at(buildSandboxArgv(spec), "--chdir", "/games/foo", "--") > 0);
}

void TestSandboxCommand::trailingCommand()
{
    const SandboxSpec spec = baseSpec();
    const QStringList argv = buildSandboxArgv(spec);
    const QStringList tail{QStringLiteral("--"),
                           QStringLiteral("dbus-run-session"),
                           QStringLiteral("--"),
                           QStringLiteral("gamescope"),
                           QStringLiteral("-W"),
                           QStringLiteral("1920"),
                           QStringLiteral("--"),
                           QStringLiteral("steam"),
                           QStringLiteral("-tenfoot")};
    QCOMPARE(argv.mid(argv.size() - tail.size()), tail);
}

void TestSandboxCommand::noGameCommandOmitsSeparator()
{
    SandboxSpec spec = baseSpec();
    spec.gameCommand.clear();
    const QStringList argv = buildSandboxArgv(spec);
    QCOMPARE(argv.last(), QStringLiteral("1920"));
}

QTEST_MAIN(TestSandboxCommand)
#include "test_sandboxcommand.moc"
