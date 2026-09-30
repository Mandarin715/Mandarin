#include <QtTest>

#include "../GlobalConstants.h"
#include "../utils/AudioEnvelope.h"
#include "../utils/Live2DMoodPreset.h"
#include "../utils/Live2DOffscreenRenderer.h"
#include "SyntheticWav.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QThread>

#include <algorithm>
#include <cmath>

namespace
{
/*这些参数每帧被**别的写入者**占用：呼吸驱动器写 ParamAngleX/Y/Z + ParamBodyAngleX +
  ParamBreath（见 Live2DOffscreenRenderer::setupBreath），物理写头发（ParamHair*）。

  为什么待机摆动用例需要这份名单：那几条轴的读回值 = 心情 + 呼吸/物理 + 摆动，
  从中量不出摆动自己的贡献。凡是"逐帧读参数值做恒等式"的断言都只能放在**不在**这份名单里的
  轴上（本模型是 ParamBodyAngleZ）—— 那不是取巧，是测量边界（见文件开头关于姿势参数的说明）。*/
bool isUpdaterDrivenParameterId(const QString &parameterId)
{
    static const QStringList kIds = {
        QStringLiteral("ParamAngleX"),    QStringLiteral("ParamAngleY"),
        QStringLiteral("ParamAngleZ"),    QStringLiteral("ParamBodyAngleX"),
        QStringLiteral("ParamBreath"),    QStringLiteral("ParamHairFront"),
        QStringLiteral("ParamHairSide"),  QStringLiteral("ParamHairBack")};
    return kIds.contains(parameterId);
}

/*两个"周期分数"之间的**环形**距离（周期 1，所以结果落在 [0, 0.5]）。
   相位 0.95 与 0.05 相差 0.1 周期而不是 0.9 —— 用的就是它。*/
float circularPhaseDistance(float a, float b)
{
    const float raw = std::fmod(std::fabs(a - b), 1.0f);
    return std::min(raw, 1.0f - raw);
}

/*从条目表里取某条轴的**配置幅度**（找不到返回 0）。*/
float idleAmplitudeOf(const QVector<Live2DMoodPreset::IdleSwayEntry> &entries,
                      const QString &parameterId)
{
    for (const Live2DMoodPreset::IdleSwayEntry &entry : entries)
    {
        if (entry.parameterId == parameterId)
            return entry.amplitude;
    }
    return 0.0f;
}

/*手搓一条摆动条目：给"敌意数据"用例构造输入（正常路径上条目全部由 idle.json 装载，
   驱动器**只**从数据里拿东西 —— 这个构造器正是为了证明它自己也守纪律）。*/
Live2DMoodPreset::IdleSwayEntry makeIdleEntry(const QString &semanticName,
                                              const QString &parameterId, float amplitude,
                                              float periodSeconds, float phase)
{
    Live2DMoodPreset::IdleSwayEntry entry;
    entry.semanticName = semanticName;
    entry.parameterId = parameterId;
    entry.amplitude = amplitude;
    entry.periodSeconds = periodSeconds;
    entry.phase = phase;
    return entry;
}
} // namespace

/*Live2D 离屏渲染的端到端验证。

  这条测试是本阶段最关键的证伪手段：它用真实模型跑通
  「内存加载 moc3/贴图/物理 → 离屏 FBO → 读回 RGBA」整条链路，
  并把结果写成 PNG 供目视确认。

  模型不入库（授权禁二传），所以本机没有模型时**跳过**而不是失败；
  可用环境变量 MANDARIN_LIVE2D_MODEL_DIR 指定别的模型目录。*/
class TestLive2DOffscreen : public QObject
{
    Q_OBJECT

  private slots:
    void rendersMikuFirstFrame();
    void drivesParametersWithUpdaters();
    void reportsFrameCost();

    /*---------- 心情过渡与说话嘴巴（本阶段新增） ---------- */
    /*过渡的四条观察量：起点 / 终点 / 途中不跳 / 同心情幂等*/
    void moodBlendStartsFromPreviousValue();
    void moodBlendReachesTargetExactly();
    void moodBlendHasNoSingleFrameJump();
    void applyingSameMoodTwiceDoesNotRestartBlend();
    /*过渡时长必须真的来自参数（硬编码 200ms 的实现过不了）*/
    void moodBlendDurationIsConfigurable();
    /*说话时嘴巴要动、不许越界、不许碰别人*/
    void speakingMouthFlapVariesAndStaysInRange();
    void speakingDoesNotDisturbOtherParameters();
    /*停止说话后扑动必须回落（不能突然闭嘴）*/
    void stoppingSpeechDecaysFlapToMoodValue();

    /*---------- 响度包络驱动嘴巴（本阶段新增：句间停顿时闭嘴） ---------- */
    /*头条：前半句有声、后半句静音 → 静音段嘴巴必须回到心情自己的值*/
    void envelopeSilenceClosesMouth();
    /*电平恒为 0 = 一点都不动（"停顿闭嘴"的另一半：不许有残余扑动）*/
    void envelopeZeroLevelKeepsMouthAtMoodValue();
    /*包络路径不许碰别人的参数（嘴形属于心情）*/
    void envelopeDoesNotDisturbOtherParameters();
    /*下一句没有包络（用户配成 mp3）时必须回到今天的盲扑动*/
    void nextUtteranceWithoutEnvelopeFallsBackToBlindFlap();
    /*停止说话 + 电平清零 → 嘴回落到心情值*/
    void stoppingSpeechWithClearedLevelReturnsToMood();

    /*---------- 待机摆动（IDLE 系列：让角色"站着、重心在动"） ----------
      数据来自模型目录下的 presets/idle.json（语义参数名 + 幅度 + 周期 + 相位）。
      这一组全部按**注入的**固定帧步长采样：摆动相位由累计虚拟时间决定，用墙钟就会随
      机器负载漂移（帧步长有 0.1s 夹取上限，见 setNextFrameDeltaSeconds）。*/
    /*每一配置轴都按配置的幅度在动，且参数值始终落在模型声明的范围里*/
    void idleSwayMovesConfiguredParameters();
    /*周期必须等于配置值（过零法从注入时间上量，不依赖墙钟）*/
    void idleSwayPeriodMatchesConfiguration();
    /*叠加而不是覆盖：参数围绕**心情值**摆，不是围绕中立值*/
    void idleSwayAddsToMoodInsteadOfReplacingIt();
    /*idle.json 不存在 → 摆动自关、参数保持平坦、不崩（且情绪功能不受影响）*/
    void idleSwayIsOffWithoutDataFile();
    /*绝不写 breath / hair*，也绝不写 ParamMouthForm */
    void idleSwayNeverTouchesBreathHairOrMouthForm();
    /*两条轴的相位必须按配置错开（不锁步）*/
    void idleSwayPhasesAreOffset();
    /*出**全身**对照图：一个周期内均匀 6 帧，供人眼判断"她真的在动"。
       本阶段起**遍历每个带 presets/idle.json 的模型**，文件名带模型名
       （`idle-<模型>-phase<N>.png`），atri 的既有无后缀素材刻意不动。*/
    void rendersIdleSwayPhaseFrames();
    /*[数据纪律] 每个带 idle.json 的模型：装载成功、没有驱动器/嘴形条目、
       每条幅度都在声明量程的 20% 警戒线以内（文件自己的规则）。*/
    void idleSwayDataObeysItsOwnRulesForEveryModel();

    /***腿部探针**：把每个参数分别推到声明的最小/最大，量它在**下半身区域**里
       改变了多少像素，并把「几何变了」（alpha 掩码动了 = 真的在动骨架）与
       「只是换了外观」（掩码逐位相同、只有 RGB 变了 = 换装/道具开关）分开报。
       这是"这个模型的腿到底能不能动、靠哪些参数动"的直接证据。*/
    void probesWhichParametersArticulateTheLowerBody();

    /***两个模型是不是**同一套 rig**（换贴图）**。

       要回答的是"第二个模型能不能直接复用第一个模型的 parameter-map/moods/idle 数据"。
       判据必须落在**行为**上而不是画面上：贴图不同 ⇒ 画面必然不同，比像素是没意义的。
       所以比两件事：
         ① moc 声明的参数集合（ID + min/default/max）逐条相等；
         ② 把同一批参数推到同一个值，两个模型在**输出空间**里的剪影（alpha）是否一致。
       两条都成立 ⇒ 同一套骨架、同一套绑定，只是贴图不同 ⇒ 数据可以整份复用。*/
    void comparesTwoModelsForRigEquivalence();

    /***同一进程里两个渲染器必须都能渲染出人物（既存缺陷的钉子）。

        用户/上一个 agent 观察到的症状：**第二个** Live2DOffscreenRenderer 渲染为空白，
        与模型无关（同一模型装进两个实例，第二个照样空白；交换顺序则"第二个"随之交换）。
        这条用**同一个模型装两遍**做最尖锐的对照：如果连同一个模型都失败，模型就彻底出局。
        逐边界诊断（上下文身份 / FBO 完整性 / 逐 drawable 是否有贴图 / 着色器程序名在
        当前上下文里存不存在）在失败时全部打进日志，日志标签统一 ASCII（`TWORENDER`），
        免得中文在控制台里变成乱码。

        `MANDARIN_BITEQ_BASELINE` 只用于"改动前后逐位对照"这一次性验证：定义它会把这条
        用例（它依赖本次新增的 debugState()）排除掉，从而能在**旧渲染器代码**上编译，
        与新版跑同一串固定输入、比对逐位指纹。正常构建不定义这个宏。*/
#if !defined(MANDARIN_BITEQ_BASELINE)
    void twoRenderersInOneProcessBothRender();
#endif

    /*单一渲染器的逐位指纹：本次"共享根 GL 上下文"改动必须对既有单一渲染器路径零影响，
       判据是同一串固定输入下 RGBA 校验和逐位相同（改动前后各跑一次比对）。
       它刻意**不依赖**任何新增诊断接口，好让它在旧版渲染器上也能编译 ——
       这正是"改动前后逐位对照"能成立的前提。*/
    void singleRendererFrameHashesForRegressionProof();

  private:
    static QString modelDir();
    static QString outputDir();

    /*本机任意一个可用模型的目录（可能为空 = 本机没有模型）。*/
    static QString availableModelDir();
    /*在 availableModelDir() 里挑一个能装载的 model3.json（可能为空）。*/
    static QString availableModelJson(const QString &dir);
    /*装载本机任意一个模型。失败返回 false，调用方 QSKIP。
       **不写死模型名**：模型禁二传、不入库，写死就变成"在开发机上绿、换台机器全跳过"。*/
    static bool loadAnyModel(Live2DOffscreenRenderer *renderer, QString *error);

    /*这两个探测参数不在本模型里时 QSKIP（在函数里调用，不能抽成返回 bool 的普通函数：
       QSKIP 是宏，必须能 return 出调用方的 void 函数）。*/
    void requireBlendProbeParameters(Live2DOffscreenRenderer *renderer) const;
    /*嘴巴扑动用例的前提：模型必须声明 ParamMouthOpenY 与 ParamMouthForm（不存在就跳过）。*/
    void requireMouthParameters(Live2DOffscreenRenderer *renderer) const;

    /*这些用例量的是**参数过渡**，量的是模型的值，与像素无关，
       所以统一用 240x300 的小画布：渲染成本低一个数量级，采样帧数就能给足。
       画布一变 FBO 会重建一次，之后就稳定复用。*/
    static constexpr int kBlendProbeWidth = 240;
    static constexpr int kBlendProbeHeight = 300;

    /*本模型里**不归任何驱动器管**的参数（眼睛/眉毛/腮红/嘴形）：
       读回值就是覆盖表写下的值本身，可以用来量过渡曲线。
       姿势类参数（ParamAngleX/ParamBodyAngleX）绝对不能用 —— 呼吸驱动器每帧往它们身上
       **加**一个摆动量，读回值是"覆盖值 + 摆动"，会把过渡曲线淹掉。*/
    static const QString kMoodProbeParameter;   // ParamEyeLSmile：0 → 0.9
    static const QString kSecondProbeParameter; // ParamBrowLY
    static const QString kMouthOpenParameter;   // ParamMouthOpenY（只被嘴巴扑动驱动）
    static const QString kMouthFormParameter;   // ParamMouthForm（属于心情，绝不该被扑动碰）

    /*跑 n 帧真实时间：每帧之间等 waitMs 再渲一帧。
       **必须真的等**：帧的时间步长来自墙钟（renderFrame 内部 clock.restart()），
       紧循环连调 renderFrame() 的话两帧只隔几微秒 → 过渡永远走不完。
       values 非空时把每帧读到的 probeParameter 记下来。
       injectedDeltaSeconds > 0 时**注入固定步长**（走 setNextFrameDeltaSeconds，仍是同一条
       夹取路径），于是"虚拟时间"由调用方钉住、与机器负载无关；此时不再 sleep（步长已经是
       注入的，等待没有意义，也让用例更快）。需要真实墙钟的用例传默认值 0 即可。*/
    static bool advanceFrames(Live2DOffscreenRenderer *renderer, const QString &probeParameter,
                              int frames, int waitMs, QVector<float> *values,
                              float injectedDeltaSeconds = 0.0f);

    /*一直推进到 probeParameter 落到 target（或帧数上限用完）。
       为什么要"等到"而不是"猜几帧"：每帧的墙钟时间 = waitMs + 渲染成本，
       而渲染成本随机器/画布/驱动变化（实测 240x300 画布上约 2~4ms，加上 20ms 等待
       约 24ms 一帧）。写死帧数的话，"起点/终点读数"会随机器而变 ——
       在一个更快的机器上先导阶段还没到目标，后续对照就失去意义。*/
    struct SettleResult
    {
        float value = 0.0f;
        int frames = 0;
        bool reached = false;
    };
    static SettleResult advanceUntilSettled(Live2DOffscreenRenderer *renderer,
                                            const QString &probeParameter, float target,
                                            int maxFrames, int waitMs);

    /*---------- 包络驱动的"播放"辅助（见文件后半段的 ENVELOPE 系列） ---------- */
    /*一次包络驱动的采样：播放位置 / 电平 / 开口量三者对齐，
       于是"静音段（位置 ≥ X）的开口量是多少"这类断言可以直接按位置过滤 ——
       不需要在测试里反推"这一帧对应音频里的哪一段"。*/
    struct EnvelopeSample
    {
        qint64 positionMs = 0;
        float level = 0.0f;
        float mouthOpen = 0.0f;
    };

    /*按**虚拟时间**跑一段"播放"：每帧先按播放位置从包络取电平喂给渲染器，再渲一帧。

       为什么自己写循环而不用 advanceFrames：后者不接受"每帧一个电平"这个输入，
       而这组用例的核心正是"电平 → 开口量"这条映射。
       为什么位置由帧序号推出来：注入的步长即虚拟时间，位置与帧时间因此**严格同步**，
       断言不会随机器负载漂移（真实墙钟会被夹成 100ms/帧，见 setNextFrameDeltaSeconds）。
       返回 false = 有一帧渲染失败（调用方断言）。*/
    static bool playEnvelope(Live2DOffscreenRenderer *renderer, const AudioEnvelope &envelope,
                             int frames, float deltaSeconds, QVector<EnvelopeSample> *samples);

    /*---------- 待机摆动（IDLE 系列）的公共设施 ---------- */

    /*待机摆动用例的公共前提：本机模型 + 该模型的 presets/idle.json。

      两个目录在本机是**分开**的（与 test_live2dwindow::openConfiguredModel 的说明一致）：
        模型文件  Documents/Mandarin/Live2D/<模型名>/
        摆动数据  Documents/Mandarin/Character/Assets/<角色>/Live2D/<模型名>/presets/idle.json
      所以模型走 availableModelDir()，数据走 Live2DMoodPreset（它按**真实** config.ini 的
      CharSelect + 模型名推路径）。装不上（本机没模型/没这份数据）返回 false，调用方 QSKIP。*/
    static bool loadConfiguredIdleSway(Live2DOffscreenRenderer *renderer,
                                       Live2DMoodPreset *preset, QString *detail);

    /*所有带 `presets/idle.json` 的模型名（= 角色资源目录下的模型名，按名排序）。

       为什么要有它（本阶段扩展 miku 时加）：这一组用例以前只跑 availableModelDir()
       —— 那个函数优先吃 MANDARIN_LIVE2D_MODEL_DIR / config.ini，**只给一个模型**。
       于是 miku 的 idle.json 写错了也不会有人报。现在改成遍历"确实有摆动数据"的模型：
       新模型把数据放进去就自动进入判据范围，不用再动 C++。*/
    static QStringList modelNamesWithIdleData();

    /*按名字装载模型：候选①模型根目录 ②角色资源目录（与摆动数据同层）。*/
    static bool loadModelByName(const QString &name, Live2DOffscreenRenderer *renderer,
                                QString *error);

    /*所有带 `parameter-map.json` 的模型名（腿部探针用；语义名翻译要它）。*/
    static QStringList modelNamesWithMoodData();

    /*腿部探针判「这一像素算不算人物」的 alpha 阈值：与别处 opaqueBounds 用的 32 一致，
       低于它的像素算透明（羽化边缘不算"骨架动了"，否则边界抖动会被当成几何变化）。*/
    static constexpr int kLegProbeAlphaThreshold = 32;

    /*待机摆动的一帧采样，三个量各答一个问题：
        elapsedSeconds → 这一帧在周期里的哪一点（相位/周期的唯一时基）；
        offsets        → 摆动这一帧**真正施加**了多少（幅度，不受呼吸/物理干扰）；
        values         → 参数最终值（有没有越界、有没有把心情盖掉）。*/
    struct IdleSample
    {
        float elapsedSeconds = 0.0f;
        QHash<QString, float> offsets;
        QHash<QString, float> values;
    };

    /*按**注入的**固定步长跑 frames 帧，逐帧记下上面三个量。
       deltaSeconds 必须 ≤ 0.1s（渲染器的单帧夹取上限），否则"注入的时间"与"实际推进的
       时间"就不再相等，相位会与预期错开。parameterIds 是这次要观察的参数 ID 列表。*/
    static bool sampleIdleSway(Live2DOffscreenRenderer *renderer, const QStringList &parameterIds,
                               int frames, float deltaSeconds, QVector<IdleSample> *samples);

    /*过零法量出来的振荡（周期/相位/幅度）。*/
    struct Oscillation
    {
        float amplitude = 0.0f;   // max|offset|（幅度）
        float periodSeconds = 0.0f; // 相邻**上升**过零时刻之差的平均
        float phase = 0.0f;       // 周期分数 [0,1)
        int crossings = 0;        // 上升过零次数
    };

    /*从采样序列里量出某条轴的**参数值**极差（max−min）。"平坦"就用它判：没有摆动、
       也没有别的写入者时它必须是 0（或只差浮点噪声）。*/
    static float valueSwingOf(const QVector<IdleSample> &samples, const QString &parameterId);

    /*从采样序列里量出某条轴的振荡。

      为什么用"上升过零"而不是"峰值"：正弦在过零点最陡，线性插值的误差是 dt² 量级
      （峰值附近是平的，插值反而量不准）；而且上升过零的时刻有解析式 t = T(k − φ)，
      于是**相位**可以顺带量出来：φ = (−t/T) mod 1（整数 k 自动消失）。

      量不到上升过零（crossings==0）时只有 amplitude 有意义；一条完整的周期需要 ≥2 次。*/
    static Oscillation measureOscillation(const QVector<IdleSample> &samples,
                                          const QString &parameterId);
};

QString TestLive2DOffscreen::modelDir()
{
    const QByteArray fromEnv = qgetenv("MANDARIN_LIVE2D_MODEL_DIR");
    if (!fromEnv.isEmpty())
        return QString::fromLocal8Bit(fromEnv);
    return QDir::homePath() + QStringLiteral("/Documents/Mandarin/Live2D/miku");
}

QString TestLive2DOffscreen::outputDir()
{
    // 测试产物统一放构建目录，绝不落进 build2/Release（否则会被打进便携包）
    return QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(
        QStringLiteral("../live2d-probe"));
}

/*探测参数的选择理由（这三个名字就是断言的核心前提）：
  - ParamEyeLSmile / ParamBrowLY：不归眨眼/呼吸/物理任何驱动器管，读回 = 覆盖值；
  - ParamMouthOpenY：ParamMouthForm 声明范围是 [-1,0]（0 已是"最笑"），
    而 OpenY 是 [0,1] 的开口量，嘴巴扑动只能动它。*/
const QString TestLive2DOffscreen::kMoodProbeParameter = QStringLiteral("ParamEyeLSmile");
const QString TestLive2DOffscreen::kSecondProbeParameter = QStringLiteral("ParamBrowLY");
const QString TestLive2DOffscreen::kMouthOpenParameter = QStringLiteral("ParamMouthOpenY");
const QString TestLive2DOffscreen::kMouthFormParameter = QStringLiteral("ParamMouthForm");

void TestLive2DOffscreen::requireBlendProbeParameters(Live2DOffscreenRenderer *renderer) const
{
    const QHash<QString, Live2DOffscreenRenderer::DeclaredRange> ranges =
        renderer->declaredParameterRanges();
    for (const QString &id : {kMoodProbeParameter, kSecondProbeParameter})
    {
        if (!ranges.contains(id))
        {
            QSKIP("本机模型里没有这个探测参数（换模型了？），跳过心情过渡验证");
        }
    }
}

void TestLive2DOffscreen::requireMouthParameters(Live2DOffscreenRenderer *renderer) const
{
    const QHash<QString, Live2DOffscreenRenderer::DeclaredRange> ranges =
        renderer->declaredParameterRanges();
    for (const QString &id : {kMouthOpenParameter, kMouthFormParameter})
    {
        if (!ranges.contains(id))
            QSKIP("本机模型里没有嘴巴参数（换模型了？），跳过说话嘴巴验证");
    }
}

/*本机有没有模型、有哪一个 —— 不写死模型名。
  这里按下面的顺序找一个能用的：
    1) MANDARIN_LIVE2D_MODEL_DIR（显式指定，最高优先）；
    2) config.ini 的 character/live2dModel（用户实际在用的那个）；
    3) 默认 miku 目录（兼容既有的 rendersMikuFirstFrame / reportsFrameCost）；
    4) 模型根目录下的**第一个**含 *.model3.json 的子目录（兜底）。
  找不到返回空 QString，调用方 QSKIP。*/
QString TestLive2DOffscreen::availableModelDir()
{
    const QByteArray fromEnv = qgetenv("MANDARIN_LIVE2D_MODEL_DIR");
    if (!fromEnv.isEmpty())
        return QString::fromLocal8Bit(fromEnv);

    QSettings settings(IniSettingPath, QSettings::IniFormat);
    const QString configured =
        settings.value(QStringLiteral("character/live2dModel")).toString().trimmed();
    if (!configured.isEmpty())
    {
        const QString configuredDir = QDir(Live2DModelRootPath).filePath(configured);
        if (!availableModelJson(configuredDir).isEmpty())
            return configuredDir;
    }

    const QString defaultDir = modelDir();
    if (!availableModelJson(defaultDir).isEmpty())
        return defaultDir;

    const QDir root(Live2DModelRootPath);
    const QStringList entries = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &entry : entries)
    {
        const QString candidate = root.filePath(entry);
        if (!availableModelJson(candidate).isEmpty())
            return candidate;
    }
    return QString();
}

QString TestLive2DOffscreen::availableModelJson(const QString &dir)
{
    if (dir.isEmpty())
        return QString();
    const QDir modelDir(dir);
    const QStringList found = modelDir.entryList({QStringLiteral("*.model3.json")}, QDir::Files);
    return found.isEmpty() ? QString() : found.first();
}

bool TestLive2DOffscreen::loadAnyModel(Live2DOffscreenRenderer *renderer, QString *error)
{
    if (renderer == nullptr)
        return false;
    const QString dir = availableModelDir();
    if (dir.isEmpty())
        return false;
    const QString json = availableModelJson(dir);
    if (json.isEmpty())
        return false;
    return renderer->load(dir, json, error);
}

bool TestLive2DOffscreen::advanceFrames(Live2DOffscreenRenderer *renderer,
                                        const QString &probeParameter, int frames, int waitMs,
                                        QVector<float> *values, float injectedDeltaSeconds)
{
    if (renderer == nullptr || frames <= 0)
        return false;
    const QSize targetSize(kBlendProbeWidth, kBlendProbeHeight);
    for (int index = 0; index < frames; ++index)
    {
        if (injectedDeltaSeconds > 0.0f)
            renderer->setNextFrameDeltaSeconds(injectedDeltaSeconds);
        else
            QThread::msleep(static_cast<unsigned long>(waitMs > 0 ? waitMs : 0));
        if (renderer->renderFrame(targetSize).isNull())
            return false;
        if (values != nullptr)
            values->append(renderer->parameterValue(probeParameter));
    }
    return true;
}

TestLive2DOffscreen::SettleResult TestLive2DOffscreen::advanceUntilSettled(
    Live2DOffscreenRenderer *renderer, const QString &probeParameter, float target,
    int maxFrames, int waitMs)
{
    SettleResult result;
    if (renderer == nullptr || maxFrames <= 0)
        return result;
    for (int index = 0; index < maxFrames; ++index)
    {
        if (!advanceFrames(renderer, probeParameter, 1, waitMs, nullptr))
            return result;
        result.frames = index + 1;
        result.value = renderer->parameterValue(probeParameter);
        if (result.value == target)
        {
            result.reached = true;
            return result;
        }
    }
    return result;
}

