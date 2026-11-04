// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include "DirectoryPlan.h"

#include "HomePathMapping.h"

#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaType>
#include <QDBusMetaType>
#include <QSet>
#include <QRegularExpression>
#include <QUuid>

#include <cstdint>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
constexpr qsizetype maximumManifestBytes = 1024 * 1024;
constexpr qsizetype maximumDirectoryRecords = 512;
constexpr qsizetype maximumGamescopeArguments = 4096;
constexpr qsizetype maximumEnvironmentEntries = 512;

bool fail(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
    return false;
}

bool containsNul(const QString &value)
{
    return value.contains(QChar::Null);
}

bool validManifestText(const QString &value, bool allowEmpty = false)
{
    return (allowEmpty || !value.isEmpty()) && !containsNul(value);
}

bool validUid(qint64 value)
{
    return value >= 0 && static_cast<quint64>(value) < UINT32_MAX;
}

bool isSafeAbsolutePath(const QString &path, bool allowRoot = false)
{
    return validManifestText(path) && QDir::isAbsolutePath(path)
        && !DirectoryPlan::directoryPathHasTraversal(path)
        && (allowRoot || QDir::cleanPath(path) != QStringLiteral("/"));
}

bool isSafeUnitName(const QString &name)
{
    static const QRegularExpression expression(QStringLiteral("^couchplay-[A-Za-z0-9_.@-]+[.]service$"));
    const QRegularExpressionMatch match = expression.match(name);
    return match.hasMatch() && match.capturedLength() == name.size();
}

bool isSafeUsername(const QString &name)
{
    static const QRegularExpression expression(QStringLiteral("^[a-z][a-z0-9_-]{0,31}$"));
    const QRegularExpressionMatch match = expression.match(name);
    return match.hasMatch() && match.capturedLength() == name.size();
}

