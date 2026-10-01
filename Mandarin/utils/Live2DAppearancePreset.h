#pragma once

#include "Live2DOffscreenRenderer.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <cmath>

// 装扮是持久选择，心情是逐句变化；各自持有状态，避免换心情脱掉衣服。
class Live2DAppearancePreset
{
public:
    struct Option { QString id, label; QHash<QString, float> values; };
    struct Group { QString id, label, defaultOption; QVector<Option> options; };
    struct Look { QString id, label; QHash<QString, QString> selections; };

    bool load(const QString &path,
              const QHash<QString, Live2DOffscreenRenderer::DeclaredRange> &ranges,
              const QSet<QString> &forbidden, QString *error = nullptr,
              const QString &modelName = QString())
    {
        *this = {};
        auto fail = [&](const QString &message) { if (error) *error = message; return false; };
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return fail("Appearance file unavailable: " + path);
        QJsonParseError parse;
        const auto doc = QJsonDocument::fromJson(file.readAll(), &parse);
        if (parse.error != QJsonParseError::NoError || !doc.isObject())
            return fail("Invalid appearance JSON");
        const auto root = doc.object();
        if (root.value("version").toInt() != 1) return fail("Unsupported appearance version");
        if (!modelName.isEmpty() && root.value("model").toString() != modelName)
            return fail("Appearance belongs to another model");
        Live2DAppearancePreset candidate;
        QSet<QString> groupIds, owners;
        for (const auto value : root.value("groups").toArray())
        {
            const auto json = value.toObject();
            Group group{json.value("id").toString(), json.value("label").toString(),
                        json.value("default").toString(), {}};
            if (group.id.isEmpty() || group.label.isEmpty() || groupIds.contains(group.id))
                return fail("Invalid/duplicate appearance group");
            groupIds.insert(group.id);
            QSet<QString> optionIds, groupParameters;
            for (const auto optionValue : json.value("options").toArray())
            {
                const auto object = optionValue.toObject();
                Option option{object.value("id").toString(), object.value("label").toString(), {}};
                if (option.id.isEmpty() || option.label.isEmpty() || optionIds.contains(option.id) ||
                    !object.value("values").isObject()) return fail("Invalid appearance option");
                optionIds.insert(option.id);
                const auto values = object.value("values").toObject();
                for (auto entry = values.begin(); entry != values.end(); ++entry)
                {
                    if (!ranges.contains(entry.key()) || forbidden.contains(entry.key()) ||
                        !entry.value().isDouble()) return fail("Forbidden/unknown parameter: " + entry.key());
                    const auto range = ranges.value(entry.key());
                    const double number = entry.value().toDouble();
                    if (!std::isfinite(number) || number < range.min || number > range.max)
                        return fail("Appearance value out of range: " + entry.key());
                    option.values.insert(entry.key(), float(number));
                    candidate.m_neutral.insert(entry.key(), range.neutral);
                    groupParameters.insert(entry.key());
                }
                group.options.append(option);
            }
            if (!optionIds.contains(group.defaultOption) || groupParameters.isEmpty())
                return fail("Appearance group has no default/parameters");
            for (const auto &id : groupParameters)
            {
                if (owners.contains(id)) return fail("Parameter owned by multiple groups: " + id);
                owners.insert(id);
            }
            candidate.m_groups.append(group);
        }
        if (candidate.m_groups.isEmpty()) return fail("Empty appearance groups");
        candidate.reset();
        QSet<QString> lookIds;
        for (const auto value : root.value("looks").toArray())
        {
            const auto object = value.toObject();
            Look look{object.value("id").toString(), object.value("label").toString(), {}};
            if (look.id.isEmpty() || look.label.isEmpty() || lookIds.contains(look.id))
                return fail("Invalid appearance look");
            lookIds.insert(look.id);
            const auto selections = object.value("selections").toObject();
            if (selections.isEmpty()) return fail("Empty appearance look");
            for (auto entry = selections.begin(); entry != selections.end(); ++entry)
            {
                if (!candidate.hasOption(entry.key(), entry.value().toString()))
                    return fail("Invalid look selection");
                look.selections.insert(entry.key(), entry.value().toString());
            }
            candidate.m_looks.append(look);
        }
        *this = candidate;
        return true;
    }
    bool hasOption(const QString &groupId, const QString &optionId) const
    {
        for (const auto &group : m_groups)
            if (group.id == groupId)
                for (const auto &option : group.options)
                    if (option.id == optionId) return true;
        return false;
    }
    bool select(const QString &group, const QString &option)
    {
        if (!hasOption(group, option)) return false;
        m_selected.insert(group, option);
        return true;
    }
    bool selectLook(const QString &id)
    {
        for (const auto &look : m_looks)
        {
            if (look.id != id) continue;
            for (auto it = look.selections.begin(); it != look.selections.end(); ++it)
                m_selected.insert(it.key(), it.value());
            return true;
        }
        return false;
    }
    void reset()
    {
        m_selected.clear();
        for (const auto &group : m_groups)
            m_selected.insert(group.id, group.defaultOption);
    }
    QHash<QString, float> values() const
    {
        auto result = m_neutral;
        for (const auto &group : m_groups)
            for (const auto &option : group.options)
                if (option.id == m_selected.value(group.id))
                    for (auto it = option.values.begin(); it != option.values.end(); ++it)
                        result.insert(it.key(), it.value());
        return result;
    }
    QVector<Group> groups() const { return m_groups; }
    QVector<Look> looks() const { return m_looks; }
    QString selected(const QString &group) const { return m_selected.value(group); }
private:
    QVector<Group> m_groups;
    QVector<Look> m_looks;
    QHash<QString, QString> m_selected;
    QHash<QString, float> m_neutral;
};