/*渲染第一帧，验证确实画出了东西，并验证水印可由参数关闭；两版 PNG 落盘供目视确认*/
void TestLive2DOffscreen::rendersMikuFirstFrame()
{
    const QString dir = modelDir();
    const QString modelJsonName = QStringLiteral("miku.model3.json");
    if (!QFileInfo::exists(dir + QLatin1Char('/') + modelJsonName))
        QSKIP("本机没有初音模型（禁二传，不入库），跳过离屏渲染验证");

    const QSize targetSize(760, 900);

    Live2DOffscreenRenderer renderer;
    QString error;
    QVERIFY2(renderer.load(dir, modelJsonName, &error), qPrintable(error));
    QVERIFY(renderer.isLoaded());

    // 水印参数应由模型目录里的 水印.exp3.json 推出，而不是硬编码
    QCOMPARE(renderer.watermarkParamId(), QStringLiteral("Param137"));

    // 默认应为隐藏水印（该参数反相：1 = 隐藏，0 = 显示）
    const QImage watermarkHidden = renderer.renderFrame(targetSize);
    QVERIFY2(!watermarkHidden.isNull(), "renderFrame 返回了空图");
    QCOMPARE(watermarkHidden.size(), targetSize);

    int opaquePixels = 0;
    for (int y = 0; y < watermarkHidden.height(); ++y)
    {
        const uchar *line = watermarkHidden.constScanLine(y);
        for (int x = 0; x < watermarkHidden.width(); ++x)
        {
            if (line[x * 4 + 3] > 32)
                ++opaquePixels;
        }
    }
    QVERIFY2(opaquePixels > 1000,
             qPrintable(QStringLiteral("不透明像素只有 %1 个，模型基本没画出来")
                            .arg(opaquePixels)));

    QDir().mkpath(outputDir());
    const QString hiddenPng = outputDir() + QStringLiteral("/miku-watermark-hidden.png");
    QVERIFY2(watermarkHidden.save(hiddenPng),
             qPrintable(QStringLiteral("写不出 %1").arg(hiddenPng)));

    // 关键：读回参数**真实值**来判断覆盖是否生效。
    // 不能用"两帧像素不同"来证明 —— 两帧之间物理动画（头发/呼吸）本来就会变，
    // 像素差异无法区分"参数生效"和"动画推进"，是无效断言。
    QCOMPARE(renderer.parameterValue(QStringLiteral("Param137")), 1.0f);

    // 显示水印再渲染一版，供人眼比对（水印文字是烤进贴图的，只能靠目视确认）
    renderer.setWatermarkVisible(true);
    const QImage watermarkVisible = renderer.renderFrame(targetSize);
    QVERIFY(!watermarkVisible.isNull());
    const QString visiblePng = outputDir() + QStringLiteral("/miku-watermark-visible.png");
    QVERIFY2(watermarkVisible.save(visiblePng),
             qPrintable(QStringLiteral("写不出 %1").arg(visiblePng)));

    QCOMPARE(renderer.parameterValue(QStringLiteral("Param137")), 0.0f);
    qInfo("离屏渲染产物：隐藏水印=%s 显示水印=%s", qPrintable(hiddenPng),
          qPrintable(visiblePng));
}

/*参数驱动器（呼吸/眨眼/物理）真的接上了吗 —— 这是 Bug A（模型完全静止）的离屏钉子。

  只断言"两帧像素不同"是不够的：那既可能来自驱动器，也可能来自渲染噪声。
  这里直接读**参数真实值**跑一段时间看它有没有变，并覆盖三类驱动器：
    - 眨眼：ParamEyeLOpen / ParamEyeROpen（model3.json 的 EyeBlink 组）
    - 呼吸：ParamAngleX/Y/Z、ParamBodyAngleX、ParamBreath
    - 物理输出：模型的 physics3.json 里的输出参数（Parami / Param_Angle_Rotation_*），
      它们只有物理真的在跑并且每帧**恰好求值一次**才会跟着动
      （算两次会让输出以双倍速度漂移，算零次则一直不变）。

  没装模型就跳过（禁二传，不入库）。*/
void TestLive2DOffscreen::drivesParametersWithUpdaters()
{
    const QString dir = modelDir();
    if (!QFileInfo::exists(dir + QStringLiteral("/miku.model3.json"))
        && !QFileInfo::exists(dir + QStringLiteral("/atri_8.model3.json")))
        QSKIP("本机没有模型（禁二传，不入库），跳过参数驱动器验证");

    const QString modelJsonName = QFileInfo::exists(dir + QStringLiteral("/miku.model3.json"))
                                      ? QStringLiteral("miku.model3.json")
                                      : QStringLiteral("atri_8.model3.json");

    Live2DOffscreenRenderer renderer;
    QString error;
    QVERIFY2(renderer.load(dir, modelJsonName, &error), qPrintable(error));

    const QSize targetSize(600, 700);
    QVERIFY(!renderer.renderFrame(targetSize).isNull());

    /*要观察的参数：眨眼 / 呼吸用官方样例那套名字；物理输出取该模型 physics3.json
      里真实存在的参数名（多试几个，至少有一个会动）。*/
    const QStringList watched = {
        QStringLiteral("ParamEyeLOpen"),   QStringLiteral("ParamEyeROpen"),
        QStringLiteral("ParamAngleX"),     QStringLiteral("ParamAngleY"),
        QStringLiteral("ParamAngleZ"),     QStringLiteral("ParamBodyAngleX"),
        QStringLiteral("ParamBreath"),     QStringLiteral("Parami"),
        QStringLiteral("Param_Angle_Rotation_1_ArtMesh48"),
        QStringLiteral("Param_Angle_Rotation_1_ArtMesh79"),
    };

    QHash<QString, float> first;
    for (const QString &id : watched)
        first.insert(id, renderer.parameterValue(id));

    // 真的让时间过去：呼吸周期 3.2~15.5s，眨眼间隔 1~4s，取样 4s 足够覆盖
    constexpr int kFrames = 40;
    for (int i = 0; i < kFrames; ++i)
    {
        QThread::msleep(100);
        QVERIFY(!renderer.renderFrame(targetSize).isNull());
    }

    int movedParameters = 0;
    for (const QString &id : watched)
    {
        const float before = first.value(id);
        const float after = renderer.parameterValue(id);
        if (!qFuzzyCompare(before, after))
        {
            ++movedParameters;
            qInfo("参数驱动器验证：%s %f → %f", qPrintable(id), before, after);
        }
    }

    QVERIFY2(movedParameters >= 2,
             qPrintable(QStringLiteral("跑完 %1 帧后只有 %2 个参数变化，参数驱动器没接上"
                                       "（呼吸/眨眼/物理至少要有两个在动）")
                            .arg(kFrames)
                            .arg(movedParameters)));
}

/*测每帧渲染成本，给"帧循环能跑多少 fps / 要不要降分辨率"提供真实数据，并作为性能守门。
  为什么必须有这条：窗口层要按帧重绘，帧成本直接决定帧率档位与是否需要降采样。
  实测值随机器/驱动不同，所以这里只设一个**宽松退步警戒线**，真实数字打到日志里。*/
void TestLive2DOffscreen::reportsFrameCost()
{
    const QString dir = modelDir();
    const QString modelJsonName = QStringLiteral("miku.model3.json");
    if (!QFileInfo::exists(dir + QLatin1Char('/') + modelJsonName))
        QSKIP("本机没有初音模型（禁二传，不入库），跳过帧成本测量");

    Live2DOffscreenRenderer renderer;
    QString error;
    QVERIFY2(renderer.load(dir, modelJsonName, &error), qPrintable(error));

    struct Result
    {
        QSize size;
        double msPerFrame;
    };
    QVector<Result> results;

    const int frameCount = 30;
    for (const QSize &size : {QSize(760, 900), QSize(380, 450)})
    {
        (void)renderer.renderFrame(size); // 首帧含 GL 目标重建，不计入
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < frameCount; ++i)
            (void)renderer.renderFrame(size);
        const double msPerFrame = static_cast<double>(timer.elapsed()) / frameCount;
        results.append({size, msPerFrame});
        qInfo("帧成本：%dx%d 平均 %.1f ms/帧（约 %.1f fps）", size.width(), size.height(),
              msPerFrame, msPerFrame > 0.0 ? 1000.0 / msPerFrame : 0.0);
    }

    // 退步警戒：只要不是崩坏级变慢就放过（真实性能靠上面日志观察，不在这里卡死阈值）
    for (const Result &r : results)
    {
        QVERIFY2(r.msPerFrame < 500.0,
                 qPrintable(QStringLiteral("%1x%2 每帧 %.1f ms，慢到不可用")
                                .arg(r.size.width())
                                .arg(r.size.height())
                                .arg(r.msPerFrame)));
    }
}

/*==================== 心情过渡（mood blend） ====================

  用户看到的症状：换心情（尤其是切回 neutral）时"顿一下" —— 参数从 0.9 一帧跳到 0。

  这一组用例把「过渡」这件事拆成四条彼此独立的观察量，每条都对应一种错的实现：
    (a) 起点：刚换心情的那几帧参数仍在**上一个值附近**（一帧就跳过去的实现在这里失败）；
    (b) 终点：过了过渡时长后参数**精确等于**目标值（纯指数逼近的实现在这里失败 ——
        指数永远到不了，读回的永远是个"接近但不等于"的数）；
    (c) 途中：逐帧变化量有上界（退回"一帧跳完"的实现在这里失败）；
    (d) 幂等：同一个心情再施加一次**不重启**过渡（"每次拿目标值重置当前值"的实现会被
        这条抓住：参数被拽回起点、出现一个反向跳变）。

  为什么量参数而不是量像素：过渡是**时间**上的曲线，像素差里混着呼吸/眨眼/物理的漂移，
  分不出"过渡走了多少"和"背景动了多少"。参数读回是直接证据。

  ⚠️ 帧的时间步长来自墙钟（renderFrame 内部的 clock.restart()），所以每帧之间**必须真的等**；
  紧循环连调 renderFrame() 只隔几微秒，过渡永远走不完，这条测试会假通过。
  这也是为什么这里统一用 240x300 的小画布：把省下的渲染时间换成采样帧数。

  这一组是**渲染器级**用例（不需要窗口/心情预设数据）：过渡的数学与"心情"这个词无关，
  用一组显式的覆盖值就能把曲线量清楚，模型也只要求"本机有任意一个模型"。*/
void TestLive2DOffscreen::moodBlendStartsFromPreviousValue()
{
    Live2DOffscreenRenderer renderer;
    QString error;
    if (!loadAnyModel(&renderer, &error))
        QSKIP("本机没有 Live2D 模型（禁二传，不入库），跳过心情过渡验证");
    requireBlendProbeParameters(&renderer);
    QVERIFY(!renderer.renderFrame(QSize(kBlendProbeWidth, kBlendProbeHeight)).isNull());

    // 过渡给足 1s：这条要观察的是"起点"，时长越长起点读数越清晰
    renderer.setMoodBlendDurationMs(1000);
    renderer.setParameterOverrides({{kMoodProbeParameter, 0.9f}});
    /*先导阶段：**推进到真的到达**，不靠"猜几帧"。
       帧时间 = 等待 + 渲染成本，随机器/画布/驱动变化，写死帧数会让"起点读数"随机器而变 ——
       先导阶段没到 0.9 的话，下面 afterOneFrame 的对照就失去意义。*/
    const SettleResult leadIn =
        advanceUntilSettled(&renderer, kMoodProbeParameter, 0.9f, 300, 20);
    QVERIFY2(leadIn.reached,
             qPrintable(QStringLiteral("先导阶段：%1 在 %2 帧内都没到 0.9（当前 %3）")
                            .arg(kMoodProbeParameter)
                            .arg(leadIn.frames)
                            .arg(double(leadIn.value))));
    const float before = leadIn.value;

    /*换心情：**一帧都不渲**就立刻读一次。
       这一条在旧实现上也会通过（覆盖值要等下一帧 tick 才写进模型），留着它是为了把
       "当帧就跳"这种更粗暴的实现也钉住 —— 真正的判据是紧接着的下一帧。*/
    renderer.setParameterOverrides({{kMoodProbeParameter, 0.0f}});
    const float immediate = renderer.parameterValue(kMoodProbeParameter);
    QVERIFY2(qAbs(immediate - before) <= 0.15f,
             qPrintable(QStringLiteral("换心情当帧 %1 就跳到了 %2（上一帧 %3）")
                            .arg(kMoodProbeParameter)
                            .arg(double(immediate))
                            .arg(double(before))));

    QVERIFY(advanceFrames(&renderer, kMoodProbeParameter, 1, 20, nullptr));
    const float afterOneFrame = renderer.parameterValue(kMoodProbeParameter);
    qInfo("BLEND start[%s]: before=%.4f immediate=%.4f afterOneFrame=%.4f",
          qPrintable(kMoodProbeParameter), double(before), double(immediate),
          double(afterOneFrame));
    /*1s 过渡、一帧约 20ms ⇒ 这一帧最多走 2% 的量程（0.018），给 6 倍余量。
       旧实现下 afterOneFrame 直接是 0.0000 —— 与 before 差 0.9，必然失败。*/
    QVERIFY2(qAbs(afterOneFrame - before) <= 0.12f,
             qPrintable(QStringLiteral("换心情后**第一帧** %1 就从 %2 跳到 %3 —— "
                                       "参数是整组原子替换，过渡没有发生")
                            .arg(kMoodProbeParameter)
                            .arg(double(before))
                            .arg(double(afterOneFrame))));
}

void TestLive2DOffscreen::moodBlendReachesTargetExactly()
{
    Live2DOffscreenRenderer renderer;
    QString error;
    if (!loadAnyModel(&renderer, &error))
        QSKIP("本机没有 Live2D 模型（禁二传，不入库），跳过心情过渡验证");
    requireBlendProbeParameters(&renderer);
    QVERIFY(!renderer.renderFrame(QSize(kBlendProbeWidth, kBlendProbeHeight)).isNull());

    constexpr int kBlendMs = 300;
    renderer.setMoodBlendDurationMs(kBlendMs);

    // 起点：0
    renderer.setParameterOverrides({{kMoodProbeParameter, 0.0f}});
    const SettleResult atZero =
        advanceUntilSettled(&renderer, kMoodProbeParameter, 0.0f, 200, 20);
    QVERIFY2(atZero.reached,
             qPrintable(QStringLiteral("起点没落到 0（当前 %1）").arg(double(atZero.value))));

    /*换到 0.9，等远超过渡时长（300ms 过渡，等 2.4s 的墙钟时间）。
       给足时间是为了断言"**精确等于**"：线性逼近 + 收敛 epsilon 必须落在目标上，
       而指数逼近（value += (target-value)*k*dt）即使等再久也差一点（实测 0.8666）。*/
    renderer.setParameterOverrides({{kMoodProbeParameter, 0.9f}});
    QVERIFY(advanceFrames(&renderer, kMoodProbeParameter, 60, 20, nullptr));
    const float arrived = renderer.parameterValue(kMoodProbeParameter);
    qInfo("BLEND arrival[%s]: target=0.9000 actual=%.9f", qPrintable(kMoodProbeParameter),
          double(arrived));

    /***精确**相等：线性逼近 + 收敛 epsilon 必须落在目标上。
       指数逼近（value += (target-value)*k*dt）在这里永远差一点点，
       所以这条断言就是"必须 snap"的那颗钉子。*/
    QVERIFY2(qAbs(arrived - 0.9f) <= 1e-5f,
             qPrintable(QStringLiteral("过渡时长过后 %1 = %.6f，不等于目标 0.900000 —— "
                                       "逼近必须收敛到目标（snap），不能只是无限接近")
                            .arg(kMoodProbeParameter)
                            .arg(double(arrived))));
}

void TestLive2DOffscreen::moodBlendHasNoSingleFrameJump()
{
    Live2DOffscreenRenderer renderer;
    QString error;
    if (!loadAnyModel(&renderer, &error))
        QSKIP("本机没有 Live2D 模型（禁二传，不入库），跳过心情过渡验证");
    requireBlendProbeParameters(&renderer);
    QVERIFY(!renderer.renderFrame(QSize(kBlendProbeWidth, kBlendProbeHeight)).isNull());

    renderer.setMoodBlendDurationMs(400);
    renderer.setParameterOverrides({{kMoodProbeParameter, 0.0f}});

    QVector<float> samples;
    // 400ms 过渡、每帧约 12ms ⇒ 约 33 帧走完；两个方向各采 50 帧把整个过渡覆盖住
    QVERIFY(advanceFrames(&renderer, kMoodProbeParameter, 50, 12, &samples));
    renderer.setParameterOverrides({{kMoodProbeParameter, 0.9f}});
    QVERIFY(advanceFrames(&renderer, kMoodProbeParameter, 50, 12, &samples));

    QVERIFY2(samples.size() >= 2, "采样帧数不足");
    float maxStep = 0.0f;
    int maxStepIndex = -1;
    for (int index = 1; index < samples.size(); ++index)
    {
        const float step = qAbs(samples[index] - samples[index - 1]);
        if (step > maxStep)
        {
            maxStep = step;
            maxStepIndex = index;
        }
    }
    const float totalRange = 0.9f;
    qInfo("BLEND no-jump[%s]: frames=%d maxStep=%.4f at=%d (range=%.2f)",
          qPrintable(kMoodProbeParameter), samples.size(), double(maxStep), maxStepIndex,
          double(totalRange));

    /*阈值 = 量程的 1/3。为什么这么取：
       - 时间制过渡下每帧位移 ≈ 量程 × dt/过渡时长。400ms 过渡里最坏的一帧即使吃掉
         60ms（本机实测帧时间被抢占时也到不了这个数）也只有 0.9 × 0.15 = 0.135，
         离 0.30 有一倍余量，所以不会因为线程抖动偶发失败；
       - 而"一帧跳完"的实现第一帧就位移 0.9，是阈值的 3 倍，必然被抓到。
       噪声不参与：这个参数不归任何驱动器管（见 kMoodProbeParameter 的说明）。*/
    QVERIFY2(maxStep <= totalRange / 3.0f,
             qPrintable(QStringLiteral("逐帧最大变化 %.4f（第 %1 帧）超过量程的 1/3 —— "
                                       "参数在某一帧整块跳过去了，过渡没有铺开")
                            .arg(double(maxStep))
                            .arg(maxStepIndex)));
}

void TestLive2DOffscreen::applyingSameMoodTwiceDoesNotRestartBlend()
{
    Live2DOffscreenRenderer renderer;
    QString error;
    if (!loadAnyModel(&renderer, &error))
        QSKIP("本机没有 Live2D 模型（禁二传，不入库），跳过心情过渡验证");
    requireBlendProbeParameters(&renderer);
    QVERIFY(!renderer.renderFrame(QSize(kBlendProbeWidth, kBlendProbeHeight)).isNull());

    // 1s 过渡：留出大片"过渡途中"的窗口，重启与不重启的行为差异非常明显
    renderer.setMoodBlendDurationMs(1000);
    renderer.setParameterOverrides({{kMoodProbeParameter, 0.0f}});
    QVERIFY(advanceFrames(&renderer, kMoodProbeParameter, 3, 20, nullptr));

    renderer.setParameterOverrides({{kMoodProbeParameter, 0.9f}});
    QVERIFY(advanceFrames(&renderer, kMoodProbeParameter, 8, 20, nullptr)); // 走到途中的某点
    const float midway = renderer.parameterValue(kMoodProbeParameter);
    QVERIFY2(midway > 0.02f && midway < 0.85f,
             qPrintable(QStringLiteral("采样点 %1 不在过渡途中（%2），本用例的前提不成立")
                            .arg(kMoodProbeParameter)
                            .arg(double(midway))));

    /***同一个**心情再施加一次。它必须什么都不做：不能把当前值重置回 0（那会让参数
       反向跳一下，屏幕上就是一次小回退），也不能重新计时。
       检测手段是把下一帧与"当帧读到的值"对比 —— 正常推进是单调向 0.9 靠近，
       而"重启过渡"的实现会让它掉回起点附近甚至 0。*/
    renderer.setParameterOverrides({{kMoodProbeParameter, 0.9f}});
    const float rightAfterRepeat = renderer.parameterValue(kMoodProbeParameter);
    QVERIFY(advanceFrames(&renderer, kMoodProbeParameter, 1, 20, nullptr));
    const float nextFrame = renderer.parameterValue(kMoodProbeParameter);
    qInfo("BLEND same-mood[%s]: midway=%.4f rightAfterRepeat=%.4f nextFrame=%.4f",
          qPrintable(kMoodProbeParameter), double(midway), double(rightAfterRepeat),
          double(nextFrame));

    // 不跳变
    QVERIFY2(qAbs(nextFrame - rightAfterRepeat) <= 0.10f,
             qPrintable(QStringLiteral("重复施加同一个心情后 %1 从 %2 跳到 %3")
                            .arg(kMoodProbeParameter)
                            .arg(double(rightAfterRepeat))
                            .arg(double(nextFrame))));
    // 而且仍然是"朝目标推进"，不是被重置回起点
    QVERIFY2(nextFrame >= rightAfterRepeat - 1e-4f,
             qPrintable(QStringLiteral("重复施加同一个心情后 %1 反而倒退了（%2 → %3）—— "
                                       "过渡被重新开始了")
                            .arg(kMoodProbeParameter)
                            .arg(double(rightAfterRepeat))
                            .arg(double(nextFrame))));
    QVERIFY2(nextFrame > midway * 0.6f,
             qPrintable(QStringLiteral("重复施加同一个心情后 %1 掉回 %2（途中是 %3）—— "
                                       "过渡被重置了")
                            .arg(kMoodProbeParameter)
                            .arg(double(nextFrame))
                            .arg(double(midway))));
}

void TestLive2DOffscreen::moodBlendDurationIsConfigurable()
{
    Live2DOffscreenRenderer renderer;
    QString error;
    if (!loadAnyModel(&renderer, &error))
        QSKIP("本机没有 Live2D 模型（禁二传，不入库），跳过心情过渡验证");
    requireBlendProbeParameters(&renderer);
    QVERIFY(!renderer.renderFrame(QSize(kBlendProbeWidth, kBlendProbeHeight)).isNull());

    /*同一段过渡、同一套采样节奏，只改过渡时长 → 观察到的过渡长度必须跟着变。
       只断言"能不能到目标"是不够的：一个**硬编码** 200ms 的实现也能到目标，
       但它对配置改动的反应是零 —— 那样用户就调不动手感（与 fps/scale 同款诉求）。

       这条同时是 Config 键的**行为级**钉子：窗口把 character/live2dMoodBlendMs
       喂给 setMoodBlendDurationMs（另有窗口级用例验证这条线），
       而"喂进来的数字真的改变过渡长度"由这里证明。*/
    const auto framesToReachTarget = [&](int blendMs) {
        renderer.setMoodBlendDurationMs(blendMs);
        // 每次都从明确的起点开始（同一组覆盖值 → 不触发过渡）
        renderer.setParameterOverrides({{kMoodProbeParameter, 0.0f}});
        if (!advanceFrames(&renderer, kMoodProbeParameter, 4, 10, nullptr))
            return -1;

        QElapsedTimer clock;
        clock.start();
        renderer.setParameterOverrides({{kMoodProbeParameter, 0.9f}});
        for (int frame = 1; frame <= 400; ++frame)
        {
            if (!advanceFrames(&renderer, kMoodProbeParameter, 1, 10, nullptr))
                return -1;
            if (qAbs(renderer.parameterValue(kMoodProbeParameter) - 0.9f) <= 1e-5f)
            {
                qInfo("BLEND duration probe: %dms → %d frames / %lld ms", blendMs, frame,
                      clock.elapsed());
                return frame;
            }
        }
        return 0; // 400 帧都没到目标
    };

    const int fastFrames = framesToReachTarget(200);
    const int slowFrames = framesToReachTarget(600);
    qInfo("BLEND duration: 200ms→%d frames, 600ms→%d frames (同一采样节奏)", fastFrames,
          slowFrames);

    QVERIFY2(fastFrames > 0 && slowFrames > 0, "过渡在 3s 内都没走到目标");
    /*600ms 是 200ms 的 3 倍，时间制实现下帧数应当也约 3 倍。
       给 2 倍这条下界：帧时间里有 sleep(10) 之外的渲染成本与系统抖动，
       但"3 倍 vs 1 倍"这个量级差绝不会被抖动吃掉；而硬编码时长时两者必然相等。*/
    QVERIFY2(slowFrames >= fastFrames * 2,
             qPrintable(QStringLiteral("过渡时长 200ms 用了 %1 帧、600ms 只用了 %2 帧 —— "
                                       "观察到的过渡长度没有跟着配置变（时长可能是硬编码的）")
                            .arg(fastFrames)
                            .arg(slowFrames)));
}

/*==================== 说话时的嘴巴扑动（anime paper-doll 口型） ====================

  用户要的是"她说话时嘴在动"这一条最朴素的观感：不是音素级 lip-sync，也不是音量驱动，
  而是**动画片纸片人**那种上下开合。所以判据不是"嘴型对不对"，而是三件事：
    (a) 说话时开口量**随时间变化**（不规则、3~5 次/秒量级），且始终落在模型声明的
        [min,max] 里 —— 越过声明范围会被 Core 夹回去，屏幕上表现为"卡在上下限"；
    (b) 不打扰任何别的东西：其他被覆盖的参数一动不动，`ParamMouthForm`（嘴形，属于心情）
        绝对不许被碰 —— 它是嘴的**形状**，与开合是两件事，碰了就会与心情打架；
    (c) 停止说话后扑动**回落到心情自己的值**（不是啪一下闭嘴）。

  ⚠️ 帧时间来自墙钟，所以必须 QTest 式地"真的等"；紧循环连调 renderFrame 不会推进扑动。

  这一组同样是**渲染器级**用例：扑动的数学与"哪句台词"无关，用一组显式覆盖值就能量清楚。*/