bool isSafeEnvironmentName(const QString &name)
{
    static const QRegularExpression expression(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
    const QRegularExpressionMatch match = expression.match(name);
    return match.hasMatch() && match.capturedLength() == name.size();
}

bool isStrictRelativeAlias(const QString &path)
{
    return !path.isEmpty() && !QDir::isAbsolutePath(path) && !DirectoryPlan::directoryPathHasTraversal(path)
        && QDir::cleanPath(path) == path && path != QStringLiteral(".");
}

bool validateDirectoryRecord(const DirectoryPreparation &record, QString *error)
{
    if (!isSafeAbsolutePath(record.source)) {
        return fail(error, QStringLiteral("Directory source must be a safe absolute path"));
    }
    if (record.mode != QStringLiteral("copy") && record.mode != QStringLiteral("bind")
        && record.mode != QStringLiteral("overlay") && record.mode != QStringLiteral("acl")) {
        return fail(error, QStringLiteral("Unsupported directory mode"));
    }
    if (!record.targetAlias.isEmpty() && !isStrictRelativeAlias(record.targetAlias)) {
        return fail(error, QStringLiteral("Directory target alias must be a safe relative path"));
    }
    if (record.mode == QStringLiteral("acl") || record.mode == QStringLiteral("bind")) {
        if (!record.stagingPath.isEmpty()) {
            return fail(error, QStringLiteral("ACL and bind entries cannot have a staging path"));
        }
        if (record.mode == QStringLiteral("acl") && !record.targetAlias.isEmpty()) {
            return fail(error, QStringLiteral("ACL-only entries cannot have a target"));
        }
    } else if (!record.stagingPath.isEmpty() && !isSafeAbsolutePath(record.stagingPath)) {
        return fail(error, QStringLiteral("Directory staging path must be a safe absolute path"));
    }
    return true;
}
QString resolvedTargetAlias(const DirectoryPreparation &record,
                            const QString &declaredHome,
                            const QString &canonicalHome)
{
    if (!record.targetAlias.isEmpty()) {
        return record.targetAlias;
    }
    const HomePathMapping mapping = mapHomeRelativePath(record.source, declaredHome, canonicalHome);
    if (mapping.kind == HomePathKind::Relative) {
        return mapping.relativePath;
    }
    if (mapping.kind != HomePathKind::External) {
        return {};
    }
    if (record.mode == QStringLiteral("copy")) {
        return QStringLiteral(".couchplay/copies/")
            + DirectoryPlan::dataDirectoryStagingSlug(record.source, declaredHome);
    }
    return QStringLiteral(".couchplay/mounts/") + record.source.mid(1);
}

bool validateManifest(const LaunchManifest &manifest, QString *error, bool recovering = false)
{
    if (manifest.directories.size() > maximumDirectoryRecords
        || manifest.gamescopeArgv.size() > maximumGamescopeArguments
        || manifest.environment.size() > maximumEnvironmentEntries) {
        return fail(error, QStringLiteral("Launch manifest collection limit exceeded"));
    }
    const QString expectedUnitName = manifest.unitRole == QStringLiteral("main")
        ? QStringLiteral("couchplay-%1.service").arg(manifest.targetUsername)
        : manifest.unitRole == QStringLiteral("stream")
            ? QStringLiteral("couchplay-stream@%1.service").arg(manifest.targetUsername)
            : QString();
    // Read-only upgrade authority; new manifests always use disjoint names.
    const bool legacyStream = recovering && manifest.unitRole == QStringLiteral("stream")
        && manifest.unitName == QStringLiteral("couchplay-%1-stream.service").arg(manifest.targetUsername);
    if (manifest.schemaVersion != 1 || !isSafeUsername(manifest.targetUsername)
        || expectedUnitName.isEmpty() || !isSafeUnitName(manifest.unitName)
        || (manifest.unitName != expectedUnitName && !legacyStream)
        || !validUid(manifest.callerUid) || !validUid(manifest.callerGid)
        || manifest.targetUid == 0 || !validUid(manifest.targetUid) || !validUid(manifest.targetGid)) {
        return fail(error, QStringLiteral("Invalid launch manifest identity or schema"));
    }
    if (!isSafeAbsolutePath(manifest.declaredHome) || !isSafeAbsolutePath(manifest.canonicalHome)
        || !isSafeAbsolutePath(manifest.targetDeclaredHome) || !isSafeAbsolutePath(manifest.targetCanonicalHome)) {
        return fail(error, QStringLiteral("Invalid resolved home path in launch manifest"));
    }
    if (manifest.gamescopeArgv.isEmpty()
        || !isSafeAbsolutePath(manifest.gamescopeArgv.constFirst())) {
        return fail(error, QStringLiteral("Launch manifest has no safe Gamescope executable argv"));
    }
    for (const QString &argument : manifest.gamescopeArgv) {
        if (!validManifestText(argument, true)) {
            return fail(error, QStringLiteral("Invalid Gamescope argument"));
        }
    }
    if (!manifest.workingDirectory.isEmpty() && !isSafeAbsolutePath(manifest.workingDirectory, true)) {
        return fail(error, QStringLiteral("Invalid working directory in launch manifest"));
    }
    for (auto it = manifest.environment.cbegin(); it != manifest.environment.cend(); ++it) {
        if (!isSafeEnvironmentName(it.key()) || !validManifestText(it.value(), true)) {
            return fail(error, QStringLiteral("Invalid protected environment entry"));
        }
    }
    if (!DirectoryPlan::validateDirectoryPreparations(manifest.directories, error)) {
        return false;
    }
    QSet<QString> destinations;
    for (const DirectoryPreparation &record : manifest.directories) {
        const QString target = resolvedTargetAlias(record, manifest.declaredHome, manifest.canonicalHome);
        if (record.mode == QStringLiteral("acl")) {
            continue;
        }
        if (!isStrictRelativeAlias(target)) {
            return fail(error, QStringLiteral("Directory target is not a safe home descendant"));
        }
        for (const QString &existing : destinations) {
            if (target == existing || target.startsWith(existing + QLatin1Char('/'))
                || existing.startsWith(target + QLatin1Char('/'))) {
                return fail(error, QStringLiteral("Directory targets overlap"));
            }
        }
        destinations.insert(target);
    }
    return true;
}

QJsonObject recordToJson(const DirectoryPreparation &record)
{
    return {{QStringLiteral("source"), record.source},
            {QStringLiteral("mode"), record.mode},
            {QStringLiteral("targetAlias"), record.targetAlias},
            {QStringLiteral("stagingPath"), record.stagingPath}};
}

QJsonObject manifestToJson(const LaunchManifest &manifest)
{
    QJsonArray directories;
    for (const DirectoryRecord &record : manifest.directories) {
        directories.append(recordToJson(record));
    }
    QJsonArray argv;
    for (const QString &argument : manifest.gamescopeArgv) {
        argv.append(argument);
    }
    QJsonObject environment;
    for (auto it = manifest.environment.cbegin(); it != manifest.environment.cend(); ++it) {
        environment.insert(it.key(), it.value());
    }
    return {{QStringLiteral("schemaVersion"), manifest.schemaVersion},
            {QStringLiteral("unitName"), manifest.unitName},
            {QStringLiteral("unitRole"), manifest.unitRole},
            {QStringLiteral("callerUid"), static_cast<qint64>(manifest.callerUid)},
            {QStringLiteral("callerGid"), static_cast<qint64>(manifest.callerGid)},
            {QStringLiteral("targetUsername"), manifest.targetUsername},
            {QStringLiteral("targetUid"), static_cast<qint64>(manifest.targetUid)},
            {QStringLiteral("targetGid"), static_cast<qint64>(manifest.targetGid)},
            {QStringLiteral("declaredHome"), manifest.declaredHome},
            {QStringLiteral("canonicalHome"), manifest.canonicalHome},
            {QStringLiteral("targetDeclaredHome"), manifest.targetDeclaredHome},
            {QStringLiteral("targetCanonicalHome"), manifest.targetCanonicalHome},
            {QStringLiteral("directories"), directories},
            {QStringLiteral("gamescopeArgv"), argv},
            {QStringLiteral("workingDirectory"), manifest.workingDirectory},
            {QStringLiteral("environment"), environment}};
}

bool objectHasExactKeys(const QJsonObject &object, const QSet<QString> &keys)
{
    if (object.size() != keys.size()) {
        return false;
    }
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (!keys.contains(it.key())) {
            return false;
        }
    }
    return true;
}

