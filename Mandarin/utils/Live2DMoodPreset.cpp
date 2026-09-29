#include "Live2DMoodPreset.h"

#include "../GlobalConstants.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

#include <algorithm>

namespace
{
/*读一份 JSON 对象。路径可能含中文（角色名/模型名）—— 一律用 QFile 读成字节再交给
  QJsonDocument**在内存里**解析，绝不把路径交给别的库去 fopen。*/
bool readJsonObject(const QString &path, QJsonObject *out, QString *error)
{
    QFile file(path);
    if (!file.exists())
    {
        if (error)
            *error = QStringLiteral("文件不存在：%1").arg(path);
        return false;
    }
    if (!file.open(QIODevice::ReadOnly))
    {
        if (error)
            *error = QStringLiteral("打不开：%1").arg(path);
        return false;
    }
    const QByteArray bytes = file.readAll();
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
    {
        if (error)
            *error = QStringLiteral("%1 解析失败：%2").arg(path, parseError.errorString());
        return false;
    }
    if (out)
        *out = doc.object();
    return true;
}

/*语义名里被驱动器/物理占用的那几个。按**语义名**再兜一层：
  即使 parameter-map 把 "breath" 映射到别的 ID（写错了），也不该去覆盖它。*/
bool isUpdaterOwnedSemanticName(const QString &semanticName)
{
    return semanticName == QStringLiteral("breath") || semanticName == QStringLiteral("hairFront") ||
           semanticName == QStringLiteral("hairSide") || semanticName == QStringLiteral("hairBack");
}
} // namespace

bool Live2DMoodPreset::isUpdaterOwnedParameter(const QString &parameterId)
{
    /*判定按**真实参数 ID**（这才是会被写进模型的那一项）。这几个参数每帧由呼吸驱动器/
      物理写入，覆盖它们等于把那套动作做废 —— 所以永远不放进覆盖表。*/
    return parameterId == QStringLiteral("ParamBreath") ||
           parameterId == QStringLiteral("ParamHairFront") ||
           parameterId == QStringLiteral("ParamHairSide") ||
           parameterId == QStringLiteral("ParamHairBack");
}

/*模型目录 = CharacterAssestPath + character/CharSelect + 模型名（与 parameter-map.json
  实际所在的层级一致）。

  ⚠️ CharSelect 只从**真实** config.ini 读（ReadNowSelectChar），不走 Live2DCharacterWindow
  的 MANDARIN_CONFIG_INI 重定向：那个重定向是给 fps/scale 做测试隔离用的，如果这里也吃它，
  测试就会在"临时 ini 里没有 CharSelect"的情况下把整个情绪功能静默关掉 —— 看起来是跳过，
  实际什么都没验证。情绪数据本来就在用户数据区，读真实配置是这条链路的前提。*/
QString Live2DMoodPreset::resolveModelDir(const QString &modelName)
{
    const QString trimmed = modelName.trimmed();
    if (trimmed.isEmpty())
        return QString();

    const QString charName = ReadNowSelectChar();
    if (charName.isEmpty() || charName == QStringLiteral("未选择"))
        return QString();

    return QDir(CharacterAssestPath)
        .filePath(charName + QStringLiteral("/Live2D/") + trimmed);
}

void Live2DMoodPreset::clearAll()
{
    m_enabled = false;
    m_modelName.clear();
    m_modelDir.clear();
    m_parameters.clear();
    m_archetypeDeltas.clear();
    m_moodAliases.clear();
    m_archetypeNames.clear();
    m_skippedUpdaterParams.clear();
    m_unknownSemanticNames.clear();
    m_clampedDeltas.clear();
    m_warnedUnknownMoods.clear();
}

bool Live2DMoodPreset::fail(const QString &reason)
{
    m_enabled = false;
    // 整条功能只提醒一次：调用方（窗口）拿到 !isEnabled() 后什么都不施加，行为是安全的。
    qWarning() << "[Live2D mood] disabled:" << reason;
    return false;
}