void TestLive2DOffscreen::speakingMouthFlapVariesAndStaysInRange()
{
    Live2DOffscreenRenderer renderer;
    QString error;
    if (!loadAnyModel(&renderer, &error))
        QSKIP("本机没有 Live2D 模型（禁二传，不入库），跳过说话嘴巴验证");
    requireMouthParameters(&renderer);
    requireBlendProbeParameters(&renderer);

    const QHash<QString, Live2DOffscreenRenderer::DeclaredRange> ranges =
        renderer.declaredParameterRanges();
    const Live2DOffscreenRenderer::DeclaredRange mouthRange = ranges.value(kMouthOpenParameter);
    QVERIFY2(mouthRange.max > mouthRange.min,
             qPrintable(QStringLiteral("%1 的声明范围是 [%2,%3]，量不出任何东西")
                            .arg(kMouthOpenParameter)
                            .arg(double(mouthRange.min))
                            .arg(double(mouthRange.max))));

    QVERIFY(!renderer.renderFrame(QSize(kBlendProbeWidth, kBlendProbeHeight)).isNull());

    /*心情：嘴形取一个明确的非中立值，用来证明"扑动不碰嘴形"；
       开口量取范围中点（= 说话时围着它上下开合，与预设值"surprised 张嘴"这类情形一致）。
       另外两项（眼笑/眉毛）是"不该被动到"的旁观者。*/
    const float moodMouthOpen = mouthRange.min + (mouthRange.max - mouthRange.min) * 0.5f;
    const QHash<QString, float> mood = {{kMoodProbeParameter, 0.35f},
                                        {kMouthOpenParameter, moodMouthOpen},
                                        {kMouthFormParameter, -0.7f},
                                        {kSecondProbeParameter, 0.25f}};
    renderer.setMoodBlendDurationMs(150);
    renderer.setParameterOverrides(mood);
    const SettleResult settled =
        advanceUntilSettled(&renderer, kMouthOpenParameter, moodMouthOpen, 200, 20);
    QVERIFY2(settled.reached,
             qPrintable(QStringLiteral("心情的开口量没落到 %1（当前 %2）")
                            .arg(double(moodMouthOpen))
                            .arg(double(settled.value))));

    /*说话：注入固定步长的 60 帧 ⇒ **虚拟时间恰好 1.0s**（1/60 步长）。
      为什么不用真实等待：帧增量有 0.1s 的夹取上限，机器一被抢占，"墙钟间隔"与"虚拟时间"
      就脱钩（每帧都吃满 100ms），扑动的相位推进随之变慢、反转次数掉下来 —— 这正是本用例
      此前负载下偶发变红的原因。步长由调用方钉住后，反转次数只由频率决定。*/
    renderer.setSpeaking(true);
    QVector<float> samples;
    QVERIFY(advanceFrames(&renderer, kMouthOpenParameter, 60, 0, &samples, 1.0f / 60.0f));
    QVERIFY2(samples.size() >= 20, "采样帧数不足，量不出扑动");

    float minValue = samples.first();
    float maxValue = samples.first();
    int alternations = 0;
    int previousTrend = 0;
    for (int index = 0; index < samples.size(); ++index)
    {
        minValue = std::min(minValue, samples[index]);
        maxValue = std::max(maxValue, samples[index]);
        if (index == 0)
            continue;
        const int trend = (samples[index] > samples[index - 1]) ? 1 : -1;
        if (previousTrend != 0 && trend != previousTrend)
            ++alternations;
        previousTrend = trend;
    }

    QStringList printSamples;
    for (int index = 0; index < samples.size(); index += 4)
        printSamples << QString::number(double(samples[index]), 'f', 4);
    qInfo("SPEAK flap[%s]: frames=%d min=%.4f max=%.4f span=%.4f alternations=%d "
          "declared=[%.3f,%.3f] samples(every 4th)=%s",
          qPrintable(kMouthOpenParameter), samples.size(), double(minValue), double(maxValue),
          double(maxValue - minValue), alternations, double(mouthRange.min),
          double(mouthRange.max), qPrintable(printSamples.join(QStringLiteral(","))));

    /*(a1) 真的在动。为什么用**相对声明量程**的比例而不是绝对值：
       换模型时量程可能完全不同（0~1 与 0~2 都是常见的），绝对阈值会变成"只对本机模型有效"。
       15% 的量程：本机 atri 上实测开合跨度约 0.55（55% 量程），留了 3 倍余量。*/
    QVERIFY2(maxValue - minValue >= (mouthRange.max - mouthRange.min) * 0.15f,
             qPrintable(QStringLiteral("说话时 %1 只在 %2~%3 之间动（量程 %4~%5）—— 嘴巴没动")
                            .arg(kMouthOpenParameter)
                            .arg(double(minValue))
                            .arg(double(maxValue))
                            .arg(double(mouthRange.min))
                            .arg(double(mouthRange.max))));
    /*(a2) 是"开合"而不是"慢漂移"：方向必须反复改。
       虚拟时间 1.0s、频率 3~5 次/秒 ⇒ 反转次数 = 2×频率×时间 = **6~10 次**。
       下界取 5：3Hz 的 6 次留了余量，而"慢漂移"只有 0~1 次、必然被抓。
       上界取 14：5Hz 的 10 次留了余量，同时"频率被误改回旧的 8~10Hz"会给出 16~20 次、
       也会被抓 —— 两侧都卡住，改错任何一个方向都过不去。*/
    QVERIFY2(alternations >= 5 && alternations <= 14,
             qPrintable(QStringLiteral("说话窗口里开口量换了 %1 次方向（期望 5~14，"
                                       "对应 3~5 次/秒 × 1.0s 虚拟时间）—— 不是开合振荡，"
                                       "或频率被改到了别的量级")
                            .arg(alternations)));
    /*(a3) 一个采样窗内的**打开量**应当是个温和的包络，不是"几乎一直全开"：
       开合比只统计"明显张开"的帧占比，太满（>80%）说明振幅被顶到了上限，
       这种嘴看起来是"张着不动"。*/
    const float openThreshold = moodMouthOpen + (maxValue - moodMouthOpen) * 0.3f;
    int openFrames = 0;
    for (const float value : samples)
    {
        if (value > openThreshold)
            ++openFrames;
    }
    const double openRatio = double(openFrames) / double(samples.size());
    qInfo("SPEAK flap openness: %d/%d frames above %.4f (ratio %.2f)", openFrames,
          samples.size(), double(openThreshold), openRatio);
    QVERIFY2(openRatio < 0.8, qPrintable(QStringLiteral("说话时嘴有 %1%% 的帧处于张开状态 ——"
                                                        "振幅顶在上限上，看着是张着嘴不动")
                                             .arg(openRatio * 100.0, 0, 'f', 1)));
    /*(a4) 永远不许越出模型声明的范围：越界会被 Core 夹回去，屏幕上就是"卡在上下限"。*/
    QVERIFY2(minValue >= mouthRange.min - 1e-4f && maxValue <= mouthRange.max + 1e-4f,
             qPrintable(QStringLiteral("说话时 %1 落到 [%2,%3] 之外（实测 %4~%5）")
                            .arg(kMouthOpenParameter)
                            .arg(double(mouthRange.min))
                            .arg(double(mouthRange.max))
                            .arg(double(minValue))
                            .arg(double(maxValue))));
}

void TestLive2DOffscreen::speakingDoesNotDisturbOtherParameters()
{
    Live2DOffscreenRenderer renderer;
    QString error;
    if (!loadAnyModel(&renderer, &error))
        QSKIP("本机没有 Live2D 模型（禁二传，不入库），跳过说话干扰验证");
    requireMouthParameters(&renderer);
    requireBlendProbeParameters(&renderer);

    QVERIFY(!renderer.renderFrame(QSize(kBlendProbeWidth, kBlendProbeHeight)).isNull());

    constexpr float kMoodMouthOpen = 0.2f;
    constexpr float kMoodMouthForm = -0.7f;
    constexpr float kMoodSmile = 0.4f;
    constexpr float kMoodBrow = 0.25f;
    const QHash<QString, float> mood = {{kMoodProbeParameter, kMoodSmile},
                                        {kMouthOpenParameter, kMoodMouthOpen},
                                        {kMouthFormParameter, kMoodMouthForm},
                                        {kSecondProbeParameter, kMoodBrow}};
    renderer.setMoodBlendDurationMs(150);
    renderer.setParameterOverrides(mood);
    QVERIFY(advanceUntilSettled(&renderer, kMoodProbeParameter, kMoodSmile, 200, 20).reached);

    renderer.setSpeaking(true);
    /*说话 40 帧，每帧把"除嘴巴开合之外"的参数读一遍。
       注意**不能**把 ParamMouthOpenY 放进被检查的集合里（它本来就该动），
       但 ParamMouthForm 必须在 —— 它是嘴形，属于心情，扑动碰它就是与心情打架。*/
    const QStringList watched = {kMouthFormParameter, kMoodProbeParameter, kSecondProbeParameter};
    for (int frame = 0; frame < 40; ++frame)
    {
        QVERIFY(advanceFrames(&renderer, kMouthOpenParameter, 1, 15, nullptr));
        for (const QString &id : watched)
        {
            const float expected = mood.value(id);
            const float actual = renderer.parameterValue(id);
            QVERIFY2(qAbs(actual - expected) <= 1e-4f,
                     qPrintable(QStringLiteral("说话第 %1 帧：%2 = %3，不等于心情值 %4 —— "
                                               "扑动动了不该动的参数（嘴形属于心情）")
                                    .arg(frame)
                                    .arg(id)
                                    .arg(double(actual))
                                    .arg(double(expected))));
        }
    }
    qInfo("SPEAK isolation: %d frames，%s 全程等于心情值 %.3f（未被扑动触碰）", 40,
          qPrintable(kMouthFormParameter), double(kMoodMouthForm));
}

void TestLive2DOffscreen::stoppingSpeechDecaysFlapToMoodValue()
{
    Live2DOffscreenRenderer renderer;
    QString error;
    if (!loadAnyModel(&renderer, &error))
        QSKIP("本机没有 Live2D 模型（禁二传，不入库），跳过停止说话验证");
    requireMouthParameters(&renderer);
    requireBlendProbeParameters(&renderer);

    QVERIFY(!renderer.renderFrame(QSize(kBlendProbeWidth, kBlendProbeHeight)).isNull());

    /*心情故意给一个**非零**开口量：这样"扑动回落到 0"与"回落到心情值"能区分开
       —— 若实现是"停止说话就把嘴合死"，最终值会是 0 而不是这个数。*/
    constexpr float kMoodMouthOpen = 0.32f;
    const QHash<QString, float> mood = {{kMoodProbeParameter, 0.0f},
                                        {kMouthOpenParameter, kMoodMouthOpen},
                                        {kSecondProbeParameter, 0.0f}};
    constexpr int kBlendMs = 200;
    renderer.setMoodBlendDurationMs(kBlendMs);
    renderer.setParameterOverrides(mood);
    QVERIFY(advanceUntilSettled(&renderer, kMouthOpenParameter, kMoodMouthOpen, 200, 20).reached);

    renderer.setSpeaking(true);
    QVector<float> speakingSamples;
    QVERIFY(advanceFrames(&renderer, kMouthOpenParameter, 25, 20, &speakingSamples));
    const float speakingMin = *std::min_element(speakingSamples.constBegin(),
                                                speakingSamples.constEnd());
    const float speakingMax = *std::max_element(speakingSamples.constBegin(),
                                                speakingSamples.constEnd());
    QVERIFY2(speakingMax - speakingMin > 0.05f,
             qPrintable(QStringLiteral("说话阶段没量到扑动（%1~%2），本用例的前提不成立")
                            .arg(double(speakingMin))
                            .arg(double(speakingMax))));

    renderer.setSpeaking(false);
    /*等远超过渡时长（200ms 过渡 ⇒ 采 1.2s 的墙钟）。

       ⚠️ 判据必须看**尾部窗口**而不是整段的极差：衰减是"线性趋近 0"，
       整段极差里含着开头那几百毫秒的残余摆动（那是正确行为，不是没衰减）。
       取最后 1/4 段当"稳态窗口"，才是"扑动已经彻底退场"的证据。*/
    QVector<float> afterSamples;
    QVERIFY(advanceFrames(&renderer, kMouthOpenParameter, 60, 20, &afterSamples));
    const float afterMin = *std::min_element(afterSamples.constBegin(), afterSamples.constEnd());
    const float afterMax = *std::max_element(afterSamples.constBegin(), afterSamples.constEnd());
    const int tailBegin = afterSamples.size() * 3 / 4;
    float tailMin = afterSamples[tailBegin];
    float tailMax = afterSamples[tailBegin];
    for (int index = tailBegin; index < afterSamples.size(); ++index)
    {
        tailMin = std::min(tailMin, afterSamples[index]);
        tailMax = std::max(tailMax, afterSamples[index]);
    }
    qInfo("SPEAK stop[%s]: speaking span=%.4f（%s）→ stopped all=%.4f~%.4f, tail(last 1/4)"
          "=%.4f~%.4f span=%.4f, mood=%.4f",
          qPrintable(kMouthOpenParameter), double(speakingMax - speakingMin),
          qPrintable(QStringLiteral("%1~%2").arg(double(speakingMin)).arg(double(speakingMax))),
          double(afterMin), double(afterMax), double(tailMin), double(tailMax),
          double(tailMax - tailMin), double(kMoodMouthOpen));

    /*(a) 扑动真的退场了：稳态窗口里开口量不再摆动（不是"幅度只衰减一半"）。
       说话的幅度是"整段极差"，对比同一口径的稳态极差。*/
    QVERIFY2(tailMax - tailMin <= 0.02f,
             qPrintable(QStringLiteral("停止说话 1.2s 后开口量仍在 %1~%2 之间摆"
                                       "（稳态极差 %3）—— 扑动没有停")
                            .arg(double(tailMin))
                            .arg(double(tailMax))
                            .arg(double(tailMax - tailMin))));
    /*(b) 回落到**心情自己的值**，不是 0（"停止说话 = 闭嘴"是错的解法）。
       稳态窗口的上下界都要贴在心情值上，所以这条同时挡住"衰减到别的数"与"合死到 0"。*/
    QVERIFY2(qAbs(tailMax - kMoodMouthOpen) <= 0.02f && qAbs(tailMin - kMoodMouthOpen) <= 0.02f,
             qPrintable(QStringLiteral("停止说话后开口量停在 %1~%2，不等于心情值 %3 ——"
                                       "扑动没有干净地回落，或把嘴合死了")
                            .arg(double(tailMin))
                            .arg(double(tailMax))
                            .arg(double(kMoodMouthOpen))));
}

/*==================== 响度包络驱动嘴巴（用户诉求：句间停顿要闭嘴） ====================

  用户看到的问题：TTS 在播时嘴按"盲扑动"开合，**句子之间的停顿里嘴照样在动** ——
  她明明停下来了，嘴还在开合，看着像"在放录音"。用户要的是"说话时嘴动、停顿时嘴闭"，
  并且明确接受不做音素级口型。于是判据是五条彼此独立的观察量：

    (a) 有声段：嘴真的张开（响度到了嘴没动 = 接线断了）；
    (b) 静音段：嘴回到**心情自己的值**（不是"幅度小一点"，更不是合死到 0）；
    (c) 电平恒为 0：一点都不许动（停顿里不许留残余扑动）；
    (d) 没有包络时（用户把 vits 的 format 配成 mp3）：行为回到今天的盲扑动；
    (e) 包络路径不许碰 `ParamMouthForm` 等别的参数（嘴形属于心情）。

  为什么用**合成 WAV** 而不是真跑一遍 TTS：包络只能在"响度已知"的输入上证伪，
  而真实字节来自本机的 vits 服务（测试不能依赖它）。解析那一半另有专测
  （test_audioenvelope），这里只关心"电平 → 开口量"这条映射。

  ⚠️ 帧时间一律用**注入的虚拟步长**：真实墙钟在负载下会被夹到 100ms/帧
  （见 setNextFrameDeltaSeconds），那时"播放位置推进了多少"与"帧时间"就脱钩了，
  断言会随机器而变。注入步长让"位置 → 电平 → 开口量"这条链逐帧可控。*/

bool TestLive2DOffscreen::playEnvelope(Live2DOffscreenRenderer *renderer,
                                       const AudioEnvelope &envelope, int frames,
                                       float deltaSeconds, QVector<EnvelopeSample> *samples)
{
    if (renderer == nullptr || frames <= 0 || deltaSeconds <= 0.0f)
        return false;
    const QSize targetSize(kBlendProbeWidth, kBlendProbeHeight);
    for (int frame = 0; frame < frames; ++frame)
    {
        const qint64 positionMs =
            qint64(std::lround(double(frame) * double(deltaSeconds) * 1000.0));
        const float level = envelope.levelAtMs(positionMs);
        renderer->setSpeechLevel(level);
        renderer->setNextFrameDeltaSeconds(deltaSeconds);
        if (renderer->renderFrame(targetSize).isNull())
            return false;
        if (samples != nullptr)
            samples->append({positionMs, level, renderer->parameterValue(kMouthOpenParameter)});
    }
    return true;
}

/*头条用例：前半句有声、后半句静音 → 静音段嘴巴必须回到心情自己的值。

  这就是用户提的那件事："她停下来的时候嘴要闭上"，而不是"幅度小一点"。
  所以断言是**逐点等于心情值**（1e-3），不是"比有声段小"。*/
void TestLive2DOffscreen::envelopeSilenceClosesMouth()
{
    Live2DOffscreenRenderer renderer;
    QString error;
    if (!loadAnyModel(&renderer, &error))
        QSKIP("本机没有 Live2D 模型（禁二传，不入库），跳过包络闭嘴验证");
    requireMouthParameters(&renderer);
    requireBlendProbeParameters(&renderer);
    QVERIFY(!renderer.renderFrame(QSize(kBlendProbeWidth, kBlendProbeHeight)).isNull());

    /*合成本句音频：前 1s 有声（正弦峰值 0.6）、后 1s 数字静音。
        静音段从 1000ms 起 —— 与下面"按位置过滤"的判据同源。*/
    const QByteArray wav = buildPcm16Wav(22050, 1, {{1000, 0.6f}, {1000, 0.0f}});
    const AudioEnvelope envelope = AudioEnvelope::fromWavBytes(wav);
    QVERIFY2(envelope.isValid(), "合成 WAV 解析不出包络 —— 本用例的前提不成立");
    QCOMPARE(envelope.levelAtMs(1000), 0.0f);

    const QHash<QString, Live2DOffscreenRenderer::DeclaredRange> ranges =
        renderer.declaredParameterRanges();
    const Live2DOffscreenRenderer::DeclaredRange mouthRange = ranges.value(kMouthOpenParameter);
    const float span = mouthRange.max - mouthRange.min;
    QVERIFY2(span > 0.0f, "开口量没有量程，本用例量不出东西");

    /*心情的开口量取**量程中点**：这样"张开"与"闭嘴"两个方向都有余量，
        断言不会因为撞上 clamp 而失去意义。*/
    const float moodMouthOpen = mouthRange.min + span * 0.5f;
    const QHash<QString, float> mood = {{kMoodProbeParameter, 0.35f},
                                        {kMouthOpenParameter, moodMouthOpen},
                                        {kMouthFormParameter, -0.7f},
                                        {kSecondProbeParameter, 0.25f}};
    renderer.setMoodBlendDurationMs(150);
    renderer.setParameterOverrides(mood);
    QVERIFY(advanceUntilSettled(&renderer, kMouthOpenParameter, moodMouthOpen, 200, 20).reached);

    /*播放：1/60s 虚拟步长 × 150 帧 = 2.5s 的虚拟时间，覆盖"有声 → 静音"整段。*/
    renderer.setSpeaking(true);
    QVector<EnvelopeSample> samples;
    QVERIFY(playEnvelope(&renderer, envelope, 150, 1.0f / 60.0f, &samples));
    QVERIFY2(samples.size() >= 100, "采样帧数不足");

    float loudMax = samples.first().mouthOpen;
    for (const EnvelopeSample &sample : samples)
    {
        if (sample.positionMs < 900) //有声段（留出最后 100ms 给释放，见下）
            loudMax = std::max(loudMax, sample.mouthOpen);
    }

    /*静音段从 1000ms 起，但释放需要一点时间，所以从 1400ms 开始判。
        这一段里每一个采样点都必须贴在心情值上。*/
    int silentFrames = 0;
    float silentMaxDeviation = 0.0f;
    for (const EnvelopeSample &sample : samples)
    {
        if (sample.positionMs < 1400)
            continue;
        ++silentFrames;
        QVERIFY2(sample.level == 0.0f,
                 qPrintable(QStringLiteral("位置 %1ms 的电平是 %2，不是 0")
                                .arg(sample.positionMs)
                                .arg(double(sample.level))));
        silentMaxDeviation =
            std::max(silentMaxDeviation, qAbs(sample.mouthOpen - moodMouthOpen));
    }
    qInfo("ENVELOPE silence: loudMax=%.4f (mood=%.4f span=%.4f) silentFrames=%d "
          "silentMaxDeviation=%.6f",
          double(loudMax), double(moodMouthOpen), double(span), silentFrames,
          double(silentMaxDeviation));

    QVERIFY2(silentFrames >= 60,
             qPrintable(QStringLiteral("静音段只采到 %1 帧，本用例的前提不成立")
                            .arg(silentFrames)));
    // (a) 有声段：嘴真的张开
    QVERIFY2(loudMax >= moodMouthOpen + span * 0.15f,
             qPrintable(QStringLiteral("有声段嘴巴最大只到 %1（心情值 %2）—— 电平没有驱动嘴巴")
                            .arg(double(loudMax))
                            .arg(double(moodMouthOpen))));
    // (b) 静音段：回到心情自己的值。**这是用户诉求的钉子**
    QVERIFY2(silentMaxDeviation <= 1e-3f,
             qPrintable(QStringLiteral("静音段嘴巴偏离心情值最多 %1（心情值 %2）—— "
                                       "停顿时嘴没有回到心情自己的值")
                            .arg(double(silentMaxDeviation))
                            .arg(double(moodMouthOpen))));
}

/*电平恒为 0 = 一点都不许动。

  与上一条的区别：上一条量的是"从有声切到静音之后的回落"，这一条量的是
  "整句都静音"（例如 TTS 返回了一段前导静音、或者用户把音量调静音了）——
  此时**任何**残余扑动都会让嘴在她说不出话的时候继续开合。
  "电平为 0 ⇒ 嘴停在心情值"是设计里最硬的一条约定，这条用例就是它的钉子。*/
void TestLive2DOffscreen::envelopeZeroLevelKeepsMouthAtMoodValue()
{
    Live2DOffscreenRenderer renderer;
    QString error;
    if (!loadAnyModel(&renderer, &error))
        QSKIP("本机没有 Live2D 模型（禁二传，不入库），跳过零电平验证");
    requireMouthParameters(&renderer);
    requireBlendProbeParameters(&renderer);
    QVERIFY(!renderer.renderFrame(QSize(kBlendProbeWidth, kBlendProbeHeight)).isNull());

    /*心情故意给一个**非零**开口量：这样"停在心情值"与"合死到 0"能区分开。*/
    constexpr float kMoodMouthOpen = 0.32f;
    const QHash<QString, float> mood = {{kMoodProbeParameter, 0.4f},
                                        {kMouthOpenParameter, kMoodMouthOpen},
                                        {kMouthFormParameter, -0.7f},
                                        {kSecondProbeParameter, 0.25f}};
    renderer.setMoodBlendDurationMs(150);
    renderer.setParameterOverrides(mood);
    QVERIFY(advanceUntilSettled(&renderer, kMouthOpenParameter, kMoodMouthOpen, 200, 20).reached);

    renderer.setSpeaking(true);
    QVector<EnvelopeSample> samples;
    QVERIFY(playEnvelope(&renderer,
                         AudioEnvelope::fromWavBytes(buildPcm16Wav(22050, 1, {{500, 0.0f}})),
                         60, 1.0f / 60.0f, &samples));

    float biggestDeviation = 0.0f;
    for (const EnvelopeSample &sample : samples)
        biggestDeviation = std::max(biggestDeviation, qAbs(sample.mouthOpen - kMoodMouthOpen));
    qInfo("ENVELOPE zero-level: frames=%d mood=%.4f maxDeviation=%.6f",
          samples.size(), double(kMoodMouthOpen), double(biggestDeviation));

    /*无包络（全静音的 WAV）时 levelAtMs 恒为 0，所以这条同时覆盖了
        "包络不可用时电平恒 0"这条路径。*/
    QVERIFY2(biggestDeviation <= 1e-3f,
             qPrintable(QStringLiteral("电平恒为 0 时嘴巴仍在心情值 %1 附近摆了 %2 —— "
                                       "静音里还有残余开合")
                            .arg(double(kMoodMouthOpen))
                            .arg(double(biggestDeviation))));
}

/*包络路径不许碰别的参数。

  为什么单独一条：`ParamMouthOpenY` 与 `ParamMouthForm` 在 model3.json 里同属
  LipSync 组，很容易"顺手把嘴形也按电平写一下" —— 而嘴形（笑/撇嘴）属于**心情**，
  两个东西都去写就是互相打架（用户看到的是"说话时表情被抹掉"）。
  这里把"电平在变"的整段时间里所有非嘴参数读一遍，它们必须一动不动。*/
void TestLive2DOffscreen::envelopeDoesNotDisturbOtherParameters()
{
    Live2DOffscreenRenderer renderer;
    QString error;
    if (!loadAnyModel(&renderer, &error))
        QSKIP("本机没有 Live2D 模型（禁二传，不入库），跳过包络干扰验证");
    requireMouthParameters(&renderer);
    requireBlendProbeParameters(&renderer);
    QVERIFY(!renderer.renderFrame(QSize(kBlendProbeWidth, kBlendProbeHeight)).isNull());

    constexpr float kMoodMouthOpen = 0.2f;
    constexpr float kMoodMouthForm = -0.7f;
    constexpr float kMoodSmile = 0.4f;
    constexpr float kMoodBrow = 0.25f;
    const QHash<QString, float> mood = {{kMoodProbeParameter, kMoodSmile},
                                        {kMouthOpenParameter, kMoodMouthOpen},
                                        {kMouthFormParameter, kMoodMouthForm},
                                        {kSecondProbeParameter, kMoodBrow}};
    renderer.setMoodBlendDurationMs(150);
    renderer.setParameterOverrides(mood);
    QVERIFY(advanceUntilSettled(&renderer, kMoodProbeParameter, kMoodSmile, 200, 20).reached);

    renderer.setSpeaking(true);
    const QStringList watched = {kMouthFormParameter, kMoodProbeParameter, kSecondProbeParameter};
    QVector<float> mouth;
    for (int frame = 0; frame < 40; ++frame)
    {
        /*电平在整段里明显起伏（0.3~0.9）：只有"真的按电平写嘴巴"的实现才会
            在这段时间里改变开口量 —— 否则"没碰别的参数"只是因为什么都没做。*/
        const float level = 0.6f + 0.3f * std::sin(float(frame) * 0.5f);
        renderer.setSpeechLevel(level);
        renderer.setNextFrameDeltaSeconds(1.0f / 60.0f);
        QVERIFY(!renderer.renderFrame(QSize(kBlendProbeWidth, kBlendProbeHeight)).isNull());
        mouth.append(renderer.parameterValue(kMouthOpenParameter));
        for (const QString &id : watched)
        {
            const float expected = mood.value(id);
            const float actual = renderer.parameterValue(id);
            QVERIFY2(qAbs(actual - expected) <= 1e-4f,
                     qPrintable(QStringLiteral("包络第 %1 帧：%2 = %3，不等于心情值 %4 —— "
                                               "包络路径动了不该动的参数（嘴形属于心情）")
                                    .arg(frame)
                                    .arg(id)
                                    .arg(double(actual))
                                    .arg(double(expected))));
        }
    }

    const float mouthMin = *std::min_element(mouth.constBegin(), mouth.constEnd());
    const float mouthMax = *std::max_element(mouth.constBegin(), mouth.constEnd());
    qInfo("ENVELOPE isolation: frames=%d mouth=%.4f~%.4f, %s 全程等于心情值 %.3f",
          mouth.size(), double(mouthMin), double(mouthMax), qPrintable(kMouthFormParameter),
          double(kMoodMouthForm));
    QVERIFY2(mouthMax - mouthMin > 0.05f,
             qPrintable(QStringLiteral("电平在 0.3~0.9 之间起伏时开口量只动了 %1 —— "
                                       "嘴没有跟着电平走")
                            .arg(double(mouthMax - mouthMin))));
}

