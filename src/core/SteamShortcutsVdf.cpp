// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include "SteamShortcutsVdf.h"

#include "SteamConfigManager.h"

#include <QHash>
#include <QList>
#include <QSet>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace {

constexpr quint8 VdfObject = 0x00;
constexpr quint8 VdfString = 0x01;
constexpr quint8 VdfInt32 = 0x02;
constexpr quint8 VdfFloat32 = 0x03;
constexpr quint8 VdfPointer = 0x04;
constexpr quint8 VdfColor = 0x06;
constexpr quint8 VdfUint64 = 0x07;
constexpr quint8 VdfEnd = 0x08;
constexpr quint8 VdfInt64 = 0x0a;

constexpr qsizetype MaxDocumentSize = 16 * 1024 * 1024;
constexpr qsizetype MaxNesting = 32;
constexpr qsizetype MaxParserRecords = 50'000;
constexpr qsizetype MaxShortcutTextSize = 1 * 1024 * 1024;
constexpr qsizetype MaxShortcutTags = 1024;

struct Record {
    qsizetype start = 0;
    qsizetype end = 0;
    qsizetype valueStart = 0;
    qsizetype valueEnd = 0;
    quint8 type = 0;
    QByteArray key;
    QList<Record> children;
};

struct Entry {
    qsizetype start = 0;
    qsizetype end = 0;
    QByteArray indexKey;
    QList<Record> fields;
};

struct Document {
    qsizetype rootEnd = 0;
    QList<Entry> entries;
};

struct ParseBudget {
    qsizetype records = 0;
};

void setError(QString *errorMessage, const QString &message)
{
    if (errorMessage) {
        *errorMessage = message;
    }
}

bool readCString(const QByteArray &data, qsizetype &position, QByteArray *value, QString *errorMessage)
{
    const qsizetype end = data.indexOf('\0', position);
    if (end < 0) {
        setError(errorMessage, QStringLiteral("Unterminated VDF string"));
        return false;
    }
    if (value) {
        *value = data.sliced(position, end - position);
    }
    position = end + 1;
    return true;
}

bool readFixed(const QByteArray &data, qsizetype &position, qsizetype size, QString *errorMessage)
{
    if (position < 0 || size < 0 || position > data.size() || size > data.size() - position) {
        setError(errorMessage, QStringLiteral("Truncated VDF value"));
        return false;
    }
    position += size;
    return true;
}

bool parseRecord(const QByteArray &data,
                qsizetype &position,
                qsizetype depth,
                ParseBudget &budget,
                Record *record,
                QString *errorMessage)
{
    if (++budget.records > MaxParserRecords) {
        setError(errorMessage, QStringLiteral("Too many VDF records"));
        return false;
    }
    if (depth > MaxNesting || position < 0 || position >= data.size()) {
        setError(errorMessage, QStringLiteral("Invalid VDF nesting or truncated record"));
        return false;
    }

    record->start = position;
    record->type = static_cast<quint8>(data.at(position++));
    if (record->type == VdfEnd) {
        setError(errorMessage, QStringLiteral("Unexpected VDF end marker"));
        return false;
    }
    if (record->type != VdfObject && record->type != VdfString && record->type != VdfInt32
        && record->type != VdfFloat32 && record->type != VdfPointer && record->type != VdfColor
        && record->type != VdfUint64 && record->type != VdfInt64) {
        setError(errorMessage,
                 QStringLiteral("Unsupported VDF type marker: 0x%1").arg(record->type, 2, 16, QLatin1Char('0')));
        return false;
    }

    if (!readCString(data, position, &record->key, errorMessage)) {
        return false;
    }
    record->valueStart = position;

    switch (record->type) {
    case VdfObject:
        while (position < data.size() && static_cast<quint8>(data.at(position)) != VdfEnd) {
            Record child;
            if (!parseRecord(data, position, depth + 1, budget, &child, errorMessage)) {
                return false;
            }
            record->children.append(std::move(child));
        }
        if (position >= data.size()) {
            setError(errorMessage, QStringLiteral("Unterminated VDF object"));
            return false;
        }
        ++position;
        record->valueEnd = position - 1;
        break;
    case VdfString:
        if (!readCString(data, position, nullptr, errorMessage)) {
            return false;
        }
        record->valueEnd = position - 1;
        break;
    case VdfInt32:
    case VdfFloat32:
    case VdfPointer:
    case VdfColor:
        if (!readFixed(data, position, 4, errorMessage)) {
            return false;
        }
        record->valueEnd = position;
        break;
    case VdfUint64:
    case VdfInt64:
        if (!readFixed(data, position, 8, errorMessage)) {
            return false;
        }
        record->valueEnd = position;
        break;
    default:
        Q_UNREACHABLE();
    }

    record->end = position;
    return true;
}