bool Live2DMoodPreset::loadParameterMap(const QString &path, QString *error)
{
    QJsonObject root;
    if (!readJsonObject(path, &root, error))
        return false;

    const QJsonObject params = root.value(QStringLiteral("params")).toObject();
    if (params.isEmpty())
    {
        if (error)
            *error = QStringLiteral("%1 里没有 params").arg(path);
        return false;
    }

    for (auto it = params.constBegin(); it != params.constEnd(); ++it)
    {
        const QString semanticName = it.key();
        const QJsonObject entry = it.value().toObject();
        const QString id = entry.value(QStringLiteral("id")).toString();
        if (semanticName.isEmpty() || id.isEmpty())
        {
            qWarning() << "[Live2D mood] parameter-map 里有缺 id 的条目，忽略:" << semanticName;
            continue;
        }
        // 范围是"参数真实上下限"，不能凭空补：缺 min/max 就当这条数据非法，整条丢掉。
        if (!entry.value(QStringLiteral("min")).isDouble() ||
            !entry.value(QStringLiteral("max")).isDouble())
        {
            qWarning() << "[Live2D mood] parameter-map 条目缺 min/max，忽略:" << semanticName
                       << id;
            continue;
        }

        ParameterRange range;
        range.id = id;
        range.min = static_cast<float>(entry.value(QStringLiteral("min")).toDouble());
        range.max = static_cast<float>(entry.value(QStringLiteral("max")).toDouble());
        range.neutral = static_cast<float>(entry.value(QStringLiteral("neutral")).toDouble());
        m_parameters.insert(semanticName, range);
    }

    return !m_parameters.isEmpty();
}

bool Live2DMoodPreset::loadMoods(const QString &path, QString *error)
{
    QJsonObject root;
    if (!readJsonObject(path, &root, error))
        return false;

    const QJsonObject archetypes = root.value(QStringLiteral("archetypes")).toObject();
    if (archetypes.isEmpty())
    {
        if (error)
            *error = QStringLiteral("%1 里没有 archetypes").arg(path);
        return false;
    }

    for (auto it = archetypes.constBegin(); it != archetypes.constEnd(); ++it)
    {
        const QString archetype = it.key();
        const QJsonObject deltas = it.value().toObject();
        if (archetype.isEmpty())
            continue;

        QHash<QString, float> values;
        for (auto delta = deltas.constBegin(); delta != deltas.constEnd(); ++delta)
        {
            if (!delta.value().isDouble())
            {
                qWarning() << "[Live2D mood] 原型里有非数值项，忽略:" << archetype << delta.key();
                continue;
            }
            values.insert(delta.key(), static_cast<float>(delta.value().toDouble()));
        }
        m_archetypeDeltas.insert(archetype, values);
        m_archetypeNames.append(archetype);
    }
    // QJsonObject 的遍历顺序是**键的字典序**（不是文件里的书写顺序），排序后行为稳定
    m_archetypeNames.sort();

    const QJsonObject aliases = root.value(QStringLiteral("moodAliases")).toObject();
    for (auto it = aliases.constBegin(); it != aliases.constEnd(); ++it)
    {
        const QString moodName = it.key();
        const QString archetype = it.value().toString();
        if (moodName.isEmpty() || archetype.isEmpty())
            continue;
        if (!m_archetypeDeltas.contains(archetype))
        {
            // 指向不存在的原型：丢掉这条别名，让它走"未知心情 → neutral"的代码兜底路径，
            // 而不是留一个查得到、却解析不出参数的半残条目。
            qWarning() << "[Live2D mood] 心情别名指向不存在的原型，忽略:" << moodName << archetype;
            continue;
        }
        m_moodAliases.insert(moodName, archetype);
    }

    if (m_moodAliases.isEmpty())
    {
        if (error)
            *error = QStringLiteral("%1 里没有可用的 moodAliases").arg(path);
        return false;
    }
    return true;
}