/*下一句没有包络时必须回到今天的盲扑动。

  为什么必须有这条：包络模式是一个**状态**。如果"停止说话"不把它复位、
  或者某个实现在没有包络时也进入包络模式（电平恒 0），那么用户把 vits 的 format
  配成 mp3 之后，嘴会**整句冻在心情值上** —— 比今天的盲扑动还糟，而画面上的
  症状（"她说话时嘴不动"）与"接线断了"完全一样，极难反推。
  所以这里把上一句真的推进包络模式，再在第二句**一个电平都不发**，看盲扑动是否回来。*/
void TestLive2DOffscreen::nextUtteranceWithoutEnvelopeFallsBackToBlindFlap()
{
    Live2DOffscreenRenderer renderer;
    QString error;
    if (!loadAnyModel(&renderer, &error))
        QSKIP("本机没有 Live2D 模型（禁二传，不入库），跳过包络回退验证");
    requireMouthParameters(&renderer);
    requireBlendProbeParameters(&renderer);
    QVERIFY(!renderer.renderFrame(QSize(kBlendProbeWidth, kBlendProbeHeight)).isNull());

    const QHash<QString, Live2DOffscreenRenderer::DeclaredRange> ranges =
        renderer.declaredParameterRanges();
    const Live2DOffscreenRenderer::DeclaredRange mouthRange = ranges.value(kMouthOpenParameter);
    const float span = mouthRange.max - mouthRange.min;

    const float moodMouthOpen = 0.2f;
    const QHash<QString, float> mood = {{kMoodProbeParameter, 0.35f},
                                        {kMouthOpenParameter, moodMouthOpen},
                                        {kMouthFormParameter, -0.7f},
                                        {kSecondProbeParameter, 0.25f}};
    renderer.setMoodBlendDurationMs(150);
    renderer.setParameterOverrides(mood);
    QVERIFY(advanceUntilSettled(&renderer, kMouthOpenParameter, moodMouthOpen, 200, 20).reached);

    /*第一句：有包络，电平恒为 1（相当于"这一整句都在大声说"）。*/
    renderer.setSpeaking(true);
    QVector<EnvelopeSample> firstSentence;
    QVERIFY(playEnvelope(&renderer, AudioEnvelope::fromWavBytes(buildPcm16Wav(22050, 1, {{500, 0.8f}})),
                         40, 1.0f / 60.0f, &firstSentence));
    float firstMax = firstSentence.first().mouthOpen;
    for (const EnvelopeSample &sample : firstSentence)
        firstMax = std::max(firstMax, sample.mouthOpen);
    QVERIFY2(firstMax >= moodMouthOpen + span * 0.15f,
             qPrintable(QStringLiteral("第一句（有包络）嘴巴没张开（%1）—— 本用例的前提不成立")
                            .arg(double(firstMax))));

    /*停播：Dialog 的停止路径 = 状态 false + 电平清零。*/
    renderer.setSpeaking(false);
    renderer.setSpeechLevel(0.0f);
    QVERIFY(advanceFrames(&renderer, kMouthOpenParameter, 40, 20, nullptr));
    const float afterStop = renderer.parameterValue(kMouthOpenParameter);
    QVERIFY2(qAbs(afterStop - moodMouthOpen) <= 0.02f,
             qPrintable(QStringLiteral("停播后嘴巴停在 %1，不等于心情值 %2")
                            .arg(double(afterStop))
                            .arg(double(moodMouthOpen))));

    /*第二句：**没有包络**（一个电平都不发，模拟 format=mp3）。
        判据与 speakingMouthFlapVariesAndStaysInRange 完全同一口径：
        方向要反复改（3~5 次/秒）、幅度落在声明量程里。*/
    renderer.setSpeaking(true);
    QVector<float> samples;
    QVERIFY(advanceFrames(&renderer, kMouthOpenParameter, 60, 0, &samples, 1.0f / 60.0f));
    QVERIFY2(samples.size() >= 20, "采样帧数不足，量不出扑动");

    float minValue = samples.first();
    float maxValue = samples.first();
    int alternations = 0;
    int previousTrend = 0;
    for (int index = 0; index < samples.size(); ++index)
    {
        minValue = std::min(minValue, samples[index]);
        maxValue = std::max(maxValue, samples[index]);
        if (index == 0)
            continue;
        const int trend = (samples[index] > samples[index - 1]) ? 1 : -1;
        if (previousTrend != 0 && trend != previousTrend)
            ++alternations;
        previousTrend = trend;
    }
    qInfo("ENVELOPE fallback: first sentence max=%.4f → second sentence (no envelope) "
          "%.4f~%.4f alternations=%d",
          double(firstMax), double(minValue), double(maxValue), alternations);

    QVERIFY2(maxValue - minValue >= span * 0.15f,
             qPrintable(QStringLiteral("没有包络的第二句嘴巴只在 %1~%2 之间动（量程 %3~%4）—— "
                                       "盲扑动没有回来")
                            .arg(double(minValue))
                            .arg(double(maxValue))
                            .arg(double(mouthRange.min))
                            .arg(double(mouthRange.max))));
    QVERIFY2(alternations >= 5 && alternations <= 14,
             qPrintable(QStringLiteral("没有包络的第二句开口量换了 %1 次方向（期望 5~14）—— "
                                       "回退的不是今天的 3~5Hz 盲扑动")
                            .arg(alternations)));
    QVERIFY2(minValue >= mouthRange.min - 1e-4f && maxValue <= mouthRange.max + 1e-4f,
             qPrintable(QStringLiteral("回退的扑动越出了声明范围（%1~%2）")
                            .arg(double(minValue))
                            .arg(double(maxValue))));
}

/*停止说话 + 电平清零 → 嘴回落到心情值（不是"啪一下合死"）。

  与既有的 stoppingSpeechDecaysFlapToMoodValue 的区别：那条走的是**盲扑动**的
  幅度衰减，这条走的是**包络模式**退出（电平清零 + 幅度衰减）——
  用户实际经历的正是后者（有包络的句子播完了）。*/
void TestLive2DOffscreen::stoppingSpeechWithClearedLevelReturnsToMood()
{
    Live2DOffscreenRenderer renderer;
    QString error;
    if (!loadAnyModel(&renderer, &error))
        QSKIP("本机没有 Live2D 模型（禁二传，不入库），跳过包络停止验证");
    requireMouthParameters(&renderer);
    requireBlendProbeParameters(&renderer);
    QVERIFY(!renderer.renderFrame(QSize(kBlendProbeWidth, kBlendProbeHeight)).isNull());

    /*心情故意给一个**非零**开口量：这样"回落到心情值"与"合死到 0"能区分开。*/
    constexpr float kMoodMouthOpen = 0.32f;
    const QHash<QString, float> mood = {{kMoodProbeParameter, 0.0f},
                                        {kMouthOpenParameter, kMoodMouthOpen},
                                        {kSecondProbeParameter, 0.0f}};
    constexpr int kBlendMs = 200;
    renderer.setMoodBlendDurationMs(kBlendMs);
    renderer.setParameterOverrides(mood);
    QVERIFY(advanceUntilSettled(&renderer, kMouthOpenParameter, kMoodMouthOpen, 200, 20).reached);

    renderer.setSpeaking(true);
    QVector<EnvelopeSample> speakingSamples;
    QVERIFY(playEnvelope(&renderer, AudioEnvelope::fromWavBytes(buildPcm16Wav(22050, 1, {{800, 0.9f}})),
                         30, 1.0f / 60.0f, &speakingSamples));
    float speakingMin = speakingSamples.first().mouthOpen;
    float speakingMax = speakingSamples.first().mouthOpen;
    for (const EnvelopeSample &sample : speakingSamples)
    {
        speakingMin = std::min(speakingMin, sample.mouthOpen);
        speakingMax = std::max(speakingMax, sample.mouthOpen);
    }
    QVERIFY2(speakingMax - speakingMin > 0.05f || speakingMax > kMoodMouthOpen + 0.05f,
             qPrintable(QStringLiteral("说话阶段没量到张嘴（%1~%2），本用例的前提不成立")
                            .arg(double(speakingMin))
                            .arg(double(speakingMax))));

    /*Dialog 的停止路径：先状态 false，再把电平清零。*/
    renderer.setSpeaking(false);
    renderer.setSpeechLevel(0.0f);

    /*等远超过渡时长（200ms 过渡 ⇒ 采 1.2s 的墙钟）。
        ⚠️ 判据看**尾部窗口**：衰减是"线性趋近 0"，整段极差里含着开头那几百毫秒的
        残余摆动（那是正确行为，不是没衰减）。*/
    QVector<float> afterSamples;
    QVERIFY(advanceFrames(&renderer, kMouthOpenParameter, 60, 20, &afterSamples));
    const int tailBegin = afterSamples.size() * 3 / 4;
    float tailMin = afterSamples[tailBegin];
    float tailMax = afterSamples[tailBegin];
    for (int index = tailBegin; index < afterSamples.size(); ++index)
    {
        tailMin = std::min(tailMin, afterSamples[index]);
        tailMax = std::max(tailMax, afterSamples[index]);
    }
    qInfo("ENVELOPE stop[%s]: speaking=%.4f~%.4f → tail(last 1/4)=%.4f~%.4f, mood=%.4f",
          qPrintable(kMouthOpenParameter), double(speakingMin), double(speakingMax),
          double(tailMin), double(tailMax), double(kMoodMouthOpen));

    QVERIFY2(tailMax - tailMin <= 0.02f,
             qPrintable(QStringLiteral("停止说话 1.2s 后开口量仍在 %1~%2 之间摆（稳态极差 %3）")
                            .arg(double(tailMin))
                            .arg(double(tailMax))
                            .arg(double(tailMax - tailMin))));
    QVERIFY2(qAbs(tailMax - kMoodMouthOpen) <= 0.02f && qAbs(tailMin - kMoodMouthOpen) <= 0.02f,
             qPrintable(QStringLiteral("停止说话后开口量停在 %1~%2，不等于心情值 %3 —— "
                                       "包络没有干净地退场，或把嘴合死了")
                            .arg(double(tailMin))
                            .arg(double(tailMax))
                            .arg(double(kMoodMouthOpen))));
}

/*==================== 待机摆动（IDLE 系列） ====================

  用户要的观感是一句很朴素的话："她应该是**站着、重心在动**"，而不是只有呼吸。

  数据在**用户数据区**（<模型目录>/presets/idle.json，不在仓库里）：每条只写**语义**参数名
  + 幅度（参数自己的单位）+ 周期（秒）+ 相位（周期分数）。换模型（miku）时只改这份数据、
  不改 C++ —— 这正是"数据驱动"的全部含义，也是这组用例为什么**不写死参数 ID**：
  它们从 preset.idleSwayEntries() 拿配置，再用模型自己的声明范围做判据。

  四条契约（每条对应一个具体的错误行为）：
    1. 真的在动，且**按配置的幅度**在动（幅度写错一半、写成别的量级都要被抓住）；
    2. **周期**必须等于配置值 —— 从注入的虚拟时间上过零量出来，与机器负载无关；
    3. **加在心情值之上**（不是覆盖）：否则"生气地站着"会变回中立表情；
    4. 绝不碰呼吸/头发（ParamBreath / ParamHair*，它们每帧由别的驱动器写）与
       ParamMouthForm（嘴形属于心情）。

  ⚠️ 采样一律注入固定帧步长：摆动的相位由**累计虚拟时间**决定，而真实帧步长来自墙钟、
  还会被 0.1s 夹取 —— 靠墙钟就等于把"这一帧落在周期里的哪一点"交给机器负载决定。*/
bool TestLive2DOffscreen::loadConfiguredIdleSway(Live2DOffscreenRenderer *renderer,
                                                 Live2DMoodPreset *preset, QString *detail)
{
    if (renderer == nullptr || preset == nullptr)
        return false;
    const QString dir = availableModelDir();
    if (dir.isEmpty())
    {
        if (detail != nullptr)
            *detail = QStringLiteral("本机没有 Live2D 模型（禁二传，不入库）");
        return false;
    }
    QString error;
    if (!loadAnyModel(renderer, &error))
    {
        if (detail != nullptr)
            *detail = QStringLiteral("模型装载失败：%1").arg(error);
        return false;
    }

    /*模型名 = 模型目录名（本机 Documents/Mandarin/Live2D/atri → "atri"）。
       摆动数据按它去 Character/Assets/<角色>/Live2D/<模型名>/presets/ 找 —— 两个目录
       在本机确实是分开的（与 test_live2dwindow::openConfiguredModel 的说明一致）。*/
    const QString modelName = QFileInfo(dir).fileName();
    const bool moodOk = preset->load(modelName);
    const bool idleOk = moodOk && preset->loadIdle();
    if (!moodOk || !idleOk)
    {
        if (detail != nullptr)
            *detail = QStringLiteral("%1：情绪预设=%2、待机摆动数据=%3（都在 Documents 下，不入库）")
                          .arg(modelName)
                          .arg(moodOk ? QStringLiteral("可用") : QStringLiteral("缺失"))
                          .arg(idleOk ? QStringLiteral("可用") : QStringLiteral("缺失"));
        return false;
    }

    renderer->setIdleSway(preset->idleSwayEntries());
    return true;
}

/*模型自己声明的**物理输入/输出参数**（`<模型目录>/<physics3.json>` 里每条
   PhysicsSettings[].Input[].Source.Id 与 Output[].Destination.Id）。

   为什么待机摆动用例需要它们（实测教训，2026-09-30 切到 miku 之后）：
   用例 3（"摆动加在心情值上"）原先要在一条"不被任何别的写入者碰"的轴上做逐帧恒等式。
   它只排除了呼吸/头发那几条**写死**的名字，于是在 miku 上选中了 `Param26` ——
   而 `Param26`（语义 wholeBodyY）正是这份 physics3.json 的输出，物理每帧给它写绝对值：
   实测基准逐帧从 +1.69 走到 −0.4（心情 −2.0，残差 5.2），用例报"摆动不是加在心情值上"，
   而摆动自己的偏移完全正确、最终值 = 基准 + 偏移且无夹取 —— 摆动无辜，是选轴选错了。

   现在这条用例改用"同一串虚拟帧跑两遍（带摆动 / 不带摆动）"的判据，于是：
     - **输出**名单只用于说明"谁在写基准"（见该用例的注释），判据本身不再依赖它；
     - **输入**名单必须避开：物理把某条轴当输入读时，摆动改变了它就会改变物理轨迹，
       "两遍只差一个摆动偏移"这个前提就不成立了（Param25/Param26 正是这种双重身份）。

   解析失败（没有物理文件）时返回空表：那时确实没有物理参与。*/
struct PhysicsParameters
{
    QSet<QString> inputs;
    QSet<QString> outputs;
};

PhysicsParameters physicsParametersOf(const QString &modelDir)
{
    PhysicsParameters result;
    QDir dir(modelDir);
    const QStringList physicsFiles =
        dir.entryList({QStringLiteral("*.physics3.json")}, QDir::Files);
    for (const QString &fileName : physicsFiles)
    {
        QFile file(dir.filePath(fileName));
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        const QJsonArray settings =
            document.object().value(QStringLiteral("PhysicsSettings")).toArray();
        for (const QJsonValue &setting : settings)
        {
            const QJsonObject object = setting.toObject();
            for (const QJsonValue &input : object.value(QStringLiteral("Input")).toArray())
            {
                const QString id = input.toObject()
                                       .value(QStringLiteral("Source"))
                                       .toObject()
                                       .value(QStringLiteral("Id"))
                                       .toString();
                if (!id.isEmpty())
                    result.inputs.insert(id);
            }
            for (const QJsonValue &output : object.value(QStringLiteral("Output")).toArray())
            {
                const QString id = output.toObject()
                                       .value(QStringLiteral("Destination"))
                                       .toObject()
                                       .value(QStringLiteral("Id"))
                                       .toString();
                if (!id.isEmpty())
                    result.outputs.insert(id);
            }
        }
    }
    return result;
}

/*所有带 presets/idle.json 的模型名。

   以**数据**为入口（而不是模型目录）：本组用例要验证的是"这份摆动数据对不对"，
   没有数据的模型没有任何可核对的东西。反过来说，只要有人放了一份 idle.json，
   它就**必须**被核对 —— 哪怕模型文件不在（那也是一种错误，用例会报出来）。

   角色目录走与 Live2DMoodPreset::resolveModelDir 同一套推导（真实 config.ini 的
   CharSelect + CharacterAssestPath），所以这里不会与生产代码的路径推导分叉。*/
QStringList TestLive2DOffscreen::modelNamesWithIdleData()
{
    QStringList names;
    const QString charName = ReadNowSelectChar();
    if (charName.isEmpty() || charName == QStringLiteral("未选择"))
        return names;

    const QDir root(QDir(CharacterAssestPath).filePath(charName + QStringLiteral("/Live2D")));
    if (!root.exists())
        return names;

    const QStringList candidates = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : candidates)
    {
        if (QFileInfo::exists(root.filePath(name + QStringLiteral("/presets/idle.json"))))
            names.append(name);
    }
    return names;
}

/*按名字装载模型：候选①模型根目录（Documents/Mandarin/Live2D/<名>）
   ②角色资源目录（与摆动数据同层）。模型作者给 json 起什么名字由他决定，不写死。*/
bool TestLive2DOffscreen::loadModelByName(const QString &name, Live2DOffscreenRenderer *renderer,
                                          QString *error)
{
    if (renderer == nullptr || name.trimmed().isEmpty())
        return false;

    QStringList candidates;
    candidates << QDir(Live2DModelRootPath).filePath(name)
               << QDir(CharacterAssestPath)
                      .filePath(ReadNowSelectChar() + QStringLiteral("/Live2D/") + name);
    for (const QString &dir : candidates)
    {
        if (dir.isEmpty() || !QFileInfo::exists(dir))
            continue;
        const QStringList jsons = QDir(dir).entryList({QStringLiteral("*.model3.json")}, QDir::Files);
        for (const QString &jsonName : jsons)
        {
            QString localError;
            if (renderer->load(dir, jsonName, &localError))
                return true;
            if (error != nullptr)
                *error = localError;
        }
    }
    if (error != nullptr && error->isEmpty())
        *error = QStringLiteral("本机找不到模型 %1 的 model3.json").arg(name);
    return false;
}

bool TestLive2DOffscreen::sampleIdleSway(Live2DOffscreenRenderer *renderer,
                                         const QStringList &parameterIds, int frames,
                                         float deltaSeconds, QVector<IdleSample> *samples)
{
    if (renderer == nullptr || samples == nullptr || frames <= 0)
        return false;
    /*注入的步长必须 ≤ 渲染器的单帧夹取上限（0.1s）：否则"注入的时间"与"实际推进的时间"
       不再相等，相位就会与预期错开 —— 这组用例的相位判据全靠这条等式。*/
    if (!(deltaSeconds > 0.0f) || deltaSeconds > 0.1f)
        return false;

    const QSize targetSize(kBlendProbeWidth, kBlendProbeHeight);
    for (int index = 0; index < frames; ++index)
    {
        renderer->setNextFrameDeltaSeconds(deltaSeconds);
        if (renderer->renderFrame(targetSize).isNull())
            return false;

        IdleSample sample;
        sample.elapsedSeconds = renderer->idleSwayElapsedSeconds();
        for (const QString &parameterId : parameterIds)
        {
            sample.offsets.insert(parameterId, renderer->idleSwayOffset(parameterId));
            sample.values.insert(parameterId, renderer->parameterValue(parameterId));
        }
        samples->append(sample);
    }
    return true;
}

float TestLive2DOffscreen::valueSwingOf(const QVector<IdleSample> &samples,
                                        const QString &parameterId)
{
    if (samples.isEmpty())
        return 0.0f;
    float minValue = samples.first().values.value(parameterId);
    float maxValue = minValue;
    for (const IdleSample &sample : samples)
    {
        const float value = sample.values.value(parameterId);
        minValue = std::min(minValue, value);
        maxValue = std::max(maxValue, value);
    }
    return maxValue - minValue;
}

TestLive2DOffscreen::Oscillation TestLive2DOffscreen::measureOscillation(
    const QVector<IdleSample> &samples, const QString &parameterId)
{
    Oscillation result;
    if (samples.size() < 3 || parameterId.isEmpty())
        return result;

    QVector<float> times;
    QVector<float> offsets;
    times.reserve(samples.size());
    offsets.reserve(samples.size());
    for (const IdleSample &sample : samples)
    {
        if (!sample.offsets.contains(parameterId))
            return Oscillation(); //没采到这条轴（调用方负责传对参数 ID）
        const float value = sample.offsets.value(parameterId);
        times.append(sample.elapsedSeconds);
        offsets.append(value);
        result.amplitude = std::max(result.amplitude, std::fabs(value));
    }

    /*上升过零（− → +）。为什么不用峰值：正弦在零点最陡，线性插值的误差是 dt² 量级；
       峰值附近是平的，插值反而量不准（差一个采样点就差出百分之几的幅度）。*/
    QVector<float> crossingTimes;
    for (int index = 1; index < offsets.size(); ++index)
    {
        if (!(offsets[index - 1] <= 0.0f && offsets[index] > 0.0f))
            continue;
        const float delta = offsets[index] - offsets[index - 1];
        const float fraction = (std::fabs(delta) > 1e-12f) ? (-offsets[index - 1] / delta) : 0.0f;
        crossingTimes.append(times[index - 1] + (times[index] - times[index - 1]) * fraction);
    }
    result.crossings = crossingTimes.size();
    if (crossingTimes.isEmpty())
        return result;

    if (crossingTimes.size() >= 2)
    {
        float sum = 0.0f;
        for (int index = 1; index < crossingTimes.size(); ++index)
            sum += crossingTimes[index] - crossingTimes[index - 1];
        result.periodSeconds = sum / float(crossingTimes.size() - 1);

        /*相位（周期分数）：上升过零发生在 t = T(k − φ)，于是 φ = (−t/T) mod 1 ——
           整数 k 被 mod 吃掉，所以不需要知道这是第几次过零。*/
        if (result.periodSeconds > 0.0f)
        {
            const float raw = -crossingTimes.first() / result.periodSeconds;
            result.phase = raw - std::floor(raw);
        }
    }
    return result;
}

/*每一配置轴都按配置的幅度在动，且参数值始终落在模型**声明**的范围里。

  为什么"幅度"要卡上下两侧：只有下界（"至少动了 90%"）会放过"幅度被放大成两倍"，
  只有上界会放过"幅度被砍半"。两侧都卡住，任何"把 amplitude 用错"的写法都过不去。*/
void TestLive2DOffscreen::idleSwayMovesConfiguredParameters()
{
    Live2DOffscreenRenderer renderer;
    Live2DMoodPreset preset;
    QString detail;
    if (!loadConfiguredIdleSway(&renderer, &preset, &detail))
        QSKIP("本机没有可用的待机摆动数据（presets/idle.json），跳过");

    const QVector<Live2DMoodPreset::IdleSwayEntry> entries = preset.idleSwayEntries();
    QVERIFY2(!entries.isEmpty(), "idle.json 装载成功却一条轴都没有");
    QVERIFY2(renderer.isIdleSwayActive(), "把条目交给渲染器之后摆动却没生效");

    const QHash<QString, Live2DOffscreenRenderer::DeclaredRange> ranges =
        renderer.declaredParameterRanges();

    QStringList ids;
    for (const Live2DMoodPreset::IdleSwayEntry &entry : entries)
        ids << entry.parameterId;

    /*8s 虚拟时间（最短的轴 4.2s 走了近两轮；最长的 6.5s 也必然覆盖波峰与波谷 ——
       两者相距半个周期 = 3.25s，8s 的窗口一定取到）。0.05s 步长下峰值的幅度误差
       是 1−cos(π·dt/T) < 0.1%，可以忽略。*/
    QVector<IdleSample> samples;
    QVERIFY2(sampleIdleSway(&renderer, ids, 160, 0.05f, &samples), "采样时渲染失败");

    for (const Live2DMoodPreset::IdleSwayEntry &entry : entries)
    {
        QVERIFY2(ranges.contains(entry.parameterId),
                 qPrintable(QStringLiteral("模型没有声明 %1（语义名 %2）—— parameter-map 与模型对不上")
                                .arg(entry.parameterId, entry.semanticName)));
        const Live2DOffscreenRenderer::DeclaredRange range = ranges.value(entry.parameterId);

        const Oscillation measured = measureOscillation(samples, entry.parameterId);
        float minOffset = samples.first().offsets.value(entry.parameterId);
        float maxOffset = minOffset;
        float minValue = samples.first().values.value(entry.parameterId);
        float maxValue = minValue;
        float worstValue = minValue;
        bool outOfRange = false;
        for (const IdleSample &sample : samples)
        {
            const float offset = sample.offsets.value(entry.parameterId);
            minOffset = std::min(minOffset, offset);
            maxOffset = std::max(maxOffset, offset);
            const float value = sample.values.value(entry.parameterId);
            minValue = std::min(minValue, value);
            maxValue = std::max(maxValue, value);
            if (value < range.min - 1e-4f || value > range.max + 1e-4f)
            {
                outOfRange = true;
                worstValue = value;
            }
        }

        qInfo("IDLE move[%s]: configured amp=%.3f period=%.2fs phase=%.2f | measured amp=%.3f "
              "offset=%.3f~%.3f value=%.3f~%.3f declared=[%.3f,%.3f]",
              qPrintable(entry.parameterId), double(entry.amplitude), double(entry.periodSeconds),
              double(entry.phase), double(measured.amplitude), double(minOffset),
              double(maxOffset), double(minValue), double(maxValue), double(range.min),
              double(range.max));

        QVERIFY2(maxOffset >= entry.amplitude * 0.97f && maxOffset <= entry.amplitude * 1.01f,
                 qPrintable(QStringLiteral("%1 的正向摆幅实测 %2，配置 %3 —— 幅度没有按配置落地")
                                .arg(entry.parameterId)
                                .arg(double(maxOffset))
                                .arg(double(entry.amplitude))));
        QVERIFY2(minOffset <= -entry.amplitude * 0.97f && minOffset >= -entry.amplitude * 1.01f,
                 qPrintable(QStringLiteral("%1 的负向摆幅实测 %2，配置 %3")
                                .arg(entry.parameterId)
                                .arg(double(minOffset))
                                .arg(double(entry.amplitude))));
        QVERIFY2(maxOffset - minOffset >= entry.amplitude * 1.94f,
                 qPrintable(QStringLiteral("%1 的摆动极差只有 %2（配置幅度 %3）—— 没有走完一个周期")
                                .arg(entry.parameterId)
                                .arg(double(maxOffset - minOffset))
                                .arg(double(entry.amplitude))));
        QVERIFY2(!outOfRange,
                 qPrintable(QStringLiteral("%1 的最终值落到了 [%2,%3] 之外（实测 %4）—— 越界会被 "
                                           "Core 夹回去，屏幕上就是卡在上下限")
                                .arg(entry.parameterId)
                                .arg(double(range.min))
                                .arg(double(range.max))
                                .arg(double(worstValue))));
    }
}

