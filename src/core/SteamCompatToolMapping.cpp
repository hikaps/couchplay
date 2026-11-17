// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 CouchPlay Contributors

#include "SteamCompatToolMapping.h"

#include <QList>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>

namespace SteamCompatToolMapping
{
namespace
{

// Text KeyValues node with the byte span it occupies in the source document
struct Node
{
    QString key;
    QString value;
    bool isBlock = false;
    qsizetype start = 0; // first byte of the key token
    qsizetype end = 0; // one past the value token or the closing brace
    qsizetype closeBrace = -1; // position of '}' for blocks
    QList<Node> children;
};

class Parser
{
public:
    explicit Parser(const QByteArray &text)
        : m_text(text)
    {
    }

    // Top-level nodes; std::nullopt on any syntax error
    std::optional<QList<Node>> parseDocument()
    {
        QList<Node> nodes;
        if (!parseNodes(nodes, false)) {
            return std::nullopt;
        }
        return nodes;
    }

private:
    enum class TokenType { End, Open, Close, String, Error };
    struct Token
    {
        TokenType type = TokenType::End;
        QString text;
        qsizetype start = 0;
        qsizetype end = 0;
    };

    void skipWhitespaceAndComments()
    {
        while (m_pos < m_text.size()) {
            const char c = m_text.at(m_pos);
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++m_pos;
            } else if (c == '/' && m_pos + 1 < m_text.size() && m_text.at(m_pos + 1) == '/') {
                while (m_pos < m_text.size() && m_text.at(m_pos) != '\n') {
                    ++m_pos;
                }
            } else {
                break;
            }
        }
    }

    Token next()
    {
        skipWhitespaceAndComments();
        Token token;
        token.start = m_pos;
        if (m_pos >= m_text.size()) {
            token.end = m_pos;
            return token;
        }
        const char c = m_text.at(m_pos);
        if (c == '{' || c == '}') {
            token.type = c == '{' ? TokenType::Open : TokenType::Close;
            token.end = ++m_pos;
            return token;
        }
        QByteArray bytes;
        if (c == '"') {
            ++m_pos;
            while (m_pos < m_text.size() && m_text.at(m_pos) != '"') {
                char ch = m_text.at(m_pos++);
                if (ch == '\\' && m_pos < m_text.size()) {
                    const char escaped = m_text.at(m_pos++);
                    ch = escaped == 'n' ? '\n' : escaped == 't' ? '\t' : escaped;
                }
                bytes.append(ch);
            }
            if (m_pos >= m_text.size()) {
                token.type = TokenType::Error;
                return token;
            }
            ++m_pos; // closing quote
        } else {
            while (m_pos < m_text.size()) {
                const char ch = m_text.at(m_pos);
                if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '{' || ch == '}' || ch == '"') {
                    break;
                }
                bytes.append(ch);
                ++m_pos;
            }
        }
        token.type = TokenType::String;
        token.text = QString::fromUtf8(bytes);
        token.end = m_pos;
        return token;
    }

    bool parseNodes(QList<Node> &nodes, bool insideBlock)
    {
        while (true) {
            const Token key = next();
            if (key.type == TokenType::End) {
                return !insideBlock;
            }
            if (key.type == TokenType::Close) {
                if (!insideBlock) {
                    return false;
                }
                m_lastClose = key.start;
                return true;
            }
            if (key.type != TokenType::String) {
                return false;
            }

            Node node;
            node.key = key.text;
            node.start = key.start;
            const Token value = next();
            if (value.type == TokenType::String) {
                node.value = value.text;
                node.end = value.end;
            } else if (value.type == TokenType::Open) {
                node.isBlock = true;
                if (!parseNodes(node.children, true)) {
                    return false;
                }
                node.closeBrace = m_lastClose;
                node.end = m_lastClose + 1;
            } else {
                return false;
            }
            nodes.append(node);
        }
    }

    const QByteArray &m_text;
    qsizetype m_pos = 0;
    qsizetype m_lastClose = -1;
};

const Node *findChild(const QList<Node> &nodes, const QString &key)
{
    for (const Node &node : nodes) {
        if (node.isBlock && node.key.compare(key, Qt::CaseInsensitive) == 0) {
            return &node;
        }
    }
    return nullptr;
}

const Node *findSteamSection(const QList<Node> &document)
{
    const Node *node = findChild(document, QStringLiteral("InstallConfigStore"));
    for (const QString &key : {QStringLiteral("Software"), QStringLiteral("Valve"), QStringLiteral("Steam")}) {
        if (!node) {
            return nullptr;
        }
        node = findChild(node->children, key);
    }
    return node;
}

// Depth of the Steam section's children: InstallConfigStore/Software/Valve/Steam/<child>
constexpr int kMappingDepth = 4;

std::optional<Tool> toolFromNode(const Node &entry)
{
    if (!entry.isBlock) {
        return std::nullopt;
    }
    Tool tool;
    for (const Node &field : entry.children) {
        if (field.isBlock) {
            continue;
        }
        if (field.key.compare(QStringLiteral("name"), Qt::CaseInsensitive) == 0) {
            tool.name = field.value;
        } else if (field.key.compare(QStringLiteral("config"), Qt::CaseInsensitive) == 0) {
            tool.config = field.value;
        } else if (field.key.compare(QStringLiteral("priority"), Qt::CaseInsensitive) == 0) {
            tool.priority = field.value;
        }
    }
    return tool;
}

QByteArray quoted(const QString &text)
{
    QByteArray escaped = text.toUtf8();
    escaped.replace("\\", "\\\\");
    escaped.replace("\"", "\\\"");
    return '"' + escaped + '"';
}

