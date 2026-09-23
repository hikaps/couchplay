// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include "SteamShortcutsVdf.h"

#include "SteamConfigManager.h"

#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <limits>

namespace {

constexpr quint8 VdfObject = 0x00;
constexpr quint8 VdfString = 0x01;
constexpr quint8 VdfInt32 = 0x02;
constexpr quint8 VdfFloat32 = 0x03;
constexpr quint8 VdfPointer = 0x04;
constexpr quint8 VdfWString = 0x05;
constexpr quint8 VdfColor = 0x06;
constexpr quint8 VdfUint64 = 0x07;
constexpr quint8 VdfEnd = 0x08;
constexpr quint8 VdfInt64 = 0x0a;
constexpr int MaxNesting = 32;

struct Field {
    qsizetype start = 0;
    qsizetype end = 0;
    qsizetype valueStart = 0;
    qsizetype valueEnd = 0;
    quint8 type = 0;
    QByteArray keyBytes;
    QString key;
    QByteArray valueBytes;
    QList<Field> children;
};

struct Entry {
    qsizetype start = 0;
    qsizetype end = 0;
    QString indexKey;
    QList<Field> fields;
};

struct Document {
    qsizetype entriesStart = 0;
    qsizetype rootEnd = 0;
    QList<Entry> entries;
};

void setError(QString *errorMessage, const QString &message)
{
    if (errorMessage) {
        *errorMessage = message;
    }
}

bool readCString(const QByteArray &data, qsizetype &pos, QByteArray *value, QString *errorMessage)
{
    const qsizetype end = data.indexOf('\0', pos);
    if (end < 0) {
        setError(errorMessage, QStringLiteral("Unterminated VDF string"));
        return false;
    }
    *value = data.mid(pos, end - pos);
    pos = end + 1;
    return true;
}

bool readFixed(const QByteArray &data, qsizetype &pos, qsizetype size, QString *errorMessage)
{
    if (size < 0 || pos > data.size() - size) {
        setError(errorMessage, QStringLiteral("Truncated VDF value"));
        return false;
    }
    pos += size;
    return true;
}

bool parseRecord(const QByteArray &data, qsizetype &pos, int depth, Field *field, QString *errorMessage)
{
    if (depth > MaxNesting || pos >= data.size()) {
        setError(errorMessage, QStringLiteral("Invalid VDF nesting or truncated record"));
        return false;
    }

    field->start = pos;
    field->type = static_cast<quint8>(data.at(pos++));
    if (field->type == VdfEnd) {
        setError(errorMessage, QStringLiteral("Unexpected VDF end marker"));
        return false;
    }
    if (field->type != VdfObject && field->type != VdfString && field->type != VdfInt32
        && field->type != VdfFloat32 && field->type != VdfPointer && field->type != VdfWString
        && field->type != VdfColor && field->type != VdfUint64 && field->type != VdfInt64) {
        setError(errorMessage, QStringLiteral("Unsupported VDF type marker: 0x%1")
                                      .arg(field->type, 2, 16, QLatin1Char('0')));
        return false;
    }

    if (!readCString(data, pos, &field->keyBytes, errorMessage)) {
        return false;
    }
    field->key = QString::fromUtf8(field->keyBytes);
    field->valueStart = pos;

    switch (field->type) {
    case VdfObject:
        while (pos < data.size() && static_cast<quint8>(data.at(pos)) != VdfEnd) {
            Field child;
            if (!parseRecord(data, pos, depth + 1, &child, errorMessage)) {
                return false;
            }
            field->children.append(child);
        }
        if (pos >= data.size()) {
            setError(errorMessage, QStringLiteral("Unterminated VDF object"));
            return false;
        }
        ++pos;
        field->valueEnd = pos - 1;
        break;
    case VdfString:
        if (!readCString(data, pos, &field->valueBytes, errorMessage)) {
            return false;
        }
        field->valueEnd = pos - 1;
        break;
    case VdfWString: {
        const qsizetype valueStart = pos;
        bool terminated = false;
        while (pos + 1 < data.size()) {
            if (data.at(pos) == '\0' && data.at(pos + 1) == '\0') {
                field->valueBytes = data.mid(valueStart, pos - valueStart);
                pos += 2;
                terminated = true;
                break;
            }
            pos += 2;
        }
        if (!terminated) {
            setError(errorMessage, QStringLiteral("Unterminated VDF wide string"));
            return false;
        }
        field->valueEnd = pos - 2;
        break;
    }
    case VdfInt32:
    case VdfFloat32:
    case VdfPointer:
    case VdfColor:
        if (!readFixed(data, pos, 4, errorMessage)) {
            return false;
        }
        field->valueBytes = data.mid(field->valueStart, 4);
        field->valueEnd = pos;
        break;
    case VdfUint64:
    case VdfInt64:
        if (!readFixed(data, pos, 8, errorMessage)) {
            return false;
        }
        field->valueBytes = data.mid(field->valueStart, 8);
        field->valueEnd = pos;
        break;
    default:
        Q_UNREACHABLE();
    }

    field->end = pos;
    return true;
}

bool isDecimalKey(const QString &key)
{
    static const QRegularExpression expression(QStringLiteral("^[0-9]+$"));
    return expression.match(key).hasMatch();
}

bool parseDocument(const QByteArray &data, Document *document, QString *errorMessage)
{
    if (data.isEmpty()) {
        setError(errorMessage, QStringLiteral("Empty shortcuts.vdf"));
        return false;
    }
    if (data.size() > SteamShortcutsVdf::MaxDocumentSize) {
        setError(errorMessage, QStringLiteral("shortcuts.vdf is too large"));
        return false;
    }

    qsizetype pos = 0;
    Field root;
    if (!parseRecord(data, pos, 0, &root, errorMessage) || root.type != VdfObject
        || root.key != QStringLiteral("shortcuts")) {
        if (!errorMessage || errorMessage->isEmpty()) {
            setError(errorMessage, QStringLiteral("Invalid shortcuts.vdf root"));
        }
        return false;
    }

    QSet<QString> indices;
    document->entriesStart = root.valueStart;
    document->rootEnd = root.valueEnd;
    for (const Field &field : root.children) {
        if (field.type != VdfObject || !isDecimalKey(field.key) || indices.contains(field.key)) {
            setError(errorMessage, QStringLiteral("Invalid or duplicate shortcut index"));
            return false;
        }
        indices.insert(field.key);
        Entry entry;
        entry.start = field.start;
        entry.end = field.end;
        entry.indexKey = field.key;
        entry.fields = field.children;
        QSet<QString> criticalFields;
        for (const Field &child : entry.fields) {
            QString logicalKey = child.key;
            if (logicalKey == QStringLiteral("Exe")) {
                logicalKey = QStringLiteral("exe");
            } else if (logicalKey == QStringLiteral("AppId")) {
                logicalKey = QStringLiteral("appid");
            }
            if (logicalKey == QStringLiteral("AppName") || logicalKey == QStringLiteral("exe")
                || logicalKey == QStringLiteral("StartDir") || logicalKey == QStringLiteral("ShortcutPath")
                || logicalKey == QStringLiteral("LaunchOptions") || logicalKey == QStringLiteral("appid")) {
                if (criticalFields.contains(logicalKey)) {
                    setError(errorMessage, QStringLiteral("Duplicate critical shortcut field: %1").arg(logicalKey));
                    return false;
                }
                criticalFields.insert(logicalKey);
            }
        }
        document->entries.append(entry);
    }

    // parseRecord consumed the root END. A legacy Steam file may contain one outer END marker.
    if (pos < data.size()) {
        if (static_cast<quint8>(data.at(pos)) != VdfEnd || pos + 1 != data.size()) {
            setError(errorMessage, QStringLiteral("Trailing bytes after shortcuts.vdf root"));
            return false;
        }
        ++pos;
    }
    if (pos != data.size()) {
        setError(errorMessage, QStringLiteral("Invalid shortcuts.vdf footer"));
        return false;
    }
    return true;
}

const Field *findField(const Entry &entry, const QString &key)
{
    for (const Field &field : entry.fields) {
        if (field.key == key || (key == QStringLiteral("exe") && field.key == QStringLiteral("Exe"))
            || (key == QStringLiteral("appid") && field.key == QStringLiteral("AppId"))) {
            return &field;
        }
    }
    return nullptr;
}

QString stringField(const Entry &entry, const QString &key)
{
    const Field *field = findField(entry, key);
    return field && field->type == VdfString ? QString::fromUtf8(field->valueBytes) : QString();
}

quint32 intField(const Entry &entry, const QString &key)
{
    const Field *field = findField(entry, key);
    if (!field || field->type != VdfInt32 || field->valueBytes.size() != 4) {
        return 0;
    }
    return static_cast<quint32>(static_cast<quint8>(field->valueBytes.at(0)))
        | (static_cast<quint32>(static_cast<quint8>(field->valueBytes.at(1))) << 8)
        | (static_cast<quint32>(static_cast<quint8>(field->valueBytes.at(2))) << 16)
        | (static_cast<quint32>(static_cast<quint8>(field->valueBytes.at(3))) << 24);
}

SteamShortcut toShortcut(const Entry &entry)
{
    SteamShortcut shortcut;
    shortcut.appId = intField(entry, QStringLiteral("appid"));
    shortcut.appName = stringField(entry, QStringLiteral("AppName"));
    shortcut.exe = stringField(entry, QStringLiteral("exe"));
    shortcut.startDir = stringField(entry, QStringLiteral("StartDir"));
    shortcut.icon = stringField(entry, QStringLiteral("icon"));
    shortcut.shortcutPath = stringField(entry, QStringLiteral("ShortcutPath"));
    shortcut.launchOptions = stringField(entry, QStringLiteral("LaunchOptions"));
    shortcut.devkitGameId = stringField(entry, QStringLiteral("DevkitGameID"));
    shortcut.flatpakAppId = stringField(entry, QStringLiteral("FlatpakAppID"));
    shortcut.sortAs = stringField(entry, QStringLiteral("sortas"));
    shortcut.isHidden = intField(entry, QStringLiteral("IsHidden")) != 0;
    shortcut.allowDesktopConfig = intField(entry, QStringLiteral("AllowDesktopConfig")) != 0;
    shortcut.allowOverlay = intField(entry, QStringLiteral("AllowOverlay")) != 0;
    shortcut.openVR = intField(entry, QStringLiteral("OpenVR")) != 0;
    shortcut.devkit = intField(entry, QStringLiteral("Devkit")) != 0;
    shortcut.devkitOverrideAppId = intField(entry, QStringLiteral("DevkitOverrideAppID"));
    shortcut.lastPlayTime = intField(entry, QStringLiteral("LastPlayTime"));

    const Field *tags = findField(entry, QStringLiteral("tags"));
    if (tags && tags->type == VdfObject) {
        for (const Field &tag : tags->children) {
            if (tag.type == VdfString) {
                shortcut.tags.append(QString::fromUtf8(tag.valueBytes));
            }
        }
    }
    return shortcut;
}

QByteArray stringRecord(const QByteArray &key, const QString &value)
{
    QByteArray result;
    result.append(char(VdfString));
    result.append(key);
    result.append('\0');
    result.append(value.toUtf8());
    result.append('\0');
    return result;
}

QByteArray intRecord(const QByteArray &key, quint32 value)
{
    QByteArray result;
    result.append(char(VdfInt32));
    result.append(key);
    result.append('\0');
    result.append(static_cast<char>(value & 0xff));
    result.append(static_cast<char>((value >> 8) & 0xff));
    result.append(static_cast<char>((value >> 16) & 0xff));
    result.append(static_cast<char>((value >> 24) & 0xff));
    return result;
}

QByteArray tagsRecord(const QStringList &tags)
{
    QByteArray result;
    result.append(char(VdfObject));
    result.append("tags");
    result.append('\0');
    for (int i = 0; i < tags.size(); ++i) {
        result += stringRecord(QByteArray::number(i), tags.at(i));
    }
    result.append(char(VdfEnd));
    return result;
}

bool validString(const QString &value)
{
    return !value.contains(QChar(0));
}

QByteArray canonicalEntry(const SteamShortcut &shortcut, const QString &indexKey)
{
    QByteArray result;
    result.append(char(VdfObject));
    result.append(indexKey.toUtf8());
    result.append('\0');
    result += intRecord("appid", shortcut.appId);
    result += stringRecord("AppName", shortcut.appName);
    result += stringRecord("exe", shortcut.exe);
    result += stringRecord("StartDir", shortcut.startDir);
    result += stringRecord("icon", shortcut.icon);
    result += stringRecord("ShortcutPath", shortcut.shortcutPath);
    result += stringRecord("LaunchOptions", shortcut.launchOptions);
    result += intRecord("IsHidden", shortcut.isHidden ? 1 : 0);
    result += intRecord("AllowDesktopConfig", shortcut.allowDesktopConfig ? 1 : 0);
    result += intRecord("AllowOverlay", shortcut.allowOverlay ? 1 : 0);
    result += intRecord("OpenVR", shortcut.openVR ? 1 : 0);
    result += intRecord("Devkit", shortcut.devkit ? 1 : 0);
    result += intRecord("DevkitOverrideAppID", shortcut.devkitOverrideAppId);
    result += stringRecord("DevkitGameID", shortcut.devkitGameId);
    result += intRecord("LastPlayTime", shortcut.lastPlayTime);
    result += stringRecord("FlatpakAppID", shortcut.flatpakAppId);
    result += stringRecord("sortas", shortcut.sortAs);
    result += tagsRecord(shortcut.tags);
    result.append(char(VdfEnd));
    return result;
}

// Steam users can customize artwork, tags, and launch flags; only these application-owned fields are refreshed.
bool isReplacedField(const QString &key)
{
    return key == QStringLiteral("AppName") || key == QStringLiteral("exe") || key == QStringLiteral("Exe")
        || key == QStringLiteral("StartDir") || key == QStringLiteral("ShortcutPath")
        || key == QStringLiteral("LaunchOptions");
}

QByteArray replaceEntry(const QByteArray &bytes, const Entry &entry, const SteamShortcut &shortcut)
{
    QByteArray result;
    result.append(char(VdfObject));
    result.append(entry.indexKey.toUtf8());
    result.append('\0');
    for (const Field &field : entry.fields) {
        if (!isReplacedField(field.key)) {
            result += bytes.mid(field.start, field.end - field.start);
        }
    }
    result += stringRecord("AppName", shortcut.appName);
    result += stringRecord("exe", shortcut.exe);
    result += stringRecord("StartDir", shortcut.startDir);
    result += stringRecord("ShortcutPath", shortcut.shortcutPath);
    result += stringRecord("LaunchOptions", shortcut.launchOptions);
    result.append(char(VdfEnd));
    return result;
}

quint32 crc32(const QByteArray &data)
{
    quint32 crc = 0xffffffffu;
    for (const unsigned char byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & static_cast<quint32>(-(static_cast<qint32>(crc) & 1)));
        }
    }
    return crc ^ 0xffffffffu;
}

