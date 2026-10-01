#pragma once
#include "AtomicJsonStore.h"
#include <QJsonArray>
#include <QSet>
#include <QUuid>

namespace MemoryStore {
inline bool ensureIds(QJsonObject &data)
{
    bool changed = false;
    QSet<QString> used;
    auto entries = data.value("help_summaries").toArray();
    for (int i = 0; i < entries.size(); ++i) {
        if (!entries[i].isObject()) continue;
        auto entry = entries[i].toObject();
        auto id = entry.value("id").toString();
        if (id.isEmpty() || used.contains(id)) {
            id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            entry["id"] = id;
            entries[i] = entry;
            changed = true;
        }
        used.insert(id);
    }
    if (changed) data["help_summaries"] = entries;
    return changed;
}
inline bool load(const QString &path, QJsonObject &data, QString *error = nullptr)
{
    QJsonObject candidate;
    if (!AtomicJsonStore::read(path, candidate, error)) return false;
    if (candidate.contains("help_summaries") && !candidate.value("help_summaries").isArray()) {
        if (error) *error = QStringLiteral("记忆摘要格式错误，原文件已保留");
        return false;
    }
    for (const auto &entry : candidate.value("help_summaries").toArray()) {
        if (!entry.isObject()) {
            if (error) *error = QStringLiteral("记忆条目格式错误，原文件已保留");
            return false;
        }
    }
    if (ensureIds(candidate) && !AtomicJsonStore::write(path, candidate, error)) return false;
    data = candidate;
    return true;
}
inline bool remove(const QString &path, const QString &id, bool all, QString *error = nullptr)
{
    QJsonObject data;
    if (!load(path, data, error)) return false;
    auto entries = data.value("help_summaries").toArray();
    bool found = all;
    for (int i = entries.size() - 1; i >= 0; --i) {
        if (all || entries[i].toObject().value("id").toString() == id) {
            entries.removeAt(i);
            found = true;
        }
    }
    if (!found) {
        if (error) *error = QStringLiteral("此条记忆已变化，请刷新列表");
        return false;
    }
    data["help_summaries"] = entries;
    return AtomicJsonStore::write(path, data, error);
}
inline bool removeForCharacter(const QString &displayedPath, const QString &currentPath,
                               const QString &id, bool all, QString *error = nullptr)
{
    if (displayedPath.isEmpty() || displayedPath != currentPath) {
        if (error) *error = QStringLiteral("角色已变化，请刷新记忆列表");
        return false;
    }
    return remove(displayedPath, id, all, error);
}
inline bool mergeExtraction(const QString &path, const QJsonObject &extraction,
                            const QString &date, QJsonObject &committed, QString *error = nullptr)
{
    QJsonObject data;
    if (!load(path, data, error)) return false;
    if (extraction.value("has_personal_info").toBool()) {
        auto info = data.value("personal_info").toObject();
        const auto additions = extraction.value("personal_info").toObject();
        for (auto it = additions.begin(); it != additions.end(); ++it) {
            if (!it.key().trimmed().isEmpty() && !it.value().toString().trimmed().isEmpty())
                info[it.key().trimmed()] = it.value().toString().trimmed();
        }
        data["personal_info"] = info;
    }
    const auto summary = extraction.value("help_summary").toString().trimmed();
    if (extraction.value("is_help").toBool() && !summary.isEmpty()) {
        auto entries = data.value("help_summaries").toArray();
        bool duplicate = false;
        for (const auto &entry : entries)
            if (entry.toObject().value("summary").toString() == summary) duplicate = true;
        if (!duplicate) {
            while (entries.size() >= 20) entries.removeFirst();
            entries.append(QJsonObject{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                                      {"topic", summary}, {"summary", summary}, {"date", date}});
            data["help_summaries"] = entries;
        }
    }
    if (!AtomicJsonStore::write(path, data, error)) return false;
    committed = data;
    return true;
}
}