bool jsonString(const QJsonObject &object, const QString &key, QString *value, bool allowEmpty = true)
{
    const QJsonValue item = object.value(key);
    if (!item.isString()) {
        return false;
    }
    *value = item.toString();
    return validManifestText(*value, allowEmpty);
}

bool jsonId(const QJsonObject &object, const QString &key, quint32 *value)
{
    const QJsonValue item = object.value(key);
    if (!item.isDouble() || item.toDouble() < 0 || item.toDouble() >= UINT32_MAX
        || item.toDouble() != static_cast<double>(static_cast<quint32>(item.toDouble()))) {
        return false;
    }
    *value = static_cast<quint32>(item.toDouble());
    return true;
}

bool readManifestJson(const QByteArray &bytes, LaunchManifest *manifest, QString *error)
{
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return fail(error, QStringLiteral("Malformed launch manifest JSON"));
    }
    const QJsonObject object = document.object();
    static const QSet<QString> keys = {
        QStringLiteral("schemaVersion"), QStringLiteral("unitName"), QStringLiteral("callerUid"),
        QStringLiteral("unitRole"),
        QStringLiteral("callerGid"), QStringLiteral("targetUsername"), QStringLiteral("targetUid"),
        QStringLiteral("targetGid"), QStringLiteral("declaredHome"), QStringLiteral("canonicalHome"),
        QStringLiteral("targetDeclaredHome"), QStringLiteral("targetCanonicalHome"), QStringLiteral("directories"),
        QStringLiteral("gamescopeArgv"), QStringLiteral("workingDirectory"), QStringLiteral("environment")};
    if (!objectHasExactKeys(object, keys)) {
        return fail(error, QStringLiteral("Unexpected launch manifest fields"));
    }
    const QJsonValue version = object.value(QStringLiteral("schemaVersion"));
    if (!version.isDouble() || version.toInt(-1) != 1 || version.toDouble() != 1.0) {
        return fail(error, QStringLiteral("Unsupported launch manifest schema"));
    }
    LaunchManifest result;
    result.schemaVersion = 1;
    if (!jsonString(object, QStringLiteral("unitName"), &result.unitName, false)
        || !jsonString(object, QStringLiteral("unitRole"), &result.unitRole, false)
        || !jsonId(object, QStringLiteral("callerUid"), &result.callerUid)
        || !jsonId(object, QStringLiteral("callerGid"), &result.callerGid)
        || !jsonString(object, QStringLiteral("targetUsername"), &result.targetUsername, false)
        || !jsonId(object, QStringLiteral("targetUid"), &result.targetUid)
        || !jsonId(object, QStringLiteral("targetGid"), &result.targetGid)
        || !jsonString(object, QStringLiteral("declaredHome"), &result.declaredHome, false)
        || !jsonString(object, QStringLiteral("canonicalHome"), &result.canonicalHome, false)
        || !jsonString(object, QStringLiteral("targetDeclaredHome"), &result.targetDeclaredHome, false)
        || !jsonString(object, QStringLiteral("targetCanonicalHome"), &result.targetCanonicalHome, false)
        || !jsonString(object, QStringLiteral("workingDirectory"), &result.workingDirectory)) {
        return fail(error, QStringLiteral("Malformed launch manifest fields"));
    }
    const QJsonValue dirs = object.value(QStringLiteral("directories"));
    const QJsonValue argv = object.value(QStringLiteral("gamescopeArgv"));
    const QJsonValue environment = object.value(QStringLiteral("environment"));
    if (!dirs.isArray() || !argv.isArray() || !environment.isObject()
        || dirs.toArray().size() > maximumDirectoryRecords
        || argv.toArray().size() > maximumGamescopeArguments
        || environment.toObject().size() > maximumEnvironmentEntries) {
        return fail(error, QStringLiteral("Malformed launch manifest arrays"));
    }
    static const QSet<QString> recordKeys = {QStringLiteral("source"), QStringLiteral("mode"),
                                             QStringLiteral("targetAlias"), QStringLiteral("stagingPath")};
    for (const QJsonValue &item : dirs.toArray()) {
        if (!item.isObject() || !objectHasExactKeys(item.toObject(), recordKeys)) {
            return fail(error, QStringLiteral("Malformed directory record in launch manifest"));
        }
        const QJsonObject record = item.toObject();
        DirectoryRecord parsed;
        if (!jsonString(record, QStringLiteral("source"), &parsed.source, false)
            || !jsonString(record, QStringLiteral("mode"), &parsed.mode, false)
            || !jsonString(record, QStringLiteral("targetAlias"), &parsed.targetAlias)
            || !jsonString(record, QStringLiteral("stagingPath"), &parsed.stagingPath)) {
            return fail(error, QStringLiteral("Malformed directory record fields"));
        }
        result.directories.append(std::move(parsed));
    }
    for (const QJsonValue &item : argv.toArray()) {
        if (!item.isString() || containsNul(item.toString())) {
            return fail(error, QStringLiteral("Malformed Gamescope argv"));
        }
        result.gamescopeArgv.append(item.toString());
    }
    const QJsonObject env = environment.toObject();
    for (auto it = env.begin(); it != env.end(); ++it) {
        if (!it.value().isString() || containsNul(it.value().toString())) {
            return fail(error, QStringLiteral("Malformed environment entry"));
        }
        result.environment.insert(it.key(), it.value().toString());
    }
    if (!validateManifest(result, error, true)) {
        return false;
    }
    *manifest = std::move(result);
    return true;
}