bool Live2DMoodPreset::load(const QString &modelName)
{
    clearAll();

    m_modelName = modelName.trimmed();
    if (m_modelName.isEmpty())
        return fail(QStringLiteral("模型名为空"));

    m_modelDir = resolveModelDir(m_modelName);
    if (m_modelDir.isEmpty())
        return fail(QStringLiteral("按 CharSelect + 模型名推不出模型目录: %1").arg(m_modelName));

    QString error;
    if (!loadParameterMap(QDir(m_modelDir).filePath(QStringLiteral("parameter-map.json")), &error))
        return fail(error);
    if (!loadMoods(QDir(m_modelDir).filePath(QStringLiteral("presets/moods.json")), &error))
        return fail(error);

    /*数据体检：把"防御性跳过"与"夹取悄悄改了初稿值"这两类情况在装载时汇总各报一次。
      放在这里而不是每次 apply 时打日志：这两件事都是**数据问题**，与 AI 说了什么无关，
      每次换心情重复刷屏只会让人学会忽略警告。*/
    auditArchetypeDeltas();

    m_enabled = true;
    qInfo() << "[Live2D mood] ready: params" << m_parameters.size() << "archetypes"
            << m_archetypeDeltas.size() << "moodAliases" << m_moodAliases.size() << "dir"
            << m_modelDir;
    return true;
}

/*装载后对**原始数据**做一遍体检（不改数据，只报告）：

  1. 原型写了驱动器/物理占用的参数（ParamBreath / ParamHair*）→ 会被跳过，必须让作者知道，
     否则他会以为那个数字生效了；
  2. 原型写了 parameter-map 里不存在的语义名（错字/换了模型）→ 同样是被丢掉；
  3. 原型写了超出 [min,max] 的值 → 会被夹取，屏幕上看到的不是他写的那个数。

  当前数据这三项都是零（测试 moodPresetValuesWithinParameterRanges 也在钉这件事），
  所以正常情况下这里一条警告都不该出现 —— 这正是"防御性"的意思。*/
void Live2DMoodPreset::auditArchetypeDeltas()
{
    m_skippedUpdaterParams.clear();
    m_unknownSemanticNames.clear();
    m_clampedDeltas.clear();

    for (auto archetype = m_archetypeDeltas.constBegin(); archetype != m_archetypeDeltas.constEnd();
         ++archetype)
    {
        const QHash<QString, float> values = archetype.value();
        for (auto value = values.constBegin(); value != values.constEnd(); ++value)
        {
            const ParameterRange range = m_parameters.value(value.key());
            if (range.id.isEmpty())
            {
                m_unknownSemanticNames.insert(value.key());
                continue;
            }
            if (isUpdaterOwnedParameter(range.id) || isUpdaterOwnedSemanticName(value.key()))
            {
                m_skippedUpdaterParams.insert(range.id);
                continue;
            }
            if (value.value() < range.min || value.value() > range.max)
            {
                m_clampedDeltas.append(QStringLiteral("%1.%2=%3→[%4,%5]")
                                           .arg(archetype.key())
                                           .arg(value.key())
                                           .arg(double(value.value()))
                                           .arg(double(range.min))
                                           .arg(double(range.max)));
            }
        }
    }

    const auto sortedList = [](const QSet<QString> &set) {
        QStringList list(set.constBegin(), set.constEnd());
        list.sort();
        return list;
    };

    /*按设计被排除的驱动器/物理参数（parameter-map 里有、但我们永远不写）：
       用 qInfo 报一句而不是 qWarning —— 这是预期行为，不是数据问题；
       真正的 qWarning 留给"数据里居然写了这几项"（下面那条，当前数据为零）。*/
    QStringList ownedExcluded;
    for (auto it = m_parameters.constBegin(); it != m_parameters.constEnd(); ++it)
    {
        if (isUpdaterOwnedParameter(it.value().id))
            ownedExcluded.append(it.value().id);
    }
    if (!ownedExcluded.isEmpty())
    {
        ownedExcluded.sort();
        qInfo() << "[Live2D mood] updater/physics-owned parameters excluded from overrides:"
                << ownedExcluded;
    }

    if (!m_skippedUpdaterParams.isEmpty())
        qWarning() << "[Live2D mood] never override updater/physics-owned parameters, skipped:"
                   << sortedList(m_skippedUpdaterParams);
    if (!m_unknownSemanticNames.isEmpty())
        qWarning() << "[Live2D mood] parameter-map has no such semantic name, entries ignored:"
                   << sortedList(m_unknownSemanticNames);
    if (!m_clampedDeltas.isEmpty())
    {
        m_clampedDeltas.sort();
        qWarning() << "[Live2D mood] archetype values out of range, clamped:" << m_clampedDeltas;
    }
}

