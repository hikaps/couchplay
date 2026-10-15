// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QVector>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits.h>
#include <fcntl.h>
#include <grp.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace
{
QString readTextFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

QJsonObject fileResult(const QString &path)
{
    QJsonObject result{{QStringLiteral("path"), path}};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.insert(QStringLiteral("error"), file.errorString());
        return result;
    }
    result.insert(QStringLiteral("readable"), true);
    result.insert(QStringLiteral("bytes"), QString::fromUtf8(file.readAll()));
    struct stat info {};
    if (::stat(QFile::encodeName(path).constData(), &info) == 0) {
        result.insert(QStringLiteral("uid"), static_cast<qint64>(info.st_uid));
        result.insert(QStringLiteral("gid"), static_cast<qint64>(info.st_gid));
        result.insert(QStringLiteral("mode"), static_cast<qint64>(info.st_mode & 07777));
    }
    return result;
}

QJsonObject writeResult(const QString &path, const QString &bytes)
{
    QJsonObject result{{QStringLiteral("path"), path}};
    const QByteArray encodedPath = QFile::encodeName(path);
    const QByteArray encodedBytes = bytes.toUtf8();
    const int fd = ::open(encodedPath.constData(), O_WRONLY | O_TRUNC | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        result.insert(QStringLiteral("written"), false);
        result.insert(QStringLiteral("errno"), errno);
        return result;
    }
    qsizetype offset = 0;
    while (offset < encodedBytes.size()) {
        const ssize_t written = ::write(fd, encodedBytes.constData() + offset,
                                        static_cast<size_t>(encodedBytes.size() - offset));
        if (written < 0) {
            const int error = errno;
            ::close(fd);
            result.insert(QStringLiteral("written"), false);
            result.insert(QStringLiteral("errno"), error);
            return result;
        }
        offset += written;
    }
    ::close(fd);
    result.insert(QStringLiteral("written"), true);
    return result;
}