bool validShortcutForWrite(const SteamShortcut &shortcut, QString *errorMessage)
{
    const QList<QString> values = {shortcut.appName, shortcut.exe, shortcut.startDir, shortcut.icon,
                                   shortcut.shortcutPath, shortcut.launchOptions, shortcut.devkitGameId,
                                   shortcut.flatpakAppId, shortcut.sortAs};
    if (std::any_of(values.cbegin(), values.cend(), [](const QString &value) { return !validString(value); })
        || std::any_of(shortcut.tags.cbegin(), shortcut.tags.cend(), [](const QString &tag) { return !validString(tag); })) {
        setError(errorMessage, QStringLiteral("Shortcut contains a NUL character"));
        return false;
    }
    if (!SteamShortcutsVdf::isProfileShortcut(shortcut)) {
        setError(errorMessage, QStringLiteral("Shortcut does not contain a CouchPlay profile marker"));
        return false;
    }
    return true;
}

} // namespace

namespace SteamShortcutsVdf {

bool decode(const QByteArray &bytes, QList<SteamShortcut> *shortcuts, QString *errorMessage)
{
    if (errorMessage) {
        errorMessage->clear();
    }
    if (!shortcuts) {
        setError(errorMessage, QStringLiteral("Shortcut output is null"));
        return false;
    }
    shortcuts->clear();

    Document document;
    if (!parseDocument(bytes, &document, errorMessage)) {
        return false;
    }
    for (const Entry &entry : document.entries) {
        shortcuts->append(toShortcut(entry));
    }
    return true;
}

bool upsert(const QByteArray &bytes, const SteamShortcut &input, QByteArray *result, QString *errorMessage)
{
    if (errorMessage) {
        errorMessage->clear();
    }
    if (!result || !validShortcutForWrite(input, errorMessage)) {
        if (!result) {
            setError(errorMessage, QStringLiteral("Shortcut result is null"));
        }
        return false;
    }

    Document document;
    if (!parseDocument(bytes, &document, errorMessage)) {
        return false;
    }

    int matchingIndex = -1;
    for (int i = 0; i < document.entries.size(); ++i) {
        const SteamShortcut existing = toShortcut(document.entries.at(i));
        if (!isProfileShortcut(existing)) {
            continue;
        }
        if (existing.shortcutPath == input.shortcutPath) {
            if (matchingIndex >= 0) {
                setError(errorMessage, QStringLiteral("Duplicate CouchPlay profile shortcut marker"));
                return false;
            }
            matchingIndex = i;
        }
    }

    SteamShortcut shortcut = input;
    if (matchingIndex >= 0) {
        const Entry &existingEntry = document.entries.at(matchingIndex);
        const SteamShortcut existing = toShortcut(existingEntry);
        shortcut.appId = existing.appId;
        *result = bytes.left(existingEntry.start) + replaceEntry(bytes, existingEntry, shortcut)
            + bytes.mid(existingEntry.end);
        return true;
    }

    QSet<QString> usedIndices;
    QSet<quint32> usedAppIds;
    for (const Entry &entry : document.entries) {
        usedIndices.insert(entry.indexKey);
        usedAppIds.insert(toShortcut(entry).appId);
    }

    int index = 0;
    while (usedIndices.contains(QString::number(index))) {
        if (index == std::numeric_limits<int>::max()) {
            setError(errorMessage, QStringLiteral("No shortcut index is available"));
            return false;
        }
        ++index;
    }

    if (shortcut.appId == 0) {
        shortcut.appId = crc32(shortcut.exe.toUtf8() + shortcut.appName.toUtf8()) | 0x80000000u;
    }
    while (usedAppIds.contains(shortcut.appId)) {
        ++shortcut.appId;
        shortcut.appId |= 0x80000000u;
    }

    const QByteArray newEntry = canonicalEntry(shortcut, QString::number(index));
    *result = bytes.left(document.rootEnd) + newEntry + bytes.mid(document.rootEnd);
    return true;
}

bool withoutProfiles(const QByteArray &bytes, QByteArray *result, QString *errorMessage)
{
    if (errorMessage) {
        errorMessage->clear();
    }
    if (!result) {
        setError(errorMessage, QStringLiteral("Shortcut result is null"));
        return false;
    }

    Document document;
    if (!parseDocument(bytes, &document, errorMessage)) {
        return false;
    }

    QSet<QString> markers;
    for (const Entry &entry : document.entries) {
        const SteamShortcut shortcut = toShortcut(entry);
        if (!isProfileShortcut(shortcut)) {
            continue;
        }
        if (markers.contains(shortcut.shortcutPath)) {
            setError(errorMessage, QStringLiteral("Duplicate CouchPlay profile shortcut marker"));
            return false;
        }
        markers.insert(shortcut.shortcutPath);
    }

    QByteArray filtered = bytes.left(document.entriesStart);
    for (const Entry &entry : document.entries) {
        if (!isProfileShortcut(toShortcut(entry))) {
            filtered.append(bytes.mid(entry.start, entry.end - entry.start));
        }
    }
    filtered.append(bytes.mid(document.rootEnd));
    *result = filtered;
    return true;
}
bool mergePreservingProfiles(const QByteArray &source,
                            const QByteArray &target,
                            QByteArray *result,
                            QString *errorMessage)
{
    if (errorMessage) {
        errorMessage->clear();
    }
    if (!result) {
        setError(errorMessage, QStringLiteral("Shortcut result is null"));
        return false;
    }

    QByteArray filteredSource;
    if (!withoutProfiles(source, &filteredSource, errorMessage)) {
        return false;
    }

    Document sourceDocument;
    Document targetDocument;
    if (!parseDocument(filteredSource, &sourceDocument, errorMessage)
        || !parseDocument(target, &targetDocument, errorMessage)) {
        return false;
    }

    QSet<QString> usedIndices;
    for (const Entry &entry : sourceDocument.entries) {
        usedIndices.insert(entry.indexKey);
    }

    QSet<QString> profileMarkers;
    QByteArray merged = filteredSource.left(sourceDocument.rootEnd);
    for (const Entry &entry : targetDocument.entries) {
        const SteamShortcut shortcut = toShortcut(entry);
        if (!isProfileShortcut(shortcut)) {
            continue;
        }
        if (profileMarkers.contains(shortcut.shortcutPath)) {
            setError(errorMessage, QStringLiteral("Duplicate CouchPlay profile shortcut marker"));
            return false;
        }
        profileMarkers.insert(shortcut.shortcutPath);

        QString indexKey = entry.indexKey;
        if (usedIndices.contains(indexKey)) {
            int index = 0;
            while (usedIndices.contains(QString::number(index))) {
                if (index == std::numeric_limits<int>::max()) {
                    setError(errorMessage, QStringLiteral("No shortcut index is available"));
                    return false;
                }
                ++index;
            }
            indexKey = QString::number(index);
        }
        usedIndices.insert(indexKey);

        const QByteArray originalEntry = target.mid(entry.start, entry.end - entry.start);
        if (indexKey == entry.indexKey) {
            merged.append(originalEntry);
        } else {
            merged.append(char(VdfObject));
            merged.append(indexKey.toUtf8());
            merged.append('\0');
            merged.append(originalEntry.mid(entry.indexKey.toUtf8().size() + 2));
        }
        if (merged.size() > MaxDocumentSize) {
            setError(errorMessage, QStringLiteral("Merged shortcuts.vdf exceeds the size limit"));
            return false;
        }
    }
    merged.append(filteredSource.mid(sourceDocument.rootEnd));
    if (merged.size() > MaxDocumentSize) {
        setError(errorMessage, QStringLiteral("Merged shortcuts.vdf exceeds the size limit"));
        return false;
    }
    *result = merged;
    return true;
}

bool isProfileShortcut(const SteamShortcut &shortcut)
{
    static const QRegularExpression marker(QStringLiteral("^couchplay://profile/[0-9a-f]{64}\\z"));
    return marker.match(shortcut.shortcutPath).hasMatch();
}

QByteArray emptyDocument()
{
    QByteArray result;
    result.append(char(VdfObject));
    result.append("shortcuts");
    result.append('\0');
    result.append(char(VdfEnd));
    result.append(char(VdfEnd));
    return result;
}

} // namespace SteamShortcutsVdf