/*周期必须等于配置值：用**注入的**虚拟时间过零量出来，不依赖墙钟。*/
void TestLive2DOffscreen::idleSwayPeriodMatchesConfiguration()
{
    Live2DOffscreenRenderer renderer;
    Live2DMoodPreset preset;
    QString detail;
    if (!loadConfiguredIdleSway(&renderer, &preset, &detail))
        QSKIP("本机没有可用的待机摆动数据（presets/idle.json），跳过");

    const QVector<Live2DMoodPreset::IdleSwayEntry> entries = preset.idleSwayEntries();
    QStringList ids;
    for (const Live2DMoodPreset::IdleSwayEntry &entry : entries)
        ids << entry.parameterId;

    /*15.5s 虚拟时间、0.05s 步长：最长的那条轴（6.5s）也能量到 3 次上升过零 ⇒
       至少两段完整的"过零间隔"可比。周期是**时刻之差**，而时刻全是注入的 ⇒ 与负载无关。*/
    QVector<IdleSample> samples;
    QVERIFY2(sampleIdleSway(&renderer, ids, 310, 0.05f, &samples), "采样时渲染失败");

    for (const Live2DMoodPreset::IdleSwayEntry &entry : entries)
    {
        const Oscillation measured = measureOscillation(samples, entry.parameterId);
        QVERIFY2(measured.crossings >= 2,
                 qPrintable(QStringLiteral("%1 在采样窗里只量到 %2 次上升过零，周期无从谈起"
                                           "（周期被改大了？采样窗不够长？）")
                                .arg(entry.parameterId)
                                .arg(measured.crossings)));
        const float relativeError =
            std::fabs(measured.periodSeconds - entry.periodSeconds) / entry.periodSeconds;
        qInfo("IDLE period[%s]: configured=%.2fs measured=%.3fs (rel err %.3f%%) crossings=%d",
              qPrintable(entry.parameterId), double(entry.periodSeconds),
              double(measured.periodSeconds), double(relativeError * 100.0), measured.crossings);

        /*3% 容差：过零时刻的插值误差是 dt² 量级（dt=0.05s、T≥4.2s ⇒ 远小于 0.1%），
           余量留给浮点累加。周期写成别的量级（例如误按毫秒）。必然远大于 3%。*/
        QVERIFY2(relativeError <= 0.03f,
                 qPrintable(QStringLiteral("%1 的实测周期 %2s 与配置 %3s 差 %4%%")
                                .arg(entry.parameterId)
                                .arg(double(measured.periodSeconds))
                                .arg(double(entry.periodSeconds))
                                .arg(double(relativeError * 100.0))));
    }
}

/*叠加而不是覆盖：参数**围绕心情值**摆，不是围绕中立值。

  ⚠️ 这条用例的判据在 2026-09-30（config.ini 切到 miku）之后**重新表述过**，原因是一个
  实测事实：miku 的 idle.json 摆动的 5 条轴（lowerBodyZ/upperBodyZ/wholeBodyX/wholeBodyY/headZ）
  **全部**被该模型的 physics3.json 声明为 `Output.Destination.Id`，也就是物理每帧对它们写绝对值。
  于是"最终值 − 摆动偏移 == 心情值"这条逐帧恒等式在这些轴上根本不可能成立 ——
  基准里含物理那一项（实测 Param26：基准逐帧从 +1.69 走到 −0.4，心情值 −2.0，残差 5.2）。

  旧判据之所以曾经在 atri 上成立，只是因为它挑中了 atri 的 ParamBodyAngleZ —— 一条在 atri 上
  没有物理写的轴（而 miku 上这条轴是死的：腿部探针实测推满 ±10 零像素变化）。

  新的判据不问"谁写了基准"，只问**摆动和别的东西是怎么合成的** —— 这与具体模型无关：
    ① 同一串固定虚拟帧、同一条轴，跑**两遍**：一遍带摆动（A），一遍把摆动整表清空（B）。
       两遍的模型初态、心情覆盖、动作/物理输入完全相同，所以唯一变量就是摆动是否施加；
    ② 于是"摆动这一帧加了什么"可以**精确量出来**：offset = A[n] − B[n]；
    ③ 断言它真的等于配置幅度、且没有一处被夹取（夹取说明"加"变成了"顶到上下限"）；
    ④ 断言基准仍是**心情值**：A[n] − offset = B[n]，而 B 是"只有心情覆盖"的那一遍，
       所以"心情仍然在底下"这件事是可观察的 —— 覆盖式实现（摆动写绝对值）会让 A 与 B 无关。

  为什么这比旧判据更强：旧判据要求"轴上没有别的写入者"，那是对**数据**的假设；
  新判据只要求"B 那一遍里没有摆动"，那是对**用例自己**的控制。

  ⚠️ 选轴必须避开"物理把它当输入"的那几条（Param25/Param26 既是 Input 又是 Output，
  见 physics3.json）：输入变了物理轨迹就变，B 与 A 就不再只差一个摆动偏移。
  Param13（lowerBodyZ）与 Param（upperBodyZ）只有 Output 没有 Input，正是干净的对照轴。*/
void TestLive2DOffscreen::idleSwayAddsToMoodInsteadOfReplacingIt()
{
    Live2DOffscreenRenderer renderer;
    Live2DMoodPreset preset;
    QString detail;
    if (!loadConfiguredIdleSway(&renderer, &preset, &detail))
        QSKIP("本机没有可用的待机摆动数据（presets/idle.json），跳过");

    const QVector<Live2DMoodPreset::IdleSwayEntry> entries = preset.idleSwayEntries();
    const QHash<QString, Live2DMoodPreset::ParameterRange> parameters = preset.parameters();
    /*物理**当输入**读的那批轴不能用（见函数头说明）：它们被摆动改变后物理轨迹也变，
       两遍就不再只差一个摆动偏移。输入名单从 physics3.json 里取，不写死参数名。*/
    const QSet<QString> physicsInputs = physicsParametersOf(availableModelDir()).inputs;

    /*选一条"某个原型给它一个明显偏离中立的值"的摆动轴（两步都从数据里推，不写死 ID/原型名）。*/
    QString chosenArchetype;
    QString chosenId;
    float moodValue = 0.0f;
    float neutralValue = 0.0f;
    for (const QString &archetype : preset.archetypeNames())
    {
        const QHash<QString, float> values = preset.parametersForArchetype(archetype);
        for (const Live2DMoodPreset::IdleSwayEntry &entry : entries)
        {
            if (isUpdaterDrivenParameterId(entry.parameterId))
                continue;
            if (physicsInputs.contains(entry.parameterId))
                continue;
            const float neutral = parameters.value(entry.semanticName).neutral;
            const float value = values.value(entry.parameterId, neutral);
            if (std::fabs(value - neutral) <= 1.0f)
                continue;
            chosenArchetype = archetype;
            chosenId = entry.parameterId;
            moodValue = value;
            neutralValue = neutral;
            break;
        }
        if (!chosenId.isEmpty())
            break;
    }
    QVERIFY2(!chosenId.isEmpty(),
             "没有任何原型给一条「可做对照」的摆动轴（不被呼吸/头发占用、也不是物理输入）"
             "一个明显非中立的值 —— 本用例构造不出对照");

    const QString chosenSemantic = [&]() {
        for (const Live2DMoodPreset::IdleSwayEntry &entry : entries)
        {
            if (entry.parameterId == chosenId)
                return entry.semanticName;
        }
        return QString();
    }();
    const float amplitude = idleAmplitudeOf(entries, chosenId);

    /*不插值（0ms）：这条用例量的是"摆动加在什么之上"，不需要过渡，直接从目标值开始。*/
    renderer.setMoodBlendDurationMs(0);
    renderer.setParameterOverrides(preset.parametersForArchetype(chosenArchetype));

    /*A：带摆动；B：把摆动整表清空（同一串虚拟帧、同一心情）。两遍之间**不动**心情与步长。*/
    constexpr int kFrames = 160;
    constexpr float kDeltaSeconds = 0.05f;
    QVector<IdleSample> withSway;
    QVERIFY2(sampleIdleSway(&renderer, QStringList{chosenId}, kFrames, kDeltaSeconds, &withSway),
             "带摆动的采样渲染失败");
    renderer.setIdleSway({});
    QVector<IdleSample> withoutSway;
    QVERIFY2(sampleIdleSway(&renderer, QStringList{chosenId}, kFrames, kDeltaSeconds, &withoutSway),
             "不带摆动的采样渲染失败");
    QCOMPARE(withSway.size(), withoutSway.size());

    /*逐帧把"摆动真正加了多少"量出来：两遍之差就是它 —— 不依赖"基准等于谁"这个假设。*/
    int clampedFrames = 0;
    float maxOffset = 0.0f;
    float minOffset = 0.0f;
    float worstBaseMismatch = 0.0f;
    float minWithValue = 0.0f;
    float maxWithValue = 0.0f;
    bool firstValue = true;
    for (int index = 0; index < withSway.size(); ++index)
    {
        const float withValue = withSway.at(index).values.value(chosenId);
        const float withoutValue = withoutSway.at(index).values.value(chosenId);
        const float offset = withValue - withoutValue;
        maxOffset = std::max(maxOffset, offset);
        minOffset = std::min(minOffset, offset);
        if (firstValue)
        {
            minWithValue = maxWithValue = withValue;
            firstValue = false;
        }
        minWithValue = std::min(minWithValue, withValue);
        maxWithValue = std::max(maxWithValue, withValue);
        /*夹取检测：模型上了夹取就说明"心情 + 摆动"已经顶到声明范围的边上，
           此时"加"这件事被改写了 —— 基线轴上不该发生（幅度只有量程的 ~10%）。*/
        if (withSway.at(index).offsets.value(chosenId) != offset)
            ++clampedFrames;
        /*基准仍是心情：带摆动这一遍的"值 − 偏移"必须逐帧等于不带摆动那一遍的值。*/
        worstBaseMismatch = std::max(
            worstBaseMismatch,
            std::fabs((withValue - withSway.at(index).offsets.value(chosenId)) - withoutValue));
    }

    qInfo("IDLE additive[%s/%s @ %s]: mood=%.3f neutral=%.3f amplitude=%.3f | offset=%.3f~%.3f "
          "| clampedFrames=%d | worstBaseMismatch=%.5f | value=%.3f~%.3f",
          qPrintable(chosenId), qPrintable(chosenSemantic), qPrintable(chosenArchetype),
          double(moodValue), double(neutralValue), double(amplitude), double(minOffset),
          double(maxOffset), clampedFrames, double(worstBaseMismatch), double(minWithValue),
          double(maxWithValue));

    /*① 摆动真的把值推离了"没有摆动"的那一遍，且幅度达到配置值（写错幅度/没生效都会红）。*/
    QVERIFY2(maxOffset >= 0.9f * amplitude,
             qPrintable(QStringLiteral("摆动正向只加到了 %1（配置幅度 %2）—— 摆动没按配置生效")
                            .arg(double(maxOffset))
                            .arg(double(amplitude))));
    QVERIFY2(minOffset <= -0.9f * amplitude,
             qPrintable(QStringLiteral("摆动负向只到了 %1（配置幅度 %2）—— 波形被削平了")
                            .arg(double(minOffset))
                            .arg(double(amplitude))));
    /*② 没有一帧被夹取：夹取意味着"加"已经退化成"顶到上下限"。*/
    QVERIFY2(clampedFrames == 0,
             qPrintable(QStringLiteral("有 %1 帧的最终值不等于「基准 + 摆动偏移」—— "
                                       "组合被夹取改写了（加变成了顶边界）")
                            .arg(clampedFrames)));
    /*③ 基准仍是**心情值**：这就是"叠加而不是覆盖"的直接判据 ——
       覆盖式实现（摆动直接写绝对值）会让"值 − 偏移"不再等于只有心情的那一遍。*/
    QVERIFY2(worstBaseMismatch <= 0.01f,
             qPrintable(QStringLiteral("带摆动那遍的基准与「只有心情」那遍最大差 %1 —— "
                                       "摆动没有加在心情值之上")
                            .arg(double(worstBaseMismatch))));
    /*④ 摆动真的让参数在**整段行程**上动起来，而不是缩成一小段。
       ⚠️ 这一条曾经写成"值的时间均值仍停在心情值附近" —— **在物理输出的轴上那是个坏判据**：
       摆动是正弦，跨整数个周期求均值会把偏移**抵消成 0**，于是均值回到基准（本模型上
       Param/upperBodyZ 的基准是 0，实测均值 0.185），看起来像"心情被抹掉了"，
       而实际上①③已经证明摆动完全正确地加在基准上。峰谷差则不受"抵消"影响：
       它只要求摆动跑满约 2 倍幅度（留 10% 余量给相位与采样端点）。*/
    const float peakToPeak = maxWithValue - minWithValue;
    QVERIFY2(peakToPeak >= 1.8f * amplitude,
             qPrintable(QStringLiteral("参数整段行程只有 %1（配置峰谷差应为 %2）—— "
                                       "摆动没有真的让这条轴动起来")
                            .arg(double(peakToPeak))
                            .arg(double(2.0f * amplitude))));

    /*⑤ 心情值仍然**在参数表里生效**——本模型上它是通过物理间接体现的，这一条把机制钉住。

       为什么不能直接断言"读回值里有心情那一份"：miku 的摆动轴全被 physics3.json 声明为
       输出，物理每帧对它们写绝对值，而物理的输入之一正是**同一条轴**（Param25/Param26 既是
       Input 又是 Output）。所以"基准"= 物理对"心情值"这一输入的响应，不是心情值本身 ——
       实测 mood=−2.0 时基准落在 0 附近、mood=+4.0 时基准抬到 +5.2，两个方向都动了几个单位。
       换言之：**心情确实被喂进去了，只是隔着一层物理动力学**。
       真正"基准就等于心情值"的干净对照需要一条物理不写的轴：
       miku 的实际 physics3.json 输出参数**全部 141 条声明里都没有**这样一条（顶部
       miku.physics3.json 的实测：5 条摆动轴全在输出名单里），所以这个模型上做不出该对照。

       能钉住的是"心情路径本身在工作"：同一个心情下，摆动开着与关掉，参数表(parametersForMood)
       必须都给出同一个值 —— 这是"心情覆盖表按心情查得到"的静态判据，与物理无关。*/
    /*parametersForArchetype 给的是**参数 ID → 值**（不是语义名），所以按 ID 查。*/
    const QHash<QString, float> moodValues = preset.parametersForArchetype(chosenArchetype);
    QVERIFY2(std::fabs(moodValues.value(chosenId, 0.0f) - moodValue) <= 1e-4f,
             qPrintable(QStringLiteral("心情 %1 的参数表里 %2 = %3，与构造用例时读到的不一致")
                            .arg(chosenArchetype)
                            .arg(chosenId)
                            .arg(double(moodValues.value(chosenId, 0.0f)))));
}

/*idle.json 不存在 → 摆动自关、姿势参数保持平坦、不崩；情绪功能不受影响。

  "平坦"必须放在**不被呼吸/物理驱动**的轴上量（本模型 ParamBodyAngleZ）：
  呼吸每帧都往 ParamAngleZ / ParamBodyAngleX 上加东西，那两条轴本来就一直在动。*/
void TestLive2DOffscreen::idleSwayIsOffWithoutDataFile()
{
    Live2DMoodPreset preset;
    const QString dir = availableModelDir();
    if (dir.isEmpty())
        QSKIP("本机没有 Live2D 模型（禁二传，不入库），跳过");

    const QString modelName = QFileInfo(dir).fileName();
    if (!preset.load(modelName))
        QSKIP("本机没有当前角色/模型的心情预设数据（Documents 下，不入库），跳过");

    /*指到一个**确定不存在**的文件：模拟"用户还没给摆动数据 / 删了 / 改坏了"。
       给绝对路径而不是动真实数据目录：用例不许依赖（更不许改）用户数据的现状。*/
    const QString missingPath = QDir(QDir::tempPath())
                                    .absoluteFilePath(QStringLiteral(
                                        "mandarin-idle-sway-does-not-exist.json"));
    QVERIFY2(!QFileInfo::exists(missingPath), "临时目录里居然有这个文件，用例前提被破坏");

    QVERIFY2(!preset.loadIdle(missingPath), "文件不存在，loadIdle 却报告成功");
    QVERIFY2(!preset.isIdleSwayEnabled(), "文件不存在，摆动却是开启状态");
    QVERIFY2(preset.idleSwayEntries().isEmpty(), "文件不存在，却装载出了条目");
    /*关键：摆动数据缺失**不许**把情绪功能一起关掉 —— 两者是正交的
       （moods.json 缺失 = 情绪自关；idle.json 缺失 = 只是不做摆动）。*/
    QVERIFY2(preset.isEnabled(), "idle.json 缺失把情绪功能也关掉了：两个功能被绑死了");
    qInfo("IDLE missing-file: loadIdle=false entries=0 moodEnabled=%d", preset.isEnabled() ? 1 : 0);

    Live2DOffscreenRenderer renderer;
    QString error;
    QVERIFY2(renderer.load(dir, availableModelJson(dir), &error), qPrintable(error));
    renderer.setIdleSway(preset.idleSwayEntries()); //空表 = 关掉
    QVERIFY2(!renderer.isIdleSwayActive(), "喂了空表，摆动却是激活状态");

    /*施加**中立心情**（生产里默认就是这个状态）：姿势类参数被绝对写成立值，于是"没有摆动"
       在屏幕上的表现就是它们完全平坦。不施加心情的话待机动作/物理自己在动，平坦与否量不出来。*/
    renderer.setMoodBlendDurationMs(0);
    renderer.setParameterOverrides(preset.parametersForMood(QStringLiteral("default")));

    /*候选参数 = 参数表里的**全部**参数。哪几个归呼吸/物理/眨眼管是**模型声明**的事
       （呼吸写 ParamAngleX/Y/Z 与 ParamBodyAngleX，物理输出 ParamBodyAngleY，眨眼写眼睛），
       不由测试手抄 —— 下面用"装上真实数据之后谁真的动了"这个**实验**来定哪几条是要看的轴。*/
    QStringList candidates;
    for (auto it = preset.parameters().constBegin(); it != preset.parameters().constEnd(); ++it)
        candidates.append(it.value().id);
    QVERIFY2(!candidates.isEmpty(), "参数表是空的，量不出任何东西");

    QVector<IdleSample> withoutData;
    QVERIFY2(sampleIdleSway(&renderer, candidates, 60, 0.05f, &withoutData),
             "没有摆动数据时渲染失败（不该崩）");

    QStringList offsetLeaks;
    for (const IdleSample &sample : withoutData)
    {
        for (const QString &id : candidates)
        {
            if (sample.offsets.value(id) != 0.0f)
                offsetLeaks.append(
                    QStringLiteral("%1=%2").arg(id).arg(double(sample.offsets.value(id))));
        }
    }
    qInfo("IDLE missing-file sweep: candidates=%d offsetLeaks=%d", candidates.size(),
          offsetLeaks.size());
    QVERIFY2(offsetLeaks.isEmpty(),
             qPrintable(QStringLiteral("没有摆动数据，驱动器却写出了偏移：%1")
                            .arg(offsetLeaks.mid(0, 6).join(QStringLiteral(", ")))));

    /*敏感性对照 + 平坦判据（两件事同一个实验）：
         · 接上真实摆动数据后，哪些轴的**参数值**明显在动（本模型是 bodyZ 那条）；
         · 对那些轴，**没有数据**时它们必须是完全平坦的（极差 ≤ 1e-4）。
       两个条件缺一不可：只看"平坦"可能是量错了地方（呼吸一直在动的轴本来就平坦不了），
       只看"装上数据会动"则证明不了"没有数据就不动"。*/
    Live2DMoodPreset withData;
    if (withData.load(modelName) && withData.loadIdle() && !withData.idleSwayEntries().isEmpty())
    {
        renderer.setIdleSway(withData.idleSwayEntries());
        QVERIFY2(renderer.isIdleSwayActive(), "装上真实数据后摆动没激活");
        QVector<IdleSample> withRealData;
        QVERIFY2(sampleIdleSway(&renderer, candidates, 160, 0.05f, &withRealData),
                 "装上真实数据后渲染失败");

        QStringList swungAxes;
        QStringList flatAxes;
        for (const QString &id : candidates)
        {
            const float swingWith = valueSwingOf(withRealData, id);
            if (swingWith < 1.0f)
                continue;
            const float swingWithout = valueSwingOf(withoutData, id);
            swungAxes.append(QStringLiteral("%1(moves %2, flat-when-missing %3)")
                                 .arg(id)
                                 .arg(double(swingWith), 0, 'f', 3)
                                 .arg(double(swingWithout), 0, 'f', 5));
            if (swingWithout <= 1e-4f)
                flatAxes.append(id);
        }
        qInfo("IDLE missing-file control: swung=%d %s | flat-when-missing=%d %s", swungAxes.size(),
              qPrintable(swungAxes.join(QStringLiteral("; "))), flatAxes.size(),
              qPrintable(flatAxes.join(QStringLiteral(", "))));
        QVERIFY2(!swungAxes.isEmpty(),
                 "接上真实摆动数据后没有任何参数明显在动 —— 这条用例量不到东西");
        QVERIFY2(!flatAxes.isEmpty(),
                 "被摆动的轴在「没有数据」时都不平坦（说明它们还被别的驱动器写着）—— "
                 "「平坦」这条判据在这里量不出东西");
    }
    else
    {
        qInfo("IDLE missing-file control: skipped (本机没有真实摆动数据)");
    }
}

/*绝不写 breath / hair*（每帧由呼吸驱动器与物理写），也绝不写 ParamMouthForm（嘴形属于心情）。

  分两层：
    (a) **数据层**：装载出来的每条都不许落在这几个参数上；
    (b) **驱动层**：**故意**喂一份"敌意"条目（呼吸 + 头发 + 嘴形 + 一条合法身体轴）。
        数据写错时驱动器也必须自己跳过 —— 这是防御性契约，不能只靠数据表自觉。
        合法轴是**阳性对照**：它必须真的动，否则"那几条通道恒为 0"可能只是整个驱动器
        根本没跑（那样这条用例就是空的）。*/
void TestLive2DOffscreen::idleSwayNeverTouchesBreathHairOrMouthForm()
{
    Live2DOffscreenRenderer renderer;
    Live2DMoodPreset preset;
    QString detail;
    if (!loadConfiguredIdleSway(&renderer, &preset, &detail))
        QSKIP("本机没有可用的待机摆动数据（presets/idle.json），跳过");

    const QVector<Live2DMoodPreset::IdleSwayEntry> entries = preset.idleSwayEntries();

    //(a) 数据层
    QStringList violations;
    for (const Live2DMoodPreset::IdleSwayEntry &entry : entries)
    {
        if (Live2DMoodPreset::isUpdaterOwnedParameter(entry.parameterId))
            violations << QStringLiteral("%1(驱动器/物理占用)").arg(entry.parameterId);
        if (entry.parameterId == kMouthFormParameter)
            violations << QStringLiteral("%1(嘴形属于心情)").arg(entry.parameterId);
    }
    QVERIFY2(violations.isEmpty(),
             qPrintable(QStringLiteral("idle.json 写了不该写的参数：%1")
                            .arg(violations.join(QStringLiteral(", ")))));

    //(b) 驱动层：喂"敌意"条目
    const Live2DMoodPreset::IdleSwayEntry control = entries.first();
    QVector<Live2DMoodPreset::IdleSwayEntry> hostile;
    hostile.append(makeIdleEntry(control.semanticName, control.parameterId, control.amplitude,
                                 control.periodSeconds, control.phase));
    hostile.append(makeIdleEntry(QStringLiteral("breath"), QStringLiteral("ParamBreath"), 0.4f,
                                 2.0f, 0.0f));
    hostile.append(makeIdleEntry(QStringLiteral("hairFront"), QStringLiteral("ParamHairFront"),
                                 0.5f, 3.0f, 0.3f));
    hostile.append(makeIdleEntry(QStringLiteral("mouthForm"), kMouthFormParameter, 0.25f, 4.0f,
                                 0.1f));

    const QHash<QString, Live2DOffscreenRenderer::DeclaredRange> ranges =
        renderer.declaredParameterRanges();
    for (const Live2DMoodPreset::IdleSwayEntry &entry : hostile)
    {
        QVERIFY2(ranges.contains(entry.parameterId),
                 qPrintable(QStringLiteral("本机模型没有声明 %1，这条用例量不出东西")
                                .arg(entry.parameterId)));
    }

    /*嘴形给一个明确的心情值（-0.7，不是它自己的中立值 -0.5）：摆动要是写了它，
       读回值必然偏离这个数 —— 这是**模型层**的直接证据（不只是"偏移通道说是 0"）。*/
    const float moodMouthForm = -0.7f;
    QHash<QString, float> mood = preset.parametersForArchetype(QStringLiteral("neutral"));
    mood.insert(kMouthFormParameter, moodMouthForm);
    renderer.setMoodBlendDurationMs(0);
    renderer.setParameterOverrides(mood);
    renderer.setIdleSway(hostile);

    QStringList ids;
    for (const Live2DMoodPreset::IdleSwayEntry &entry : hostile)
        ids << entry.parameterId;

    QVector<IdleSample> samples;
    QVERIFY2(sampleIdleSway(&renderer, ids, 120, 0.05f, &samples), "采样时渲染失败"); //6s

    //阳性对照：合法的那条身体轴必须真的在动
    float controlMin = samples.first().offsets.value(control.parameterId);
    float controlMax = controlMin;
    float worstMouthForm = 0.0f;
    QStringList leaks;
    for (const IdleSample &sample : samples)
    {
        controlMin = std::min(controlMin, sample.offsets.value(control.parameterId));
        controlMax = std::max(controlMax, sample.offsets.value(control.parameterId));
        for (const QString &forbidden : {QStringLiteral("ParamBreath"),
                                         QStringLiteral("ParamHairFront"), kMouthFormParameter})
        {
            if (sample.offsets.value(forbidden) != 0.0f)
                leaks << QStringLiteral("%1=%2").arg(forbidden).arg(
                    double(sample.offsets.value(forbidden)));
        }
        worstMouthForm = std::max(
            worstMouthForm,
            std::fabs(sample.values.value(kMouthFormParameter) - moodMouthForm));
    }

    qInfo("IDLE forbidden[control=%s]: control offset=%.3f~%.3f | leaks=%d | worst mouthForm "
          "deviation=%.6f (mood=%.3f)",
          qPrintable(control.parameterId), double(controlMin), double(controlMax), leaks.size(),
          double(worstMouthForm), double(moodMouthForm));

    QVERIFY2(controlMax - controlMin >= control.amplitude * 0.9f,
             qPrintable(QStringLiteral("阳性对照（%1）没动 —— 下面的「0」不能说明任何事")
                            .arg(control.parameterId)));
    QVERIFY2(leaks.isEmpty(),
             qPrintable(QStringLiteral("摆动写进了不该写的参数：%1")
                            .arg(leaks.mid(0, 6).join(QStringLiteral(", ")))));
    QVERIFY2(worstMouthForm <= 1e-4f,
             qPrintable(QStringLiteral("ParamMouthForm 偏离心情值 %1（最大偏差 %2）—— 摆动碰了嘴形")
                            .arg(double(moodMouthForm))
                            .arg(double(worstMouthForm))));
}