QJsonObject credentialSnapshot()
{
    QJsonObject result;
    result.insert(QStringLiteral("pid"), static_cast<qint64>(::getpid()));
    result.insert(QStringLiteral("uid"), static_cast<qint64>(::getuid()));
    result.insert(QStringLiteral("euid"), static_cast<qint64>(::geteuid()));
    result.insert(QStringLiteral("gid"), static_cast<qint64>(::getgid()));
    result.insert(QStringLiteral("egid"), static_cast<qint64>(::getegid()));
    result.insert(QStringLiteral("home"), QString::fromLocal8Bit(qgetenv("HOME")));
    result.insert(QStringLiteral("lang"), QString::fromLocal8Bit(qgetenv("LANG")));
    result.insert(QStringLiteral("lcTime"), QString::fromLocal8Bit(qgetenv("LC_TIME")));
    char cwd[PATH_MAX] = {};
    if (::getcwd(cwd, sizeof(cwd))) {
        result.insert(QStringLiteral("cwd"), QString::fromLocal8Bit(cwd));
    }
    char mountNamespace[128] = {};
    const ssize_t namespaceLength = ::readlink("/proc/self/ns/mnt", mountNamespace, sizeof(mountNamespace) - 1);
    if (namespaceLength >= 0) {
        result.insert(QStringLiteral("mountNamespace"), QString::fromLocal8Bit(mountNamespace, namespaceLength));
    }
    char userNamespace[128] = {};
    const ssize_t userNamespaceLength = ::readlink("/proc/self/ns/user", userNamespace, sizeof(userNamespace) - 1);
    if (userNamespaceLength >= 0) {
        result.insert(QStringLiteral("userNamespace"), QString::fromLocal8Bit(userNamespace, userNamespaceLength));
    }

    const int groupCount = ::getgroups(0, nullptr);
    if (groupCount >= 0) {
        QVector<gid_t> groups(groupCount);
        if (groupCount == 0 || ::getgroups(groupCount, groups.data()) >= 0) {
            QJsonArray array;
            for (gid_t group : groups) {
                array.append(static_cast<qint64>(group));
            }
            result.insert(QStringLiteral("groups"), array);
        }
    }

    const QString status = readTextFile(QStringLiteral("/proc/self/status"));
    for (const QString &line : status.split(QLatin1Char('\n'))) {
        const qsizetype colon = line.indexOf(QLatin1Char(':'));
        if (colon < 0) {
            continue;
        }
        const QString name = line.left(colon);
        if (name == QStringLiteral("CapEff") || name == QStringLiteral("CapPrm")
            || name == QStringLiteral("CapInh") || name == QStringLiteral("CapAmb")
            || name == QStringLiteral("CapBnd") || name == QStringLiteral("NoNewPrivs")) {
            result.insert(name, line.mid(colon + 1).trimmed());
        }
    }

    QJsonArray inheritedDescriptors;
    const QStringList openFiles = QDir(QStringLiteral("/proc/self/fd"))
                                      .entryList(QDir::NoDotAndDotDot | QDir::AllEntries);
    for (const QString &entry : openFiles) {
        bool valid = false;
        const int fd = entry.toInt(&valid);
        if (valid && fd > STDERR_FILENO && ::fcntl(fd, F_GETFD) >= 0) {
            inheritedDescriptors.append(fd);
        }
    }
    result.insert(QStringLiteral("inheritedDescriptors"), inheritedDescriptors);
    const QStringList temporaryLowerPaths =
        QDir(QStringLiteral("/tmp")).entryList({QStringLiteral(".lower*")},
                                                QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    result.insert(QStringLiteral("privateTmpLowerPaths"), QJsonArray::fromStringList(temporaryLowerPaths));
    result.insert(QStringLiteral("notifySocketPresent"), !qEnvironmentVariableIsEmpty("NOTIFY_SOCKET"));
    return result;
}

int runGame(const QStringList &arguments)
{
    if (arguments.size() < 3 || arguments.at(1) != QStringLiteral("--probe-game")) {
        return 2;
    }
    const QString reportPath = arguments.at(2);
    QJsonObject report = credentialSnapshot();
    QJsonArray reads;
    QJsonArray writes;
    QJsonArray absent;
    QString holdPath;
    int sleepMs = 0;
    for (qsizetype i = 3; i < arguments.size();) {
        const QString option = arguments.at(i++);
        if (option == QStringLiteral("--read") && i + 1 < arguments.size()) {
            QJsonObject read = fileResult(arguments.at(i++));
            read.insert(QStringLiteral("expected"), arguments.at(i++));
            reads.append(read);
        } else if (option == QStringLiteral("--write") && i + 1 < arguments.size()) {
            const QString path = arguments.at(i++);
            const QString bytes = arguments.at(i++);
            writes.append(writeResult(path, bytes));
        } else if (option == QStringLiteral("--absent") && i < arguments.size()) {
            const QString path = arguments.at(i++);
            absent.append(QJsonObject{{QStringLiteral("path"), path},
                                      {QStringLiteral("exists"), QFileInfo::exists(path)}});
        } else if (option == QStringLiteral("--sleep-ms") && i < arguments.size()) {
            bool valid = false;
            const int requested = arguments.at(i++).toInt(&valid);
            if (!valid || requested < 0 || requested > 30000) {
                report.insert(QStringLiteral("argumentError"), option);
                break;
            }
            sleepMs = requested;
        } else if (option == QStringLiteral("--hold") && i < arguments.size()) {
            holdPath = arguments.at(i++);
        } else {
            report.insert(QStringLiteral("argumentError"), option);
            break;
        }
    }
    report.insert(QStringLiteral("reads"), reads);
    report.insert(QStringLiteral("writes"), writes);
    report.insert(QStringLiteral("absent"), absent);
    report.insert(QStringLiteral("notifySocketPresent"), !qEnvironmentVariableIsEmpty("NOTIFY_SOCKET"));

    QFile reportFile(reportPath);
    if (!reportFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::fprintf(stderr, "namespace probe cannot write report %s: %s\n",
                     QFile::encodeName(reportPath).constData(),
                     qPrintable(reportFile.errorString()));
        return 3;
    }
    reportFile.write(QJsonDocument(report).toJson(QJsonDocument::Compact));
    reportFile.close();

    if (sleepMs > 0) {
        ::usleep(static_cast<useconds_t>(sleepMs) * 1000U);
    }
    if (!holdPath.isEmpty()) {
        const QByteArray encoded = QFile::encodeName(holdPath);
        while (::access(encoded.constData(), F_OK) != 0) {
            ::usleep(20000);
        }
    }
    return 0;
}

int runGamescope(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--") == 0) {
            if (i + 1 == argc) {
                std::fprintf(stderr, "namespace Gamescope probe received no game command\n");
                return 2;
            }
            ::execv(argv[i + 1], &argv[i + 1]);
            std::perror("namespace Gamescope probe execv");
            return 127;
        }
    }
    std::fprintf(stderr, "namespace Gamescope probe received no -- separator\n");
    return 2;
}
}

int main(int argc, char **argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--probe-game") == 0) {
        QStringList arguments;
        for (int i = 0; i < argc; ++i) {
            arguments.append(QString::fromLocal8Bit(argv[i]));
        }
        return runGame(arguments);
    }
    return runGamescope(argc, argv);
}
