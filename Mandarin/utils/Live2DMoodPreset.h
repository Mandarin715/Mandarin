#ifndef LIVE2DMOODPRESET_H
#define LIVE2DMOODPRESET_H

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

/*心情预设装载器：把「AI 说的心情名」翻译成「一组 Live2D 参数覆盖值」。

  数据有两份，都在**用户数据区**（`Documents/`，不在仓库里），本模块只读：

    1) `<模型目录>/parameter-map.json`
       语义参数名 → 真实参数 ID + 取值范围 + 中立值（27 个）。范围取自模型自带的
       vtube.json / cdi3.json，**不许在这里重新推导**。
    2) `<模型目录>/presets/moods.json`
       `archetypes`：14 个情绪原型，每个只写"与中立值的差异"；
       `moodAliases`：26 个心情名（= 角色 `Tachie/` 下 PNG 的文件名）→ 原型。

  三条设计决定（都对应一个具体的错误行为）：

  1. **心情名不认识就回退 neutral**，而不是"保持上一次的表情"。
     数据表覆盖不到的词（用户自己改了 PNG、AI 编了个新词）不能把上一个情绪留在屏幕上 ——
     那会变成"她一直在生气，只是没人知道为什么"。回退是**代码**负责的，不靠数据表兜底。
  2. **两份 JSON 任何一份缺失/非法 → 整个功能自关**（log 一次、什么都不施加、绝不崩）。
     这两份文件在 `Documents/` 下，用户可能删掉、手改坏、或换了个还没写预设的模型；
     缺文件时"表情不动"是正确降级，不是错误。
  3. **驱动器/物理占用的参数一律不覆盖**（ParamBreath / ParamHairFront|Side|Back）。
     它们每帧由呼吸驱动器/物理写入，覆盖它们只会把那套动作做废。这里做的是
     **防御性跳过**：即使数据里写了也丢掉并打一条警告（当前数据里没有这几项）。

  为什么"中立值也要逐条写进覆盖表"（而不是只写差异）：覆盖是**整组替换**的，
  只有把当前心情涉及的每个参数都显式给值，切回 neutral 时上一个情绪的条目才会被换掉。
  见 Live2DOffscreenRenderer::setParameterOverrides。*/
class Live2DMoodPreset
{
  public:
    /*parameter-map.json 里一条参数的取值范围*/
    struct ParameterRange
    {
        QString id;
        float min = 0.0f;
        float max = 0.0f;
        float neutral = 0.0f;
    };

    /*按模型名装载两份 JSON。返回是否可用；不可用时 isEnabled()==false，
      调用方应当"什么都不施加"（并保证不残留上一种情绪）。*/
    bool load(const QString &modelName);

    bool isEnabled() const { return m_enabled; }
    QString modelName() const { return m_modelName; }
    QString modelDir() const { return m_modelDir; }

    /*模型目录 = CharacterAssestPath + config.ini 的 character/CharSelect + 模型名。
      找不到角色/目录时返回空。**只读**用户配置，绝不写。*/
    static QString resolveModelDir(const QString &modelName);

    /*该参数 ID 是否被驱动器/物理占用（呼吸、头发）——这类参数永远不进覆盖表*/
    static bool isUpdaterOwnedParameter(const QString &parameterId);

    /*JSON 里的原型名（14 个，字典序；校准图按这个顺序出图）*/
    QStringList archetypeNames() const { return m_archetypeNames; }
    /*语义名 → 范围（27 条；breath/hair 也在里面，它们只是不会被覆盖）*/
    QHash<QString, ParameterRange> parameters() const { return m_parameters; }
    /*原型 → (语义名 → 值)，**原样**（未夹取）：测试要拿它验证"初稿没有越界"*/
    QHash<QString, QHash<QString, float>> archetypeDeltas() const { return m_archetypeDeltas; }
    /*心情名 → 原型*/
    QHash<QString, QString> moodAliases() const { return m_moodAliases; }

    /*心情名 → 原型；未知返回 neutral（纯查询，不记日志）*/
    QString archetypeForMood(const QString &moodName) const;

    /*原型 → 一个代表它的心情名（校准图与测试用；取字典序最小的别名，结果稳定）。
      该原型没有任何别名时返回空。*/
    QString representativeMoodForArchetype(const QString &archetype) const;

    /*原型 → (参数 ID → 已夹取到 [min,max] 的值)：含全部中立值 + 该原型的差异。
      原型名未知时按 neutral 处理。*/
    QHash<QString, float> parametersForArchetype(const QString &archetype) const;

    /*心情名 → (参数 ID → 值)。**未知心情回退 neutral 并只记一次日志。**/
    QHash<QString, float> parametersForMood(const QString &moodName);

  private:
    void clearAll();
    bool loadParameterMap(const QString &path, QString *error);
    bool loadMoods(const QString &path, QString *error);
    /*装载失败：只打一条警告并返回 false（调用方据此把功能整个关掉）*/
    bool fail(const QString &reason);
    /*装载后对原始数据做体检：被跳过的驱动器参数 / 表里没有的语义名 / 越界会被夹取的值，
      各汇总成一条警告（当前数据三项皆为零，所以这是纯粹的防御性代码）*/
    void auditArchetypeDeltas();

    /*把一条"我们拥有的"参数写进覆盖表：解析语义名 → 参数 ID、夹取范围、
      跳过驱动器占用的参数。返回是否真的写进去了。*/
    bool insertOwnedParameter(QHash<QString, float> *out, const QString &semanticName,
                              float value) const;

    bool m_enabled = false;
    QString m_modelName;
    QString m_modelDir;

    QHash<QString, ParameterRange> m_parameters;                 // 语义名 -> 范围
    QHash<QString, QHash<QString, float>> m_archetypeDeltas;     // 原型 -> (语义名 -> 值)
    QHash<QString, QString> m_moodAliases;                       // 心情名 -> 原型
    QStringList m_archetypeNames;

    /*数据体检的结论与"未知心情名只提醒一次"的记号*/
    QSet<QString> m_skippedUpdaterParams;
    QSet<QString> m_unknownSemanticNames;
    QSet<QString> m_warnedUnknownMoods;
    QStringList m_clampedDeltas;
};

#endif // LIVE2DMOODPRESET_H