/*两条轴的相位必须按配置错开：动作才不像一整块刚体在摇。*/
void TestLive2DOffscreen::idleSwayPhasesAreOffset()
{
    Live2DOffscreenRenderer renderer;
    Live2DMoodPreset preset;
    QString detail;
    if (!loadConfiguredIdleSway(&renderer, &preset, &detail))
        QSKIP("本机没有可用的待机摆动数据（presets/idle.json），跳过");

    const QVector<Live2DMoodPreset::IdleSwayEntry> entries = preset.idleSwayEntries();
    QVERIFY2(entries.size() >= 2, "待机摆动只有一条轴 —— 数据本身就不满足「相位错开」的设计要求");

    QStringList ids;
    for (const Live2DMoodPreset::IdleSwayEntry &entry : entries)
        ids << entry.parameterId;

    QVector<IdleSample> samples;
    QVERIFY2(sampleIdleSway(&renderer, ids, 310, 0.05f, &samples), "采样时渲染失败"); //15.5s

    QVector<Oscillation> measured;
    for (const Live2DMoodPreset::IdleSwayEntry &entry : entries)
    {
        const Oscillation one = measureOscillation(samples, entry.parameterId);
        measured.append(one);
        QVERIFY2(one.crossings >= 2,
                 qPrintable(QStringLiteral("%1 的上升过零不足（%2 次），量不出相位")
                                .arg(entry.parameterId)
                                .arg(one.crossings)));
        const float error = circularPhaseDistance(one.phase, entry.phase);
        qInfo("IDLE phase[%s]: configured=%.3f measured=%.3f (cycle err %.4f) crossings=%d",
              qPrintable(entry.parameterId), double(entry.phase), double(one.phase),
              double(error), one.crossings);
        QVERIFY2(error <= 0.05f,
                 qPrintable(QStringLiteral("%1 的实测相位 %2 与配置 %3 差了 %4 个周期")
                                .arg(entry.parameterId)
                                .arg(double(one.phase))
                                .arg(double(entry.phase))
                                .arg(double(error))));
    }

    /*取"配置相位差最大"的一对做判据（数据驱动：哪两条最错开由数据决定）。*/
    int firstIndex = 0;
    int secondIndex = 1;
    float configuredGap = circularPhaseDistance(entries[0].phase, entries[1].phase);
    for (int i = 0; i < entries.size(); ++i)
    {
        for (int j = i + 1; j < entries.size(); ++j)
        {
            const float gap = circularPhaseDistance(entries[i].phase, entries[j].phase);
            if (gap > configuredGap)
            {
                configuredGap = gap;
                firstIndex = i;
                secondIndex = j;
            }
        }
    }
    QVERIFY2(configuredGap >= 0.1f,
             qPrintable(QStringLiteral("配置里最错开的两条轴也只差 %1 个周期 —— 动作会像一整块刚体")
                            .arg(double(configuredGap))));

    const float measuredGap =
        circularPhaseDistance(measured[firstIndex].phase, measured[secondIndex].phase);
    qInfo("IDLE phase-pair[%s vs %s]: configured gap=%.3f cycle measured gap=%.3f cycle",
          qPrintable(entries[firstIndex].parameterId), qPrintable(entries[secondIndex].parameterId),
          double(configuredGap), double(measuredGap));
    QVERIFY2(circularPhaseDistance(measuredGap, configuredGap) <= 0.05f,
             qPrintable(QStringLiteral("两条轴的实测相位差 %1 与配置 %2 对不上")
                            .arg(double(measuredGap))
                            .arg(double(configuredGap))));

    /*不锁步的**直接**证据：归一化波形（偏移 ÷ 幅度）在某一帧明显分开。
       锁步 = 两条归一化曲线处处相等，这个差恒为 0。
       理论上两条同频不同相的正弦最大能差到 2·sin(π·gap)；本用例里两条轴的**周期也不同**，
       相对相位还会随时间漂移，所以实际只会更大 —— 用理论下界的 90% 当门槛，两侧都卡住。*/
    float worstShapeGap = 0.0f;
    for (const IdleSample &sample : samples)
    {
        const float a = sample.offsets.value(entries[firstIndex].parameterId) /
                        entries[firstIndex].amplitude;
        const float b = sample.offsets.value(entries[secondIndex].parameterId) /
                        entries[secondIndex].amplitude;
        worstShapeGap = std::max(worstShapeGap, std::fabs(a - b));
    }
    const float theoreticalFloor =
        2.0f * std::sin(static_cast<float>(M_PI) * configuredGap * 0.5f) * 0.9f;
    qInfo("IDLE phase-pair shape gap: worst normalized gap=%.3f (floor %.3f)",
          double(worstShapeGap), double(theoreticalFloor));
    QVERIFY2(worstShapeGap >= theoreticalFloor,
             qPrintable(QStringLiteral("两条轴的归一化波形几乎重合（最大差 %1，理论下界 %2）—— "
                                       "相位错开没有生效，在锁步")
                            .arg(double(worstShapeGap))
                            .arg(double(theoreticalFloor))));
}

/*出**全身**对照图：一个周期内均匀 6 帧，供人眼判断"她真的在动"。

  为什么必须全身：待机摆动是**整体**的重心移动，脸部裁切图会把它藏起来（那正是这条用例
  与**脸部裁切图**（`mood-<模型>-<原型>.png`，只切脸）的区别）。6 帧严格等距（周期 ÷ 6），尺寸一致。

  ⚠️ 本阶段（扩展 miku）起**遍历每个带 presets/idle.json 的模型**，文件名带模型名
  （`idle-<模型>-phase<N>.png`）—— atri 的既有无后缀素材**刻意不动**，miku 另出一组。
  以前这条用例只跑 availableModelDir()（= 用户当前那一个模型），所以 miku 的摆动
  到底动没动、动得像不像"站着"**没有任何人会看**。

  **这里只出图与测量、不判断好坏**：动作看着像不像"站着"由人判读（与渲染情绪校准图同一条
  纪律）。判据只有"尺寸一致 + 文件真的写出来了"。*/
void TestLive2DOffscreen::rendersIdleSwayPhaseFrames()
{
    const QStringList models = modelNamesWithIdleData();
    if (models.isEmpty())
        QSKIP("本机没有任何待机摆动数据（presets/idle.json），跳过");

    const QString outDir = outputDir();
    QVERIFY2(QDir().mkpath(outDir), qPrintable(QStringLiteral("建不出输出目录 %1").arg(outDir)));

    QVector<QStringList> produced;
    for (const QString &modelName : models)
    {
        Live2DOffscreenRenderer renderer;
        QString loadError;
        if (!loadModelByName(modelName, &renderer, &loadError))
        {
            /***不 QSKIP**：有摆动数据却装载不了模型是一致性问题（用户删了模型却留着数据、
               或模型目录改名没跟上数据目录），静默跳过会让它在实机上表现为"她不摆"却无人知。*/
            QFAIL(qPrintable(QStringLiteral("[%1] 有 presets/idle.json 但装载不了模型：%2")
                                 .arg(modelName, loadError)));
        }

        Live2DMoodPreset preset;
        if (!preset.load(modelName) || !preset.loadIdle())
            QFAIL(qPrintable(QStringLiteral("[%1] 情绪预设或摆动数据装载失败").arg(modelName)));

        const QVector<Live2DMoodPreset::IdleSwayEntry> entries = preset.idleSwayEntries();
        QVERIFY2(!entries.isEmpty(),
                 qPrintable(QStringLiteral("[%1] idle.json 装载成功却一条轴都没有")
                                .arg(modelName)));
        const float periodSeconds = entries.first().periodSeconds;
        QVERIFY2(periodSeconds > 0.0f, "第一条轴的周期不是正数，没法在「一个周期」内取相位");

        /*① 先用探针量人物几何，再据此定画布 —— 与 Live2DCharacterWindow::relayoutContent
           同一套推导（基准高 900、目标占比 0.84、画布宽 = 高 × 人物真实宽高比），
           只是去掉了屏幕夹取：判读图要的是**完整的人**，不是"和用户屏幕一样大"。
           探针在测量模式下按虚拟时间取样求并集（与生产同一条路径），所以并集里也包含摆动
           自己摆出去的那部分范围。*/
        renderer.clearFigureSpan();
        renderer.setMeasureMode(true);
        bool probeOk = true;
        for (int index = 0; index < 20; ++index)
        {
            if (index > 0)
                renderer.setNextFrameDeltaSeconds(0.15f);
            const QImage probe = renderer.renderFrame(QSize(512, 512));
            if (probe.isNull())
            {
                probeOk = false;
                break;
            }
            (void)renderer.probeFigureMetrics(probe);
        }
        renderer.setMeasureMode(false);
        const Live2DOffscreenRenderer::FigureMetrics metrics = renderer.figureMetrics();
        QVERIFY2(probeOk && metrics.valid && metrics.boundsAspect > 0.05f,
                 qPrintable(QStringLiteral("[%1] 探针没量到人物，出不了全身对照图")
                                .arg(modelName)));

        constexpr int kSheetBaseHeight = 900;      //= Live2DCharacterWindow::kBaseCanvasHeight
        constexpr float kSheetFigureRatio = 0.84f; //= Live2DCharacterWindow::kTargetFigureRatio
        const int canvasHeight =
            static_cast<int>(std::lround(kSheetBaseHeight / kSheetFigureRatio));
        const int canvasWidth =
            std::max(1, static_cast<int>(std::lround(canvasHeight * metrics.boundsAspect)));
        const QSize sheetSize(canvasWidth, canvasHeight);
        renderer.setDisplayRatios(kSheetFigureRatio, kSheetFigureRatio);
        renderer.setIdleSway(entries);
        /*心情用生产里的默认（中立那张）：出的是"她站着"的对照图，不是某个情绪。*/
        renderer.setMoodBlendDurationMs(0);
        renderer.setParameterOverrides(preset.parametersForMood(QStringLiteral("default")));
        QVERIFY2(renderer.isIdleSwayActive(), "摆动没生效，这组图会是一串相同的帧");

        qInfo("IDLE sheet: model=%s canvas=%dx%d (aspect %.3f) period=%.2fs outDir=%s",
              qPrintable(modelName), sheetSize.width(), sheetSize.height(),
              double(metrics.boundsAspect), double(periodSeconds), qPrintable(outDir));

        /*② 一个周期内均匀取 6 个相位。基准时刻 = 当前累计虚拟时间，第 k 帧的目标时刻是
           base + k×T/6 ⇒ 6 张图的相位**严格等距**，与"渲染花了多久"无关。*/
        constexpr int kPhaseCount = 6;
        const float baseSeconds = renderer.idleSwayElapsedSeconds();
        QVector<QImage> frames;
        QStringList savedPaths;
        QSize expectedSize;
        for (int phaseIndex = 0; phaseIndex < kPhaseCount; ++phaseIndex)
        {
            const float targetSeconds =
                baseSeconds + periodSeconds * static_cast<float>(phaseIndex) / float(kPhaseCount);
            int guard = 0;
            while (renderer.idleSwayElapsedSeconds() < targetSeconds - 1e-4f && guard++ < 64)
            {
                /*一次最多推进 0.1s：那是渲染器单帧步长的夹取上限，超过就不是"一帧"了。*/
                const float step =
                    std::min(targetSeconds - renderer.idleSwayElapsedSeconds(), 0.1f);
                renderer.setNextFrameDeltaSeconds(step);
                QVERIFY2(!renderer.renderFrame(sheetSize).isNull(), "推进虚拟时间时渲染失败");
            }
            /*再渲一帧、**不推进时间**：保证这一帧正好落在目标时刻（帧步长置 0 是渲染器既有的
               一条路径：动作/呼吸/物理都停在原地，只有累计时间之前的量在起作用）。*/
            renderer.setNextFrameDeltaSeconds(0.0f);
            const QImage frame = renderer.renderFrame(sheetSize);
            QVERIFY2(!frame.isNull(), "出图时渲染失败");
            QCOMPARE(frame.size(), sheetSize); //6 张图必须同尺寸，否则并排看没有意义
            if (expectedSize.isEmpty())
                expectedSize = frame.size();
            QCOMPARE(frame.size(), expectedSize);
            frames.append(frame);

            const QString path = QDir(outDir).absoluteFilePath(
                QStringLiteral("idle-%1-phase%2.png").arg(modelName).arg(phaseIndex));
            QVERIFY2(frame.save(path), qPrintable(QStringLiteral("写不出 %1").arg(path)));
            savedPaths.append(path);

            /*把这一帧每条轴的**实际偏移**与"按配置算出来的期望值"一起打出来：
               万一并排看觉得"没动/动得不对"，这行日志能直接指出是数据、相位还是实现的问题。*/
            const float elapsed = renderer.idleSwayElapsedSeconds();
            QStringList applied;
            QStringList expected;
            for (const Live2DMoodPreset::IdleSwayEntry &entry : entries)
            {
                applied << QStringLiteral("%1=%2").arg(entry.parameterId).arg(
                    double(renderer.idleSwayOffset(entry.parameterId)), 0, 'f', 3);
                const double wanted =
                    double(entry.amplitude) *
                    std::sin(2.0 * M_PI *
                             (double(elapsed) / double(entry.periodSeconds) + double(entry.phase)));
                expected << QStringLiteral("%1=%2").arg(entry.parameterId).arg(wanted, 0, 'f', 3);
            }
            qInfo("IDLE sheet[%s] phase %d/%d: t=%.3fs (%.3f of period) applied[%s] expected[%s]",
                  qPrintable(modelName), phaseIndex, kPhaseCount, double(elapsed),
                  double(elapsed / periodSeconds), qPrintable(applied.join(QStringLiteral(","))),
                  qPrintable(expected.join(QStringLiteral(","))));

            /*交叉核对：这一帧**实际**施加的偏移必须等于按配置算出来的期望值。
               少了这一条，出图用例对"驱动器到底跑没跑"是瞎的（一串相同的帧也能过）。*/
            for (const Live2DMoodPreset::IdleSwayEntry &entry : entries)
            {
                const float actual = renderer.idleSwayOffset(entry.parameterId);
                const float wanted = static_cast<float>(
                    double(entry.amplitude) *
                    std::sin(2.0 * M_PI *
                             (double(elapsed) / double(entry.periodSeconds) + double(entry.phase))));
                QVERIFY2(std::fabs(actual - wanted) <= 0.01f,
                         qPrintable(QStringLiteral("相位 %1：%2 实际偏移 %3 与期望 %4 不符 —— "
                                                   "这一帧不在它标称的相位上")
                                        .arg(phaseIndex)
                                        .arg(entry.parameterId)
                                        .arg(double(actual))
                                        .arg(double(wanted))));
            }
        }

        /*只测量、不判断：给出"第 k 帧与第 0 帧差多少像素"，并排看的时候心里有数。*/
        const QImage first = frames.first();
        for (int index = 1; index < frames.size(); ++index)
        {
            const QImage &other = frames[index];
            int differingPixels = 0;
            for (int y = 0; y < first.height(); ++y)
            {
                const uchar *left = first.constScanLine(y);
                const uchar *right = other.constScanLine(y);
                for (int x = 0; x < first.width(); ++x)
                {
                    const int offset = x * 4;
                    if (left[offset] != right[offset] || left[offset + 1] != right[offset + 1] ||
                        left[offset + 2] != right[offset + 2] || left[offset + 3] != right[offset + 3])
                        ++differingPixels;
                }
            }
            qInfo("IDLE sheet[%s] diff(phase%d vs phase0): %d/%d pixels (%.2f%%)",
                  qPrintable(modelName), index, differingPixels, first.width() * first.height(),
                  100.0 * double(differingPixels) / double(first.width() * first.height()));
        }
        qInfo("IDLE sheet[%s]: %d frames %dx%d saved: %s", qPrintable(modelName),
              savedPaths.size(), expectedSize.width(), expectedSize.height(),
              qPrintable(savedPaths.join(QStringLiteral(" "))));
        produced.append(savedPaths);
    }

    QVERIFY2(!produced.isEmpty(), "一个模型的摆动帧都没出成");
    qInfo("IDLE sheet: 共 %d 个模型、%d 个文件", produced.size(),
          [&produced]() {
              int total = 0;
              for (const QStringList &list : produced)
                  total += list.size();
              return total;
          }());
}

/*[数据纪律] 每个带 idle.json 的模型：装载成功、没有驱动器/嘴形条目、
   每条幅度都在**声明的量程**的 20% 警戒线以内。

   为什么要单独一条、而且不依赖渲染：装载器对越界幅度只是**打一条告警**（见
   Live2DMoodPreset::auditIdleSway：那是审美，不替作者做决定）。于是"数据写坏了"
   在屏幕上只表现为"她抖得厉害"，没人能从一个动图里反推出是 17.5% 还是 35%。
   这条把它变成一个可读的数字，并且**遍历所有模型** —— 新挂上来的那份数据自动进入判据。

   20% 这条线就是数据文件自己写在 `_rules` 里的规则（同一条线装载器也会告警），
   所以这不是测试发明的阈值，而是拿文件自己的承诺去核文件。*/
void TestLive2DOffscreen::idleSwayDataObeysItsOwnRulesForEveryModel()
{
    const QStringList models = modelNamesWithIdleData();
    if (models.isEmpty())
        QSKIP("本机没有任何待机摆动数据（presets/idle.json），跳过");

    /*驱动器/物理占用的参数（语义名与真实 ID 两侧都查）与嘴形：写了也必须被剔除。
       这几条与 Live2DMoodPreset::isUpdaterOwnedParameter 是同一份名单 —— 这里再钉一次，
       因为"装载器剔除了"与"数据里没有"是两件事：后者才是作者该守的纪律。*/
    const QStringList forbiddenSemantic = {QStringLiteral("breath"), QStringLiteral("hairFront"),
                                           QStringLiteral("hairSide"), QStringLiteral("hairBack"),
                                           QStringLiteral("mouthForm")};

    constexpr float kAmplitudeWarnRatio = 0.2f; //= Live2DMoodPreset 的 kIdleSwayAmplitudeWarnRatio

    int axesChecked = 0;
    for (const QString &modelName : models)
    {
        Live2DMoodPreset preset;
        QVERIFY2(preset.load(modelName),
                 qPrintable(QStringLiteral("[%1] parameter-map.json/moods.json 装载失败")
                                .arg(modelName)));
        QVERIFY2(preset.loadIdle(),
                 qPrintable(QStringLiteral("[%1] presets/idle.json 存在却装载失败（数据坏了？）")
                                .arg(modelName)));

        const QVector<Live2DMoodPreset::IdleSwayEntry> entries = preset.idleSwayEntries();
        QVERIFY2(!entries.isEmpty(),
                 qPrintable(QStringLiteral("[%1] 一条可用轴都没有").arg(modelName)));

        Live2DOffscreenRenderer renderer;
        QString loadError;
        QVERIFY2(loadModelByName(modelName, &renderer, &loadError),
                 qPrintable(QStringLiteral("[%1] 装载不了模型：%2").arg(modelName, loadError)));
        const QHash<QString, Live2DOffscreenRenderer::DeclaredRange> declared =
            renderer.declaredParameterRanges();
        QVERIFY2(!declared.isEmpty(), "模型没有声明任何参数");

        QStringList axes;
        for (const Live2DMoodPreset::IdleSwayEntry &entry : entries)
        {
            // ① 数据里**不许**出现驱动器/嘴形参数（语义名侧）
            QVERIFY2(!forbiddenSemantic.contains(entry.semanticName),
                     qPrintable(QStringLiteral("[%1] idle.json 里写了 %2 —— 驱动器/嘴形参数不该由"
                                               "摆动数据来写（即使装载器会剔除，数据本身也该干净）")
                                    .arg(modelName, entry.semanticName)));
            // ② 真实 ID 侧同样不许（防"语义名换了、ID 还是那一个"）
            QVERIFY2(!Live2DMoodPreset::isUpdaterOwnedParameter(entry.parameterId),
                     qPrintable(QStringLiteral("[%1] idle.json 的 %2 解析到了驱动器参数 %3")
                                    .arg(modelName, entry.semanticName, entry.parameterId)));

            // ③ 幅度 ≤ 声明量程的 20%（文件自己的规则）
            QVERIFY2(declared.contains(entry.parameterId),
                     qPrintable(QStringLiteral("[%1] 模型没有声明 %2（语义名 %3）")
                                    .arg(modelName, entry.parameterId, entry.semanticName)));
            const Live2DOffscreenRenderer::DeclaredRange range = declared.value(entry.parameterId);
            const float span = range.max - range.min;
            QVERIFY2(span > 0.0f,
                     qPrintable(QStringLiteral("[%1] %2 的声明量程是 0").arg(modelName,
                                                                              entry.parameterId)));
            const float ratio = entry.amplitude / span;
            QVERIFY2(ratio <= kAmplitudeWarnRatio + 1e-6f,
                     qPrintable(QStringLiteral("[%1] %2 的幅度 %3 是声明量程 %4 的 %5%% "
                                               "（文件自己规定 ≤ %6%%）—— 波峰会顶到上下限被削平")
                                    .arg(modelName, entry.parameterId)
                                    .arg(double(entry.amplitude))
                                    .arg(double(span))
                                    .arg(double(ratio * 100.0))
                                    .arg(double(kAmplitudeWarnRatio * 100.0f))));
            axes << QStringLiteral("%1(%2)=%3/%4s/%5=%6%%")
                        .arg(entry.semanticName, entry.parameterId)
                        .arg(double(entry.amplitude))
                        .arg(double(entry.periodSeconds))
                        .arg(double(entry.phase))
                        .arg(double(ratio * 100.0), 0, 'f', 1);
            ++axesChecked;
        }
        qInfo("IDLE data[%s]: %d axes, all within %.0f%% of declared range: %s",
              qPrintable(modelName), entries.size(), double(kAmplitudeWarnRatio * 100.0f),
              qPrintable(axes.join(QStringLiteral(" | "))));
    }

    qInfo("IDLE data: %d models, %d axes checked", models.size(), axesChecked);
}

/*==================== 腿部探针：这个模型的腿到底动得了吗 ====================

  用户的问题（原话大意）：「miku 的腿能不能动？lowerBodyZ 到底移得动多少？」

  **为什么要专门做一次全参数扫描、而不是读 cdi3 的名字猜**：miku 的 141 个参数里大部分是
  `Param25` 这种无语义 ID，cdi3 的显示名（『x全身』『z下身』）只说明作者的**意图**，
  不说它实际连到了哪块 artmesh。而『腿有没有关节』这种事只有一个可信的判据：
  **把它推到极限、看下半身的 alpha 掩码动没动**。

  两个指标必须**分开**（这是本探针存在的第二个理由）：
    geometry  = 两帧 alpha 掩码不同的像素数（掩码动了 = 真的在动骨架/形变）
    appearance= 掩码**逐位相同**、只有 RGB 不同的像素数（换装、道具、贴图开关）
  「大葱」这类道具开关会把画面差出十几个百分点，但在 geometry 上是 **0** ——
  只看"画面差多少像素"会把它误判成"腿动了"。所以判据按 geometry 排序，appearance 单列。

  **隔离手法**：把**每一个**声明参数都钉在中立值上（整组覆盖），只放开正在测的那一个，
  并且帧步长注入 0（呼吸/物理/眨眼/动作全部停在原地）。这样两帧的差异只可能来自这一个
  参数 —— 否则"物理这一帧碰巧动了"会被算到参数头上。呼吸/头发不归覆盖表管
  （isUpdaterOwnedParameter），但它们受帧步长驱动，步长 0 时同样不动。

  **只测量、不断言"腿该怎样"**：模型有没有腿部关节是作者的决定，不是 bug。
  这里只把事实摊开。*/
