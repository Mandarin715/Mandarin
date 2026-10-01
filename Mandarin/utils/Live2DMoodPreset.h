#ifndef LIVE2DMOODPRESET_H
#define LIVE2DMOODPRESET_H

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

/*心情预设装载器：把「AI 说的心情名」翻译成「一组 Live2D 参数覆盖值」。

  数据有三份，都在**用户数据区**（`Documents/`，不在仓库里），本模块只读：

    1) `<模型目录>/parameter-map.json`
       语义参数名 → 真实参数 ID + 取值范围 + 中立值（27 个）。范围取自模型自带的
       moc 声明，**不许从 vtube.json / cdi3.json 抄范围**。
    2) `<模型目录>/presets/moods.json`
       `archetypes`：14 个情绪原型，每个只写"与中立值不同的绝对目标值"；
       `moodAliases`：26 个心情名（= 角色 `Tachie/` 下 PNG 的文件名）→ 原型。
    3) `<模型目录>/presets/idle.json`
       `sway`：待机摆动的若干条轴（**语义**参数名 + 幅度 + 周期 + 相位）。让角色"站着、
       重心在动"而不是只有呼吸。它与上面两份**分开装载**：缺了它只是不做摆动，
       情绪功能照常（反之亦然）—— 见 loadIdle 的说明。

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

    /*待机摆动的一条轴（`presets/idle.json` 的一项）。

      为什么幅度用**参数自己的单位**而不是比例：数据作者看的是模型文档里的
      「ParamBodyAngleZ ±10」，写 3.5 比写 17.5% 更直观，也与 parameter-map.json 同一种量纲
      —— 换模型时只要 parameter-map 有同名语义项，这张表就能整体复用。

      为什么相位是**周期分数**（0~1）而不是弧度/角度：写表的人要的是"错开 1/4 周期"，
      弧度制会把这件事变成一道除法题；而且分数与周期解耦，改 period 不会顺带改变错开程度。*/
    struct IdleSwayEntry
    {
        QString semanticName;              //数据里的语义名（日志/测试用）
        QString parameterId;               //解析出的真实参数 ID（渲染器按它写模型）
        float amplitude = 0.0f;            //参数单位；建议 ≤ 声明量程的 20%
        float periodSeconds = 1.0f;        //周期（秒）
        float phase = 0.0f;                //相位（周期分数 0~1）
    };

    /*按模型名装载两份 JSON。返回是否可用；不可用时 isEnabled()==false，
      调用方应当"什么都不施加"（并保证不残留上一种情绪）。
      directoryOverride 非空时从指定预设目录读取，供离线校准/测试使用；默认路径不变。*/
    bool load(const QString &modelName, const QString &directoryOverride = QString());

    /*装载**待机摆动**数据（`<模型目录>/presets/idle.json`）。

      与上面的 load() **分开**是有意的：这两份数据的作用与失效后果不同 ——
      moods.json 缺失时情绪功能整个自关；idle.json 缺失时只是"不做摆动"，角色照样呼吸、
      眨眼、跟着 AI 的心情变表情。并进 load() 的失败路径会把"用户还没给摆动数据"
      变成"情绪也不能用"，那是把两个正交的功能绑死（有测试钉这一条）。

      任何一条不可用（文件缺失/非法/条目全被跳过）→ 摆动自关：log 一次、返回 false，
      **绝不崩**，也绝不把 m_enabled（情绪功能）改掉。数据在用户数据区，随时可能缺。

      pathOverride 非空时读那个文件（用来验证"文件不存在 = 无摆动"，避免用例依赖
      用户数据目录的现状）；为空时按模型目录推 presets/idle.json。*/
    bool loadIdle(const QString &pathOverride = QString());

    bool isEnabled() const { return m_enabled; }
    QString modelName() const { return m_modelName; }
    QString modelDir() const { return m_modelDir; }

    /*摆动数据是否可用（装载成功且至少有一条可用条目）*/
    bool isIdleSwayEnabled() const { return m_idleSwayEnabled; }
    /*可用条目（语义名已解析成参数 ID、驱动器占用的已剔除）。装载失败时为空。*/
    QVector<IdleSwayEntry> idleSwayEntries() const { return m_idleSwayEntries; }

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
    /*摆动状态单独清：load() 会重新推导模型目录，旧的摆动条目绝不能留在表里*/
    void clearIdle();
    bool loadParameterMap(const QString &path, QString *error);
    bool loadMoods(const QString &path, QString *error);
    /*装载失败：只打一条警告并返回 false（调用方据此把功能整个关掉）*/
    bool fail(const QString &reason);
    /*摆动数据不可用：只关摆动并打一条警告 —— **不碰 m_enabled**（见 loadIdle 的说明）*/
    bool failIdle(const QString &reason);
    /*装载后对原始数据做体检：被跳过的驱动器参数 / 表里没有的语义名 / 越界会被夹取的值，
      各汇总成一条警告（当前数据三项皆为零，所以这是纯粹的防御性代码）*/
    void auditArchetypeDeltas();
    /*摆动的同类体检：比"声明量程的 x%"还大的幅度、写在 [0,1) 之外的相位各汇总一条告警。
      这些不改数据（照常生效），只是让作者知道屏幕上看到的不是他写的那个数。*/
    void auditIdleSway();

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

    /*待机摆动（presets/idle.json）。m_idleSwayEnabled 与 m_enabled 是**两个**开关：
       前者只管摆动，后者管情绪；任何一份数据缺失都只关自己那一个（见 loadIdle）。*/
    bool m_idleSwayEnabled = false;
    QVector<IdleSwayEntry> m_idleSwayEntries;
    /*摆动体检的结论（都只在装载时各报一次，不阻止生效）*/
    QStringList m_idleOversizedAmplitudes;
    QStringList m_idleOddPhases;

    /*数据体检的结论与"未知心情名只提醒一次"的记号*/
    QSet<QString> m_skippedUpdaterParams;
    QSet<QString> m_unknownSemanticNames;
    QSet<QString> m_warnedUnknownMoods;
    QStringList m_clampedDeltas;
};

#endif // LIVE2DMOODPRESET_H