bool isDecimalKey(const QByteArray &key)
{
        if (key.isEmpty() || (key.size() > 1 && key.at(0) == '0')) {
    for (const unsigned char character : key) {
        if (character < '0' || character > '9') {
            return false;
        }
    }
    return true;
}

bool isCriticalKey(const QByteArray &key)
{
    return key == "AppName" || key == "exe" || key == "Exe" || key == "StartDir" || key == "ShortcutPath"
        || key == "LaunchOptions" || key == "appid" || key == "AppId";
}

QByteArray logicalCriticalKey(const QByteArray &key)
{
    if (key == "Exe") {
        return QByteArrayLiteral("exe");
    }
    if (key == "AppId") {
        return QByteArrayLiteral("appid");
    }
    return key;
}

bool parseDocument(const QByteArray &data, Document *document, QString *errorMessage)
{
    if (data.isEmpty()) {
        setError(errorMessage, QStringLiteral("Empty shortcuts.vdf"));
        return false;
    }
    if (data.size() > MaxDocumentSize) {
        setError(errorMessage, QStringLiteral("shortcuts.vdf is too large"));
        return false;
    }

    qsizetype position = 0;
    ParseBudget budget;
    Record root;
    if (!parseRecord(data, position, 0, budget, &root, errorMessage) || root.type != VdfObject
        || root.key != QByteArrayLiteral("shortcuts")) {
        if (!errorMessage || errorMessage->isEmpty()) {
            setError(errorMessage, QStringLiteral("Invalid shortcuts.vdf root"));
        }
        return false;
    }

    QSet<QByteArray> indices;
    document->rootEnd = root.valueEnd;
    for (const Record &field : root.children) {
        if (field.type != VdfObject || !isDecimalKey(field.key) || indices.contains(field.key)) {
            setError(errorMessage, QStringLiteral("Invalid or duplicate shortcut index"));
            return false;
        }
        bool validIndex = false;
        const quint64 numericIndex = field.key.toULongLong(&validIndex);
        if (!validIndex || QByteArray::number(numericIndex) != field.key) {
            setError(errorMessage, QStringLiteral("Invalid or duplicate shortcut index"));
            return false;
        }
        indices.insert(field.key);

        Entry entry;
        entry.start = field.start;
        entry.end = field.end;
        entry.indexKey = field.key;
        entry.fields = field.children;

        QSet<QByteArray> criticalFields;
        for (const Record &child : entry.fields) {
            if (!isCriticalKey(child.key)) {
                continue;
            }
            if ((child.key == "appid" || child.key == "AppId") && child.type != VdfInt32) {
                setError(errorMessage, QStringLiteral("Invalid shortcut AppId type"));
                return false;
            }
            const QByteArray logicalKey = logicalCriticalKey(child.key);
            if (criticalFields.contains(logicalKey)) {
                setError(errorMessage, QStringLiteral("Duplicate critical shortcut field"));
                return false;
            }
            criticalFields.insert(logicalKey);
        }
        document->entries.append(std::move(entry));
    }

    // Steam normally writes one final end marker after the root object.
    if (position < data.size()) {
        if (static_cast<quint8>(data.at(position)) != VdfEnd || position + 1 != data.size()) {
            setError(errorMessage, QStringLiteral("Trailing bytes after shortcuts.vdf root"));
            return false;
        }
        ++position;
    }
    if (position != data.size()) {
        setError(errorMessage, QStringLiteral("Invalid shortcuts.vdf footer"));
        return false;
    }
    return true;
}

bool sameString(const QByteArray &data, const Record &record, const QByteArray &value)
{
    if (record.type != VdfString || record.valueEnd - record.valueStart != value.size()) {
        return false;
    }
    return std::memcmp(data.constData() + record.valueStart, value.constData(), static_cast<size_t>(value.size())) == 0;
}

bool readAppId(const QByteArray &data, const Entry &entry, quint32 *appId)
{
    for (const Record &field : entry.fields) {
        if (field.key == QByteArrayLiteral("appid") || field.key == QByteArrayLiteral("AppId")) {
            if (field.type != VdfInt32 || field.valueEnd - field.valueStart != 4) {
                return false;
            }
            const auto *value = reinterpret_cast<const unsigned char *>(data.constData() + field.valueStart);
            *appId = static_cast<quint32>(value[0]) | (static_cast<quint32>(value[1]) << 8)
                | (static_cast<quint32>(value[2]) << 16) | (static_cast<quint32>(value[3]) << 24);
            return true;
        }
    }
    return false;
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

bool allocateUniqueAppId(quint32 preferred,
                         const QByteArray &appName,
                         const QByteArray &exe,
                         const QSet<quint32> &usedAppIds,
                         quint32 *appId)
{
    quint32 candidate = preferred;
    if (candidate == 0) {
        candidate = crc32(exe + appName) | 0x80000000u;
    }

    const quint64 attempts = static_cast<quint64>(usedAppIds.size()) + 1;
    for (quint64 attempt = 0; attempt < attempts; ++attempt) {
        if (candidate != 0 && !usedAppIds.contains(candidate)) {
            *appId = candidate;
            return true;
        }
        ++candidate;
        candidate |= 0x80000000u;
    }
    return false;
}

QByteArray intRecord(const QByteArray &key, quint32 value)
{
    QByteArray result;
    result.reserve(1 + key.size() + 1 + 4);
    result.append(char(VdfInt32));
    result.append(key);
    result.append('\0');
    result.append(static_cast<char>(value & 0xff));
    result.append(static_cast<char>((value >> 8) & 0xff));
    result.append(static_cast<char>((value >> 16) & 0xff));
    result.append(static_cast<char>((value >> 24) & 0xff));
    return result;
}

QByteArray stringRecord(const QByteArray &key, const QString &value)
{
    const QByteArray encoded = value.toUtf8();
    QByteArray result;
    result.reserve(1 + key.size() + 1 + encoded.size() + 1);
    result.append(char(VdfString));
    result.append(key);
    result.append('\0');
    result.append(encoded);
    result.append('\0');
    return result;
}

QByteArray stringRecord(const QByteArray &key, const QByteArray &value)
{
    QByteArray result;
    result.reserve(1 + key.size() + 1 + value.size() + 1);
    result.append(char(VdfString));
    result.append(key);
    result.append('\0');
    result.append(value);
    result.append('\0');
    return result;
}

QByteArray tagsRecord(const QStringList &tags)
{
    QByteArray result;
    result.append(char(VdfObject));
    result.append("tags\0", 5);
    for (int i = 0; i < tags.size(); ++i) {
        result += stringRecord(QByteArray::number(i), tags.at(i));
    }
    result.append(char(VdfEnd));
    return result;
}

bool validText(const QString &value)
{
    return value.size() <= MaxShortcutTextSize && !value.contains(QChar(0));
}

bool validProfileMarker(const QByteArray &marker)
{
    const QByteArray prefix = QByteArrayLiteral("couchplay://profile/");
    if (marker.size() != prefix.size() + 64 || !marker.startsWith(prefix)) {
        return false;
    }
    for (const unsigned char character : marker.sliced(prefix.size())) {
        const bool decimal = character >= '0' && character <= '9';
        const bool lowerHex = character >= 'a' && character <= 'f';
        if (!decimal && !lowerHex) {
            return false;
        }
    }
    return true;
}

bool validShortcutForWrite(const SteamShortcut &shortcut, QByteArray *marker, QString *errorMessage)
{
    const QList<QString> values = {shortcut.appName, shortcut.exe, shortcut.startDir, shortcut.icon,
                                   shortcut.shortcutPath, shortcut.launchOptions, shortcut.devkitGameId,
                                   shortcut.flatpakAppId, shortcut.sortAs};
    if (std::any_of(values.cbegin(), values.cend(), [](const QString &value) { return !validText(value); })
        || std::any_of(shortcut.tags.cbegin(), shortcut.tags.cend(), [](const QString &tag) { return !validText(tag); })) {
        setError(errorMessage, QStringLiteral("Shortcut contains an invalid text value"));
        return false;
    }
    if (shortcut.tags.size() > MaxShortcutTags) {
        setError(errorMessage, QStringLiteral("Shortcut contains too many tags"));
        return false;
    }

    *marker = shortcut.shortcutPath.toUtf8();
    if (!validProfileMarker(*marker)) {
        setError(errorMessage, QStringLiteral("Shortcut does not contain a CouchPlay profile marker"));
        return false;
    }

    qsizetype estimatedBytes = 0;
    const auto addEstimate = [&estimatedBytes](const QString &value) {
        constexpr qsizetype maxUtf8BytesPerCharacter = 4;
        if (value.size() > (MaxDocumentSize - estimatedBytes) / maxUtf8BytesPerCharacter) {
            return false;
        }
        estimatedBytes += value.size() * maxUtf8BytesPerCharacter;
        return true;
    };
    for (const QString &value : values) {
        if (!addEstimate(value)) {
            setError(errorMessage, QStringLiteral("Shortcut fields are too large"));
            return false;
        }
    }
    for (const QString &tag : shortcut.tags) {
        if (!addEstimate(tag)) {
            setError(errorMessage, QStringLiteral("Shortcut tags are too large"));
            return false;
        }
    }
    return true;
}

QByteArray canonicalEntry(const SteamShortcut &shortcut,
                          const QByteArray &indexKey,
                          const QByteArray &marker,
                          quint32 appId)
{
    QByteArray result;
    result.append(char(VdfObject));
    result.append(indexKey);
    result.append('\0');
    result += intRecord(QByteArrayLiteral("appid"), appId);
    result += stringRecord(QByteArrayLiteral("AppName"), shortcut.appName);
    result += stringRecord(QByteArrayLiteral("exe"), shortcut.exe);
    result += stringRecord(QByteArrayLiteral("StartDir"), shortcut.startDir);
    result += stringRecord(QByteArrayLiteral("icon"), shortcut.icon);
    result += stringRecord(QByteArrayLiteral("ShortcutPath"), marker);
    result += stringRecord(QByteArrayLiteral("LaunchOptions"), shortcut.launchOptions);
    result += intRecord(QByteArrayLiteral("IsHidden"), shortcut.isHidden ? 1 : 0);
    result += intRecord(QByteArrayLiteral("AllowDesktopConfig"), shortcut.allowDesktopConfig ? 1 : 0);
    result += intRecord(QByteArrayLiteral("AllowOverlay"), shortcut.allowOverlay ? 1 : 0);
    result += intRecord(QByteArrayLiteral("OpenVR"), shortcut.openVR ? 1 : 0);
    result += intRecord(QByteArrayLiteral("Devkit"), shortcut.devkit ? 1 : 0);
    result += intRecord(QByteArrayLiteral("DevkitOverrideAppID"), shortcut.devkitOverrideAppId);
    result += stringRecord(QByteArrayLiteral("DevkitGameID"), shortcut.devkitGameId);
    result += intRecord(QByteArrayLiteral("LastPlayTime"), shortcut.lastPlayTime);
    result += stringRecord(QByteArrayLiteral("FlatpakAppID"), shortcut.flatpakAppId);
    result += stringRecord(QByteArrayLiteral("sortas"), shortcut.sortAs);
    result += tagsRecord(shortcut.tags);
    result.append(char(VdfEnd));
    return result;
}

bool isReplacedField(const QByteArray &key)
{
    return key == QByteArrayLiteral("AppName") || key == QByteArrayLiteral("exe") || key == QByteArrayLiteral("Exe")
        || key == QByteArrayLiteral("StartDir") || key == QByteArrayLiteral("ShortcutPath")
        || key == QByteArrayLiteral("LaunchOptions");
}

QByteArray replacementEntry(const QByteArray &data,
                            const Entry &entry,
                            const SteamShortcut &shortcut,
                            const QByteArray &marker,
                            quint32 appId)
{
    QByteArray result;
    result.append(char(VdfObject));
    result.append(entry.indexKey);
    result.append('\0');

    bool replacedAppId = false;
    for (const Record &field : entry.fields) {
        if (isReplacedField(field.key)) {
            continue;
        }
        if (field.key == QByteArrayLiteral("appid") || field.key == QByteArrayLiteral("AppId")) {
            result += intRecord(field.key, appId);
            replacedAppId = true;
        } else {
            result.append(data.constData() + field.start, field.end - field.start);
        }
    }
    if (!replacedAppId) {
        result += intRecord(QByteArrayLiteral("appid"), appId);
    }
    result += stringRecord(QByteArrayLiteral("AppName"), shortcut.appName);
    result += stringRecord(QByteArrayLiteral("exe"), shortcut.exe);
    result += stringRecord(QByteArrayLiteral("StartDir"), shortcut.startDir);
    result += stringRecord(QByteArrayLiteral("ShortcutPath"), marker);
    result += stringRecord(QByteArrayLiteral("LaunchOptions"), shortcut.launchOptions);
    result.append(char(VdfEnd));
    return result;
}

} // namespace

namespace SteamShortcutsVdf {

bool upsert(const QByteArray &bytes, const SteamShortcut &shortcut, QByteArray *result, QString *errorMessage)
{
    if (errorMessage) {
        errorMessage->clear();
    }
    if (!result) {
        setError(errorMessage, QStringLiteral("Shortcut result is null"));
        return false;
    }

    QByteArray marker;
    if (!validShortcutForWrite(shortcut, &marker, errorMessage)) {
        return false;
    }

    Document document;
    if (!parseDocument(bytes, &document, errorMessage)) {
        return false;
    }

    int matchingIndex = -1;
    QSet<quint32> usedAppIds;
    for (int index = 0; index < document.entries.size(); ++index) {
        const Entry &entry = document.entries.at(index);
        quint32 appId = 0;
        if (readAppId(bytes, entry, &appId) && appId != 0 && usedAppIds.contains(appId)) {
            setError(errorMessage, QStringLiteral("Duplicate shortcut AppId"));
            return false;
        }
        if (appId != 0) {
            usedAppIds.insert(appId);
        }
        for (const Record &field : entry.fields) {
            if (field.key == QByteArrayLiteral("ShortcutPath") && sameString(bytes, field, marker)) {
                if (matchingIndex >= 0) {
                    setError(errorMessage, QStringLiteral("Duplicate CouchPlay profile shortcut marker"));
                    return false;
                }
                matchingIndex = index;
            }
        }
    }

    quint32 appId = shortcut.appId;
    if (matchingIndex >= 0) {
        const Entry &entry = document.entries.at(matchingIndex);
        quint32 existingAppId = 0;
        if (readAppId(bytes, entry, &existingAppId)) {
            appId = existingAppId;
        }
        usedAppIds.remove(existingAppId);
        if (!allocateUniqueAppId(appId, shortcut.appName.toUtf8(), shortcut.exe.toUtf8(), usedAppIds, &appId)) {
            setError(errorMessage, QStringLiteral("No unique shortcut AppId is available"));
            return false;
        }

        const QByteArray replacement = replacementEntry(bytes, entry, shortcut, marker, appId);
        const qsizetype oldSize = entry.end - entry.start;
        if (replacement.size() > MaxDocumentSize - (bytes.size() - oldSize)) {
            setError(errorMessage, QStringLiteral("Updated shortcuts.vdf exceeds the size limit"));
            return false;
        }
        QByteArray updated;
        updated.reserve(bytes.size() - oldSize + replacement.size());
        updated.append(bytes.constData(), entry.start);
        updated += replacement;
        updated.append(bytes.constData() + entry.end, bytes.size() - entry.end);
        *result = std::move(updated);
        return true;
    }

    QSet<QByteArray> usedIndices;
    for (const Entry &entry : document.entries) {
        usedIndices.insert(entry.indexKey);
    }
    QByteArray indexKey;
    for (quint64 index = 0;; ++index) {
        indexKey = QByteArray::number(index);
        if (!usedIndices.contains(indexKey)) {
            break;
        }
        if (index == std::numeric_limits<quint64>::max()) {
            setError(errorMessage, QStringLiteral("No shortcut index is available"));
            return false;
        }
    }

    if (!allocateUniqueAppId(appId, shortcut.appName.toUtf8(), shortcut.exe.toUtf8(), usedAppIds, &appId)) {
        setError(errorMessage, QStringLiteral("No unique shortcut AppId is available"));
        return false;
    }
    const QByteArray newEntry = canonicalEntry(shortcut, indexKey, marker, appId);
    if (newEntry.size() > MaxDocumentSize - bytes.size()) {
        setError(errorMessage, QStringLiteral("Updated shortcuts.vdf exceeds the size limit"));
        return false;
    }

    QByteArray updated;
    updated.reserve(bytes.size() + newEntry.size());
    updated.append(bytes.constData(), document.rootEnd);
    updated += newEntry;
    updated.append(bytes.constData() + document.rootEnd, bytes.size() - document.rootEnd);
    *result = std::move(updated);
    return true;
}

QByteArray emptyDocument()
{
    QByteArray result;
    result.append(char(VdfObject));
    result.append("shortcuts\0", 10);
    result.append(char(VdfEnd));
    result.append(char(VdfEnd));
    return result;
}

} // namespace SteamShortcutsVdf