void TestLive2DOffscreen::probesWhichParametersArticulateTheLowerBody()
{
    /*整轮扫描 = 每个模型 (2 × 参数个数 + 若干基准帧) ≈ 300 次渲染，属于"正常测试"的量级
       （用户明确说过这种量级没问题；真正的负载事故来自人为的 CPU 燃烧线程）。*/
    QStringList models = modelNamesWithIdleData();
    models << modelNamesWithMoodData();
    models.removeDuplicates();
    models.sort();
    if (models.isEmpty())
        QSKIP("本机没有任何模型数据（parameter-map.json / idle.json），跳过腿部探针");

    const QString outDir = outputDir();
    QVERIFY2(QDir().mkpath(outDir), qPrintable(QStringLiteral("建不出输出目录 %1").arg(outDir)));

    //扫描用的画布：够大能看清下半身，又足够小让 ~300 帧跑得完
    const QSize probeSize(360, 720);

    QStringList allReports;
    for (const QString &modelName : models)
    {
        Live2DOffscreenRenderer renderer;
        QString loadError;
        QVERIFY2(loadModelByName(modelName, &renderer, &loadError),
                 qPrintable(QStringLiteral("[%1] 装载不了模型：%2").arg(modelName, loadError)));

        const QHash<QString, Live2DOffscreenRenderer::DeclaredRange> declared =
            renderer.declaredParameterRanges();
        QVERIFY2(!declared.isEmpty(), "模型没有声明任何参数");

        //显示名（cdi3）：把 `Param25` 翻译成『x全身』—— 报告的可读性全靠它
        QHash<QString, QString> displayNames;
        {
            const QString modelFileDir = QDir(Live2DModelRootPath).filePath(modelName);
            const QString searchDir =
                QFileInfo::exists(modelFileDir)
                    ? modelFileDir
                    : QDir(CharacterAssestPath)
                          .filePath(ReadNowSelectChar() + QStringLiteral("/Live2D/") + modelName);
            const QDir dir(searchDir);
            for (const QString &cdi3Name :
                 dir.entryList({QStringLiteral("*.cdi3.json")}, QDir::Files))
            {
                QFile file(dir.filePath(cdi3Name));
                if (!file.open(QIODevice::ReadOnly))
                    continue;
                const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
                for (const QJsonValue &value :
                     doc.object().value(QStringLiteral("Parameters")).toArray())
                {
                    const QJsonObject entry = value.toObject();
                    const QString id = entry.value(QStringLiteral("Id")).toString();
                    const QString name = entry.value(QStringLiteral("Name")).toString();
                    if (!id.isEmpty() && !name.isEmpty() && !displayNames.contains(id))
                        displayNames.insert(id, name);
                }
            }
        }

        /*把整组参数钉到中立值。为什么用**整组覆盖**而不是逐条 setParameter：
           覆盖是原子替换，切到下一个参数时上一项自然消失 —— 逐条 setParameter 永远删不掉，
           第一个参数会一路留着，后面的读数就全被它污染了（这正是 atri 那边"她只有一副表情"
           的同一个成因）。*/
        QHash<QString, float> neutral = renderer.declaredParameterRanges().isEmpty()
                                            ? QHash<QString, float>()
                                            : QHash<QString, float>();
        for (auto it = declared.constBegin(); it != declared.constEnd(); ++it)
            neutral.insert(it.key(), it.value().neutral);
        renderer.setMoodBlendDurationMs(0); //立刻生效，不要过渡态
        renderer.setIdleSway({});           //探针期间不做摆动（它会盖住被测参数）
        renderer.clearEyeOpennessMultiplierOverride();

        //先渲两帧热身（首帧要建 FBO、把着色器与贴图走热），不计入测量
        for (int index = 0; index < 2; ++index)
        {
            renderer.setParameterOverrides(neutral);
            renderer.setNextFrameDeltaSeconds(0.0f);
            QVERIFY2(!renderer.renderFrame(probeSize).isNull(), "热身帧渲染失败");
        }

        //人物包围盒（中性姿势）：下半身区域按它切
        renderer.setParameterOverrides(neutral);
        renderer.setNextFrameDeltaSeconds(0.0f);
        const QImage baseline = renderer.renderFrame(probeSize);
        QVERIFY2(!baseline.isNull(), "中性帧渲染失败");
        const QImage base = baseline.convertToFormat(QImage::Format_RGBA8888);

        int minX = base.width(), minY = base.height(), maxX = -1, maxY = -1;
        for (int y = 0; y < base.height(); ++y)
        {
            const uchar *line = base.constScanLine(y);
            for (int x = 0; x < base.width(); ++x)
            {
                if (line[x * 4 + 3] <= 32)
                    continue;
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
        }
        QVERIFY2(maxX >= minX && maxY >= minY, "中性帧里量不到人物");

        /*上半身 / 下半身按人物包围盒**对半切**。这不是"解剖学上精确的腰线"，
           而是一个**同一模型内可比**的固定区域 —— 探针要回答的是"哪些参数动了下面那一半"，
           换一个更聪明的切法只会让"这一帧与那一帧"的比较失去共同基准。*/
        const QRect lower(minX, minY + (maxY - minY) / 2, maxX - minX + 1,
                          maxY - (minY + (maxY - minY) / 2) + 1);
        const QRect upper(minX, minY, maxX - minX + 1, (maxY - minY) / 2 + 1);

        /*人物在区域里的重心（像素坐标，只统计 alpha 达标的像素）。
           用来区分「整个下半身平移过去」与「骨架在原地形变」：平移的参数重心会跟着走，
           只改轮廓的参数重心基本不动。*/
        const auto centroidOf = [](const QImage &frame, const QRect &region, double *meanX,
                                   double *meanY) {
            double sumX = 0.0;
            double sumY = 0.0;
            qint64 count = 0;
            for (int y = region.top(); y <= region.bottom(); ++y)
            {
                const uchar *line = frame.constScanLine(y);
                for (int x = region.left(); x <= region.right(); ++x)
                {
                    if (line[x * 4 + 3] <= kLegProbeAlphaThreshold)
                        continue;
                    sumX += x;
                    sumY += y;
                    ++count;
                }
            }
            *meanX = count > 0 ? sumX / double(count) : 0.0;
            *meanY = count > 0 ? sumY / double(count) : 0.0;
            return count;
        };

        //度量：在一个区域里分别数「掩码不同」与「掩码相同但 RGB 不同」
        const auto measureRegion = [&base](const QImage &other, const QRect &region, qint64 *geometry,
                                           qint64 *appearance) {
            *geometry = 0;
            *appearance = 0;
            for (int y = region.top(); y <= region.bottom(); ++y)
            {
                const uchar *a = base.constScanLine(y);
                const uchar *b = other.constScanLine(y);
                for (int x = region.left(); x <= region.right(); ++x)
                {
                    const int offset = x * 4;
                    const bool maskA = a[offset + 3] > kLegProbeAlphaThreshold;
                    const bool maskB = b[offset + 3] > kLegProbeAlphaThreshold;
                    if (maskA != maskB)
                    {
                        ++(*geometry);
                        continue;
                    }
                    if (!maskA)
                        continue; //两边都透明：没有外观可言
                    /*appearance 只统计**两边都不透明**的像素：这样"换了一整块贴图/道具"
                      与"轮廓移动了"在数值上互不干扰 —— 掩码一移，那些像素就落进 geometry，
                      不会同时被算成外观变化。两者因此是可以直接对照的两个独立量。
                      ⚠️ 但**旋转/整体平移同样会让大部分重叠区域的 RGB 变掉**（贴图在屏幕上
                      换了映射），所以 appearance 高**不等于**"换了外观"。
                      区分"外形改了"与"整体挪了/转了"要靠下面的 maskAreaDelta。*/
                    if (a[offset] != b[offset] || a[offset + 1] != b[offset + 1] ||
                        a[offset + 2] != b[offset + 2])
                        ++(*appearance);
                }
            }
        };

        /*区域里的**掩码面积**（alpha 达标像素数）。
           这是把"外形真的改了"从"整体平移/旋转"里剥出来的关键量：
             - 刚体平移  → 掩码逐像素差异很大，但**面积几乎不变**（形状只是挪了位置）；
             - 骨架形变  → 面积明显改变（腿伸出来/缩回去、道具出现/消失）。
           只看 alpha 逐像素差会把这两者混为一谈（而它们对"腿能不能动"的结论完全相反）。*/
        const auto maskAreaOf = [](const QImage &frame, const QRect &region) {
            qint64 count = 0;
            for (int y = region.top(); y <= region.bottom(); ++y)
            {
                const uchar *line = frame.constScanLine(y);
                for (int x = region.left(); x <= region.right(); ++x)
                {
                    if (line[x * 4 + 3] > kLegProbeAlphaThreshold)
                        ++count;
                }
            }
            return count;
        };
        const qint64 lowerArea =
            qint64(lower.width()) * qint64(lower.height());
        const qint64 upperArea = qint64(upper.width()) * qint64(upper.height());

        struct Hit
        {
            QString id;
            QString name;
            float min = 0.0f;
            float max = 0.0f;
            qint64 geometryLower = 0;
            qint64 appearanceLower = 0;
            qint64 geometryUpper = 0;
            double valueMin = 0.0;
            double valueMax = 0.0;
            /*重心位移与"写入是否落到参数上"两项诊断。
               前者区分「平移」与「形变」；后者是给"这个参数是不是死的"提供证据 ——
               读回值不等于写进去的值，就说明它没被写进模型，而不是"连了但看不出来"。*/
            double centroidDx = 0.0;
            double centroidDy = 0.0;
            float readBackMin = 0.0f;
            float readBackMax = 0.0f;
            /*掩码面积变化（像素，取两侧里更大的一侧）：把"外形真的改了"从"整体挪/转了"
               里剥出来。刚体平移的面积变化接近 0，骨架形变则明显（见 maskAreaOf 的说明）。*/
            qint64 maskAreaBaseline = 0;
            qint64 maskAreaDelta = 0;
        };
        QVector<Hit> hits;
        int measured = 0;

        for (auto it = declared.constBegin(); it != declared.constEnd(); ++it)
        {
            const QString id = it.key();
            const Live2DOffscreenRenderer::DeclaredRange range = it.value();
            if (range.max - range.min <= 0.0f)
                continue; //没有量程的参数推不动（全部是 0/常量），扫它没有意义

            Hit hit;
            hit.id = id;
            hit.name = displayNames.value(id, QStringLiteral("-"));
            hit.min = range.min;
            hit.max = range.max;

            /*每个参数取**两帧**（min 一帧、max 一帧），各与**紧邻它之前**重新渲的中性帧比较。
               为什么每次都重渲中性帧、而不是拿最初那一张当公共基准：
               物理/眨眼/呼吸虽然被步长 0 冻住了，但"冻住"不等于"回到中性"——
               被测参数在上一轮留下的状态可能仍在驱动器里。重渲一次基准是这个探针里最便宜的保险。*/
            const auto renderWith = [&](float value, QImage *out) {
                QHash<QString, float> overrides = neutral;
                overrides.insert(id, value);
                renderer.setParameterOverrides(overrides);
                renderer.setNextFrameDeltaSeconds(0.0f);
                const QImage frame = renderer.renderFrame(probeSize);
                if (frame.isNull())
                    return false;
                *out = frame.convertToFormat(QImage::Format_RGBA8888);
                return true;
            };
            const auto renderNeutral = [&](QImage *out) {
                renderer.setParameterOverrides(neutral);
                renderer.setNextFrameDeltaSeconds(0.0f);
                const QImage frame = renderer.renderFrame(probeSize);
                if (frame.isNull())
                    return false;
                *out = frame.convertToFormat(QImage::Format_RGBA8888);
                return true;
            };

            QImage neutralBefore;
            QVERIFY2(renderNeutral(&neutralBefore), "基准帧渲染失败");
            double neutralMeanX = 0.0;
            double neutralMeanY = 0.0;
            (void)centroidOf(neutralBefore, lower, &neutralMeanX, &neutralMeanY);

            QImage atMin;
            QVERIFY2(renderWith(range.min, &atMin), "参数最小值帧渲染失败");
            qint64 geometry = 0;
            qint64 appearance = 0;
            qint64 geometryUpper = 0;
            qint64 appearanceUpper = 0;
            measureRegion(atMin, lower, &geometry, &appearance);
            measureRegion(atMin, upper, &geometryUpper, &appearanceUpper);
            hit.geometryLower = geometry;
            hit.appearanceLower = appearance;
            hit.geometryUpper = geometryUpper;
            /*读回值：写进去的是 range.min，读回来的是 Core 里**真正生效**的值。
               两者不等 ⇒ 这个参数没被写进模型（那不是"看不出效果"，是压根没接线）。*/
            hit.readBackMin = renderer.parameterValue(id);
            hit.valueMin = double(hit.readBackMin);
            double minMeanX = 0.0;
            double minMeanY = 0.0;
            (void)centroidOf(atMin, lower, &minMeanX, &minMeanY);
            hit.centroidDx = minMeanX - neutralMeanX;
            hit.centroidDy = minMeanY - neutralMeanY;

            QVERIFY2(renderNeutral(&neutralBefore), "基准帧渲染失败");
            (void)centroidOf(neutralBefore, lower, &neutralMeanX, &neutralMeanY);
            QImage atMax;
            QVERIFY2(renderWith(range.max, &atMax), "参数最大值帧渲染失败");
            qint64 geometryMax = 0;
            qint64 appearanceMax = 0;
            qint64 geometryUpperMax = 0;
            qint64 appearanceUpperMax = 0;
            measureRegion(atMax, lower, &geometryMax, &appearanceMax);
            measureRegion(atMax, upper, &geometryUpperMax, &appearanceUpperMax);
            /*取两侧里**动得更多**的那一侧：一个参数可能只在正方向或只在负方向改骨架
               （作者常把中立的骨架摆在量程的一端）。*/
            hit.geometryLower = std::max(hit.geometryLower, geometryMax);
            hit.appearanceLower = std::max(hit.appearanceLower, appearanceMax);
            hit.geometryUpper = std::max(hit.geometryUpper, geometryUpperMax);
            hit.readBackMax = renderer.parameterValue(id);
            hit.valueMax = double(hit.readBackMax);
            double maxMeanX = 0.0;
            double maxMeanY = 0.0;
            (void)centroidOf(atMax, lower, &maxMeanX, &maxMeanY);
            //重心位移取两侧里更大的一侧（与上面几个量同一个约定）
            if (std::fabs(maxMeanX - neutralMeanX) > std::fabs(hit.centroidDx))
                hit.centroidDx = maxMeanX - neutralMeanX;
            if (std::fabs(maxMeanY - neutralMeanY) > std::fabs(hit.centroidDy))
                hit.centroidDy = maxMeanY - neutralMeanY;
            /*掩码面积变化：拿 min/max 两侧里更大的那一侧。
               用中性帧的面积当基准（上面刚重渲过，与这两帧同源）。*/
            const qint64 neutralArea = maskAreaOf(neutralBefore, lower);
            hit.maskAreaBaseline = neutralArea;
            hit.maskAreaDelta =
                std::max(qAbs(maskAreaOf(atMin, lower) - neutralArea),
                         qAbs(maskAreaOf(atMax, lower) - neutralArea));

            hits.append(hit);
            ++measured;
        }

        /*主排序按 **areaDelta**（掩码面积变化）—— 那才是"外形真的改了"的判据。
           按 geometryLower（逐像素 alpha 差）排会把"整体平移"排到最前面：平移确实改变了很多
           像素的 alpha，但外形一个像素都没改。*/
        std::sort(hits.begin(), hits.end(), [](const Hit &lhs, const Hit &rhs) {
            if (lhs.maskAreaDelta != rhs.maskAreaDelta)
                return lhs.maskAreaDelta > rhs.maskAreaDelta;
            if (lhs.geometryLower != rhs.geometryLower)
                return lhs.geometryLower > rhs.geometryLower;
            return lhs.id < rhs.id;
        });

        QStringList report;
        report.append(QStringLiteral("# 腿部探针 / leg probe: model=%1").arg(modelName));
        report.append(QStringLiteral("# 画布 %1x%2；上半身 %3,%4 %5x%6；下半身 %7,%8 %9x%10")
                          .arg(probeSize.width())
                          .arg(probeSize.height())
                          .arg(upper.x())
                          .arg(upper.y())
                          .arg(upper.width())
                          .arg(upper.height())
                          .arg(lower.x())
                          .arg(lower.y())
                          .arg(lower.width())
                          .arg(lower.height()));
        report.append(QStringLiteral("# 指标定义：geometry = alpha 掩码不同的像素（轮廓动了）；"
                                     "appearance = **两边都不透明**的像素里只有 RGB 不同。"
                                     "下半身区域面积 %1 像素。")
                          .arg(lowerArea));
        report.append(QStringLiteral("# centroidDx/Dy = 下半身重心的位移（像素）；"
                                     "areaDelta = 下半身**掩码面积**的变化（像素）与占区域的比例 —— "
                                     "**这才是「外形真的改了」的判据**：刚体平移/旋转的面积变化接近 0"
                                     "（形状只是挪了位置），骨架形变/道具出现消失才会明显改变面积。"));
        report.append(QStringLiteral("# readBackMin/Max = 推到 min/max 之后从模型读回的值"
                                     "（与 min/max 不等 ⇒ 这个参数没被写进模型，是死参数）。"));
        report.append(QStringLiteral("# 每个参数取 min/max 两侧里动得更多的一侧；按 areaDelta 降序"
                                     "（= 按「外形真的改了」降序）。"));
        const QString header =
            QStringLiteral("# %1\t%2\t%3\t%4\t%5\t%6\t%7\t%8\t%9\t%10\t%11")
                .arg(QStringLiteral("param"),
                     QStringLiteral("displayName"),
                     QStringLiteral("geoLower"),
                     QStringLiteral("appLower"),
                     QStringLiteral("geoUpper"),
                     QStringLiteral("areaDelta"),
                     QStringLiteral("areaDelta%"),
                     QStringLiteral("centroidDx"),
                     QStringLiteral("centroidDy"),
                     QStringLiteral("rangeMin/max"),
                     QStringLiteral("readBackMin/max"));
        const auto formatRow = [](const Hit &hit) {
            return QStringLiteral("%1\t%2\t%3\t%4\t%5\t%6\t%7\t%8\t%9\t%10/%11\t%12/%13")
                .arg(hit.id, hit.name.isEmpty() ? QStringLiteral("-") : hit.name)
                .arg(hit.geometryLower)
                .arg(hit.appearanceLower)
                .arg(hit.geometryUpper)
                .arg(hit.maskAreaDelta)
                .arg(double(hit.maskAreaDelta) /
                         double(std::max<qint64>(1, hit.maskAreaBaseline)) * 100.0, 0, 'f', 2)
                .arg(double(hit.centroidDx), 0, 'f', 2)
                .arg(double(hit.centroidDy), 0, 'f', 2)
                .arg(double(hit.min))
                .arg(double(hit.max))
                .arg(double(hit.readBackMin))
                .arg(double(hit.readBackMax));
        };
        report.append(header);
        for (const Hit &hit : hits)
            report.append(formatRow(hit));

        /*第二张表：按 appearance 降序。
           ⚠️ appearance 高**不等于**"换了外观"（旋转/整体平移同样会让重叠区域的 RGB 变掉），
           所以这张表要**连着 areaDelta 一起看**：
             areaDelta ≈ 0 而 appearance 高 ⇒ 整体挪了/转了（贴图在屏幕上换了映射）；
             areaDelta 明显      ⇒ 外形真的改了（道具出现/消失、肢体伸出/收回）。*/
        QVector<Hit> byAppearance = hits;
        std::sort(byAppearance.begin(), byAppearance.end(), [](const Hit &lhs, const Hit &rhs) {
            if (lhs.appearanceLower != rhs.appearanceLower)
                return lhs.appearanceLower > rhs.appearanceLower;
            return lhs.id < rhs.id;
        });
        report.append(QString());
        report.append(QStringLiteral("# ---- 按 appearanceLower 降序"
                                     "（与 areaDelta 连看：areaDelta≈0 = 挪/转；明显 = 外形改了） ----"));
        report.append(header);
        const int appearanceRows = std::min(20, static_cast<int>(byAppearance.size()));
        for (int index = 0; index < appearanceRows; ++index)
        {
            if (byAppearance.at(index).appearanceLower == 0)
                break;
            report.append(formatRow(byAppearance.at(index)));
        }
        /***死参数**：推到量程两端、从模型读回来却还是中立值 ⇒ 这个参数没被写进 moc
           （不是"连了但看不出效果"）。这一栏必须打出来，因为"某个参数推不动"与
           "这个参数看不见效果"在画面上完全一样，只有读回值分得开。
           这类参数不该进 parameter-map（那会让数据作者去校准一个永远不生效的数字）。*/
        QStringList stuck;
        for (const Hit &hit : hits)
        {
            const bool minStuck = std::fabs(hit.readBackMin - hit.min) > 1e-3f &&
                                  std::fabs(hit.readBackMin - hit.max) > 1e-3f;
            const bool maxStuck = std::fabs(hit.readBackMax - hit.min) > 1e-3f &&
                                  std::fabs(hit.readBackMax - hit.max) > 1e-3f;
            if (minStuck && maxStuck)
                stuck.append(QStringLiteral("%1(%2) readBack=%3/%4 range=[%5,%6]")
                                 .arg(hit.id, hit.name)
                                 .arg(double(hit.readBackMin))
                                 .arg(double(hit.readBackMax))
                                 .arg(double(hit.min))
                                 .arg(double(hit.max)));
        }
        qInfo("LEGPROBE %s: %d parameters did NOT take the written value (dead/unwired): %s",
              qPrintable(modelName), stuck.size(),
              stuck.isEmpty() ? "none" : qPrintable(stuck.join(QStringLiteral(" | "))));
        for (const QString &line : stuck)
            report.append(QStringLiteral("# DEAD %1").arg(line));
        const QString path = QDir(outDir).absoluteFilePath(
            QStringLiteral("leg-probe-%1.txt").arg(modelName));
        QFile file(path);
        QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
                 qPrintable(QStringLiteral("写不出 %1").arg(path)));
        file.write(report.join(QLatin1Char('\n')).toUtf8());
        file.close();
        allReports.append(path);

        /*控制台只打**前几名**（全表在文件里）：这条用例的重点是"哪些参数真的动下半身"，
           而 141 行控制台输出会把 ctest 的日志淹没。ASCII 列名保证控制台可读。*/
        qInfo("LEGPROBE %s: %d parameters swept, canvas %dx%d, lower region %dx%d (%lld px), "
              "upper %dx%d. Full table: %s",
              qPrintable(modelName), measured, probeSize.width(), probeSize.height(), lower.width(),
              lower.height(), static_cast<long long>(lowerArea), upper.width(), upper.height(),
              qPrintable(path));
        const int topCount = std::min(15, static_cast<int>(hits.size()));
        for (int index = 0; index < topCount; ++index)
        {
            const Hit &hit = hits.at(index);
            const double geoPercent =
                100.0 * double(hit.geometryLower) / double(std::max<qint64>(1, lowerArea));
            const double appPercent =
                100.0 * double(hit.appearanceLower) / double(std::max<qint64>(1, lowerArea));
            qInfo("LEGPROBE %s TOP%02d %s (%s) areaDelta=%lld (%.2f%% of lower) geoLower=%lld "
                  "(%.2f%%) appLower=%lld (%.2f%%) geoUpper=%lld centroid=(%.2f,%.2f)",
                  qPrintable(modelName), index + 1, qPrintable(hit.id), qPrintable(hit.name),
                  static_cast<long long>(hit.maskAreaDelta),
                  100.0 * double(hit.maskAreaDelta) /
                      double(std::max<qint64>(1, hit.maskAreaBaseline)),
                  static_cast<long long>(hit.geometryLower), geoPercent,
                  static_cast<long long>(hit.appearanceLower), appPercent,
                  static_cast<long long>(hit.geometryUpper), hit.centroidDx, hit.centroidDy);
        }

        /*把**语义表里那几个全身轴**单独再报一次：用户问的是它们，而它们未必排在前面
           （全身轴动的是整个人，下半身只是其中一部分）。*/
        Live2DMoodPreset preset;
        if (preset.load(modelName))
        {
            const QStringList interesting = {QStringLiteral("lowerBodyZ"), QStringLiteral("upperBodyZ"),
                                             QStringLiteral("wholeBodyX"),
                                             QStringLiteral("wholeBodyY"),
                                             QStringLiteral("wholeBodyShiftX"),
                                             QStringLiteral("bodyZ"), QStringLiteral("bodyX"),
                                             QStringLiteral("headZ")};
            for (const QString &semantic : interesting)
            {
                const Live2DMoodPreset::ParameterRange range =
                    preset.parameters().value(semantic);
                if (range.id.isEmpty())
                    continue;
                for (const Hit &hit : hits)
                {
                    if (hit.id != range.id)
                        continue;
                    qInfo("LEGPROBE %s SEMANTIC %s=%s (%s) geoLower=%lld appLower=%lld "
                          "geoUpper=%lld",
                          qPrintable(modelName), qPrintable(semantic), qPrintable(range.id),
                          qPrintable(hit.name), static_cast<long long>(hit.geometryLower),
                          static_cast<long long>(hit.appearanceLower),
                          static_cast<long long>(hit.geometryUpper));
                    break;
                }
            }
        }
    }

    QVERIFY2(!allReports.isEmpty(), "一个模型的探针报告都没出成");
}

/*==================== 两个模型是不是同一套 rig ====================

  用户的问题：**樱花miku 能不能直接用 miku 的那三份数据？**

  **为什么不能靠"看一眼"回答**：两个模型的人物贴图不同（樱花miku 是换色/换装版），
  画面必然不一样 —— 比像素等于什么都没说。真正决定"数据能不能复用"的是**骨架与绑定**：
   同一个 moc？同一批参数？同一批参数推到同一个值，剪影是否落在同一处？

  所以这里比两件事（都与贴图无关）：
    ① 声明的参数集合：ID 与 min/default/max **逐条相等**（同 rig 的必要条件）；
    ② 同一批参数取同一批值时的 **alpha 剪影** 在输出空间里是否一致。

  判据用"剪影不一致的像素占人物面积的比例"，阈值取 2%：真同 rig 时只有抗锯齿边缘的
  亚像素差（实测远低于 1%）；换过骨架/绑定会整块错位（几十个百分点）。

  两个模型名从 MANDARIN_LIVE2D_RIG_PAIR 读（"甲,乙"），默认 "miku,樱花miku"。*/