QString Live2DMoodPreset::archetypeForMood(const QString &moodName) const
{
    const QString trimmed = moodName.trimmed();
    const QString archetype = m_moodAliases.value(trimmed);
    if (!archetype.isEmpty())
        return archetype;
    // 词表外的词（用户改了 Tachie 文件名 / AI 编了个新词）一律回退 neutral
    return QStringLiteral("neutral");
}

QString Live2DMoodPreset::representativeMoodForArchetype(const QString &archetype) const
{
    if (archetype.isEmpty())
        return QString();
    /*取字典序最小的别名：QHash 的遍历顺序是随机的，直接取第一个会让"校准图用的是哪个
       心情名"每次都不同 —— 那种不确定性对"对照图"是致命的。*/
    QStringList candidates;
    for (auto it = m_moodAliases.constBegin(); it != m_moodAliases.constEnd(); ++it)
    {
        if (it.value() == archetype)
            candidates.append(it.key());
    }
    if (candidates.isEmpty())
        return QString();
    candidates.sort();
    return candidates.first();
}

bool Live2DMoodPreset::insertOwnedParameter(QHash<QString, float> *out,
                                            const QString &semanticName, float value) const
{
    if (out == nullptr)
        return false;

    const ParameterRange range = m_parameters.value(semanticName);
    if (range.id.isEmpty())
        return false; //数据表里没有这个语义名（auditArchetypeDeltas 已在装载时报过）
    if (isUpdaterOwnedParameter(range.id) || isUpdaterOwnedSemanticName(semanticName))
        return false; //驱动器/物理占用：永远不进覆盖表（同上，装载时已报过）

    // 夹取到参数自身的取值范围：数据是"初稿"，越界值只会让模型进入未定义姿势
    if (value < range.min)
        value = range.min;
    if (value > range.max)
        value = range.max;
    out->insert(range.id, value);
    return true;
}

QHash<QString, float> Live2DMoodPreset::parametersForArchetype(const QString &archetype) const
{
    QHash<QString, float> result;

    /*① 中立基线：每个"我们拥有的"参数都给一个显式中立值。
       为什么中立值也要写进去：覆盖是整组替换的 —— 只有把整组都给全，
       切回 neutral 时上一个情绪的条目才会被真正换掉（见 setParameterOverrides）。*/
    for (auto it = m_parameters.constBegin(); it != m_parameters.constEnd(); ++it)
        (void)insertOwnedParameter(&result, it.key(), it.value().neutral);

    /*② 叠上原型的差异（只写了与中立值不同的那些参数）。*/
    const QHash<QString, float> deltas = m_archetypeDeltas.value(archetype);
    for (auto it = deltas.constBegin(); it != deltas.constEnd(); ++it)
        (void)insertOwnedParameter(&result, it.key(), it.value());

    return result;
}

QHash<QString, float> Live2DMoodPreset::parametersForMood(const QString &moodName)
{
    const QString trimmed = moodName.trimmed();
    const QString archetype = archetypeForMood(trimmed);
    if (!m_moodAliases.contains(trimmed) && !m_warnedUnknownMoods.contains(trimmed))
    {
        /*未知心情 → neutral：**绝不能**"保持上一次的表情"（那就是"她只有一副表情"的成因），
           也绝不能什么都不做（否则屏幕上留着上一种情绪，且没人知道为什么）。
           每个未知名只提醒一次，避免 AI 频繁造词时刷屏。*/
        m_warnedUnknownMoods.insert(trimmed);
        qWarning() << "[Live2D mood] unknown mood name, fall back to neutral:" << trimmed;
    }
    return parametersForArchetype(archetype);
}