// Entry text without leading indentation, ending at its closing brace
QByteArray serializeEntry(const QString &appId, const Tool &tool, int depth)
{
    const QByteArray indent(depth, '\t');
    const QByteArray fieldIndent(depth + 1, '\t');
    QByteArray out = quoted(appId) + '\n' + indent + "{\n";
    out += fieldIndent + "\"name\"\t\t" + quoted(tool.name) + '\n';
    out += fieldIndent + "\"config\"\t\t" + quoted(tool.config) + '\n';
    out += fieldIndent + "\"priority\"\t\t" + quoted(tool.priority) + '\n';
    out += indent + '}';
    return out;
}

// Start of the line holding @p pos when only indentation precedes it there, else @p pos
qsizetype lineStartIfIndented(const QByteArray &text, qsizetype pos)
{
    qsizetype i = pos;
    while (i > 0 && (text.at(i - 1) == ' ' || text.at(i - 1) == '\t')) {
        --i;
    }
    return (i == 0 || text.at(i - 1) == '\n') ? i : pos;
}

struct Edit
{
    qsizetype start;
    qsizetype end;
    QByteArray replacement;
};

}

std::optional<QMap<QString, Tool>> read(const QByteArray &configVdf)
{
    const auto document = Parser(configVdf).parseDocument();
    if (!document) {
        return std::nullopt;
    }
    QMap<QString, Tool> mapping;
    const Node *steam = findSteamSection(*document);
    const Node *compat = steam ? findChild(steam->children, QStringLiteral("CompatToolMapping")) : nullptr;
    if (!compat) {
        return mapping;
    }
    for (const Node &entry : compat->children) {
        const auto tool = toolFromNode(entry);
        if (tool && !mapping.contains(entry.key)) {
            mapping.insert(entry.key, *tool);
        }
    }
    return mapping;
}

std::optional<QByteArray> apply(const QByteArray &configVdf, const Changes &changes)
{
    const auto document = Parser(configVdf).parseDocument();
    if (!document) {
        return std::nullopt;
    }
    const Node *steam = findSteamSection(*document);
    if (!steam) {
        return std::nullopt;
    }
    const Node *compat = findChild(steam->children, QStringLiteral("CompatToolMapping"));

    QList<Edit> edits;
    QByteArray additions;
    const int entryDepth = kMappingDepth + 1;

    for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
        const QString &appId = it.key();
        const std::optional<Tool> &wanted = it.value();
        bool kept = false;
        if (compat) {
            for (const Node &entry : compat->children) {
                if (entry.key != appId) {
                    continue;
                }
                const bool keep = wanted && !kept;
                if (keep && toolFromNode(entry) == wanted) {
                    kept = true;
                    continue;
                }
                if (keep) {
                    edits.append({entry.start, entry.end, serializeEntry(appId, *wanted, entryDepth)});
                    kept = true;
                    continue;
                }
                // Remove the entry together with its indentation and line break
                qsizetype start = lineStartIfIndented(configVdf, entry.start);
                qsizetype end = entry.end;
                if (start != entry.start && end < configVdf.size() && configVdf.at(end) == '\n') {
                    ++end;
                }
                edits.append({start, end, QByteArray()});
            }
        }
        if (wanted && !kept) {
            additions += QByteArray(entryDepth, '\t') + serializeEntry(appId, *wanted, entryDepth) + '\n';
        }
    }

    if (!additions.isEmpty()) {
        if (compat) {
            const qsizetype at = lineStartIfIndented(configVdf, compat->closeBrace);
            edits.append({at, at, at == compat->closeBrace ? '\n' + additions : additions});
        } else {
            const QByteArray indent(kMappingDepth, '\t');
            const QByteArray block = indent + "\"CompatToolMapping\"\n" + indent + "{\n" + additions + indent + "}\n";
            const qsizetype at = lineStartIfIndented(configVdf, steam->closeBrace);
            edits.append({at, at, at == steam->closeBrace ? '\n' + block : block});
        }
    }

    // Edit spans never overlap; apply back to front so earlier offsets stay valid
    std::sort(edits.begin(), edits.end(), [](const Edit &a, const Edit &b) { return a.start > b.start; });
    QByteArray result = configVdf;
    for (const Edit &edit : edits) {
        result.replace(edit.start, edit.end - edit.start, edit.replacement);
    }
    return result;
}

QVariantMap toVariantMap(const Changes &changes)
{
    QVariantMap map;
    for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
        map.insert(it.key(), it.value() ? QStringList{it.value()->name, it.value()->config, it.value()->priority}
                                        : QStringList());
    }
    return map;
}

std::optional<Changes> fromVariantMap(const QVariantMap &map)
{
    static const QRegularExpression appIdPattern(QStringLiteral("^[0-9]{1,10}$"));
    static const QRegularExpression controlChars(QStringLiteral("[\\x00-\\x1f\\x7f]"));
    constexpr qsizetype maxFieldLength = 256;

    Changes changes;
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        if (!appIdPattern.match(it.key()).hasMatch() || it.value().typeId() != QMetaType::QStringList) {
            return std::nullopt;
        }
        const QStringList fields = it.value().toStringList();
        if (fields.isEmpty()) {
            changes.insert(it.key(), std::nullopt);
            continue;
        }
        if (fields.size() != 3 || fields.first().isEmpty()) {
            return std::nullopt;
        }
        for (const QString &field : fields) {
            if (field.size() > maxFieldLength || controlChars.match(field).hasMatch()) {
                return std::nullopt;
            }
        }
        changes.insert(it.key(), Tool{fields.at(0), fields.at(1), fields.at(2)});
    }
    return changes;
}

}