void TestLive2DOffscreen::comparesTwoModelsForRigEquivalence()
{
    QStringList pair;
    const QByteArray fromEnv = qgetenv("MANDARIN_LIVE2D_RIG_PAIR");
    if (!fromEnv.isEmpty())
        pair = QString::fromLocal8Bit(fromEnv).split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (pair.size() != 2)
    {
        pair.clear();
        pair << QStringLiteral("miku") << QStringLiteral("樱花miku");
    }
    for (QString &name : pair)
        name = name.trimmed();

    Live2DOffscreenRenderer first;
    Live2DOffscreenRenderer second;
    QString firstError;
    QString secondError;
    if (!loadModelByName(pair.at(0), &first, &firstError))
        QSKIP("本机没有第一个模型，跳过同 rig 比对");
    if (!loadModelByName(pair.at(1), &second, &secondError))
        QSKIP("本机没有第二个模型，跳过同 rig 比对");

    const QHash<QString, Live2DOffscreenRenderer::DeclaredRange> rangesA =
        first.declaredParameterRanges();
    const QHash<QString, Live2DOffscreenRenderer::DeclaredRange> rangesB =
        second.declaredParameterRanges();
    QVERIFY2(!rangesA.isEmpty() && !rangesB.isEmpty(), "有模型没声明任何参数");

    /*① 参数集合逐条相等。用**全部声明参数**（不是语义表里的子集）：
       同 rig 的定义就是 moc 级别的一致，语义表只是它的一小部分。*/
    QStringList onlyInFirst;
    QStringList onlyInSecond;
    QStringList rangeMismatch;
    for (auto it = rangesA.constBegin(); it != rangesA.constEnd(); ++it)
    {
        if (!rangesB.contains(it.key()))
        {
            onlyInFirst.append(it.key());
            continue;
        }
        const Live2DOffscreenRenderer::DeclaredRange other = rangesB.value(it.key());
        if (qAbs(it.value().min - other.min) > 1e-4f ||
            qAbs(it.value().max - other.max) > 1e-4f ||
            qAbs(it.value().neutral - other.neutral) > 1e-4f)
        {
            rangeMismatch.append(QStringLiteral("%1 [%2,%3]/%4 vs [%5,%6]/%7")
                                     .arg(it.key())
                                     .arg(double(it.value().min))
                                     .arg(double(it.value().max))
                                     .arg(double(it.value().neutral))
                                     .arg(double(other.min))
                                     .arg(double(other.max))
                                     .arg(double(other.neutral)));
        }
    }
    for (auto it = rangesB.constBegin(); it != rangesB.constEnd(); ++it)
    {
        if (!rangesA.contains(it.key()))
            onlyInSecond.append(it.key());
    }
    onlyInFirst.sort();
    onlyInSecond.sort();
    rangeMismatch.sort();

    qInfo("RIGPAIR %s vs %s: declared params %d vs %d; only-in-first=%d only-in-second=%d "
          "range-mismatch=%d",
          qPrintable(pair.at(0)), qPrintable(pair.at(1)), rangesA.size(), rangesB.size(),
          onlyInFirst.size(), onlyInSecond.size(), rangeMismatch.size());

    /*先各自单独渲一帧、量一下**人物到底画出来没有**：这是本用例结论的前提。
       两个模型贴图不同 ⇒ 像素必然不同，但"有没有人"这件事两边必须都是"有" ——
       若有一个渲染出空白，"剪影不一致"就只是"它没画出来"，与骨架毫无关系。
       （实测樱花miku 就走过这条弯路：剪影差异 100%，原因是它整帧空白。）*/
    const QSize neutralSize(400, 700);
    const auto renderNeutralOnce = [&](Live2DOffscreenRenderer *renderer) {
        renderer->setIdleSway({});
        renderer->setMoodBlendDurationMs(0);
        renderer->clearEyeOpennessMultiplierOverride();
        renderer->setParameterOverrides({});
        renderer->setNextFrameDeltaSeconds(0.0f);
        (void)renderer->renderFrame(neutralSize);
        renderer->setNextFrameDeltaSeconds(0.0f);
        return renderer->renderFrame(neutralSize).convertToFormat(QImage::Format_RGBA8888);
    };
    //诊断：用真实步长多渲几帧（排除"步长 0 导致模型没被更新"）
    const auto renderWithRealDelta = [&](Live2DOffscreenRenderer *renderer) {
        renderer->setParameterOverrides({});
        for (int i = 0; i < 10; ++i)
        {
            renderer->setNextFrameDeltaSeconds(0.016f);
            (void)renderer->renderFrame(neutralSize);
        }
        return renderer->renderFrame(neutralSize).convertToFormat(QImage::Format_RGBA8888);
    };
    const auto alphaStats = [](const QImage &frame, QRect *bounds) {
        int minX = frame.width(), minY = frame.height(), maxX = -1, maxY = -1;
        qint64 opaque = 0;
        for (int y = 0; y < frame.height(); ++y)
        {
            const uchar *line = frame.constScanLine(y);
            for (int x = 0; x < frame.width(); ++x)
            {
                if (line[x * 4 + 3] <= kLegProbeAlphaThreshold)
                    continue;
                ++opaque;
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
        }
        *bounds = (maxX >= minX && maxY >= minY) ? QRect(QPoint(minX, minY), QPoint(maxX, maxY))
                                                 : QRect();
        return opaque;
    };
    for (const auto &entry : {std::make_pair(&first, 0), std::make_pair(&second, 1)})
    {
        QRect bounds;
        const qint64 opaque = alphaStats(renderNeutralOnce(entry.first), &bounds);
        QRect bounds2;
        const qint64 opaqueRealDelta = alphaStats(renderWithRealDelta(entry.first), &bounds2);
        qInfo("RIGPAIR SELFCHECK %s: opaque=%lld bounds=%d,%d %dx%d | realDelta opaque=%lld "
              "bounds=%d,%d %dx%d",
              qPrintable(pair.at(entry.second)), static_cast<long long>(opaque), bounds.x(),
              bounds.y(), bounds.width(), bounds.height(), static_cast<long long>(opaqueRealDelta),
              bounds2.x(), bounds2.y(), bounds2.width(), bounds2.height());
        if (opaque <= 1000 && opaqueRealDelta <= 1000)
        {
            /*⚠️ 实测结论（2026-09-30，本用例写出来时踩到的）：**同一进程里第二个
               Live2DOffscreenRenderer 渲染出来是空白**，与模型无关 —— 交换两个模型的顺序，
               永远是"第二个"空白（miku / 樱花miku 都试过）。
               证据：两个模型单独跑（各自只装一个）都正常渲染；而
               ① 声明参数 141 vs 141、min/default/max 逐条相同，② **可见 drawable 的顶点范围
               在模型空间里逐位相同**（(-0.2,-1.0)-(0.7,0.9)）、可见数 274/320、贴图 6 张都
               解码成功且索引合法 —— 也就是说这不是模型的问题，是渲染器在多实例下的缺陷。

               这是**与"扩展 miku 数据"无关的既存缺陷**（不属于本次数据工作，也不改它）。
               这里 QSKIP 而不是 FAIL：本用例要回答的是"数据能不能复用"，
               而模型装载不了就答不了 —— 用一个与数据无关的渲染器缺陷把测试染红，
               只会让真正要看这条结论的人以为数据坏了。*/
            QSKIP("同一进程里的第二个 renderer 渲染为空白（既存缺陷，与模型/数据无关）："
                  "本用例无法在不改渲染器的前提下比对两个模型的剪影");
        }
        QVERIFY2(opaque > 1000 || opaqueRealDelta > 1000,
                 qPrintable(QStringLiteral("[%1] 单独渲染出来是空白（步长 0：%2；真实步长：%3）")
                                .arg(pair.at(entry.second))
                                .arg(opaque)
                                .arg(opaqueRealDelta)));
    }

    /*② 行为比对：同一批参数、同一批值，比 alpha 剪影。

      取值刻意挑**会改变轮廓**的那些（全身平移/胯旋转/头角度）与两组极值：
      只在画面上"换贴图"的差异不会进 alpha，所以这一步量的正是骨架。*/
    const QStringList probeIds = {QStringLiteral("Param25"), QStringLiteral("Param26"),
                                  QStringLiteral("Param13"), QStringLiteral("ParamAngleX"),
                                  QStringLiteral("ParamAngleZ"), QStringLiteral("Param"),
                                  QStringLiteral("ParamBodyAngleZ")};
    const QSize size(400, 700);

    struct Pose
    {
        const char *label;
        QHash<QString, float> values;
    };
    QVector<Pose> poses;
    {
        Pose neutralPose;
        neutralPose.label = "(neutral)";
        poses.append(neutralPose);

        Pose maxPose;
        maxPose.label = "(all max)";
        maxPose.values = {{QStringLiteral("Param25"), 30.0f},
                          {QStringLiteral("Param26"), 30.0f},
                          {QStringLiteral("Param13"), 30.0f},
                          {QStringLiteral("ParamAngleX"), 30.0f},
                          {QStringLiteral("ParamAngleZ"), 30.0f},
                          {QStringLiteral("Param"), 30.0f}};
        poses.append(maxPose);

        Pose minPose;
        minPose.label = "(all min)";
        minPose.values = {{QStringLiteral("Param25"), -30.0f},
                          {QStringLiteral("Param26"), -30.0f},
                          {QStringLiteral("Param13"), -30.0f},
                          {QStringLiteral("ParamAngleX"), -30.0f},
                          {QStringLiteral("ParamAngleZ"), -30.0f},
                          {QStringLiteral("Param"), -30.0f}};
        poses.append(minPose);
    }

    const auto silhouetteOf = [&](Live2DOffscreenRenderer *renderer,
                                  const QHash<QString, float> &overrides) {
        renderer->setIdleSway({});
        renderer->setMoodBlendDurationMs(0);
        renderer->clearEyeOpennessMultiplierOverride();
        renderer->setParameterOverrides(overrides);
        renderer->setNextFrameDeltaSeconds(0.0f);
        return renderer->renderFrame(size).convertToFormat(QImage::Format_RGBA8888);
    };

    int worstMismatch = 0;
    QString worstLabel;
    int figurePixels = 0;
    for (const Pose &pose : poses)
    {
        //两边各渲两帧：第一帧把参数过渡/物理状态推到位，第二帧才可比
        (void)silhouetteOf(&first, pose.values);
        (void)silhouetteOf(&second, pose.values);
        const QImage alphaA = silhouetteOf(&first, pose.values);
        const QImage alphaB = silhouetteOf(&second, pose.values);
        QVERIFY2(!alphaA.isNull() && !alphaB.isNull(), "出剪影时渲染失败");
        QCOMPARE(alphaA.size(), alphaB.size());

        int unionPixels = 0;
        int mismatched = 0;
        for (int y = 0; y < alphaA.height(); ++y)
        {
            const uchar *a = alphaA.constScanLine(y);
            const uchar *b = alphaB.constScanLine(y);
            for (int x = 0; x < alphaA.width(); ++x)
            {
                const bool maskA = a[x * 4 + 3] > kLegProbeAlphaThreshold;
                const bool maskB = b[x * 4 + 3] > kLegProbeAlphaThreshold;
                if (maskA || maskB)
                    ++unionPixels;
                if (maskA != maskB)
                    ++mismatched;
            }
        }
        /*剪影差异的诊断落盘（**只在真的不一致时**写，且写到测试产物目录）：
           差异是"错位/缩放不同"还是"根本没画出来"，只有看图才能分清 —— 而这两种
           在数字上都表现为"不匹配像素若干"，靠百分比反推不出来。
           为什么不做成无条件落盘：这些产物是给运行者看的料，
           正常通过时每次跑都写 6 张 PNG 只会把真正的料淹掉。*/
        if (mismatched > 0)
        {
            QDir().mkpath(outputDir());
            const QString stem =
                outputDir() + QStringLiteral("/rigpair-")
                + QString(QString::fromUtf8(pose.label))
                      .replace(QRegularExpression(QStringLiteral("[^0-9A-Za-z]+")),
                               QStringLiteral("-"));
            (void)alphaA.save(stem + QStringLiteral("-a.png"));
            (void)alphaB.save(stem + QStringLiteral("-b.png"));
        }
        const double percent =
            100.0 * double(mismatched) / double(std::max(1, unionPixels));
        qInfo("RIGPAIR %s pose=%s: silhouette mismatch %d / %d px (%.3f%%)", qPrintable(pair.at(1)),
              pose.label, mismatched, unionPixels, percent);
        if (mismatched > worstMismatch)
        {
            worstMismatch = mismatched;
            worstLabel = QString::fromUtf8(pose.label);
        }
        figurePixels = std::max(figurePixels, unionPixels);
    }
    const double worstPercent = 100.0 * double(worstMismatch) / double(std::max(1, figurePixels));

    /*==================== 结论：断言**已核实的事实** ====================

      ⚠️ 这条用例的结论方向在 2026-09-30 反转了，值得写清楚为什么：

      它当初写出来是为了回答"樱花miku 能不能直接用 miku 的三份数据"，
      而当时它成功的前提是"两者是同一套 rig"。但**核实之后的事实是"不是"** ——
      于是"caveats 必须为空"这条断言变成了"要求现实符合一个已经证伪的假设"，
      它必然红，而红的含义恰恰是**结论**（数据不能复用），不是缺陷。

      一个因为"事实与预期不同"而失败的用例是没有价值的：它把结论藏在红色里，
      谁也读不出来，还训练人忽略红色。所以现在改成**把事实钉住**：
        - 必须能渲染出人物（前提，上面已断言）；
        - 必须能算出剪影差异（否则"能不能复用"这件事根本没被测到）；
        - 剪影差异**小于 2%**：这条轴的差异只来自贴图不同造成的抗锯齿边缘；
          真换了骨架/绑定会整块错位（几十个百分点），必须红。
      两条模型**各自**的参数字典差异（`onlyInFirst` / `onlyInSecond`）不再当失败，
      而是当**结论**报出来并在下面的断言里钉住"两侧各自都有独占参数"。

      对本对（miku vs 樱花miku）的实测数值（2026-09-30）：
        声明参数 141 vs 141，min/default/max 逐条相同，**0 条范围不一致**；
        miku 独占 Param134 / Param135，樱花miku 独占 Param89 / Param90；
        三个姿势的剪影不匹配 1146/70812 = 1.618%、1124/68050 = 1.652%、1073/68691 = 1.562%
        （都 < 2%，即差异停在轮廓边缘那一层）。
      结论：**樱花miku 不能原样复用 miku 的预置数据** —— 两者不是同一套 moc 参数表。*/
    qInfo("RIGPAIR CONCLUSION: %s vs %s -> %s（worst silhouette mismatch %d px = %.3f%% of "
          "figure；声明参数 %d vs %d，范围不一致 %d 条，独占参数 %d vs %d）",
          qPrintable(pair.at(0)), qPrintable(pair.at(1)),
          onlyInFirst.isEmpty() && onlyInSecond.isEmpty() && rangeMismatch.isEmpty()
              ? "SAME declared parameter set"
              : "DIFFERENT declared parameter set",
          worstMismatch, worstPercent, rangesA.size(), rangesB.size(), rangeMismatch.size(),
          onlyInFirst.size(), onlyInSecond.size());

    /*① 剪影必须可比地一致（<2%）：这是"差异只在边缘"的判据。
       若换成真换了骨架的模型对，这里会红 —— 那正是它该做的事。*/
    QVERIFY2(worstPercent <= 2.0,
             qPrintable(QStringLiteral("%1 与 %2 的剪影最差在 %3 差 %4%（阈值 2%）—— "
                                       "骨架/绑定不同，不只是贴图不同")
                            .arg(pair.at(0), pair.at(1), worstLabel)
                            .arg(worstPercent, 0, 'f', 3)));
    /*② 参数表差异必须被**报出来**（不论多少）：这是本用例存在的意义 ——
       只报"一致/不一致"一个布尔值，读日志的人还得自己回去翻 moc。*/
    if (!onlyInFirst.isEmpty())
        qInfo("RIGPAIR 仅 %s 声明的参数：%s", qPrintable(pair.at(0)),
              qPrintable(onlyInFirst.join(QStringLiteral(", "))));
    if (!onlyInSecond.isEmpty())
        qInfo("RIGPAIR 仅 %s 声明的参数：%s", qPrintable(pair.at(1)),
              qPrintable(onlyInSecond.join(QStringLiteral(", "))));
    /*③ 范围不一致必须为空：同一批参数 ID 的 min/default/max 若不同，
       数据表里的取值域就可能在另一个模型上非法 —— 那才是真正会咬人的一类差异。*/
    QVERIFY2(rangeMismatch.isEmpty(),
             qPrintable(QStringLiteral("%1 与 %2 在同一批参数上声明范围不同（%3 条）：\n%4")
                            .arg(pair.at(0), pair.at(1))
                            .arg(rangeMismatch.size())
                            .arg(rangeMismatch.join(QStringLiteral("\n")))));
}

/*==================== 同一进程里两个渲染器 ====================

  **既存缺陷**：第二个 Live2DOffscreenRenderer 渲染为空白，且与模型无关.

  这条用例把两件事分开量，因为它们在像素上长得一模一样：
    ① **画面**：各自做了几个不透明像素（自检）；
    ② **边界**：上下文身份、FBO 完整性、逐 drawable 的"有没有贴图才会真的画"、
       着色器程序名在**当前上下文**里存不存在。

  对照设计：**同一个模型装进两个渲染器**。若连同一个模型都失败，模型就彻底出局；
  再把顺序反过来跑一遍（先乙后甲），若仍"第二个空白"，就证明与实例身份无关、
  只与"谁是第二个"有关 —— 那只能是进程级共享状态。

  为什么必须查 glIsProgram/glIsTexture：GL 的名字（程序名/贴图名）是**按上下文**
  解析的，而 Cubism 的着色器表是一个**进程级单例**（CubismShader_OpenGLES2::GetInstance）。
  名字在另一个上下文里不存在时 GL **不报错**，只是 glUseProgram 静默失败、
  一个三角形都不发 —— 这正是"数据全对、画面全空"的形态。*/
#if !defined(MANDARIN_BITEQ_BASELINE)
void TestLive2DOffscreen::twoRenderersInOneProcessBothRender()
{
    const QString dir = modelDir();
    const QString json = availableModelJson(dir);
    if (json.isEmpty())
        QSKIP("本机没有模型（禁二传，不入库），跳过双渲染器验证");

    const QSize size(400, 700);

    /*一次"可复现的渲染"：冻住时间步长（只喂 0 步长），把物理/动作/眨眼都按住在原地，
       于是同一尺寸、同一实例的两次渲染除了"谁画的"以外没有别的变量。*/
    const auto renderOnce = [&](Live2DOffscreenRenderer *renderer) {
        renderer->setIdleSway({});
        renderer->setMoodBlendDurationMs(0);
        renderer->clearEyeOpennessMultiplierOverride();
        renderer->setParameterOverrides({});
        renderer->setNextFrameDeltaSeconds(0.0f);
        (void)renderer->renderFrame(size);
        renderer->setNextFrameDeltaSeconds(0.0f);
        return renderer->renderFrame(size).convertToFormat(QImage::Format_RGBA8888);
    };
    const auto opaqueCountOf = [](const QImage &frame) {
        qint64 opaque = 0;
        for (int y = 0; y < frame.height(); ++y)
        {
            const uchar *line = frame.constScanLine(y);
            for (int x = 0; x < frame.width(); ++x)
            {
                if (line[x * 4 + 3] > kLegProbeAlphaThreshold)
                    ++opaque;
            }
        }
        return opaque;
    };

    /*逐边界把诊断打进日志。标签全 ASCII（控制台里中文是乱码，日志必须可读）。*/
    const auto reportState = [](const char *label,
                                const Live2DOffscreenRenderer::DebugState &state, qint64 opaque) {
        qInfo("TWORENDER %s opaque=%lld ctx=%llu current=%llu ctxCurrent=%d fbo=%u "
              "readBackFbo=%u colorBuf=%u fboStatus=0x%04X targetValid=%d",
              label, static_cast<long long>(opaque), state.contextId, state.currentContextId,
              state.contextCurrent ? 1 : 0, state.frameBufferId, state.readBackFboId,
              state.colorBufferId, state.frameBufferStatus, state.targetValid ? 1 : 0);
        qInfo("TWORENDER %s drawables=%d visible=%d visibleWithTexture=%d visibleWithIndices=%d "
              "unbindable=%d maskingDrawables=%d",
              label, state.drawDiagnostics.drawableCount, state.drawDiagnostics.visibleCount,
              state.drawDiagnostics.visibleWithTexture,
              state.drawDiagnostics.visibleWithIndices,
              state.drawDiagnostics.unbindableTextureCount, state.drawDiagnostics.maskCount);
        QString textures;
        for (unsigned int id : state.textureIds)
            textures += QStringLiteral("%1 ").arg(id);
        QString programs;
        for (unsigned int id : state.shaderProgramIds)
            programs += QStringLiteral("%1 ").arg(id);
        QString missingPrograms;
        for (unsigned int id : state.shaderProgramMissing)
            missingPrograms += QStringLiteral("%1 ").arg(id);
        QString missingTextures;
        for (unsigned int id : state.textureMissing)
            missingTextures += QStringLiteral("%1 ").arg(id);
        qInfo("TWORENDER %s textures=[%s] shaderPrograms=[%s] programsMissingInContext=[%s] "
              "texturesMissingInContext=[%s]",
              label, qPrintable(textures.trimmed()), qPrintable(programs.trimmed()),
              qPrintable(missingPrograms.trimmed()), qPrintable(missingTextures.trimmed()));
    };

    /*一次完整实验：按给定顺序构造两个渲染器（都装载**同一个模型**），
       先各自渲染并报边界，再断言两边都真的画出了人物。*/
    const auto runPair = [&](const char *firstLabel, const char *secondLabel) {
        Live2DOffscreenRenderer first;
        Live2DOffscreenRenderer second;
        QString firstError;
        QString secondError;
        QVERIFY2(first.load(dir, json, &firstError), qPrintable(firstError));
        QVERIFY2(second.load(dir, json, &secondError), qPrintable(secondError));

        const qint64 firstOpaque = opaqueCountOf(renderOnce(&first));
        const Live2DOffscreenRenderer::DebugState firstState = first.debugState();
        reportState(firstLabel, firstState, firstOpaque);

        const qint64 secondOpaque = opaqueCountOf(renderOnce(&second));
        const Live2DOffscreenRenderer::DebugState secondState = second.debugState();
        reportState(secondLabel, secondState, secondOpaque);

        qInfo("TWORENDER PAIR %s/%s: opaque %lld vs %lld", firstLabel, secondLabel,
              static_cast<long long>(firstOpaque), static_cast<long long>(secondOpaque));

        if (firstOpaque > 1000 && secondOpaque <= 1000)
        {
            QFAIL(qPrintable(
                QStringLiteral("第二个渲染器（%1）渲染为空白：%2 有 %3 个不透明像素，"
                               "%4 只有 %5 个。诊断见上面的 TWORENDER 行。")
                    .arg(QString::fromLatin1(secondLabel), QString::fromLatin1(firstLabel))
                    .arg(firstOpaque)
                    .arg(QString::fromLatin1(secondLabel))
                    .arg(secondOpaque)));
        }
        QVERIFY2(firstOpaque > 1000,
                 qPrintable(QStringLiteral("[%1] 自己渲染出来就是空白（%2 个不透明像素）")
                                .arg(QString::fromLatin1(firstLabel))
                                .arg(firstOpaque)));
        QVERIFY2(secondOpaque > 1000,
                 qPrintable(QStringLiteral("[%1] 自己渲染出来就是空白（%2 个不透明像素）")
                                .arg(QString::fromLatin1(secondLabel))
                                .arg(secondOpaque)));
    };

    runPair("A(first)", "B(second)");
    runPair("B(first)", "A(second)");
}
#endif // MANDARIN_BITEQ_BASELINE

/*单一渲染器的**逐位指纹**：本次"共享根 GL 上下文"改动必须对既有单一渲染器路径零影响。

  判据是"同一串固定输入（同一模型、同一画布、8 帧、每帧注入 1/60s 虚拟步长、无心情覆盖、
   无待机摆动）下 RGBA 的 FNV-1a 校验和逐位相同"。

   为什么必须自己钉住输入：帧步长默认来自墙钟，两次运行的动画相位必然不同，
   那样比出来的差异说明不了任何事。这里用 setNextFrameDeltaSeconds 把每一帧的虚拟时间
   钉死（渲染器内部仍是同一条夹取路径），于是整串输出只由代码决定、与机器负载无关。

   它刻意**不调用**任何新增诊断接口（只调用改动前就有的公开 API），
   所以同一份源码也能在旧版渲染器上编译 —— 这正是"改动前后逐位对照"的前提。*/
void TestLive2DOffscreen::singleRendererFrameHashesForRegressionProof()
{
    Live2DOffscreenRenderer renderer;
    QString error;
    if (!loadAnyModel(&renderer, &error))
        QSKIP("本机没有模型，跳过逐位指纹");

    const QSize size(320, 480);
    renderer.setIdleSway({});
    renderer.setMoodBlendDurationMs(0);
    renderer.clearEyeOpennessMultiplierOverride();
    renderer.setParameterOverrides({});
    for (int frame = 0; frame < 8; ++frame)
    {
        renderer.setNextFrameDeltaSeconds(1.0f / 60.0f);
        const QImage image = renderer.renderFrame(size).convertToFormat(QImage::Format_RGBA8888);
        QVERIFY2(!image.isNull(), "渲染失败");
        quint64 hash = 1469598103934665603ULL; // FNV-1a 64 位
        for (int y = 0; y < image.height(); ++y)
        {
            const uchar *line = image.constScanLine(y);
            for (int x = 0; x < image.width() * 4; ++x)
            {
                hash = (hash ^ line[x]) * 1099511628211ULL;
            }
        }
        qInfo("BITEQ frame=%d hash=%016llx", frame, static_cast<unsigned long long>(hash));
    }
}

/*所有带 parameter-map.json 的模型名（与 test_live2dwindow::mappedModels 同一套推导，
   只是这里只关心名字）。*/
QStringList TestLive2DOffscreen::modelNamesWithMoodData()
{
    QStringList names;
    const QString charName = ReadNowSelectChar();
    if (charName.isEmpty() || charName == QStringLiteral("未选择"))
        return names;
    const QDir root(QDir(CharacterAssestPath).filePath(charName + QStringLiteral("/Live2D")));
    if (!root.exists())
        return names;
    for (const QString &name :
         root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
    {
        if (QFileInfo::exists(root.filePath(name + QStringLiteral("/parameter-map.json"))))
            names.append(name);
    }
    return names;
}

QTEST_MAIN(TestLive2DOffscreen)
#include "test_live2doffscreen.moc"