int openManifestDirectory(bool create)
{
    const int root = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root < 0) {
        return -errno;
    }
    int run = ::openat(root, "run", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    ::close(root);
    if (run < 0) {
        return -errno;
    }
    struct stat st{};
    if (::fstat(run, &st) != 0 || st.st_uid != 0 || (st.st_mode & 0022) != 0) {
        const int error = errno ? errno : EPERM;
        ::close(run);
        return -error;
    }
    int couchplay = ::openat(run, "couchplay", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (couchplay < 0 && create && errno == ENOENT) {
        if (::mkdirat(run, "couchplay", 0755) == 0 || errno == EEXIST) {
            couchplay = ::openat(run, "couchplay", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        }
    }
    ::close(run);
    if (couchplay < 0) {
        return -errno;
    }
    if (::fstat(couchplay, &st) != 0 || st.st_uid != 0 || (st.st_mode & 0022) != 0) {
        const int error = errno ? errno : EPERM;
        ::close(couchplay);
        return -error;
    }
    int plans = ::openat(couchplay, "launch-plans", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (plans < 0 && create && errno == ENOENT) {
        if (::mkdirat(couchplay, "launch-plans", 0700) == 0 || errno == EEXIST) {
            plans = ::openat(couchplay, "launch-plans", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        }
    }
    ::close(couchplay);
    if (plans < 0) {
        return -errno;
    }
    if (::fstat(plans, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0 || (st.st_mode & 07777) != 0700) {
        const int error = errno ? errno : EPERM;
        ::close(plans);
        return -error;
    }
    return plans;
}

bool writeAll(int fd, const char *data, qsizetype size)
{
    qsizetype offset = 0;
    while (offset < size) {
        const ssize_t count = ::write(fd, data + offset, static_cast<size_t>(size - offset));
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (count == 0) {
            errno = EIO;
            return false;
        }
        offset += count;
    }
    return true;
}

bool readAll(int fd, QByteArray *bytes)
{
    char buffer[16384];
    for (;;) {
        const ssize_t count = ::read(fd, buffer, sizeof(buffer));
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (count == 0) {
            return true;
        }
        if (bytes->size() > maximumManifestBytes - count) {
            errno = EFBIG;
            return false;
        }
        bytes->append(buffer, static_cast<qsizetype>(count));
    }
}

bool safeManifestLeaf(const QString &token)
{
    return DirectoryPlan::isLaunchToken(token);
}
}

QVariantMap DirectoryPreparation::toVariant() const
{
    return {{QStringLiteral("source"), source},
            {QStringLiteral("mode"), mode},
            {QStringLiteral("targetAlias"), targetAlias},
            {QStringLiteral("stagingPath"), stagingPath}};
}

QDBusArgument &operator<<(QDBusArgument &argument, const SystemdBindPath &path)
{
    argument.beginStructure();
    argument << path.source << path.target << path.ignoreMissing << path.flags;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, SystemdBindPath &path)
{
    argument.beginStructure();
    argument >> path.source >> path.target >> path.ignoreMissing >> path.flags;
    argument.endStructure();
    return argument;
}
QDBusArgument &operator<<(QDBusArgument &argument, const SystemdProperty &property)
{
    argument.beginStructure();
    argument << property.name << property.value;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, SystemdProperty &property)
{
    argument.beginStructure();
    argument >> property.name >> property.value;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument, const SystemdExecCommand &command)
{
    argument.beginStructure();
    argument << command.path << command.argv << command.flags;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, SystemdExecCommand &command)
{
    argument.beginStructure();
    argument >> command.path >> command.argv >> command.flags;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument, const SystemdAuxiliaryUnit &unit)
{
    argument.beginStructure();
    argument << unit.name << unit.properties;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, SystemdAuxiliaryUnit &unit)
{
    argument.beginStructure();
    argument >> unit.name >> unit.properties;
    argument.endStructure();
    return argument;
}

namespace DirectoryPlan
{
bool directoryPathHasTraversal(const QString &path)
{
    return path.contains(QChar::Null) || pathHasDotDotComponent(path);
}

bool isSafeRelativeDirectoryPath(const QString &path)
{
    return isStrictRelativeAlias(path);
}

bool parseDirectoryPreparations(const QVariantList &input,
                                QList<DirectoryPreparation> *output,
                                QString *error)
{
    if (!output) {
        return fail(error, QStringLiteral("Missing directory plan output"));
    }
    output->clear();
    if (input.size() > maximumDirectoryRecords) {
        return fail(error, QStringLiteral("Too many directory preparations"));
    }
    static const QSet<QString> keys = {QStringLiteral("source"), QStringLiteral("mode"),
                                       QStringLiteral("targetAlias"), QStringLiteral("stagingPath")};
    QList<DirectoryPreparation> parsed;
    parsed.reserve(input.size());
    for (const QVariant &entry : input) {
        QVariant value = entry;
        if (value.metaType().id() == qMetaTypeId<QDBusVariant>()) {
            value = value.value<QDBusVariant>().variant();
        }
        QVariantMap map;
        if (value.metaType().id() == QMetaType::QVariantMap) {
            map = value.toMap();
        } else if (value.metaType().id() == qMetaTypeId<QDBusArgument>()) {
            const QDBusArgument argument = value.value<QDBusArgument>();
            if (argument.currentType() != QDBusArgument::MapType) {
                return fail(error, QStringLiteral("Directory plan entries must be maps"));
            }
            map = qdbus_cast<QVariantMap>(argument);
        } else {
            return fail(error, QStringLiteral("Directory plan entries must be maps"));
        }
        if (map.size() != keys.size()) {
            return fail(error, QStringLiteral("Directory plan entry has an unexpected field set"));
        }
        for (auto it = map.cbegin(); it != map.cend(); ++it) {
            if (!keys.contains(it.key()) || it.value().metaType().id() != QMetaType::QString) {
                return fail(error, QStringLiteral("Directory plan fields must be known strings"));
            }
        }
        DirectoryPreparation record{map.value(QStringLiteral("source")).toString(),
                                    map.value(QStringLiteral("mode")).toString(),
                                    map.value(QStringLiteral("targetAlias")).toString(),
                                    map.value(QStringLiteral("stagingPath")).toString()};
        if (!validateDirectoryRecord(record, error)) {
            return false;
        }
        parsed.append(std::move(record));
    }
    if (!validateDirectoryPreparations(parsed, error)) {
        return false;
    }
    *output = std::move(parsed);
    return true;
}

bool validateDirectoryPreparations(const QList<DirectoryPreparation> &input, QString *error)
{
    if (input.size() > maximumDirectoryRecords) {
        return fail(error, QStringLiteral("Too many directory preparations"));
    }
    QSet<QString> explicitTargets;
    for (const DirectoryPreparation &record : input) {
        if (!validateDirectoryRecord(record, error)) {
            return false;
        }
        if (record.mode == QStringLiteral("acl") || record.targetAlias.isEmpty()) {
            continue;
        }
        for (const QString &existing : explicitTargets) {
            if (record.targetAlias == existing || record.targetAlias.startsWith(existing + QLatin1Char('/'))
                || existing.startsWith(record.targetAlias + QLatin1Char('/'))) {
                return fail(error, QStringLiteral("Directory targets overlap"));
            }
        }
        explicitTargets.insert(record.targetAlias);
    }
    return true;
}

QString dataDirectoryStagingSlug(const QString &dirPath, const QString &compositorHome)
{
    QString relative;
    if (!compositorHome.isEmpty() && dirPath.startsWith(compositorHome + QLatin1Char('/'))) {
        relative = dirPath.mid(compositorHome.length() + 1);
    } else {
        relative = dirPath;
        while (relative.startsWith(QLatin1Char('/'))) {
            relative.remove(0, 1);
        }
    }
    QString slug = relative;
    slug.replace(QLatin1Char('/'), QLatin1Char('_'));
    const QByteArray hash = QCryptographicHash::hash(relative.toUtf8(), QCryptographicHash::Sha256).toHex().left(8);
    slug += QLatin1Char('-') + QString::fromLatin1(hash);
    return slug;
}

bool isLaunchToken(const QString &token)
{
    static const QRegularExpression expression(
        QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
    const QRegularExpressionMatch match = expression.match(token);
    return token.size() == 36 && match.hasMatch() && match.capturedLength() == token.size();
}
QString copyScratchName(const QString &leafName, const QString &launchToken)
{
    if (leafName.isEmpty() || leafName == QStringLiteral(".") || leafName == QStringLiteral("..")
        || leafName.contains(QLatin1Char('/')) || containsNul(leafName) || !isLaunchToken(launchToken)) {
        return {};
    }
    const QByteArray hash = QCryptographicHash::hash(leafName.toUtf8(), QCryptographicHash::Sha256).toHex().left(16);
    return QStringLiteral(".cptmp-%1-%2").arg(launchToken, QString::fromLatin1(hash));
}

bool writeLaunchManifest(const LaunchManifest &manifest, QString *token, QString *error)
{
    if (::geteuid() != 0) {
        return fail(error, QStringLiteral("Launch manifests may only be written by root"));
    }
    if (!token || !validateManifest(manifest, error)) {
        return false;
    }
    const QByteArray bytes = QJsonDocument(manifestToJson(manifest)).toJson(QJsonDocument::Compact);
    if (bytes.isEmpty() || bytes.size() > maximumManifestBytes) {
        return fail(error, QStringLiteral("Launch manifest is too large"));
    }
    const int dirFd = openManifestDirectory(true);
    if (dirFd < 0) {
        return fail(error, QStringLiteral("Could not open trusted launch manifest directory: %1")
                               .arg(QString::fromLocal8Bit(strerror(-dirFd))));
    }
    for (int attempt = 0; attempt < 8; ++attempt) {
        const QString newToken = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        const QByteArray tokenBytes = newToken.toLatin1();
        const int fd = ::openat(dirFd, tokenBytes.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) {
            if (errno == EEXIST) {
                continue;
            }
            const int err = errno;
            ::close(dirFd);
            return fail(error, QStringLiteral("Could not create launch manifest: %1").arg(QString::fromLocal8Bit(strerror(err))));
        }
        bool ok = ::fchown(fd, 0, 0) == 0 && ::fchmod(fd, 0600) == 0 && writeAll(fd, bytes.constData(), bytes.size())
            && ::fsync(fd) == 0;
        const int writeError = errno;
        struct stat st{};
        ok = ok && ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1 && st.st_uid == 0
            && (st.st_mode & 07777) == 0600;
        if (::close(fd) != 0 && ok) {
            ok = false;
        }
        if (ok && ::fsync(dirFd) != 0) {
            ok = false;
        }
        if (!ok) {
            const int err = errno ? errno : writeError;
            ::unlinkat(dirFd, tokenBytes.constData(), 0);
            ::close(dirFd);
            return fail(error, QStringLiteral("Could not persist launch manifest: %1").arg(QString::fromLocal8Bit(strerror(err))));
        }
        *token = newToken;
        ::close(dirFd);
        return true;
    }
    ::close(dirFd);
    return fail(error, QStringLiteral("Could not allocate a unique launch token"));
}

bool readLaunchManifest(const QString &token, LaunchManifest *manifest, QString *error)
{
    if (::geteuid() != 0 || !manifest || !safeManifestLeaf(token)) {
        return fail(error, QStringLiteral("Invalid launch manifest token or caller"));
    }
    const int dirFd = openManifestDirectory(false);
    if (dirFd < 0) {
        return fail(error, QStringLiteral("Could not open trusted launch manifest directory"));
    }
    const QByteArray name = token.toLatin1();
    const int fd = ::openat(dirFd, name.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    const int openError = errno;
    ::close(dirFd);
    if (fd < 0) {
        return fail(error, QStringLiteral("Could not open launch manifest %1 in /run/couchplay/launch-plans: %2")
                               .arg(token, QString::fromLocal8Bit(strerror(openError))));
    }
    struct stat st{};
    bool trusted = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0 && st.st_nlink == 1
        && (st.st_mode & 07777) == 0600 && st.st_size > 0 && st.st_size <= maximumManifestBytes;
    QByteArray bytes;
    const bool readOk = trusted && readAll(fd, &bytes);
    const int readError = errno;
    ::close(fd);
    if (!trusted || !readOk) {
        return fail(error, QStringLiteral("Launch manifest is not a trusted regular file: %1")
                               .arg(QString::fromLocal8Bit(strerror(readError ? readError : EPERM))));
    }
    return readManifestJson(bytes, manifest, error);
}

bool unlinkLaunchManifest(const QString &token, QString *error)
{
    if (::geteuid() != 0 || !safeManifestLeaf(token)) {
        return fail(error, QStringLiteral("Invalid launch manifest token or caller"));
    }
    const int dirFd = openManifestDirectory(false);
    if (dirFd < 0) {
        return fail(error, QStringLiteral("Could not open trusted launch manifest directory"));
    }
    const QByteArray name = token.toLatin1();
    struct stat st{};
    const bool trusted = ::fstatat(dirFd, name.constData(), &st, AT_SYMLINK_NOFOLLOW) == 0
        && S_ISREG(st.st_mode) && st.st_uid == 0 && st.st_nlink == 1 && (st.st_mode & 0777) == 0600;
    if (!trusted) {
        const int err = errno ? errno : EPERM;
        ::close(dirFd);
        return fail(error, QStringLiteral("Refusing to remove an untrusted launch manifest: %1")
                               .arg(QString::fromLocal8Bit(strerror(err))));
    }
    const bool removed = ::unlinkat(dirFd, name.constData(), 0) == 0 && ::fsync(dirFd) == 0;
    const int err = errno;
    ::close(dirFd);
    if (!removed) {
        return fail(error, QStringLiteral("Could not remove launch manifest: %1").arg(QString::fromLocal8Bit(strerror(err))));
    }
    return true;
}


void registerSystemdTypes()
{
    qDBusRegisterMetaType<SystemdBindPath>();
    qDBusRegisterMetaType<QList<SystemdBindPath>>();
    qDBusRegisterMetaType<SystemdProperty>();
    qDBusRegisterMetaType<QList<SystemdProperty>>();
    qDBusRegisterMetaType<SystemdExecCommand>();
    qDBusRegisterMetaType<QList<SystemdExecCommand>>();
    qDBusRegisterMetaType<SystemdAuxiliaryUnit>();
    qDBusRegisterMetaType<QList<SystemdAuxiliaryUnit>>();
}
}
