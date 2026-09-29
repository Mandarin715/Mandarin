#include <QtTest>

#include "../GlobalConstants.h"
#include "../utils/Live2DMoodPreset.h"
#include "../windows/character/live2dcharacterwindow.h"

#include <QBitmap>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QRegion>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <vector>

/*Live2D 立绘窗口的端到端验证：真正要证伪的是「窗口只是个空白矩形」——
  也就是渲染帧确实被登记进窗口层、确实被画出来、并且穿透/命中判定确实来自模型 alpha。

  只用**公开 API** 断言（isModelLoaded / contentSize / renderedImage / renderSize / mask），
  不翻私有成员、不假设渲染器内部实现。

  模型不入库（授权禁二传），本机没有模型时**跳过**而不是失败；
  可用环境变量 MANDARIN_LIVE2D_MODEL_DIR 指定别的模型目录。

  **配置隔离（重要）**：本测试绝不碰用户的真实 `config.ini`。
  initTestCase() 把配置读取重定向到 QTemporaryDir 里的临时文件
  （经 MANDARIN_CONFIG_INI，见 Live2DCharacterWindow 的 settingsPath()），
  测试要什么帧率/缩放就往临时文件里写。以前的做法是"改真实配置再恢复"，
  实测已经因为中途退出/异常把用户的值永久覆盖过一次 —— 测试永远不该有机会写它。

  关于坐标系（踩过的坑，写在这里免得再踩）：窗口的 paintEvent 是按**设备像素**把帧画满的，
  所以「图像像素坐标」与「窗口设备无关坐标」一一对应；而 renderedImage() 给的是物理像素帧，
  两者之间要按 QImage::devicePixelRatio() 换算。测试里凡是跨这两种量纲的地方都显式换算。*/
class TestLive2DWindow : public QObject
{
    Q_OBJECT

  private slots:
    /*整套测试共用的临时配置：用完即删，用户真实 config.ini 全程只读*/
    void initTestCase();
    void cleanupTestCase();

    void shapesWindowFromRenderedModel();
    void animatesAcrossFrames();
    void keepsWholeFigureInsideCanvas();
    void fillsFrameInBothAxes();
    void loadsModelByDirectoryName();
    void reportsFullPipelineFrameCost();
    /*回归断言：1.0x 下「登记帧 → 上屏」必须是精确拷贝（画布设备像素对齐）。*/
    void paintsRegisteredFrameWithoutResampling();
    /*诊断用：量「清晰度」在渲染管线的哪一段被吃掉。**只测量、不断言阈值。**/
    void reportsSharpnessAcrossScales();

    /*---------- 心情 → 表情（本阶段新增） ----------*/
    /*心情词表与实际 Tachie/ 目录双向核对：少一个别名 = 那个心情会掉到 neutral*/
    void moodPresetCoversEveryTachieMood();
    /*[范围纪律] parameter-map.json 的 min/max/neutral 必须**逐条等于**模型自己声明的
      最小值/最大值/默认值 —— 范围是模型的事实，不是可以手抄的东西*/
    void parameterMapRangesMatchModelDeclarations();
    /*初稿数值不许越界、不许碰驱动器/物理占用的参数*/
    void moodPresetValuesWithinParameterRanges();
    /*词表外的词必须回退 neutral（而不是"保持上一种情绪"）*/
    void unknownMoodFallsBackToNeutral();
    /*换心情真的会改变渲染出来的帧，且预设值真的落到模型参数上*/
    void applyingMoodChangesRenderedFrame();
    /*还没装载模型时施加心情也必须安全（启动期时序：AI 的第一句可能早于首次布局）*/
    void applyingMoodWithoutModelIsSafe();
    /*[眨眼合成] 情绪的睁闭眼必须是**乘在眨眼结果上**的：闭眼看得见、眨眼还活着、
      不碰眼睛的心情对眨眼零影响。旧实现（绝对覆盖后又被眨眼盖掉）在这条上必然失败。*/
    void moodEyeOpennessComposesWithBlink();
    void moodIgnoringEyesLeavesBlinkUnchanged();
    /***残留情绪的回归测试**：happy → neutral 后参数必须全部回到中立*/
    void switchingMoodDoesNotAccumulate();
    /*校准素材：14 个原型各出一张固定区域的脸部裁切图（只出图，不判断好坏）*/
    void rendersMoodArchetypeCalibrationSheet();

    /*---------- 心情过渡与说话嘴巴的窗口层接线（本阶段新增） ----------*/
    /*character/live2dMoodBlendMs 必须真的被读出来并夹取（硬编码 200ms 过不了）*/
    void moodBlendDurationComesFromConfig();
    /*窗口的 SetSpeaking（Dialog::requestSpeakState 的落点）必须一路走到渲染器：
       嘴巴随时间开合、且回落到心情值。这条钉住"接线"，渲染器级的数学另有专测。*/
    void setSpeakingDrivesMouthFlapThroughWindow();

  private:
    static QString modelDir();
    static QString modelDirFor(const QString &name);
    /*用户实际在用的模型名（config.ini 的 character/live2dModel，当前是 atri）*/
    static QString preferredModelName();
    /*按当前用户配置（CharSelect + live2dModel）装载情绪预设。
      返回 false 表示数据不在（模型/JSON 都在 Documents/ 下，禁二传不入库）→ 调用方 QSKIP。*/
    static bool loadConfiguredMoodPreset(Live2DMoodPreset *preset);
    /*情绪数据目录（= Live2DMoodPreset 推导出来的那一层，parameter-map.json 在这里）。
      注意它**不是**模型文件目录 —— 本机模型在 Documents/Mandarin/Live2D/<模型名>，
      情绪数据在 Documents/Mandarin/Character/Assets/<角色>/Live2D/<模型名>。
      用 renderer 自己的装载去打开同一个 moc（不另写一套 moc 解析，也就不会把
      "数据对不对"这件事绑死在测试自己的读取实现上）。返回 false = 本机没有。*/
    static bool openConfiguredModel(Live2DOffscreenRenderer *renderer);
    /*读回值能直接断言"等于预设值"的参数清单。
      排除在外的、以及**为什么**排除 —— 这几条本身就是本阶段的实测结论：
        - ParamMouthForm：moc 自己把范围声明成 **[-1, 0] / 默认 -0.5**。预设里写的
          happy +0.9 / excited +0.8 / confident +0.6 都**越界**，会被 Core 夹成 0.0
          （范围核对测试 parameterMapRangesMatchModelDeclarations 现在钉着这件事，
          越界值也在装载时汇总成一条警告）—— 这是**数据待校准**，不是覆盖没生效，
          所以这里排除它；偏差仍会打进下面"未落地清单"的日志，不会悄悄消失；
        - ParamEyeLOpen / ParamEyeROpen：它们是**乘数**语义（情绪值 × 眨眼值），
          读回值逐帧随眨眼变化，不能与预设值直接比较 —— 由 moodEyeOpennessComposesWithBlink
          专门验证（那里同时看最终值与眨眼原始值）；
        - ParamAngleX/Y/Z、ParamBodyAngleX：呼吸驱动器每帧**加**一个摆动量（加性），
          读回值是"预设值 + 摆动"，不能与预设值直接比较；
        - ParamBreath / ParamHair*：物理/呼吸所有，预设根本不写（见 Live2DMoodPreset）。*/
    static QStringList readBackParameters();

    /*切换心情后**等过渡走完**（并且真的渲出一帧），然后才让调用方去读参数/读帧。

      为什么非等不可（两个各自独立的理由，缺一都会让断言读到错误的东西）：
        ① 覆盖值现在是**逐帧过渡**的（见 Live2DOffscreenRenderer::setMoodBlendDurationMs）：
           刚 reloadContent 完读回来的还是上一个心情的值 —— 直接断言"等于预设值"必然失败，
           而那不是 bug，是过渡正好走到一半；
        ② applyMood 不再同步渲染一帧（那 4ms 主线程卡顿正是本阶段要修的"顿"的一部分），
           所以要等帧循环把新参数渲出来。窗口隐藏时帧循环是停的，这里就自己渲。

      ⚠️ 这不等于"把断言放宽"：等待之后的断言仍然是**精确**的（参数必须等于预设值、
      帧必须与切换前不同），只是把"什么时候读"从一个随机的瞬间改成"过渡已经结束"。
      过渡本身的行为由 test_live2doffscreen 的 BLEND 系列单独钉住，
      以及本文件 moodBlendDurationComesFromConfig 钉住配置键。

      为什么要顺带推动一帧：过渡参数只在 renderFrame 的 tick 里推进，光等墙钟不动帧
      是永远走不完的（帧循环在窗口隐藏时也是停的）。

      blendMs：本次要等的过渡时长；传 0 则用**当前生效**的 200ms 默认值。
      返回 false 表示等待期间一帧都渲不出来（真实的失败，调用方应当断言）。*/
    static bool waitForMoodSettle(Live2DCharacterWindow *window, int blendMs = 0,
                                 int extraFrames = 3);

    /*在一串真实帧上采样眼睛（与眨眼原始值）。

      为什么必须跨帧采样：眨眼是**时间**上的事件（默认 4s 间隔里眨一次），
      单帧既证明不了"闭眼看得见"（那一帧眨眼可能恰好全睁），也证明不了"眨眼还活着"。
      返回值的 min/max 就是这两件事的证据：min 小 = 闭眼落到了屏幕上；
      blinkMin < blinkMax = 眨眼在整个采样窗口里仍在变化。

      只记**数值区间**、不留帧：这几百帧的像素对照既贵又不可比（相位不同），
      像素证据由相邻帧的 renderEyeProbe 负责。
      eyeParameterId：要采样的眼睛参数（ParamEyeLOpen 等）
      samples：采样帧数；moodName 非空时先切到该心情（reloadContent 会立刻出一帧）。*/
    struct EyeBlinkSamples
    {
        int frames = 0;
        float composedMin = 0.0f;
        float composedMax = 0.0f;
        float blinkMin = 1.0f;
        float blinkMax = 1.0f;
    };
    static EyeBlinkSamples sampleEyeOpenness(Live2DCharacterWindow *window,
                                             const QString &eyeParameterId,
                                             const QString &moodName, int samples,
                                             int waitMs = 100);

    /*再走一帧并读回"这一帧"的三样东西：像素帧、眼睛参数最终值、眨眼原始值。

      eyeMultiplier：这一帧的睁闭眼乘数（1.0 = 眼睛完全交给眨眼；<1 = 情绪把眼睛压小）。
      **必须显式给**，因为它决定了帧的样子 —— renderEyeProbe 内部在读值之前会把
      乘数设成这个值再渲染，所以"读到的值"与"渲出的帧"必定属于同一帧。*/
    /*返回 false = 这一帧没渲出来（窗口被销毁/渲染失败），调用方不能拿空帧去比对。
       为什么要这个返回值：下面所有"相邻帧"证据都必须成立在**真的渲出了一帧**之上，
       否则 QImage() 的尺寸比较会给出与时间无关的假结论。*/
    static bool renderEyeProbe(Live2DCharacterWindow *window,
                               const QHash<QString, float> &eyeMultiplier,
                               const QString &eyeParameterId, QImage *frame, float *composed,
                               float *blink);
    /*脸部裁切区：横取人物包围盒中间 45%、纵取上部 22%（与清晰度诊断同一套比例，
      那里已经证实"人物包围盒上部"就是脸）。14 张对照图**共用同一个 QRect**。*/
    static QRect faceRegionOfInterest(const QRect &figureBounds);

    QTemporaryDir m_tempDir;
    QString m_tempConfigPath;
    /*扫描一帧，返回不透明像素数与一个确定不透明的点（画布中部，避开边缘羽化）。
      点在图像**像素**坐标系里。*/
    static int findOpaquePoint(const QImage &frame, QPoint *opaquePoint);

    /*两帧之间「逐通道差值超过阈值」的像素数。用于「模型到底动没动」这条观察量：
      冻结的模型两帧逐位相同，差值恒为 0。*/
    static qint64 countDifferingPixels(const QImage &a, const QImage &b, int channelDelta);

    /*一帧里 alpha > threshold 的像素包围盒；没有任何这样的像素时返回 false。
      bbox = (minX, minY) 到 (maxX, maxY)，单位是图像**像素**。*/
    static bool opaqueBounds(const QImage &frame, int alphaThreshold, QRect *bounds);

    /*在 figureBounds 内找一个**确实实心**的点：该点连同 kSolidRadius 邻域全部 alpha 达标。
      为什么要邻域而不是单点：单点可能落在羽化边缘上，用它当"人物身上的点"会偶发假失败。
      点坐标写在 output 里，单位是 frame 的**像素**。*/
    static constexpr int kSolidRadius = 2;
    static bool findSolidPoint(const QImage &frame, const QRect &figureBounds,
                               int alphaThreshold, QPoint *output);

    /*==========================================================
      清晰度诊断（reportsSharpnessAcrossScales 专用）
      ==========================================================

      为什么这些量要一起报：单看"某个像素"完全分不清模糊来自哪里。
      - 渲染器帧（renderedImage）与最终抓图（grab）在同一像素尺寸下对比，
        才能判定 paintEvent 那一步到底有没有重采样（边界 D）；
      - "只量 alpha>=250"这个掩码本身也会被重采样改变：图被缩小（下采样）时
        羽化边缘会被吃得更多，掩码随之缩小，指标读数就跟着变。所以 D 的对比必须
        额外用「双方都完全不透明的固定矩形」再量一次，把"掩码差异"从"真的被模糊"里剥出来。*/
    struct SharpnessStats
    {
        /*只统计 alpha >= maskAlpha 且 4 邻域也都达标的像素（透明背景/羽化边缘不参与）*/
        double laplacianVariance = 0.0; // 4 邻域 Laplacian 的方差
        double meanGradient = 0.0;      // 中心差分梯度幅值的均值
        double luminanceStdDev = 0.0;   // 亮度标准差：对照量，说明"图像本身有多少内容"
        qint64 measuredPixels = 0;      // 进入统计的像素数（= 掩码面积）
        QRect opaqueBounds;             // alpha>=maskAlpha 的包围盒（不管邻域）
        /*整图测量时（restrictTo 为空）顺手求出的"自身与四邻域都 alpha>=maskAlpha"
           的像素并集。D 的对比用它当固定矩形，保证矩形里每个像素真的都完全不透明。*/
        QRect solidFill;
        QSize imageSize;
    };

    /*只统计**完全不透明**的像素：alpha >= 250。
      半透明羽化边缘与透明背景都不参与，所以指标量的是人物身上的纹理锐度，
      而不是"背景有多大片"。透明像素的 RGB 在未预乘 RGBA 里可能是 0，
      混进统计会把读数彻底污染 —— 这是本函数存在的第一个理由。*/
    static constexpr int kOpaqueMaskAlpha = 250;

    /*量一张图的锐度。maskAlpha 之外还可以给一个固定矩形 rect（图像像素坐标）：
       给了就只量那个矩形（用来把"掩码被平滑"这个变量从对比里去掉）。*/
    static SharpnessStats measureSharpness(const QImage &image, int maskAlpha,
                                          const QRect &restrictTo = QRect());

    /*两张**同尺寸**图的逐通道最大绝对差、以及有多少像素至少有一个通道不同。
       diffPixels 按"任一通道不同"计一次。maxChannelDiff<0 表示尺寸不一致、无法比较。*/
    static qint64 comparePixels(const QImage &expected, const QImage &actual,
                                int *maxChannelDiff);
};

QString TestLive2DWindow::modelDir()
{
    const QByteArray fromEnv = qgetenv("MANDARIN_LIVE2D_MODEL_DIR");
    if (!fromEnv.isEmpty())
        return QString::fromLocal8Bit(fromEnv);
    return QDir(Live2DModelRootPath).filePath(QStringLiteral("miku"));
}

QString TestLive2DWindow::modelDirFor(const QString &name)
{
    return QDir(Live2DModelRootPath).filePath(name);
}

/*「用户实际在用的模型」= config.ini 里的 character/live2dModel（当前是 atri）。
  动画/裁切这类"用户看得见"的结论必须在这个模型上得出，不能只在 miku 上测。
  没有该目录时返回空，调用方 QSKIP。*/
QString TestLive2DWindow::preferredModelName()
{
    QSettings settings(IniSettingPath, QSettings::IniFormat);
    const QString configured =
        settings.value(QStringLiteral("character/live2dModel")).toString().trimmed();
    if (!configured.isEmpty() && QFileInfo::exists(modelDirFor(configured)))
        return configured;
    return QStringLiteral("atri");
}

/*情绪数据（parameter-map.json / presets/moods.json）与模型一样在用户数据区，
  禁二传、不入库：拿不到就当"本机没这套数据"，调用方 QSKIP 而不是失败。

  注意预设的**路径推导**（Live2DMoodPreset::resolveModelDir）读的是**真实** config.ini 的
  character/CharSelect（+ CharacterAssestPath + 模型名）：那是用户数据实际所在的位置。
  MANDARIN_CONFIG_INI 的重定向只管 fps/scale —— 如果这里也吃它，测试就会在"临时 ini 里
  没有 CharSelect"时把功能静默关掉，看着是跳过、实际什么都没验证。*/
bool TestLive2DWindow::loadConfiguredMoodPreset(Live2DMoodPreset *preset)
{
    if (preset == nullptr)
        return false;
    const QString modelName = preferredModelName();
    if (modelName.isEmpty())
        return false;
    return preset->load(modelName);
}

QStringList TestLive2DWindow::readBackParameters()
{
    // ParamMouthForm / ParamEye{L,R}Open 故意不在列：理由见声明处注释
    return {QStringLiteral("ParamEyeLSmile"), QStringLiteral("ParamEyeRSmile"),
            QStringLiteral("ParamCheek"), QStringLiteral("ParamBrowLY"),
            QStringLiteral("ParamBrowRY")};
}

bool TestLive2DWindow::waitForMoodSettle(Live2DCharacterWindow *window, int blendMs,
                                         int extraFrames)
{
    if (window == nullptr)
        return false;

    /*每次新建窗口时过渡时长的默认值就是 200ms（临时配置里只写 fps/scale），
       所以这里不必去问窗口要时长；非默认值的用例显式把 blendMs 传进来。*/
    constexpr int kDefaultBlendMs = 200;
    const int settleMs = (blendMs > 0 ? blendMs : kDefaultBlendMs) + 120;

    QElapsedTimer clock;
    clock.start();
    int rendered = 0;
    while (clock.elapsed() < settleMs || rendered < extraFrames)
    {
        if (clock.elapsed() > settleMs + 2000)
            break; //兜底：窗口一直渲不出帧时别把测试吊死，交给调用方断言
        if (window->renderFrameNow())
            ++rendered;
        QTest::qWait(10);
    }
    return rendered > 0;
}

TestLive2DWindow::EyeBlinkSamples
TestLive2DWindow::sampleEyeOpenness(Live2DCharacterWindow *window, const QString &eyeParameterId,
                                    const QString &moodName, int samples, int waitMs)
{
    EyeBlinkSamples result;
    if (window == nullptr || samples <= 0)
        return result;

    if (!moodName.isEmpty())
    {
        window->reloadContent(moodName); //会立刻出一帧（本函数随后每帧都取一次）
        /*⚠️ 但那一帧**不保证**已经带上新心情：覆盖值是逐帧过渡的，而且 applyMood 不再同步
           渲染。这里先等过渡走完，后面的采样窗口里读到的才是这个心情真正该有的值
           （不然前几十帧采到的是"上一个心情 → 这个心情"的半路值，区间会被污染）。*/
        waitForMoodSettle(window);
    }

    /*每帧之间等的毫秒数（默认 100）决定"采样窗口在**虚拟时间**里有多长"。
      眨眼间隔是随机 0~7s 一次（默认 SetBlinkingInterval(4.0)），
      默认值 100ms × 130 帧 = 13s 才足以稳过一次眨眼；
      只验"乘数恒等"的用例不需要覆盖眨眼，可以调短（见各调用点的说明）。*/
    constexpr int kDefaultWaitMs = 100;
    const int sampleWaitMs = waitMs > 0 ? waitMs : kDefaultWaitMs;

    bool first = true;
    for (int index = 0; index < samples; ++index)
    {
        /*⚠️ 必须真的等一小会儿再渲下一帧。帧的时间步长来自 QElapsedTimer 的**墙钟**，
           这里连着调 renderFrameNow() 的话两帧之间只隔几微秒 → 眨眼状态机根本走不动，
           采样窗口再长也只能看到一个常量（实测瞎眼：眨眼原始值区间 0 宽）。
           这不是"测试技巧"：眨眼本来就是时间上的事件，不推进时间就看不到它。*/
        QTest::qWait(sampleWaitMs);
        if (!window->renderFrameNow())
            break;
        const float composed = window->parameterValue(eyeParameterId);
        const float blink = window->blinkValue();
        if (first)
        {
            result.composedMin = result.composedMax = composed;
            result.blinkMin = result.blinkMax = blink;
            first = false;
        }
        else
        {
            result.composedMin = std::min(result.composedMin, composed);
            result.composedMax = std::max(result.composedMax, composed);
            result.blinkMin = std::min(result.blinkMin, blink);
            result.blinkMax = std::max(result.blinkMax, blink);
        }
        ++result.frames;
    }
    return result;
}

bool TestLive2DWindow::renderEyeProbe(Live2DCharacterWindow *window,
                                      const QHash<QString, float> &eyeMultiplier,
                                      const QString &eyeParameterId, QImage *frame, float *composed,
                                      float *blink)
{
    if (window == nullptr)
        return false;
    //先定这一帧的睁闭眼乘数，再渲染：读到的值与渲出的帧才属于同一帧
    window->setEyeOpennessMultiplierForTest(eyeMultiplier);
    if (!window->renderFrameNow())
        return false;
    if (frame != nullptr)
        *frame = window->renderedImage();
    if (composed != nullptr)
        *composed = window->parameterValue(eyeParameterId);
    if (blink != nullptr)
        *blink = window->blinkValue();
    return true;
}

/*把"眼睛全睁"的乘数（= 眨眼自己说了算，值恒为 1）+ "眼睛闭到某心情的开度"
   各做一份，供 renderEyeProbe 交替使用。key 是**真实参数 ID**（ParamEyeLOpen 等）。*/
static QHash<QString, float> eyeMultiplierFrom(const QStringList &eyeParameterIds, float value)
{
    QHash<QString, float> multipliers;
    for (const QString &id : eyeParameterIds)
        multipliers.insert(id, value);
    return multipliers;
}

namespace {
/*==================== 眨眼合成用例里"相邻帧"这件事的前提（实测结论，别再踩）====================

  test_live2dwindow 里所有"眼睛闭下去有没有落到屏幕上"的证据都建立在**相邻两帧**之上：
  只切睁闭眼乘数、连渲两帧，两帧之间物理/呼吸/待机动作几乎不动，于是像素差的主项是眼睛。
  这个前提**不是"没有 wait 就等于时间没走"**：帧的时间步长取自墙钟，
  被抢占时一帧的 deltaSeconds 可以是正常值的几十倍 —— 2026-09-29 在 26 个 CPU 燃烧线程
  把 24 逻辑核压到 100% 时实测复现：
    drift（连渲两帧、只切乘数）= 712 像素，而 reference 帧之间的眨眼原始值**都是 1.0000**，
    闭眼造成的 eyeDiff = 1223 ⇒ 比值只有 2x < 5x，用例失败。
  也就是说 712 那笔漂移纯粹来自"某一帧被卡了很久、呼吸/待机动作跳了一大步"，
  与眨眼相位无关（眨眼区间是 0 宽）—— 光等"眨眼全睁"挡不住它。

  所以判据改成**条件式的**：连渲两帧，只有在这一对帧真的满足
    (a) 眨眼全程处于全睁（否则眼睛开度差会被眨眼进度吃掉），且
    (b) 两帧之间真实流逝的时间够短（否则呼吸/物理已经跳了一大步）
  时才把它当作"相邻帧"。任一条不满足就重新取一对（不是放宽断言：断言仍是
  "闭眼像素差必须远超相邻帧漂移"，只是保证用来定标的那对帧真的可比）。
  两条都不满足时用**超时**报错，而不是悄悄用坏数据继续。====================*/

/*眨眼原始值 >= 这个数才算"全睁"。眨眼状态机的 interval 态恰好给 1.0，
   所以 0.999 是"落在 interval 态里"的判据，不是"差不多睁着"。*/
constexpr float kEyeProbeBlinkOpen = 0.999f;

/*一对"相邻帧"之间允许流逝的墙钟上限（毫秒）。

  为什么要有上限：drift 与两帧之间流逝的**时间**成正比（呼吸/待机动作是时间的函数），
  所以"时间没走"才是这个对比成立的前提。上限不是"性能阈值"，而是"这两帧还算相邻吗"的判据。

  为什么取 150ms：本机 atri（400x936、dpr 1.25）单帧渲染实测 5~10ms（见日志），
  150ms 已经是正常帧时间的 15~30 倍，只有真的被抢占/换页才会超过它；
  而空闲时两帧只用 10~20ms，离上限一个数量级，所以不会把正常情形挡在门外。
  实测（26 个燃烧线程、负载 100%）只需要重试极少数几次就能同时满足 (a)(b)。

  超时给 60s：这个循环里每次重试都要**真的渲一帧**，而在被抢占的机器上一帧可能要几百毫秒，
  所以"重试次数"不能当超时单位，得用墙钟。60s 远大于眨眼间隔上限（7s）与任何合理抖动，
  却仍然会在"帧真的渲不出来/眨眼真的卡死"时失败，不会把测试吊死。*/
constexpr int kEyeProbeMaxGapMs = 150;
constexpr int kEyeProbeDeadlineMs = 60000;
} // namespace

QRect TestLive2DWindow::faceRegionOfInterest(const QRect &figureBounds)
{
    if (figureBounds.isEmpty())
        return QRect();
    const int width = std::max(8, static_cast<int>(std::lround(figureBounds.width() * 0.45)));
    const int height = std::max(8, static_cast<int>(std::lround(figureBounds.height() * 0.22)));
    const int x = figureBounds.x() + (figureBounds.width() - width) / 2;
    const int y = figureBounds.y();
    return QRect(x, y, width, height);
}

/*把配置读取重定向到一次性临时文件：用户真实 config.ini 全程只读。

  为什么这样才安全：窗口侧经 MANDARIN_CONFIG_INI 决定读哪个 ini（见 settingsPath()），
  测试只往 QTemporaryDir 里写。即使某个用例 QSKIP / 断言失败 / 进程崩溃，
  真实配置也一个字节都不会被动到 —— "改完再恢复"那种做法做不到这一点。*/
void TestLive2DWindow::initTestCase()
{
    QVERIFY2(m_tempDir.isValid(), "建不出临时目录，无法隔离配置");

    m_tempConfigPath = m_tempDir.filePath(QStringLiteral("config.ini"));
    {
        // 内容随意：各用例需要什么值自己往这个临时文件里写
        QSettings settings(m_tempConfigPath, QSettings::IniFormat);
        settings.setValue("character/live2dFps", 60);
        settings.setValue("character/live2dScale", 1.0);
        settings.sync();
    }
    QVERIFY2(QFileInfo::exists(m_tempConfigPath), "临时配置文件没写出来");

    qputenv("MANDARIN_CONFIG_INI", m_tempConfigPath.toLocal8Bit());
    qInfo("配置已重定向到临时文件：%s（真实 config.ini 只读）", qPrintable(m_tempConfigPath));
}

void TestLive2DWindow::cleanupTestCase()
{
    qunsetenv("MANDARIN_CONFIG_INI");
    m_tempConfigPath.clear();
    // m_tempDir 析构时自动删除整棵目录
}

int TestLive2DWindow::findOpaquePoint(const QImage &frame, QPoint *opaquePoint)
{
    if (opaquePoint)
        *opaquePoint = QPoint(-1, -1);
    if (frame.isNull() || frame.format() != QImage::Format_RGBA8888)
        return 0;

    int opaquePixels = 0;
    for (int y = 0; y < frame.height(); ++y)
    {
        const uchar *line = frame.constScanLine(y);
        for (int x = 0; x < frame.width(); ++x)
        {
            if (line[x * 4 + 3] <= 200)
                continue;
            ++opaquePixels;
            // 取靠画布中部的实心像素，避开边缘的半透明羽化
            if (opaquePoint && opaquePoint->x() < 0 && x > frame.width() / 4 &&
                x < frame.width() * 3 / 4 && y > frame.height() / 4 &&
                y < frame.height() * 3 / 4)
            {
                *opaquePoint = QPoint(x, y);
            }
        }
    }
    return opaquePixels;
}

qint64 TestLive2DWindow::countDifferingPixels(const QImage &a, const QImage &b, int channelDelta)
{
    if (a.isNull() || b.isNull() || a.size() != b.size())
        return -1; // 尺寸不一致本身就是「变了」，但那样是异常，交给调用方断言

    const QImage lhs = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage rhs = b.convertToFormat(QImage::Format_RGBA8888);
    qint64 differing = 0;
    for (int y = 0; y < lhs.height(); ++y)
    {
        const uchar *l = lhs.constScanLine(y);
        const uchar *r = rhs.constScanLine(y);
        for (int x = 0; x < lhs.width(); ++x)
        {
            for (int c = 0; c < 4; ++c)
            {
                if (qAbs(int(l[x * 4 + c]) - int(r[x * 4 + c])) > channelDelta)
                {
                    ++differing;
                    break;
                }
            }
        }
    }
    return differing;
}

bool TestLive2DWindow::opaqueBounds(const QImage &frame, int alphaThreshold, QRect *bounds)
{    if (bounds)
        *bounds = QRect();
    if (frame.isNull())
        return false;

    const QImage rgba = frame.convertToFormat(QImage::Format_RGBA8888);
    int minX = rgba.width();
    int minY = rgba.height();
    int maxX = -1;
    int maxY = -1;
    for (int y = 0; y < rgba.height(); ++y)
    {
        const uchar *line = rgba.constScanLine(y);
        for (int x = 0; x < rgba.width(); ++x)
        {
            if (line[x * 4 + 3] <= alphaThreshold)
                continue;
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
        }
    }
    if (maxX < minX || maxY < minY)
        return false;
    if (bounds)
        *bounds = QRect(QPoint(minX, minY), QPoint(maxX, maxY));
    return true;
}

bool TestLive2DWindow::findSolidPoint(const QImage &frame, const QRect &figureBounds,
                                      int alphaThreshold, QPoint *output)
{
    if (output)
        *output = QPoint(-1, -1);
    if (frame.isNull() || figureBounds.isEmpty())
        return false;

    const QImage rgba = frame.convertToFormat(QImage::Format_RGBA8888);
    // 邻域内任意一点越界就没法判定"实心"，所以先收缩可行域
    const QRect region = figureBounds.adjusted(kSolidRadius, kSolidRadius, -kSolidRadius,
                                               -kSolidRadius)
                             .intersected(rgba.rect());
    if (region.isEmpty())
        return false;

    // 从人物包围盒中心开始找：中心附近的实心点最能代表"人物身上"
    const QPoint center = region.center();
    const int maxRadius = std::max(region.width(), region.height());
    for (int radius = 0; radius <= maxRadius; radius += 3)
    {
        for (int dy = -radius; dy <= radius; dy += 3)
        {
            for (int dx = -radius; dx <= radius; dx += 3)
            {
                const QPoint candidate(center.x() + dx, center.y() + dy);
                if (!region.contains(candidate))
                    continue;
                bool solid = true;
                for (int oy = -kSolidRadius; oy <= kSolidRadius && solid; ++oy)
                {
                    const uchar *line = rgba.constScanLine(candidate.y() + oy);
                    for (int ox = -kSolidRadius; ox <= kSolidRadius; ++ox)
                    {
                        if (line[(candidate.x() + ox) * 4 + 3] <= alphaThreshold)
                        {
                            solid = false;
                            break;
                        }
                    }
                }
                if (!solid)
                    continue;
                if (output)
                    *output = candidate;
                return true;
            }
        }
    }
    return false;
}

/*==================== 清晰度测量的实现 ====================*/

/*亮度的定义写死在这里（BT.601 权重的整数近似，全 0~255 量纲）：
   清晰度指标只用于**同类图之间的相对比较**，所以只要处处用同一个定义即可，
   不需要（也不该）引入色彩管理那套。*/
static inline double luminanceAt(const uchar *rgbaLine, int x)
{
    return 0.299 * rgbaLine[x * 4 + 0] + 0.587 * rgbaLine[x * 4 + 1] +
           0.114 * rgbaLine[x * 4 + 2];
}

TestLive2DWindow::SharpnessStats TestLive2DWindow::measureSharpness(const QImage &image,
                                                                   int maskAlpha,
                                                                   const QRect &restrictTo)
{
    SharpnessStats stats;
    if (image.isNull())
        return stats;

    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    stats.imageSize = rgba.size();

    const QRect domain =
        restrictTo.isEmpty() ? rgba.rect() : restrictTo.intersected(rgba.rect());
    if (domain.width() < 3 || domain.height() < 3)
        return stats;

    const int w = rgba.width();
    const int h = rgba.height();
    std::vector<double> lum(static_cast<size_t>(w) * h, 0.0);

    /*先把不透明包围盒量出来（整图 alpha>=maskAlpha），再在包围盒里量锐度。
       包围盒本身是要报的观察量：它说明"人物占了多少像素"，
       没有它就看不出"渲染分辨率变大"到底有没有落到内容上。*/
    int minX = w, minY = h, maxX = -1, maxY = -1;
    for (int y = 0; y < h; ++y)
    {
        const uchar *line = rgba.constScanLine(y);
        for (int x = 0; x < w; ++x)
        {
            if (line[x * 4 + 3] < maskAlpha)
                continue;
            if (x < minX) minX = x;
            if (x > maxX) maxX = x;
            if (y < minY) minY = y;
            if (y > maxY) maxY = y;
        }
    }
    if (maxX < minX || maxY < minY)
        return stats;
    const QRect bbox(QPoint(minX, minY), QPoint(maxX, maxY));
    stats.opaqueBounds = bbox;

    /*统计域 = 包围盒（若给了固定矩形就用它）。再各收 1 像素：
       4 邻域 Laplacian 与中心差分都要读到左右/上下邻居，
       不收缩就会把"邻域越界"的钳位值当成真实梯度，边缘一圈读数会虚高。*/
    const QRect body = (restrictTo.isEmpty() ? bbox : domain).intersected(bbox);
    if (body.width() < 3 || body.height() < 3)
        return stats;

    const int x0 = body.left() + 1;
    const int x1 = body.right() - 1;
    const int y0 = body.top() + 1;
    const int y1 = body.bottom() - 1;
    if (x1 < x0 || y1 < y0)
        return stats;

    const int sx0 = std::max(0, body.left() - 1);
    const int sy0 = std::max(0, body.top() - 1);
    const int sx1 = std::min(w - 1, body.right() + 1);
    const int sy1 = std::min(h - 1, body.bottom() + 1);

    /*亮度只算一次，候选域内的像素顺便在这里缓存好*/
    for (int y = sy0; y <= sy1; ++y)
    {
        const uchar *line = rgba.constScanLine(y);
        for (int x = sx0; x <= sx1; ++x)
            lum[static_cast<size_t>(y) * w + x] = luminanceAt(line, x);
    }

    /*参与统计的像素：自身 alpha 够，且四邻域也都够。
       为什么要连邻域一起要求：只要有一个邻居是透明的（例如人物剪影的边缘），
       那个邻居的 RGB 就不代表人物纹理，插进去算梯度等于量"人物和背景的落差"
       而不是量"人物有多清晰"。*/
    QList<QRect> solidRuns;
    solidRuns.reserve(y1 - y0 + 1);

    double sumLum = 0.0;
    double sumLumSq = 0.0;
    double sumLap = 0.0;
    double sumLapSq = 0.0;
    double sumGrad = 0.0;

    for (int y = y0; y <= y1; ++y)
    {
        const uchar *line = rgba.constScanLine(y);
        const uchar *above = rgba.constScanLine(y - 1);
        const uchar *below = rgba.constScanLine(y + 1);
        int runStart = -1;
        for (int x = x0; x <= x1 + 1; ++x)
        {
            bool solid = false;
            if (x <= x1)
            {
                solid = line[x * 4 + 3] >= maskAlpha && line[(x - 1) * 4 + 3] >= maskAlpha &&
                        line[(x + 1) * 4 + 3] >= maskAlpha && above[x * 4 + 3] >= maskAlpha &&
                        below[x * 4 + 3] >= maskAlpha;
            }
            if (solid && runStart < 0)
            {
                runStart = x;
            }
            else if (!solid && runStart >= 0)
            {
                solidRuns.append(QRect(runStart, y, x - runStart, 1));
                runStart = -1;
            }

            if (!solid)
                continue;

            const double center = lum[static_cast<size_t>(y) * w + x];
            const double left = lum[static_cast<size_t>(y) * w + (x - 1)];
            const double right = lum[static_cast<size_t>(y) * w + (x + 1)];
            const double up = lum[static_cast<size_t>(y - 1) * w + x];
            const double down = lum[static_cast<size_t>(y + 1) * w + x];

            /*4 邻域 Laplacian（中心 -4、上下左右各 +1）。纯色区域 = 0，
               有纹理/边缘的地方绝对值大 —— 它的方差就是经典的清晰度指标：
               图被模糊后高频被压掉，方差随之显著下降。*/
            const double lap = 4.0 * center - (left + right + up + down);
            /*中心差分梯度幅值：比 Laplacian 更直观的"相邻像素变化有多陡"*/
            const double gx = (right - left) * 0.5;
            const double gy = (down - up) * 0.5;
            const double grad = std::sqrt(gx * gx + gy * gy);

            sumLum += center;
            sumLumSq += center * center;
            sumLap += lap;
            sumLapSq += lap * lap;
            sumGrad += grad;
            ++stats.measuredPixels;
        }
    }

    if (stats.measuredPixels <= 0)
        return stats;

    const double n = static_cast<double>(stats.measuredPixels);
    const double meanLum = sumLum / n;
    const double meanLap = sumLap / n;
    stats.laplacianVariance = sumLapSq / n - meanLap * meanLap;
    if (stats.laplacianVariance < 0.0)
        stats.laplacianVariance = 0.0; //浮点抵消
    stats.meanGradient = sumGrad / n;
    stats.luminanceStdDev =
        std::sqrt(std::max(0.0, sumLumSq / n - meanLum * meanLum));

    /*顺手把"完全不透明"的实心区域求并集：D 的对比要在同一个固定矩形里重量一次，
       用实心区域而不是整个包围盒，才能保证矩形里每个像素真的都 alpha>=250
       （人物剪影形状不规则，包围盒的四个角通常是透明的）。*/
    if (restrictTo.isEmpty())
    {
        for (const QRect &run : solidRuns)
            stats.solidFill = stats.solidFill.isNull() ? run : stats.solidFill.united(run);
    }
    return stats;
}

qint64 TestLive2DWindow::comparePixels(const QImage &expected, const QImage &actual,
                                       int *maxChannelDiff)
{
    if (maxChannelDiff)
        *maxChannelDiff = -1;
    if (expected.isNull() || actual.isNull() || expected.size() != actual.size())
        return -1;

    const QImage a = expected.convertToFormat(QImage::Format_RGBA8888);
    const QImage b = actual.convertToFormat(QImage::Format_RGBA8888);
    int maxDiff = 0;
    qint64 differing = 0;
    for (int y = 0; y < a.height(); ++y)
    {
        const uchar *la = a.constScanLine(y);
        const uchar *lb = b.constScanLine(y);
        for (int x = 0; x < a.width(); ++x)
        {
            bool thisPixelDiffers = false;
            for (int c = 0; c < 4; ++c)
            {
                const int d = qAbs(int(la[x * 4 + c]) - int(lb[x * 4 + c]));
                if (d > maxDiff)
                    maxDiff = d;
                if (d > 0)
                    thisPixelDiffers = true;
            }
            if (thisPixelDiffers)
                ++differing;
        }
    }
    if (maxChannelDiff)
        *maxChannelDiff = maxDiff;
    return differing;
}

/*装载模型 → 窗口按渲染帧定尺寸 → 帧被画出来 → 形状/命中判定确实来自 alpha*/
void TestLive2DWindow::shapesWindowFromRenderedModel()
{
    /*用「用户实际配置的模型」而不是写死 miku：本用例会存一张窗口抓图，而本项目不许
      自动启动桌宠，抓图就是唯一能被人工复查的证据 —— 它必须是你真正会看到的那套模型。*/
    const QString modelName = preferredModelName();
    const QString dir = modelDirFor(modelName);
    if (dir.isEmpty())
        QSKIP("本机没有可用模型（禁二传，不入库），跳过立绘窗口验证");

    Live2DCharacterWindow window;
    window.loadModel(modelName);
    QVERIFY2(window.isModelLoaded(), "模型装载失败（Live2DCharacterWindow::loadModel）");

    // 先真正显示一次：Windows 上的原生窗口区域要有窗口才谈得上生效。
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QCoreApplication::processEvents();

    const QSize content = window.contentSize();
    QVERIFY2(!content.isEmpty(), "contentSize() 为空：还没有登记任何渲染帧");
    qInfo("DEBUG show 之后 widgetDpr=%f size=%dx%d content=%dx%d render=%dx%d",
          double(window.devicePixelRatioF()), window.width(), window.height(),
          content.width(), content.height(), window.renderSize().width(),
          window.renderSize().height());
    QCOMPARE(window.size(), content);
    QCOMPARE(window.modelDir(), dir);

    const QImage frame = window.renderedImage();
    // renderedImage() 给的是**物理像素**帧：逻辑尺寸 × dpr × live2dScale。
    // 这里断言的部分是「物理帧确实比逻辑画布更精细」，以及「它保住了逻辑宽高比」
    //（dpr 在窗口映射前后会变，所以不逐像素去对 contentSize，那样断言的是巧合而不是行为）。
    QCOMPARE(window.renderSize(), frame.size());
    QVERIFY2(frame.width() >= content.width() && frame.height() >= content.height(),
             "渲染分辨率不应低于逻辑尺寸");
    QVERIFY2(qAbs(static_cast<double>(frame.width()) / frame.height() -
                  static_cast<double>(content.width()) / content.height()) < 0.02,
             "渲染帧宽高比与逻辑画布不一致（画面会被拉伸）");

    // 帧必须是「人物 + 周围透明」，而不是一张不透明矩形：这才是能被 alpha 塑形的依据。
    QPoint framePoint;
    const int opaquePixels = findOpaquePoint(frame, &framePoint);
    const qint64 framePixels = static_cast<qint64>(frame.width()) * frame.height();
    QVERIFY2(opaquePixels > 1000,
             qPrintable(QStringLiteral("不透明像素只有 %1 个，模型基本没画出来")
                            .arg(opaquePixels)));
    QVERIFY2(opaquePixels < framePixels * 0.9,
             qPrintable(QStringLiteral("不透明像素占 %1%，看着是整块矩形而不是人物剪影")
                            .arg(static_cast<double>(opaquePixels) * 100.0 / framePixels)));
    QVERIFY2(framePoint.x() >= 0, "画布中部找不到不透明像素，无法验证穿透/命中");

    // 冻结帧循环：hideEvent 会停表，此时 m_scaledImg 不再自己变；再显式渲一帧，
    // 这样「采点用的帧」和「命中判定读的帧」是**同一帧**（否则高帧率下物理/呼吸一直在推进，
    // 采到的点可能已经不是不透明像素，会偶发假失败）。
    window.hide();
    QCoreApplication::processEvents();
    QVERIFY(window.renderFrameNow());

    // 把帧画到窗口上再量一次：这条断言钉住「窗口真的把帧画出来了」，
    // 而不是只登记了一张图（少了 paintEvent 的话，这里会几乎没有不透明像素）。
    const QPixmap grabbed = window.grab();
    QVERIFY2(!grabbed.isNull(), "window.grab() 失败，无法确认帧真的被画出来");
    QCOMPARE(grabbed.size(), (window.size() * window.devicePixelRatioF()));

    // 存一份抓图：这是「桌宠在屏幕上长什么样」的唯一可查证据
    //（本项目不许自动启动桌宠，所以人工目视只能靠这张图）。
    // 文件名带模型名：否则帧成本用例（固定用 miku）会把它覆盖成别的模型，
    // 人工复查时就分不清看到的是哪一套。
    const QString grabPath = QDir(QCoreApplication::applicationDirPath())
                                 .absoluteFilePath(QStringLiteral("../live2d-probe/window-grab-%1.png")
                                                       .arg(modelName));
    QDir().mkpath(QFileInfo(grabPath).absolutePath());
    QVERIFY2(grabbed.save(grabPath), qPrintable(QStringLiteral("写不出 %1").arg(grabPath)));
    qInfo("窗口抓图已保存：%s", qPrintable(grabPath));
    const QImage painted = grabbed.toImage().convertToFormat(QImage::Format_RGBA8888);
    // 绘制结果就是窗口的设备像素数（帧可能更大 —— 那是 live2dScale 的超采样，
    // 最终由 Qt 下采样到设备像素；渲染分辨率不会改变桌宠在屏幕上的物理大小）。
    QCOMPARE(painted.size(), (window.size() * window.devicePixelRatioF()));
    QVERIFY2(window.renderSize().width() >= painted.width() &&
                 window.renderSize().height() >= painted.height(),
             "渲染分辨率不应低于窗口设备像素数，否则 live2dScale 白设");

#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
    // 这两个平台把窗口形状裁成模型剪影：断言 mask 非空且严格小于整窗
    //（「窗口只是空白矩形」的 bug 会在这里失败）。
    const QRegion maskRegion = window.mask();
    QVERIFY2(!maskRegion.isEmpty(), "窗口 mask 为空：交互区没有从渲染帧算出来");
    const QRect maskBounds = maskRegion.boundingRect();
    QVERIFY2(maskBounds.width() < window.width() || maskBounds.height() < window.height(),
             qPrintable(QStringLiteral("窗口 mask 覆盖了整窗 %1x%2，等于没塑形")
                            .arg(window.width())
                            .arg(window.height())));

    // 形状必须跟着人物剪影走：mask 包围盒与帧 alpha 的包围盒宽高比应大体一致。
    const QImage alphaMask = frame.createAlphaMask();
    QRect figureBounds;
    for (int y = 0; y < alphaMask.height(); ++y)
    {
        for (int x = 0; x < alphaMask.width(); ++x)
        {
            if (alphaMask.pixelIndex(x, y) == 0)
                continue;
            const QRect pixel(x, y, 1, 1);
            figureBounds = figureBounds.isNull() ? pixel : figureBounds.united(pixel);
        }
    }
    QVERIFY2(!figureBounds.isEmpty(), "渲染帧的 alpha 蒙版是全空的");
    const double maskAspect =
        static_cast<double>(maskBounds.width()) / maskBounds.height();
    const double figureAspect =
        static_cast<double>(figureBounds.width()) / figureBounds.height();
    QVERIFY2(qAbs(maskAspect - figureAspect) < figureAspect * 0.2,
             qPrintable(QStringLiteral("窗口 mask 宽高比 %1 与人物剪影 %2 差得太远")
                            .arg(maskAspect)
                            .arg(figureAspect)));
    qInfo("非 Windows 平台：mask 包围盒=%dx%d，人物剪影=%dx%d", maskBounds.width(),
          maskBounds.height(), figureBounds.width(), figureBounds.height());
#else
    // Windows：与 Tachie 完全一致的行为 —— 不裁剪窗口形状，只 clearMask()。
    // 理由见 Tachie::relayoutContent()：硬裁半透明边缘会露出「略微缩小/边缘异常」，
    // 所以穿透/可点击完全靠基类鼠标事件里的 alpha 命中判定（读 m_scaledImg，不看 mask）。
    // 因此这里断言的**不是** mask 塑形，而是与 Tachie 相同的可观察行为：mask 必须为空。
    QVERIFY2(window.mask().isEmpty(),
             "Windows 下窗口不应设置 mask（Tachie 同款行为：只 clearMask）");

    /*塑形的等价观察量①：绘制结果里确实有人物（不是空白矩形，也不是满屏不透明底板）。

      采点用 findSolidPoint 而不是"中部第一个不透明像素"：
      acceptsClickAt 的逻辑坐标 → 帧像素映射与 dpr 强相关，单点采到羽化边缘或空洞上
      就会偶发假失败。这里从**绘制结果里人物包围盒的中心**找一个连邻域都实心的点，
      再按 dpr 换算回逻辑坐标去问命中判定 —— 断言的仍然是"人物身上可点、透明处穿透"。
      统计口径放在窗口自己的绘制结果上，避免和渲染帧的物理像素量纲混淆。*/
    QPoint paintedBoundsPoint;
    const int paintedOpaque = findOpaquePoint(painted, &paintedBoundsPoint);
    const qint64 paintedPixels = static_cast<qint64>(painted.width()) * painted.height();
    QVERIFY2(paintedOpaque > 1000, "窗口绘制结果里几乎没有不透明像素（帧没被画出来）");
    QVERIFY2(paintedOpaque < paintedPixels * 0.9,
             "窗口绘制结果几乎全是不透明，等于一块矩形背板而不是人物");

    QRect paintedFigureBounds;
    QVERIFY2(opaqueBounds(painted, 32, &paintedFigureBounds), "窗口绘制结果里没有不透明像素");
    QPoint solidPixel;
    QVERIFY2(findSolidPoint(painted, paintedFigureBounds, 32, &solidPixel),
             "在绘制结果的人物包围盒里找不到实心点");
    const qreal paintedDpr = window.devicePixelRatioF() > 0.0 ? window.devicePixelRatioF() : 1.0;
    const QPoint hitPoint(static_cast<int>(std::lround(solidPixel.x() / paintedDpr)),
                          static_cast<int>(std::lround(solidPixel.y() / paintedDpr)));
    qInfo("命中判定采样点：绘制像素 %d,%d → 逻辑 %d,%d (dpr=%.3f)",
          solidPixel.x(), solidPixel.y(), hitPoint.x(), hitPoint.y(), double(paintedDpr));
    QVERIFY2(hitPoint.x() >= 0 && hitPoint.y() >= 0 && hitPoint.x() < window.width() &&
                 hitPoint.y() < window.height(),
             qPrintable(QStringLiteral("绘制结果里的人物点 %1,%2 落在窗口 %3x%4 之外，"
                                       "说明绘制尺寸与窗口尺寸不一致")
                            .arg(hitPoint.x())
                            .arg(hitPoint.y())
                            .arg(window.width())
                            .arg(window.height())));

    // 塑形的等价观察量②：命中判定必须来自 alpha ——
    // 人物身上的点应可交互，透明角落应不可交互（穿透），并且**接受区的包围盒要贴着人物**，
    // 而不是铺满整窗（铺满就说明判定退化成了一块矩形背板）。
    QVERIFY2(window.acceptsClickAt(hitPoint),
             qPrintable(QStringLiteral("人物身上的点 %1,%2 判定为不可交互，命中判定有问题")
                            .arg(hitPoint.x())
                            .arg(hitPoint.y())));

    const QPoint transparentCorner(1, 1);
    QVERIFY2(painted.pixelColor(transparentCorner).alpha() < 10,
             "窗口左上角不是透明像素，无法用作穿透反例");
    QVERIFY2(!window.acceptsClickAt(transparentCorner),
             "窗口左上角（人物之外的透明区）被判定为可交互，透明区应当是穿透的");

    // 粗网格扫描：接受区必须只覆盖人物那一块
    constexpr int kStride = 8;
    QRect acceptedBounds;
    int acceptedCount = 0;
    int sampledCount = 0;
    for (int y = 0; y < window.height(); y += kStride)
    {
        for (int x = 0; x < window.width(); x += kStride)
        {
            ++sampledCount;
            if (!window.acceptsClickAt(QPoint(x, y)))
                continue;
            ++acceptedCount;
            acceptedBounds = acceptedBounds.isNull() ? QRect(x, y, 1, 1)
                                                     : acceptedBounds.united(QRect(x, y, 1, 1));
        }
    }
    QVERIFY2(acceptedCount > 0, "整窗没有任何可交互点，人物不可点击");
    QVERIFY2(acceptedCount < sampledCount,
             "整窗每个采样点都可交互，等于没有按 alpha 塑形");
    QVERIFY2(acceptedBounds.width() < window.width() ||
                 acceptedBounds.height() < window.height(),
             qPrintable(QStringLiteral("可交互区包围盒 %1x%2 铺满了窗口 %3x%4")
                            .arg(acceptedBounds.width())
                            .arg(acceptedBounds.height())
                            .arg(window.width())
                            .arg(window.height())));

    // 与绘制结果里的人物包围盒对照（允许一个采样格的误差）。
    // 注意换算：painted 是设备像素，接受区是逻辑坐标，要按 painted/窗口 的实际比例折回来。
    QRegion paintedRegion(QBitmap::fromImage(
        painted.convertToFormat(QImage::Format_ARGB32).createAlphaMask()));
    QVERIFY2(!paintedRegion.isEmpty(), "窗口绘制结果全透明，等于什么都没画");
    const QRect paintedBounds = paintedRegion.boundingRect();
    const double deviceToLogical =
        window.width() > 0 ? static_cast<double>(window.width()) / painted.width() : 1.0;
    const QRect figureBounds(
        static_cast<int>(std::lround(paintedBounds.x() * deviceToLogical)),
        static_cast<int>(std::lround(paintedBounds.y() * deviceToLogical)),
        static_cast<int>(std::lround(paintedBounds.width() * deviceToLogical)),
        static_cast<int>(std::lround(paintedBounds.height() * deviceToLogical)));
    const int tolerance = kStride * 2;
    QVERIFY2(qAbs(acceptedBounds.x() - figureBounds.x()) <= tolerance &&
                 qAbs(acceptedBounds.y() - figureBounds.y()) <= tolerance &&
                 qAbs(acceptedBounds.width() - figureBounds.width()) <= tolerance * 2 &&
                 qAbs(acceptedBounds.height() - figureBounds.height()) <= tolerance * 2,
             qPrintable(QStringLiteral("可交互区(%1,%2 %3x%4)与人物绘制区(%5,%6 %7x%8)对不上")
                            .arg(acceptedBounds.x())
                            .arg(acceptedBounds.y())
                            .arg(acceptedBounds.width())
                            .arg(acceptedBounds.height())
                            .arg(figureBounds.x())
                            .arg(figureBounds.y())
                            .arg(figureBounds.width())
                            .arg(figureBounds.height())));

    qInfo("Windows：断言 clearMask + alpha 命中判定（Tachie 同款），逻辑画布=%dx%d，"
          "渲染帧=%dx%d，绘制不透明像素=%d，可交互区=%d,%d %dx%d，人物绘制区=%d,%d %dx%d",
          content.width(), content.height(), frame.width(), frame.height(), paintedOpaque,
          acceptedBounds.x(), acceptedBounds.y(), acceptedBounds.width(),
          acceptedBounds.height(), figureBounds.x(), figureBounds.y(), figureBounds.width(),
          figureBounds.height());
#endif
}

/*「宠物真的活着吗」——这条测试钉住 Bug A（模型冻在默认姿势）。

  为什么不能只看「一帧里有不透明像素」：那只能证明模型画出来了，证明不了它会动。
  这里走的是**真实帧路径**（renderFrameNow → renderAndRegisterFrame → renderFrame → tick），
  中间插入真实流逝的时间（QThread::msleep），所以 deltaSeconds 是真时间，
  呼吸/眨眼/待机动作该推进的都推进了。

  冻结的实现两帧逐位相同 → 差值为 0；只要参数驱动器接上了，差值就是成千上万像素。
  阈值取「显著大于噪声」的量级：这个模型任何一帧之间都不可能只差几百像素，
  冻结时又恰好是 0，所以不存在"靠噪声蒙对"的可能。*/
void TestLive2DWindow::animatesAcrossFrames()
{
    const QString modelName = preferredModelName();
    const QString dir = modelDirFor(modelName);
    if (!QFileInfo::exists(dir))
        QSKIP("本机没有用户配置的模型（禁二传，不入库），跳过动画验证");

    /*帧率与缩放在 initTestCase 写进**临时**配置：这条测试量的是"动不动"，
      不是性能。用的是临时文件里的 60fps / 1.0x，用户的 config.ini 不参与、也不被改。*/
    {
        Live2DCharacterWindow window;
        window.loadModel(modelName);
        QVERIFY2(window.isModelLoaded(), "模型装载失败，无法验证动画");

        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCoreApplication::processEvents();

        QVERIFY2(!window.contentSize().isEmpty(), "contentSize() 为空，还没有帧");

        // 起手帧：停下帧循环再自己渲一帧，保证「基准帧」与后面比较的是同一条路径。
        window.hide();
        QCoreApplication::processEvents();
        QVERIFY2(window.renderFrameNow(), "基准帧渲染失败");
        const QImage before = window.renderedImage();
        QVERIFY2(!before.isNull(), "基准帧为空");

        /*推进：真的让时间过去。逐帧之间也睡一小会儿，这样 tick() 收到的是
          真实 deltaSeconds（而不是紧循环里那种 <0.1ms 的假时间）。
          实测（本机 RTX 5070 Ti Laptop）加上驱动器后，相邻帧（间隔 100ms）差 5 万~13 万像素，
          参考帧与 3.2s 后的帧差 13 万像素（约 21%）—— 与阈值 2000 差两个数量级，
          所以这条断言既不可能被噪声蒙对，也不会因为时序抖动而偶发失败。*/
        constexpr int kFrames = 60;
        for (int i = 0; i < kFrames; ++i)
        {
            QThread::msleep(16);
            QCoreApplication::processEvents();
            QVERIFY2(window.renderFrameNow(), "推进帧渲染失败");
        }

        // 让动作走到与基准帧明显不同的相位（呼吸周期 3.2~15.5s，待机动作 2.667s 一循环）
        QThread::msleep(3200);
        QCoreApplication::processEvents();
        QVERIFY2(window.renderFrameNow(), "比较帧渲染失败");
        const QImage after = window.renderedImage();
        QVERIFY2(!after.isNull(), "比较帧为空");

        QCOMPARE(after.size(), before.size());
        constexpr int kChannelDelta = 8; // 抗 8bit 量化/抗锯齿的通道噪声
        const qint64 differing = countDifferingPixels(before, after, kChannelDelta);
        QVERIFY2(differing >= 0, "两帧尺寸不一致，无法比较");
        const qint64 total = static_cast<qint64>(before.width()) * before.height();
        qInfo("动画验证[%s]：%d 帧 + 真实流逝时间后，逐通道差 >%d 的像素 = %lld / %lld（%.3f%%）",
              qPrintable(modelName), kFrames, kChannelDelta, differing, total,
              static_cast<double>(differing) * 100.0 / static_cast<double>(total));

        /*冻结的实现这里是 0（实测逐位相同）；只差"某一帧的渲染状态"这种噪声量级也才几百。
          阈值取 2000：远高于噪声，又远低于真实动画的量级。*/
        QVERIFY2(differing > 2000,
                 qPrintable(QStringLiteral("两帧只差 %1 个像素，模型看上去是静止的"
                                               "（没有呼吸/眨眼/待机动作）")
                                .arg(differing)));
    }
}

/*「人物完整落在画布内吗」——这条测试钉住 Bug B（右侧双马尾被裁掉）。

  观察量：渲染帧里 alpha>threshold 的包围盒，必须与画布**四条边都留出余量**。
  画布是"人物包围盒 + 0 边距"时，包围盒必然顶到某条边（余量 0），这条断言就会失败。

  为什么取一段时间内两帧的**并集**：待机动作会让姿势移动，单帧的包围盒可能恰好在
  某条边上松一点。并集是"人物在这段时间里占过的最大范围"，用它判断有没有被裁更严格。

  注意：这里不假设人物在画布里居中 —— 只要求四条边都有余量，居不居中由实现自己决定。*/
void TestLive2DWindow::keepsWholeFigureInsideCanvas()
{
    const QString modelName = preferredModelName();
    const QString dir = modelDirFor(modelName);
    if (!QFileInfo::exists(dir))
        QSKIP("本机没有用户配置的模型（禁二传，不入库），跳过画布余量验证");

    /*用临时配置（60fps / 1.0x）确实能复现 bug：裁切与帧率/渲染倍数无关，
      它只取决于逻辑画布与人物包围盒的关系。用户真实 config.ini 全程只读。*/
    Live2DCharacterWindow window;
    window.loadModel(modelName);
    QVERIFY2(window.isModelLoaded(), "模型装载失败，无法验证画布余量");

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QCoreApplication::processEvents();
    QVERIFY2(window.renderFrameNow(), "首帧渲染失败");


    const QSize logical = window.contentSize();
    QVERIFY2(!logical.isEmpty(), "contentSize() 为空");

    // 一帧不足以代表"人物占过的最大范围"（姿势会动），取两帧并集。
    QRect unionBounds;
    for (int pass = 0; pass < 2; ++pass)
    {
        if (pass > 0)
        {
            for (int i = 0; i < 30; ++i)
            {
                QThread::msleep(16);
                QCoreApplication::processEvents();
                QVERIFY2(window.renderFrameNow(), "推进帧渲染失败");
            }
            QVERIFY2(window.renderFrameNow(), "第二帧渲染失败");
        }

        const QImage frame = window.renderedImage();
        QVERIFY2(!frame.isNull(), "渲染帧为空");
        QRect bounds;
        QVERIFY2(opaqueBounds(frame, 32, &bounds), "渲染帧里没有任何不透明像素");
        unionBounds = unionBounds.isNull() ? bounds : unionBounds.united(bounds);
    }

    // 渲染帧是物理像素，画布是逻辑像素；两者只差一个统一比例，用比例折回画布坐标。
    const QImage frame = window.renderedImage();
    const double scale = static_cast<double>(logical.width()) / frame.width();
    const QRect figure(
        static_cast<int>(std::lround(unionBounds.x() * scale)),
        static_cast<int>(std::lround(unionBounds.y() * scale)),
        static_cast<int>(std::lround(unionBounds.width() * scale)),
        static_cast<int>(std::lround(unionBounds.height() * scale)));

    const int left = figure.left();
    const int top = figure.top();
    const int right = logical.width() - 1 - figure.right();
    const int bottom = logical.height() - 1 - figure.bottom();

    // 「几条边有真实余量」的判据：至少画布短边的百分之几，而不是"≥1 像素"
    const int minMargin = std::max(2, static_cast<int>(std::lround(
                                          std::min(logical.width(), logical.height()) * 0.02)));
    qInfo("画布余量验证[%s]：逻辑画布=%dx%d，渲染帧=%dx%d，人物并集包围盒=%d,%d %dx%d，"
          "四边余量 左=%d 上=%d 右=%d 下=%d（要求每条 ≥%d）",
          qPrintable(modelName), logical.width(), logical.height(), frame.width(),
          frame.height(), figure.x(), figure.y(), figure.width(), figure.height(), left, top,
          right, bottom, minMargin);

    QVERIFY2(left >= minMargin && top >= minMargin && right >= minMargin && bottom >= minMargin,
             qPrintable(QStringLiteral("人物顶到画布边缘：四边余量 左=%1 上=%2 右=%3 下=%4，"
                                       "每条至少要 %5（画布 %6x%7，人物包围盒 %8,%9 %10x%11）")
                            .arg(left)
                            .arg(top)
                            .arg(right)
                            .arg(bottom)
                            .arg(minMargin)
                            .arg(logical.width())
                            .arg(logical.height())
                            .arg(figure.x())
                            .arg(figure.y())
                            .arg(figure.width())
                            .arg(figure.height())));
}

/*「人物到底有没有把画布填满」——这条测试钉住 Bug C（画布宽度按**被污染的探针宽高比**定，
   于是画布比人物宽 2.2~2.35 倍：人物只占画布宽度的 36~38%，左右各空出约 250 逻辑像素）。

   观察量：真实渲染帧里 alpha 包围盒占**画布宽度**与**画布高度**的比例。
   高度占比本来就被 relayoutContent 的「实测校正」钉在 0.84 上（所以光看高度永远发现不了
   这个 bug），必须在宽度方向也断言。判据取 60%：远高于修复前的 36~38%（一定失败），
   又比目标 84% 松一截，给不同模型/不同姿势留余量。

   为什么对**本机所有模型**都跑（atri / miku / 樱花miku）：画布宽高比来自探针度量，
   这个 bug 对所有模型都成立，只在 miku 上测会漏掉"换了模型又不灵"（miku 双马尾横向铺开，
   症状本来就轻一些）。顺带把每个模型的窗口抓图存成 window-grab-<模型名>.png ——
   本项目不许自动启动桌宠，抓图是人工目视"人物是否填满画面"的唯一证据。*/
void TestLive2DWindow::fillsFrameInBothAxes()
{
    static const char *const kModels[] = {"atri", "miku", "樱花miku"};

    int checked = 0;
    for (const char *rawName : kModels)
    {
        const QString modelName = QString::fromUtf8(rawName);
        const QString dir = modelDirFor(modelName);
        if (!QFileInfo::exists(dir))
        {
            qInfo("跳过 %s（本机没有该模型目录，禁二传不入库）", rawName);
            continue;
        }

        Live2DCharacterWindow window;
        window.loadModel(modelName);
        QVERIFY2(window.isModelLoaded(),
                 qPrintable(QStringLiteral("模型 %1 装载失败，无法验证画布填充").arg(modelName)));

        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCoreApplication::processEvents();

        // 冻住帧循环后再自己渲一帧：保证「量的帧」就是布局校正之后的那一帧
        window.hide();
        QCoreApplication::processEvents();
        QVERIFY2(window.renderFrameNow(), "渲染失败，无法量画布填充");

        const QSize logical = window.contentSize();
        QVERIFY2(!logical.isEmpty(), "contentSize() 为空");
        const QImage frame = window.renderedImage();
        QVERIFY2(!frame.isNull(), "渲染帧为空");

        QRect bounds;
        QVERIFY2(opaqueBounds(frame, 32, &bounds), "渲染帧里没有任何不透明像素");

        // 渲染帧是**物理**像素，画布是**逻辑**像素：两者只差一个统一比例，按比例折回画布坐标。
        const double toLogical = static_cast<double>(logical.width()) / frame.width();
        const double figureWidth = bounds.width() * toLogical;
        const double figureHeight = bounds.height() * toLogical;
        const double occupancyWidth = figureWidth / logical.width();
        const double occupancyHeight = figureHeight / logical.height();

        qInfo("画布填充验证[%s]：逻辑画布=%dx%d，渲染帧=%dx%d，人物包围盒(帧像素)=%d,%d %dx%d "
              "→ 画布内人物=%.1fx%.1f，**宽度占比=%.4f**，高度占比=%.4f",
              rawName, logical.width(), logical.height(), frame.width(), frame.height(), bounds.x(),
              bounds.y(), bounds.width(), bounds.height(), figureWidth, figureHeight,
              occupancyWidth, occupancyHeight);

        // 抓图：人工目视证据
        const QPixmap grabbed = window.grab();
        QVERIFY2(!grabbed.isNull(), "window.grab() 失败，无法留人工复查证据");
        const QString grabPath =
            QDir(QCoreApplication::applicationDirPath())
                .absoluteFilePath(QStringLiteral("../live2d-probe/window-grab-%1.png").arg(modelName));
        QDir().mkpath(QFileInfo(grabPath).absolutePath());
        QVERIFY2(grabbed.save(grabPath), qPrintable(QStringLiteral("写不出 %1").arg(grabPath)));

        /*宽度占比：这是本次修复的核心判据（修复前 0.36~0.38）。
           人物必须在**两个方向**都接近目标占比，否则就是"画布比人物宽出一大截、
           人物缩在中间"。*/
        QVERIFY2(occupancyWidth >= 0.60,
                 qPrintable(QStringLiteral("[%1] 人物只占画布宽度的 %2%（要求 ≥60%）："
                                           "画布 %3x%4 比人物 %5x%6 宽太多，"
                                           "人物被挤在中间，两侧全是空白")
                                .arg(modelName)
                                .arg(occupancyWidth * 100.0, 0, 'f', 1)
                                .arg(logical.width())
                                .arg(logical.height())
                                .arg(figureWidth, 0, 'f', 1)
                                .arg(figureHeight, 0, 'f', 1)));

        /*高度占比：钉住既有的垂直目标（窗口侧的 kTargetFigureRatio = 0.84）。
           上下留 ~8% 余量 ⇒ 允许 0.75~0.92 的区间，既能抓住"垂直方向退化"，
           又不会因为探针与真实帧的姿势差异而偶发失败。*/
        QVERIFY2(occupancyHeight >= 0.75 && occupancyHeight <= 0.92,
                 qPrintable(QStringLiteral("[%1] 人物占画布高度的 %2%，偏离垂直目标 84% "
                                           "（允许 75%~92%）")
                                .arg(modelName)
                                .arg(occupancyHeight * 100.0, 0, 'f', 1)));

        ++checked;
    }

    if (checked == 0)
        QSKIP("本机没有 atri / miku / 樱花miku 任何一个模型目录（禁二传，不入库）");
}

/*模型入口文件名不一定等于目录名 —— 这条测试钉住宽松查找。

  实机用例：`Live2D/atri/` 目录里的入口是 `atri_8.model3.json`（不是 atri.model3.json）。
  模型文件受授权保护不能改名，所以查找必须回退到"扫目录"。按目录名 atri 装载必须成功。

  目录不存在就跳过（模型不入库）。*/
void TestLive2DWindow::loadsModelByDirectoryName()
{
    struct Case
    {
        const char *dirName;
        const char *expectedJson; // 该目录里实际存在的入口文件名
    };
    const Case cases[] = {
        {"atri", "atri_8.model3.json"}, // 目录名 ≠ 文件名：这条就是本次修复的目标
        {"miku", "miku.model3.json"},   // 正命名：必须继续可用
        {"樱花miku", "樱花miku.model3.json"}, // 另一台正命名模型（存在才测）
    };

    int checked = 0;
    for (const Case &c : cases)
    {
        const QString dir = modelDirFor(QString::fromUtf8(c.dirName));
        if (!QFileInfo::exists(dir))
        {
            qInfo("跳过 %s（本机没有该模型目录）", c.dirName);
            continue;
        }
        QVERIFY2(QFileInfo::exists(dir + QLatin1Char('/') + QString::fromUtf8(c.expectedJson)),
                 qPrintable(QStringLiteral("%1 目录里没有预期的入口文件 %2（模型文件不该被改）")
                                .arg(dir)
                                .arg(QString::fromUtf8(c.expectedJson))));

        Live2DCharacterWindow window;
        window.loadModel(QString::fromUtf8(c.dirName));
        QVERIFY2(window.isModelLoaded(),
                 qPrintable(QStringLiteral("按目录名 %1 装载失败：入口文件实际叫 %2")
                                .arg(QString::fromUtf8(c.dirName))
                                .arg(QString::fromUtf8(c.expectedJson))));
        QCOMPARE(window.modelDir(), dir);

        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCoreApplication::processEvents();
        QVERIFY2(window.renderFrameNow(), "渲染失败");
        QRect bounds;
        QVERIFY2(opaqueBounds(window.renderedImage(), 32, &bounds),
                 qPrintable(QStringLiteral("%1 渲染帧里没有不透明像素").arg(c.dirName)));
        qInfo("按目录名装载成功：%s → 入口 %s，帧 %dx%d，人物 %dx%d", c.dirName, c.expectedJson,
              window.renderedImage().width(), window.renderedImage().height(), bounds.width(),
              bounds.height());
        ++checked;
    }

    if (checked == 0)
        QSKIP("本机没有 atri / miku / 樱花miku 任何一个模型目录（禁二传，不入库）");
}

/*量「真实全链路」每帧成本：renderFrame() + 登记 alpha 图 + 交互区节拍（每 10 帧一次）。
  为什么必须量整条链路而不能只看 renderFrame：高刷屏下帧预算只有几毫秒，
  控件侧的开销（QImage 拷贝、updateRenderedImage）与周期性的 QBitmap mask
  完全可能反超渲染本身。这条测试是把「能不能上高帧率」变成数字的地方。

  配置都量：用户实际在用的档位（120fps，缩放 1.0 与 1.5 各一档 —— 这是"8.33ms 帧预算
  到底够不够"的唯一依据）与**代码默认值** 60fps / 1.0x（「弱机器开箱行为」的真实成本）。
  每一档还要在**用户真正在用的模型**（preferredModelName，当前 atri）上再量一遍：
  每帧成本不只随分辨率走，也随模型的 drawable 数走，拿别的模型当替身是在报别人的数字。
  所有配置都写进**临时**配置文件（见 initTestCase），用户的 config.ini 全程只读。
  注：帧率档位只影响定时器间隔，不影响这里的每帧成本 —— 两条 1.0x 行本就该给出同一个数字，
  分列出来是为了让"1.0x 该是多少"在日志里直接可查，不必自己推。*/
void TestLive2DWindow::reportsFullPipelineFrameCost()
{
    const QString dir = modelDir();
    const QString modelJsonName = QStringLiteral("miku.model3.json");
    if (!QFileInfo::exists(dir + QLatin1Char('/') + modelJsonName))
        QSKIP("本机没有初音模型（禁二传，不入库），跳过全链路帧成本测量");

    // 用户实际在用的模型（config.ini 的 character/live2dModel，当前 atri）；没有就只量 miku
    const QString userModel = preferredModelName();

    const struct
    {
        const char *label;
        const char *model;
        int fps;
        double scale;
    } configurations[] = {
        {"用户档位 120fps/1.5x", "atri", 120, 1.5},
        {"用户档位 120fps/1.0x", "atri", 120, 1.0},
        {"用户档位 120fps/1.5x", "miku", 120, 1.5},
        {"用户档位 120fps/1.0x", "miku", 120, 1.0},
        {"代码默认值 60fps/1.0x", "miku", 60, 1.0},
    };

    int measured = 0;
    for (const auto &config : configurations)
    {
        const QString modelName = QString::fromUtf8(config.model);
        /*模型不入库：本机没有该模型目录就跳过这一行，而不是把整条用例废掉
           （另一台上可能只有 miku 或只有 atri）。一行都没跑成会在末尾报错。*/
        if (!QFileInfo::exists(modelDirFor(modelName)))
        {
            qInfo("跳过帧成本档位 [%s/%s]（本机没有该模型目录）", config.label,
                  config.model);
            continue;
        }

        /*只写临时配置；不需要 RAII —— 临时目录随测试进程一起消失，
          而且下一个用例会自己再写一次，用户真实配置从头到尾没参与。*/
        {
            QSettings settings(m_tempConfigPath, QSettings::IniFormat);
            settings.setValue("character/live2dFps", config.fps);
            settings.setValue("character/live2dScale", config.scale);
            settings.sync();
        }

        Live2DCharacterWindow window;
        window.loadModel(modelName);
        QVERIFY2(window.isModelLoaded(),
                 qPrintable(QStringLiteral("[%1] 模型装载失败，无法量帧成本")
                                .arg(QString::fromUtf8(config.model))));

        // 必须真的显示一次：首次画布布局在 showEvent 里做（那时 dpr 才是真实值）
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCoreApplication::processEvents();

        const QSize logical = window.contentSize();
        const QSize physical = window.renderSize();
        QVERIFY(!logical.isEmpty());
        QCOMPARE(window.size(), logical);

        // 先热身一帧：首帧要重建 FBO 并把着色器/贴图路径走热，不能计入平均
        QVERIFY(window.renderFrameNow());

        constexpr int kFrames = 60;
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < kFrames; ++i)
            QVERIFY(window.renderFrameNow());
        const double msPerFrame = static_cast<double>(timer.elapsed()) / kFrames;
        const double fps = msPerFrame > 0.0 ? 1000.0 / msPerFrame : 0.0;

        qInfo("全链路帧成本[%s/%s]：逻辑 %dx%d，渲染 %dx%d，平均 %.2f ms/帧（约 %.1f fps），"
              "节拍 %d ms",
              config.label, config.model, logical.width(), logical.height(), physical.width(),
              physical.height(), msPerFrame, fps, window.frameIntervalMs());

        // 只做宽松退步警戒：真实数字靠上面日志观察，不在测试里卡死阈值。
        QVERIFY2(msPerFrame < 200.0,
                 qPrintable(QStringLiteral("[%1] 全链路每帧 %2 ms，慢到不可用")
                                .arg(QString::fromUtf8(config.label))
                                .arg(msPerFrame)));
        ++measured;
    }

    QVERIFY2(measured > 0, "一个档位都没量到（模型目录都不在），帧成本没有数据");
}

/*「上屏那一步是不是精确拷贝」——本文件里唯一一条关于绘制质量的**断言**（其余是测量）。

  钉住的机理（已由纯 Qt 对照实验 test_devicepixelblit 单独证实，不依赖 Live2D）：
  paintEvent 用 drawImage(rect(), m_scaledImg, 满源矩形) 把整帧铺满窗口。**源矩形给的是图像
  像素、目标矩形是逻辑坐标**，所以 Qt 铺的是"帧像素网格 → 窗口设备像素网格（rect() × dpr）"；
  只有帧的像素尺寸恰好等于窗口设备矩形才是 1:1。登记帧尺寸是 lround(canvas × dpr)，
  窗口设备矩形是 canvas × dpr —— canvas × dpr 不是整数时两者必然差零点几个像素：
  本机 atri 画布 400x938、dpr 1.25 ⇒ 登记帧 lround(938×1.25) = 1173 行，窗口设备矩形 1172.5 行。
  1173 行被压进 1172.5 行（纵向比例 0.99957），逐行相位从 0 漂到 0.5 ——
  这就是实测「登记帧 vs grab() 差 17.31% 像素、清晰度 −11.9%」的全部原因。
  纯 Qt 对照实验里的数字：像素尺寸对不上 ⇒ 99.6% 像素不同、Laplacian 方差 −49%；
  对得上 ⇒ 逐位相同、方差 +0.000%。

  所以画布必须**设备像素对齐**：canvas × dpr 是整数 ⇔ canvas 是 dpr 既约分母的整数倍
  （1.25→4、1.5→2、1.75→4、2.0→1、1.0→1）。本用例直接把这条不变量也断言掉，
  这样以后谁把尺寸算法改回去，报错会指向根因而不是"像素差了多少"。

  为什么只在 1.0x 断言「逐位一致」：live2dScale > 1 时绘制**本来就该**平滑下采样 ——
  那是超采样换来的清晰度，不是 bug（对齐之后 1.5x 的上下比例恰好是精确的 1.5，
  采样相位不再漂移，这一点由 reportsSharpnessAcrossScales 的测量日志观察）。*/
void TestLive2DWindow::paintsRegisteredFrameWithoutResampling()
{
    const QString modelName = preferredModelName();
    const QString dir = modelDirFor(modelName);
    if (!QFileInfo::exists(dir))
        QSKIP("本机没有用户配置的模型（禁二传，不入库），跳过绘制一致性验证");

    // 只写临时配置：1.0x 才是"应当精确 1:1"的那一档
    {
        QSettings settings(m_tempConfigPath, QSettings::IniFormat);
        settings.setValue("character/live2dFps", 120);
        settings.setValue("character/live2dScale", 1.0);
        settings.sync();
    }

    Live2DCharacterWindow window;
    window.loadModel(modelName);
    QVERIFY2(window.isModelLoaded(), "模型装载失败，无法验证绘制一致性");

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QCoreApplication::processEvents();

    // 冻住帧循环再自己渲一帧：保证"登记的帧"就是"被画的帧"（否则姿势会漂）
    window.hide();
    QCoreApplication::processEvents();
    QVERIFY2(window.renderFrameNow(), "渲染失败");
    QCoreApplication::processEvents();

    const QImage registered = window.renderedImage();
    QVERIFY2(!registered.isNull(), "登记帧为空");

    const QSize logical = window.size();
    const qreal dpr = window.devicePixelRatioF() > 0.0 ? window.devicePixelRatioF() : 1.0;
    const double deviceW = logical.width() * dpr;
    const double deviceH = logical.height() * dpr;

    /*先**量**再断言：把"画布 × dpr 是不是整数"这个根因量打出来（这是本用例的算术证据），
     断言放在逐像素比较之后 —— 这样修复前跑一次就能同时拿到"算术不对"与"像素差多少"两笔证据，
     而不是第一条断言就把人拦在门外。
     "两者逻辑尺寸差"那一项：帧的 dpr 取窗口 dpr（见 renderAndRegisterFrame），
     所以它等价于"帧像素尺寸 − 窗口设备矩形"，也就是绘制那一步的纵向/横向比例 −1。*/
    const bool deviceAligned =
        std::abs(deviceW - std::round(deviceW)) < 1e-6 && std::abs(deviceH - std::round(deviceH)) < 1e-6;
    qInfo("画布算术：逻辑 %dx%d × dpr %.4f = %.1fx%.1f（设备矩形 %dx%d），登记帧 %dx%d，"
          "两者逻辑尺寸差 %.4fx%.4f 行/列，设备像素对齐=%s",
          logical.width(), logical.height(), double(dpr), deviceW, deviceH, qRound(deviceW),
          qRound(deviceH), registered.width(), registered.height(),
          registered.width() / dpr - logical.width(), registered.height() / dpr - logical.height(),
          deviceAligned ? "YES" : "NO");

    // 登记帧自己的比例必须是窗口 dpr（paintEvent 的映射正是按它折算逻辑尺寸的）
    QVERIFY2(qAbs(registered.devicePixelRatio() - dpr) < 1e-9,
             "登记帧的 devicePixelRatio 不等于窗口 dpr，绘制映射的前提不成立");

    const QPixmap grabbed = window.grab();
    QVERIFY2(!grabbed.isNull(), "window.grab() 失败");

    /*像素尺寸必须**完全一致**：精确拷贝不可能改变像素网格。
      这一条不满足时后面的逐像素比较根本无从谈起（尺寸不同 = 一定发生了重采样）。*/
    QCOMPARE(registered.size(), QSize(qRound(deviceW), qRound(deviceH)));
    QCOMPARE(grabbed.toImage().size(), registered.size());

    /*在哪个**色彩空间**比，是这条用例最容易搞错的地方，所以写清楚：

      真正被屏幕合成的是**预乘** RGBA。登记帧给的是非预乘 RGBA8888，grab() 回来的是预乘格式，
      于是有两种比法：
        (1) 双方都折回非预乘 RGBA8888 再比 —— 直观，但 un-premultiply 在 alpha 很小时会把
            舍入放大（alpha=3、预乘值差 1 级 ⇒ 非预乘差可达 ~85 级）。这样量到的是"报告格式的
            舍入"，不是"画错了"；
        (2) 双方都折到预乘 ARGB32_Premultiplied 再比 —— 与光栅引擎实际写入的数值同域，
            这才是"上屏是不是精确拷贝"的正确判据。

      两种都量、都打进日志，**断言用 (2)**；(1) 的数字留着当参照。实测（atri、dpr 1.25）：
      (2) 预乘域 0/585000 个像素不同、maxChannelDiff=0 —— 绘制一个预乘字节都没动；
      (1) 非预乘域有 9468 个像素不同（其中 3718 个差 >1，maxChannelDiff=10）——
      把登记帧自己走一遍"预乘 → 非预乘"的往返（roundTrip），它在非预乘域与画出来的图**逐位相同**，
      所以 (1) 的全部残差就是这条往返自身的舍入，跟绘制无关：登记帧是非预乘 RGBA8888，
      而任何东西要上屏都必须经过预乘缓冲，低 alpha 像素的非预乘 RGB 在那里本来就存不下。

      容差 1 级的理由：预乘转换用 (x·a+127)/255 取整，源侧与画侧各做一次，最坏差 1 级；
      实测预乘域是逐位相同的（日志 premultMaxDiff=0），1 级只是留给别的 Qt 版本的余量。*/
    constexpr int kPaintBlitTolerance = 1;
    const QImage source = registered.convertToFormat(QImage::Format_RGBA8888);
    const QImage painted = grabbed.toImage().convertToFormat(QImage::Format_RGBA8888);
    const QImage premultSource =
        registered.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QImage premultPainted =
        grabbed.toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);

    int rawMaxDiff = 0;
    qint64 rawDiffering = 0;     // 非预乘空间：逐位不同的像素（修复前这一项占 17.31%）
    qint64 rawOverTolerance = 0; // 非预乘空间：差 > 容差的像素
    qint64 invisibleSkipped = 0; // 双方都全透明：非预乘 RGB 不可观察，跳过
    int premultMaxDiff = 0;
    qint64 premultDiffering = 0;
    qint64 premultOverTolerance = 0;

    for (int y = 0; y < source.height(); ++y)
    {
        const uchar *a = source.constScanLine(y);
        const uchar *b = painted.constScanLine(y);
        const uchar *pa = premultSource.constScanLine(y);
        const uchar *pb = premultPainted.constScanLine(y);
        for (int x = 0; x < source.width(); ++x)
        {
            if (a[x * 4 + 3] == 0 && b[x * 4 + 3] == 0)
            {
                ++invisibleSkipped;
            }
            else
            {
                int pixelMax = 0;
                for (int c = 0; c < 4; ++c)
                    pixelMax = std::max(pixelMax, qAbs(int(a[x * 4 + c]) - int(b[x * 4 + c])));
                rawMaxDiff = std::max(rawMaxDiff, pixelMax);
                if (pixelMax > 0)
                    ++rawDiffering;
                if (pixelMax > kPaintBlitTolerance)
                    ++rawOverTolerance;
            }

            int premultPixelMax = 0;
            for (int c = 0; c < 4; ++c)
                premultPixelMax =
                    std::max(premultPixelMax, qAbs(int(pa[x * 4 + c]) - int(pb[x * 4 + c])));
            premultMaxDiff = std::max(premultMaxDiff, premultPixelMax);
            if (premultPixelMax > 0)
                ++premultDiffering;
            if (premultPixelMax > kPaintBlitTolerance)
                ++premultOverTolerance;
        }
    }

    const qint64 total = static_cast<qint64>(painted.width()) * painted.height();

    /*残差归因（免得把"报告格式的往返舍入"误当成"画错了"）：
      让登记帧自己走一遍"预乘 → 非预乘"的往返，再与画出来的图在非预乘域比。
      若两者逐位相同，就证明非预乘域的全部残差都来自 un-premultiply 的舍入放大，
      绘制本身一个预乘字节都没动过。*/
    const QImage roundTrip = premultSource.convertToFormat(QImage::Format_RGBA8888);
    int roundTripMaxDiff = -1;
    const qint64 roundTripDiff = comparePixels(roundTrip, painted, &roundTripMaxDiff);

    qInfo("绘制一致性[%s]：画布 %dx%d × dpr %.3f = %dx%d（整数），登记帧 %dx%d、dpr %.3f，"
          "抓图 %dx%d（帧格式 %d / 抓图格式 %d）| 预乘域：逐通道差 >%d 的像素 = %lld/%lld"
          "（%.4f%%），maxChannelDiff=%d，逐位不同 %lld | 非预乘域：差 >%d 的像素 = %lld"
          "（maxChannelDiff=%d，逐位不同 %lld）| 归因：un-premultiply 往返自身就造成 %lld 个"
          "像素不同（maxChannelDiff=%d），全透明跳过 %lld",
          qPrintable(modelName), logical.width(), logical.height(), double(dpr), qRound(deviceW),
          qRound(deviceH), registered.width(), registered.height(),
          double(registered.devicePixelRatio()), grabbed.width(), grabbed.height(),
          int(registered.format()), int(grabbed.toImage().format()), kPaintBlitTolerance,
          premultOverTolerance, total, premultOverTolerance * 100.0 / static_cast<double>(total),
          premultMaxDiff, premultDiffering, kPaintBlitTolerance, rawOverTolerance, rawMaxDiff,
          rawDiffering, roundTripDiff, roundTripMaxDiff, invisibleSkipped);

    QVERIFY2(premultOverTolerance == 0,
             qPrintable(QStringLiteral("上屏不是精确拷贝（预乘域）：%1/%2 像素的通道差超过 %3 级，"
                                       "maxChannelDiff=%4。画布 %5x%6、登记帧 %7x%8，"
                                       "逐通道差很小的残差只可能来自预乘舍入，而这里超了 %3 级")
                            .arg(premultOverTolerance)
                            .arg(total)
                            .arg(kPaintBlitTolerance)
                            .arg(premultMaxDiff)
                            .arg(logical.width())
                            .arg(logical.height())
                            .arg(registered.width())
                            .arg(registered.height())));

    /*根因不变量（放在最后，见上面的说明）：画布必须设备像素对齐。
      它保证"精确拷贝"不是碰巧成立，而是尺寸算法本身就排除了重采样。*/
    QVERIFY2(deviceAligned,
             qPrintable(QStringLiteral("画布 %1x%2 × dpr %3 = %4x%5 不是整数："
                                       "登记帧的逻辑尺寸必然与窗口不等，上屏那一步只能重采样")
                            .arg(logical.width())
                            .arg(logical.height())
                            .arg(double(dpr))
                            .arg(deviceW)
                            .arg(deviceH)));
}

/*「桌宠在屏幕上看着不清晰」——这条用例**只测量，不作断言**。

  为什么不做成断言：清晰度没有客观的"及格线"。不同模型、不同姿势、不同时刻的读数
  本来就会浮动，卡一个阈值只会变成偶发失败。这里把每个边界的数字打成日志、把可比对的图
  存成 PNG，由人来看数字决定要不要改。（**"上屏是不是精确拷贝"那条是断言**，
  在 paintsRegisteredFrameWithoutResampling 里，因为它有一条客观判据。）

  管线与可能丢清晰度的位置：
    [A] Cubism moc3 + 4096 宽贴图 → GL 渲染进离屏 FBO（分辨率 = 逻辑×dpr×live2dScale）
    [B] glReadPixels → QImage（renderSize 那么大）
    [C] 登记进窗口（m_scaledImg），其 devicePixelRatio 被设成**窗口自己的 dpr**
    [D] paintEvent: drawImage(rect(), img) —— 不是精确 1:1 时 Qt 会重采样
    [E] 上屏，屏幕 dpr 1.25（画布 936 逻辑 → 1170 设备像素，整数，所以 D 是精确拷贝）

  本用例量四件事：
    1) 缩放扫描 1.0 / 1.25 / 1.5 / 2.0，量**抓图**（= 真正上屏的东西）的锐度。
       抓图在所有缩放下都是同一个设备像素尺寸，所以数字可以直接横着比。
       （>1.0 的档位是超采样：绘制本来就要平滑下采样，掉一点方差是收益的代价。）
    2) 边界 D：1.0x 下把"登记帧"与"画出来的抓图"在同一像素尺寸下逐像素比，
       再用**同一个完全不透明矩形**重量一次，把"掩码被重采样"从"像素值被改"里剥出来。
    3) 边界 E：把尺寸/dpr/rect 的算术原样打出来（画布 × dpr 是否整数，一眼可查）。
    4) 存 1:1 裁切（不放大）与整张抓图到 build2/tests/live2d-probe/，供人眼像素级比对；
       1.0x 那一轮额外存一张"登记帧"的同位置裁切，两图可以并排看。

  配置隔离：缩放只写进 initTestCase 建好的**临时** ini（MANDARIN_CONFIG_INI），
  用户的真实 config.ini 全程只读。（窗口侧读的是 settingsPath()，
  applyFrameRateFromConfig 里那一处读 IniSettingPath 是帧率、本用例不碰。）*/
void TestLive2DWindow::reportsSharpnessAcrossScales()
{
    const QString modelName = preferredModelName();
    const QString dir = modelDirFor(modelName);
    if (!QFileInfo::exists(dir))
        QSKIP("本机没有用户配置的模型（禁二传，不入库），跳过清晰度测量");

    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString outDir = appDir.absoluteFilePath(QStringLiteral("../live2d-probe"));
    QVERIFY2(QDir().mkpath(outDir), qPrintable(QStringLiteral("建不出输出目录 %1").arg(outDir)));

    /*ROI 比例固定：横取包围盒中间 45%、纵取上部 22%（脸/头发区）。
       因为包围盒高度在所有缩放下都是同一个设备像素数（抓图尺寸恒定），
       算出来的 ROI 尺寸也就恒定 —— 这正是"可以像素级横着比"的前提。*/
    constexpr double kRoiWidthFraction = 0.45;
    constexpr double kRoiHeightFraction = 0.22;

    const auto regionOfInterest = [](const QRect &bbox) {
        const int rw = std::max(8, static_cast<int>(bbox.width() * kRoiWidthFraction));
        const int rh = std::max(8, static_cast<int>(bbox.height() * kRoiHeightFraction));
        const int rx = bbox.x() + (bbox.width() - rw) / 2;
        const int ry = bbox.y();
        return QRect(rx, ry, rw, rh);
    };

    QStringList log;
    const auto emitLog = [&log]() { qInfo().noquote() << log.join(QLatin1Char('\n')); };
    /*内容指纹：用来回答"我对比的两张图，到底是不是同一帧"。
       只报统计量（像素数/包围盒）分不出"改了姿势"和"改了像素值"，哈希能。*/
    const auto contentHash = [](const QImage &img) {
        const QImage rgba = img.convertToFormat(QImage::Format_RGBA8888);
        return QString::fromLatin1(QCryptographicHash::hash(
                                       QByteArray(reinterpret_cast<const char *>(rgba.constBits()),
                                                  static_cast<int>(rgba.sizeInBytes())),
                                       QCryptographicHash::Md5)
                                       .toHex());
    };

    const double scales[] = {1.0, 1.25, 1.5, 2.0};
    QSize roiSize;
    bool roiSizeKnown = false;
    QSize grabSizeAtScale1;
    bool grabSizeAtScale1Known = false;

    for (double scale : scales)
    {
        /*只写临时配置。窗口在 loadModel() 里读它（applyRenderScaleFromConfig）。*/
        {
            QSettings settings(m_tempConfigPath, QSettings::IniFormat);
            settings.setValue("character/live2dFps", 60);
            settings.setValue("character/live2dScale", scale);
            settings.sync();
        }

        log << QStringLiteral("SHARPNESS scale=%1 --------").arg(scale);

        Live2DCharacterWindow window;
        window.loadModel(modelName);
        QVERIFY2(window.isModelLoaded(),
                 qPrintable(QStringLiteral("scale %1：模型装载失败").arg(scale)));

        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCoreApplication::processEvents();

        /*冻住帧循环（hideEvent 停表），再自己渲一帧：
           这样"登记的帧"与"抓到的图"之间姿势不会自己漂移，D 的逐像素对比才成立。*/
        window.hide();
        QCoreApplication::processEvents();
        QVERIFY2(window.renderFrameNow(), "渲染失败，无法量清晰度");
        QCoreApplication::processEvents();

        const QImage registered = window.renderedImage();
        QVERIFY2(!registered.isNull(), "登记帧为空");
        /*登记帧的内容指纹。抓图之后再取一次：两次一致 ⇒ 我比的是同一帧，
           不一致 ⇒ 说明抓图过程本身让窗口换了一帧，D 的逐像素对比必须先排除这一项。*/
        const QString registeredHashBefore = contentHash(registered);
        /*抓图前后各读一次 dpr：抓图本身有可能改变窗口的 dpr（跨屏/重绘），
           只读一次会分不清"登记时用的 dpr"和"抓图时用的 dpr"。*/
        const qreal dprBeforeGrab = window.devicePixelRatioF();
        const QPixmap grabbed = window.grab();
        QVERIFY2(!grabbed.isNull(), "window.grab() 失败");
        const qreal dprAfterGrab = window.devicePixelRatioF();
        const QImage painted = grabbed.toImage().convertToFormat(QImage::Format_RGBA8888);
        const QString registeredHashAfter = contentHash(registered);
        log << QStringLiteral("  [3a] dpr before grab=%1 after grab=%2 (dpr used to compute "
                              "renderSize = %3)")
                   .arg(dprBeforeGrab)
                   .arg(dprAfterGrab)
                   .arg(dprBeforeGrab);
        log << QStringLiteral("  [3b] registered content hash before grab=%1 after grab=%2 same=%3")
                   .arg(registeredHashBefore)
                   .arg(registeredHashAfter)
                   .arg(registeredHashBefore == registeredHashAfter ? QStringLiteral("YES")
                                                                    : QStringLiteral("NO"));

        /*---- 实验 3：边界 E 的算术（每个缩放都打一遍，数字本身就是要报的量）----*/
        const QSize logical = window.size();
        const qreal dpr = window.devicePixelRatioF();
        const QSize renderSize = window.renderSize();
        const qreal logicalW = logical.width() * dpr;
        const qreal logicalH = logical.height() * dpr;
        const int roundedW = int(std::lround(logicalW));
        const int roundedH = int(std::lround(logicalH));
        const int truncatedW = int(logicalW);
        const int truncatedH = int(logicalH);

        log << QStringLiteral(
                   "  [3] widget logical=%1x%2 dpr=%3 logical*dpr=%4x%5 (rounded %6x%7 / "
                   "truncated %8x%9)")
                   .arg(logical.width())
                   .arg(logical.height())
                   .arg(dpr)
                   .arg(logicalW)
                   .arg(logicalH)
                   .arg(roundedW)
                   .arg(roundedH)
                   .arg(truncatedW)
                   .arg(truncatedH);
        log << QStringLiteral("      renderSize(registered image)=%1x%2  registered image dpr=%3 "
                              " rect()=%4,%5 %6x%7")
                   .arg(renderSize.width())
                   .arg(renderSize.height())
                   .arg(registered.devicePixelRatio())
                   .arg(window.rect().x())
                   .arg(window.rect().y())
                   .arg(window.rect().width())
                   .arg(window.rect().height());
        const bool imageDprMatchesWindow = (qAbs(registered.devicePixelRatio() - dpr) < 1e-9);
        const bool renderMatchesWindowDevicePixels = (renderSize == QSize(roundedW, roundedH));
        QString dprMatch = imageDprMatchesWindow ? QStringLiteral("YES") : QStringLiteral("NO");
        QString sizeMatch = renderMatchesWindowDevicePixels ? QStringLiteral("YES") : QStringLiteral("NO");
        QString line3;
        line3 = QStringLiteral("      grab painted image =%1x%2  grab pixmap dpr=%3")
                    .arg(painted.width())
                    .arg(painted.height())
                    .arg(grabbed.devicePixelRatio());
        line3 += QStringLiteral("  registered==window*dpr? %1 (diff %2x%3)")
                     .arg(sizeMatch)
                     .arg(renderSize.width() - roundedW)
                     .arg(renderSize.height() - roundedH);
        line3 += QStringLiteral("  registered image dpr==window dpr? %1").arg(dprMatch);
        log << line3;

        /*---- 实验 1：在抓图上量锐度（= 真正上屏的东西）----*/
        const SharpnessStats paintedStats = measureSharpness(painted, kOpaqueMaskAlpha);

        if (!roiSizeKnown)
            roiSize = regionOfInterest(paintedStats.opaqueBounds).size();

        QString line1 = QStringLiteral("  [1] grab  laplacianVariance=%1 meanGradient=%2 "
                                       "lumStdDev=%3 opaquePixels=%4 bbox=%5,%6 %7x%8 "
                                       "image=%9x%10")
                            .arg(paintedStats.laplacianVariance, 0, 'f', 4)
                            .arg(paintedStats.meanGradient, 0, 'f', 4)
                            .arg(paintedStats.luminanceStdDev, 0, 'f', 4)
                            .arg(paintedStats.measuredPixels)
                            .arg(paintedStats.opaqueBounds.x())
                            .arg(paintedStats.opaqueBounds.y())
                            .arg(paintedStats.opaqueBounds.width())
                            .arg(paintedStats.opaqueBounds.height())
                            .arg(painted.width())
                            .arg(painted.height());
        line1 += QStringLiteral("  [roi probe %1x%2 from 1.0x]")
                     .arg(roiSize.width())
                     .arg(roiSize.height());
        log << line1;

        /*---- 实验 4：1:1 裁切（不放大）。裁切区一律从抓图的包围盒推出。----*/
        const QRect roi = regionOfInterest(paintedStats.opaqueBounds);
        if (!roiSizeKnown)
        {
            roiSize = roi.size();
            roiSizeKnown = true;
        }
        else
        {
            /*ROI 尺寸跨缩放要基本一致（差几像素只是 alpha 阈值上的包围盒抖动，
               它在"人物占 970 行、取上部 22%"下只影响裁切窗口在垂直方向 1~2 像素）。
               这里用容差而不是严格相等：严格相等会变成偶发失败，
               而这条用例的定位是**测量**，不该因为 1 像素抖动就中断整轮扫描。*/
            const int tol = 4;
            if (qAbs(roi.width() - roiSize.width()) > tol ||
                qAbs(roi.height() - roiSize.height()) > tol)
            {
                log << QStringLiteral("  [4] WARNING roi size %1x%2 differs from %3x%4 by more than "
                                      "%5 px - cross-scale crop comparison is not 1:1")
                           .arg(roi.width())
                           .arg(roi.height())
                           .arg(roiSize.width())
                           .arg(roiSize.height())
                           .arg(tol);
            }
        }

        const QString cropPath =
            QDir(outDir).absoluteFilePath(QStringLiteral("sharpness-crop-scale%1.png").arg(scale));
        const QString fullPath =
            QDir(outDir).absoluteFilePath(QStringLiteral("sharpness-full-scale%1.png").arg(scale));
        QVERIFY2(painted.copy(roi).save(cropPath),
                 qPrintable(QStringLiteral("写不出 %1").arg(cropPath)));
        QVERIFY2(grabbed.save(fullPath), qPrintable(QStringLiteral("写不出 %1").arg(fullPath)));
        log << QStringLiteral("  [4] crop roi=%1,%2 %3x%4 -> %5")
                   .arg(roi.x())
                   .arg(roi.y())
                   .arg(roi.width())
                   .arg(roi.height())
                   .arg(cropPath);
        log << QStringLiteral("      full grab -> %1").arg(fullPath);

        /*---- 实验 2：只在 1.0x 上做边界 D 的逐像素对比 ----*/
        if (qFuzzyCompare(scale, 1.0))
        {
            grabSizeAtScale1 = painted.size();
            grabSizeAtScale1Known = true;

            /*登记帧自己也量一份（同一套指标），用来回答"损失是在 B/C 就发生了，
               还是 D 又补了一刀"。*/
            const SharpnessStats registeredStats =
                measureSharpness(registered, kOpaqueMaskAlpha);
            log << QStringLiteral("  [1'] registered (pre-paint) lapVar=%1 grad=%2 lumStdDev=%3 "
                                  "opaquePixels=%4 bbox=%5,%6 %7x%8")
                       .arg(registeredStats.laplacianVariance, 0, 'f', 4)
                       .arg(registeredStats.meanGradient, 0, 'f', 4)
                       .arg(registeredStats.luminanceStdDev, 0, 'f', 4)
                       .arg(registeredStats.measuredPixels)
                       .arg(registeredStats.opaqueBounds.x())
                       .arg(registeredStats.opaqueBounds.y())
                       .arg(registeredStats.opaqueBounds.width())
                       .arg(registeredStats.opaqueBounds.height());

            log << QStringLiteral("  [2] boundary D @1.0x: registered=%1x%2 painted=%3x%4")
                       .arg(registered.width())
                       .arg(registered.height())
                       .arg(painted.width())
                       .arg(painted.height());

            if (registered.size() == painted.size())
            {
                int maxDiff = 0;
                const qint64 differing = comparePixels(registered, painted, &maxDiff);
                const qint64 total = static_cast<qint64>(painted.width()) * painted.height();
                log << QStringLiteral(
                           "      registered vs painted: maxChannelDiff=%1 differingPixels=%2/%3 "
                           "(%4%)")
                           .arg(maxDiff)
                           .arg(differing)
                           .arg(total)
                           .arg(differing * 100.0 / static_cast<double>(total), 0, 'f', 5);

                /*固定矩形对照：矩形取"登记帧里自身与四邻域都完全不透明"的区域，
                   两张图用**同一个**矩形重量一次。这样差异只可能来自"像素值被改"
                   （重采样），而不是"掩码边界被重采样改变"——整图口径会把这两者混在一起。*/
                const QRect solidRoi = regionOfInterest(registeredStats.solidFill);
                if (!solidRoi.isEmpty() && registeredStats.solidFill.contains(solidRoi))
                {
                    const SharpnessStats regInRoi =
                        measureSharpness(registered, kOpaqueMaskAlpha, solidRoi);
                    const SharpnessStats paintInRoi =
                        measureSharpness(painted, kOpaqueMaskAlpha, solidRoi);
                    log << QStringLiteral(
                               "      fixed ROI=%1,%2 %3x%4 (all alpha>=250): registered "
                               "lapVar=%5 grad=%6 px=%7 | painted lapVar=%8 grad=%9 px=%10 "
                               "(%11%)")
                               .arg(solidRoi.x())
                               .arg(solidRoi.y())
                               .arg(solidRoi.width())
                               .arg(solidRoi.height())
                               .arg(regInRoi.laplacianVariance, 0, 'f', 4)
                               .arg(regInRoi.meanGradient, 0, 'f', 4)
                               .arg(regInRoi.measuredPixels)
                               .arg(paintInRoi.laplacianVariance, 0, 'f', 4)
                               .arg(paintInRoi.meanGradient, 0, 'f', 4)
                               .arg(paintInRoi.measuredPixels)
                               .arg((paintInRoi.laplacianVariance - regInRoi.laplacianVariance) *
                                        100.0 / std::max(1e-9, regInRoi.laplacianVariance),
                                    0, 'f', 3);
                }
                else
                {
                    log << QStringLiteral("      fixed ROI skipped: solidFill=%1,%2 %3x%4")
                               .arg(registeredStats.solidFill.x())
                               .arg(registeredStats.solidFill.y())
                               .arg(registeredStats.solidFill.width())
                               .arg(registeredStats.solidFill.height());
                }
            }
            else
            {
                /*尺寸不同 = 绘制改变了像素网格，逐像素比较无从谈起。
                   对齐修复之后这条分支不该再出现；真出现了，先看 [3] 那行的
                   "logical*dpr" 与"registered image"对不对得上。*/
                log << QStringLiteral("      sizes differ (%1x%2 vs %3x%4) -> no 1:1 blit at 1.0x; "
                                      "per-pixel compare skipped")
                           .arg(registered.width())
                           .arg(registered.height())
                           .arg(painted.width())
                           .arg(painted.height());
            }

            const QString regCropPath = QDir(outDir).absoluteFilePath(
                QStringLiteral("sharpness-crop-scale1.0-registered.png"));
            QVERIFY2(registered.copy(roi).save(regCropPath),
                     qPrintable(QStringLiteral("写不出 %1").arg(regCropPath)));
            log << QStringLiteral("  [4] registered crop -> %1").arg(regCropPath);
        }

        emitLog();
        log.clear();
    }

    QVERIFY2(grabSizeAtScale1Known, "1.0x 那一轮没跑到，实验 2 没数据");
    log << QStringLiteral("SHARPNESS summary: ROI size=%1x%2 (constant across scales), "
                          "grab size @1.0x=%3x%4")
               .arg(roiSize.width())
               .arg(roiSize.height())
               .arg(grabSizeAtScale1.width())
               .arg(grabSizeAtScale1.height());
    log << QStringLiteral("SHARPNESS note: this is a MEASUREMENT, not an assertion - "
                          "no sharpness threshold is enforced.");
    emitLog();
}

/*==================== 心情 → 表情 ====================

  背景（本阶段的 bug）：AI 每句回复都发一个心情名，而 Live2D 路径把它当成**模型名** ——
  找不到同名模型就静默 return，于是立绘永远一副表情。
  `reloadContent(心情名)` 现在按基类契约"按名切换内容"，Live2D 侧解释成"换情绪预设"。

  数据（用户数据区，不入库）：
    parameter-map.json  语义参数名 → 参数 ID + [min,max] + neutral
    presets/moods.json  14 个情绪原型（只写与 neutral 的差异）+ 26 个心情名 → 原型
  心情名 = 角色 Tachie/ 下 PNG 的文件名（AI 的可选词表也是从那里生成的）。

  参数取值域（min/max/neutral）**只能来自模型自己声明的值**（moc 的 Core API），
  不许手推/手抄：手抄的代价是"表情里写着一个永远到不了的值"（mouthForm 的正数被夹成 0）
  与"中立位不是模型的静息值"。这条纪律由 parameterMapRangesMatchModelDeclarations 钉住。*/

/*词表核对：Tachie/ 里每个 PNG 名都必须能在 moods.json 里解析出原型。

  为什么必须**双向**核对：只查"别名 → 原型"会漏掉"用户新加了一张立绘、词表没跟上"——
  那个心情在实机上会安静地回退成 neutral，也就是"她对这个词没反应"。
  反方向（别名表里有、Tachie 里没有）则是孤儿条目，说明数据该清理了。*/
void TestLive2DWindow::moodPresetCoversEveryTachieMood()
{
    const QString tachieDir = ReadCharacterTachiePath();
    if (tachieDir.isEmpty())
        QSKIP("本机没有当前角色的 Tachie 目录（角色资源不入库），跳过心情词表核对");

    QStringList pngNames;
    for (const QFileInfo &info : QDir(tachieDir).entryInfoList({QStringLiteral("*.png")}, QDir::Files))
        pngNames.append(info.completeBaseName());
    pngNames.sort();
    QVERIFY2(!pngNames.isEmpty(),
             qPrintable(QStringLiteral("%1 里没有任何 PNG，心情词表无从核对").arg(tachieDir)));

    Live2DMoodPreset preset;
    if (!loadConfiguredMoodPreset(&preset))
        QSKIP("本机没有当前角色/模型的心情预设数据（Documents 下，不入库），跳过");

    const QHash<QString, QString> aliases = preset.moodAliases();

    // ① 立绘名 → 别名：缺一个就是"这个词她没反应"
    QStringList missing;
    for (const QString &moodName : pngNames)
    {
        if (!aliases.contains(moodName))
            missing.append(moodName);
    }
    QVERIFY2(missing.isEmpty(),
             qPrintable(QStringLiteral("Tachie 里有 %1 个立绘名在 moods.json 里没有别名"
                                       "（这些心情会静默回退成 neutral）：%2")
                            .arg(missing.size())
                            .arg(missing.join(QStringLiteral(", ")))));

    // ② 别名 → 立绘名：孤儿别名（立绘已删）也要报出来
    QStringList orphans;
    for (auto it = aliases.constBegin(); it != aliases.constEnd(); ++it)
    {
        if (!pngNames.contains(it.key()))
            orphans.append(it.key());
    }
    QVERIFY2(orphans.isEmpty(),
             qPrintable(QStringLiteral("moods.json 里有 %1 个别名在 Tachie 里找不到对应立绘：%2")
                            .arg(orphans.size())
                            .arg(orphans.join(QStringLiteral(", ")))));

    // ③ 每个原型都得能解析出参数组、并且查得到一个代表它的心情名 ——
    //    否则校准对照图会出现空的一格 / 一格都出不来
    for (const QString &archetype : preset.archetypeNames())
    {
        const QHash<QString, float> params = preset.parametersForArchetype(archetype);
        QVERIFY2(!params.isEmpty(),
                 qPrintable(QStringLiteral("原型 %1 解析不出任何参数").arg(archetype)));
        QVERIFY2(!preset.representativeMoodForArchetype(archetype).isEmpty(),
                 qPrintable(QStringLiteral("原型 %1 没有任何别名指向它，校准图出不了这一格")
                                .arg(archetype)));
    }

    qInfo("心情词表核对[%s]：Tachie 立绘 %d 个、别名 %d 条、原型 %d 个，双向零缺失零孤儿",
          qPrintable(preset.modelName()), pngNames.size(), aliases.size(),
          preset.archetypeNames().size());
}

/*按用户配置装载**模型**（不是情绪数据）。两处路径在本机是分开的：

  模型文件  Documents/Mandarin/Live2D/<模型名>/
  情绪数据  Documents/Mandarin/Character/Assets/<角色>/Live2D/<模型名>/

  （名字对不上是模型作者与用户各自的组织方式，代码从第一天就走"两个候选目录"匹配，
  见 Live2DCharacterWindow::resolveModelDir；这里沿用同样的两条路径，不新造规则。）*/
bool TestLive2DWindow::openConfiguredModel(Live2DOffscreenRenderer *renderer)
{
    if (renderer == nullptr)
        return false;
    const QString modelName = preferredModelName();
    if (modelName.isEmpty())
        return false;

    /*候选①：用户配置的模型根目录；候选②：角色资源目录（与情绪数据同层）。
      两者都找不到就返回 false（调用方 QSKIP）。*/
    QStringList candidates;
    candidates << modelDirFor(modelName)
               << Live2DMoodPreset::resolveModelDir(modelName);
    for (const QString &dir : candidates)
    {
        if (dir.isEmpty() || !QFileInfo::exists(dir))
            continue;
        const QStringList jsonCandidates =
            QDir(dir).entryList({QStringLiteral("*.model3.json")}, QDir::Files);
        for (const QString &jsonName : jsonCandidates)
        {
            QString error;
            if (renderer->load(dir, jsonName, &error))
                return true;
        }
    }
    return false;
}

/*==================== 范围纪律：数据必须来自模型，不能手抄 ====================

  背景（这条测试就是为它写的）：`parameter-map.json` 的 [min,max] 当初是**手推**的，
  参照的是模型自带的 `*.vtube.json`（VTS 的 OutputRange）。VTS 的 OutputRange 是
  **某个 VTube Studio 配置允许把参数推到多远**，不是这个 moc 声明的取值域 —— 两回事。

  实测后果（本模型 atri，都是 Core 自己夹的，写在屏幕上看得见）：
    - `ParamMouthForm`：moc 声明 **[-1, 0] / 默认 -0.5**，map 写了 [-1, +1] / 中立 0.0。
      happy/excited/confident 想笑（+0.6~+0.9）被夹成 0.0，读回值永远是 0；
      更糟的是中立位被钉在 0.0 —— 而模型的**静息值**是 -0.5，于是"中立"这张脸从一开始
      就不是模型作者设计的静息表情。
    - `ParamEyeLOpen/ROpen`：moc 最大 **1.0**，map 写了 1.9（surprised 的 1.7 被夹成 1.0）。
    - `ParamMouthOpenY`：moc 最大 **1.0**，map 写了 2.1。

  所以这里**逐条**核对：map 的 min/max/neutral 必须等于模型声明的 最小/最大/默认，
  只允许一个抗浮点噪声的 eps（JSON 里的十进制字面量到 float 的往返）。
  少一条、多一条、任一项不等，都是数据错 —— 而且要在目视校准之前就报出来，
  否则人会去校准一个永远不会生效的数字。

  **报出全部差异再断言**：只报第一条会让人修一轮跑一轮，n 条差异要跑 n 遍。*/
void TestLive2DWindow::parameterMapRangesMatchModelDeclarations()
{
    Live2DMoodPreset preset;
    if (!loadConfiguredMoodPreset(&preset))
        QSKIP("本机没有当前角色/模型的心情预设数据（Documents 下，不入库），跳过");

    Live2DOffscreenRenderer renderer;
    if (!openConfiguredModel(&renderer))
        QSKIP("本机没有用户配置的模型（禁二传，不入库），跳过参数范围核对");

    // 模型自己声明的取值域，直接问 Core（renderer 已经把 moc 打开成 CubismModel）
    const QHash<QString, Live2DOffscreenRenderer::DeclaredRange> declared =
        renderer.declaredParameterRanges();
    QVERIFY2(!declared.isEmpty(), "模型没有声明任何参数（装载失败或读不出取值域）");

    /*JSON 里的十进制字面量经 double→float 往返会有 ~1e-7 的噪声；模型自己声明的
      0.5/-0.5 也一样。1e-4 足够盖住噪声，又远小于任何一个有意的手抄偏差
      （最小的那个也有 0.1 量级：mouthForm 的 +1 vs 0）。*/
    constexpr float kEpsilon = 1e-4f;

    const QHash<QString, Live2DMoodPreset::ParameterRange> ranges = preset.parameters();
    QVERIFY2(!ranges.isEmpty(), "parameter-map.json 里一条参数都没有");

    QStringList missing;   // map 里写了、模型里没有
    QStringList wrongMin;  // map.min ≠ 模型声明的最小值
    QStringList wrongMax;
    QStringList wrongNeutral;
    int matched = 0;

    for (auto it = ranges.constBegin(); it != ranges.constEnd(); ++it)
    {
        const QString semanticName = it.key();
        const Live2DMoodPreset::ParameterRange range = it.value();
        if (!declared.contains(range.id))
        {
            missing.append(QStringLiteral("%1(%2)").arg(semanticName, range.id));
            continue;
        }
        const Live2DOffscreenRenderer::DeclaredRange d = declared.value(range.id);
        bool ok = true;
        if (qAbs(range.min - d.min) > kEpsilon)
        {
            wrongMin.append(QStringLiteral("%1(%2) map %3 ≠ 模型 %4")
                                .arg(semanticName, range.id)
                                .arg(double(range.min))
                                .arg(double(d.min)));
            ok = false;
        }
        if (qAbs(range.max - d.max) > kEpsilon)
        {
            wrongMax.append(QStringLiteral("%1(%2) map %3 ≠ 模型 %4")
                                .arg(semanticName, range.id)
                                .arg(double(range.max))
                                .arg(double(d.max)));
            ok = false;
        }
        if (qAbs(range.neutral - d.neutral) > kEpsilon)
        {
            wrongNeutral.append(QStringLiteral("%1(%2) map %3 ≠ 模型默认 %4")
                                    .arg(semanticName, range.id)
                                    .arg(double(range.neutral))
                                    .arg(double(d.neutral)));
            ok = false;
        }
        if (ok)
            ++matched;
    }

    /*把"模型的真值"打出来（每台机器都能看到自己那份数据的事实）：
      目视校准的人应该照着这张表去调，而不是照 map 里的数字。*/
    QStringList table;
    for (auto it = ranges.constBegin(); it != ranges.constEnd(); ++it)
    {
        const Live2DMoodPreset::ParameterRange range = it.value();
        const Live2DOffscreenRenderer::DeclaredRange d = declared.value(range.id);
        table.append(QStringLiteral("%1 %2 map=[%3,%4] neutral=%5 | model=[%6,%7] default=%8")
                         .arg(it.key(), range.id)
                         .arg(double(range.min))
                         .arg(double(range.max))
                         .arg(double(range.neutral))
                         .arg(double(d.min))
                         .arg(double(d.max))
                         .arg(double(d.neutral)));
    }
    table.sort();
    for (const QString &line : table)
        qInfo("%s", qPrintable(QStringLiteral("RANGE ") + line));

    QStringList problems;
    if (!missing.isEmpty())
        problems.append(QStringLiteral("模型里没有这些参数（%1）：%2")
                            .arg(missing.size())
                            .arg(missing.join(QStringLiteral(", "))));
    if (!wrongMin.isEmpty())
        problems.append(QStringLiteral("min 不是模型声明的最小值（%1）：%2")
                            .arg(wrongMin.size())
                            .arg(wrongMin.join(QStringLiteral(" | "))));
    if (!wrongMax.isEmpty())
        problems.append(QStringLiteral("max 不是模型声明的最大值（%1）：%2")
                            .arg(wrongMax.size())
                            .arg(wrongMax.join(QStringLiteral(" | "))));
    if (!wrongNeutral.isEmpty())
        problems.append(QStringLiteral("neutral 不是模型声明的默认值（%1）：%2")
                            .arg(wrongNeutral.size())
                            .arg(wrongNeutral.join(QStringLiteral(" | "))));

    qInfo("参数范围核对[%s]：模型声明 %d 个参数，parameter-map 里 %d 条，逐项相符 %d 条",
          qPrintable(preset.modelName()), declared.size(), ranges.size(), matched);

    QVERIFY2(problems.isEmpty(),
             qPrintable(QStringLiteral("parameter-map.json 的取值域与模型声明不符 —— "
                                       "范围/中立值必须从模型读，不能手抄：\n%1")
                            .arg(problems.join(QStringLiteral("\n")))));
}

/*数值范围核对：moods.json 的每个原型的**原始**取值都必须在 parameter-map 的 [min,max] 内，
  且不许出现驱动器/物理占用的参数。

  为什么查"原始值"而不是查夹取后的结果：装载器会把越界值夹回范围内（那是运行期的安全网），
  所以夹取后的结果永远合法 —— 那条断言等于什么都没验。真正要钉住的是**初稿数据本身**：
  越界 = 数据写错了，得在目视校准前先发现，而不是被夹取悄悄改掉。*/
void TestLive2DWindow::moodPresetValuesWithinParameterRanges()
{
    Live2DMoodPreset preset;
    if (!loadConfiguredMoodPreset(&preset))
        QSKIP("本机没有当前角色/模型的心情预设数据（Documents 下，不入库），跳过");

    const QHash<QString, Live2DMoodPreset::ParameterRange> ranges = preset.parameters();
    const QHash<QString, QHash<QString, float>> deltas = preset.archetypeDeltas();
    QVERIFY2(!ranges.isEmpty() && !deltas.isEmpty(), "参数表或原型表是空的");

    int checkedValues = 0;
    for (auto archetype = deltas.constBegin(); archetype != deltas.constEnd(); ++archetype)
    {
        const QHash<QString, float> values = archetype.value();
        for (auto value = values.constBegin(); value != values.constEnd(); ++value)
        {
            const QString semanticName = value.key();
            const Live2DMoodPreset::ParameterRange range = ranges.value(semanticName);
            QVERIFY2(!range.id.isEmpty(),
                     qPrintable(QStringLiteral("原型 %1 用了 parameter-map 里没有的语义名 %2")
                                    .arg(archetype.key())
                                    .arg(semanticName)));
            // 不许碰驱动器/物理占用的参数（呼吸、头发）：语义名与真实 ID 两侧都查
            QVERIFY2(!Live2DMoodPreset::isUpdaterOwnedParameter(range.id),
                     qPrintable(QStringLiteral("原型 %1 写了驱动器/物理占用的参数 %2 (%3)")
                                    .arg(archetype.key())
                                    .arg(semanticName)
                                    .arg(range.id)));
            QVERIFY2(semanticName != QStringLiteral("breath") &&
                         semanticName != QStringLiteral("hairFront") &&
                         semanticName != QStringLiteral("hairSide") &&
                         semanticName != QStringLiteral("hairBack"),
                     qPrintable(QStringLiteral("原型 %1 写了驱动器/物理占用的语义名 %2")
                                    .arg(archetype.key())
                                    .arg(semanticName)));
            QVERIFY2(value.value() >= range.min && value.value() <= range.max,
                     qPrintable(QStringLiteral("原型 %1 的 %2 = %3 越界（%4 的允许范围 [%5, %6]）")
                                    .arg(archetype.key())
                                    .arg(semanticName)
                                    .arg(double(value.value()))
                                    .arg(range.id)
                                    .arg(double(range.min))
                                    .arg(double(range.max))));
            ++checkedValues;
        }
    }

    // 夹取**结果**的一层一致性：解析出的参数组里同样不许有驱动器参数，且值都在范围内
    QHash<QString, Live2DMoodPreset::ParameterRange> byId;
    for (auto it = ranges.constBegin(); it != ranges.constEnd(); ++it)
        byId.insert(it.value().id, it.value());

    int checkedResolved = 0;
    for (const QString &archetype : preset.archetypeNames())
    {
        const QHash<QString, float> params = preset.parametersForArchetype(archetype);
        for (auto it = params.constBegin(); it != params.constEnd(); ++it)
        {
            QVERIFY2(!Live2DMoodPreset::isUpdaterOwnedParameter(it.key()),
                     qPrintable(QStringLiteral("原型 %1 的覆盖表里混进了驱动器参数 %2")
                                    .arg(archetype)
                                    .arg(it.key())));
            const Live2DMoodPreset::ParameterRange range = byId.value(it.key());
            QVERIFY2(!range.id.isEmpty() && it.value() >= range.min && it.value() <= range.max,
                     qPrintable(QStringLiteral("原型 %1 的 %2 = %3 不在 [%4, %5] 内")
                                    .arg(archetype)
                                    .arg(it.key())
                                    .arg(double(it.value()))
                                    .arg(double(range.min))
                                    .arg(double(range.max))));
            ++checkedResolved;
        }
    }

    qInfo("心情数值核对[%s]：原型 %d 个、初稿取值 %d 项全部在 parameter-map 的 [min,max] 内，"
          "无一落在 breath/hair*；解析后的覆盖表共 %d 项（%d 个原型）",
          qPrintable(preset.modelName()), deltas.size(), checkedValues, checkedResolved,
          preset.archetypeNames().size());
}

/*未知心情 → neutral。

  这是"AI 说了个词表外的词"这条路：绝不能什么都不做（屏幕上留着上一种情绪，
  而且没有任何人知道为什么），也绝不能崩。必须显式回到中立。*/
void TestLive2DWindow::unknownMoodFallsBackToNeutral()
{
    Live2DMoodPreset preset;
    if (!loadConfiguredMoodPreset(&preset))
        QSKIP("本机没有当前角色/模型的心情预设数据（Documents 下，不入库），跳过");

    const QHash<QString, float> neutral = preset.parametersForArchetype(QStringLiteral("neutral"));
    QVERIFY2(!neutral.isEmpty(), "neutral 解析不出参数，回退目标本身是空的");

    // 词表外的词（含空串/纯空白）：必须与 neutral 逐项相同
    const QStringList unknown = {QStringLiteral("绝对不是心情名"), QStringLiteral("SLEEPY"),
                                 QStringLiteral("happy"), QString(), QStringLiteral("   ")};
    for (const QString &moodName : unknown)
    {
        const QHash<QString, float> resolved = preset.parametersForMood(moodName);
        QCOMPARE(resolved.size(), neutral.size());
        for (auto it = neutral.constBegin(); it != neutral.constEnd(); ++it)
        {
            QVERIFY2(resolved.contains(it.key()),
                     qPrintable(QStringLiteral("未知心情 %1 的解析结果缺参数 %2")
                                    .arg(moodName, it.key())));
            QVERIFY2(qAbs(resolved.value(it.key()) - it.value()) <= 1e-4f,
                     qPrintable(QStringLiteral("未知心情 %1 的 %2 = %3，不等于中立值 %4")
                                    .arg(moodName)
                                    .arg(it.key())
                                    .arg(double(resolved.value(it.key())))
                                    .arg(double(it.value()))));
        }
    }

    /*反例必须成立：**词表内的**词不能被当成未知。
      取一个"会明显改变参数"的原型（sleepy：闭眼 + 张嘴 + 低头）来对照，
      否则"全都不认识"这种退化实现也能通过上面那几条。*/
    const QString knownMood = QStringLiteral("睡觉");
    if (preset.moodAliases().contains(knownMood))
    {
        const QHash<QString, float> resolved = preset.parametersForMood(knownMood);
        QCOMPARE(resolved.size(), neutral.size());
        int differing = 0;
        for (auto it = resolved.constBegin(); it != resolved.constEnd(); ++it)
        {
            if (qAbs(it.value() - neutral.value(it.key())) > 1e-4f)
                ++differing;
        }
        QVERIFY2(differing > 0,
                 qPrintable(QStringLiteral("词表内的心情 %1 解析结果与 neutral 完全一致")
                                .arg(knownMood)));
        qInfo("未知心情回退验证[%s]：%d 个未知名全部回退 neutral；词表内的 %s 与 neutral 有 %d 项不同",
              qPrintable(preset.modelName()), unknown.size(), qPrintable(knownMood), differing);
    }
    else
    {
        qInfo("未知心情回退验证[%s]：%d 个未知名全部回退 neutral（本机没有 %s 这个别名）",
              qPrintable(preset.modelName()), unknown.size(), qPrintable(knownMood));
    }
}

/*换心情真的会改变渲染出来的帧，且预设值真的落到模型参数上。

  为什么要**两条**观察量：
  - 像素差单独用不可靠：呼吸/眨眼/物理本来就让相邻帧不同（这正是 animatesAcrossFrames
    在断言的事），所以必须在同一轮里量一个"什么都不改时的相邻帧漂移"当对照；
  - 参数读回是直接证据：预设写的就是 0.9，模型里读出来就得是 0.9。
    读回只挑**不归任何驱动器管**的参数（见 readBackParameters 的说明）。*/
void TestLive2DWindow::applyingMoodChangesRenderedFrame()
{
    const QString modelName = preferredModelName();
    const QString dir = modelDirFor(modelName);
    if (!QFileInfo::exists(dir))
        QSKIP("本机没有用户配置的模型（禁二传，不入库），跳过心情渲染验证");

    // 只写临时配置：本用例量的是"换心情帧会不会变"，不是性能，取 60fps/1.0x 即可
    {
        QSettings settings(m_tempConfigPath, QSettings::IniFormat);
        settings.setValue("character/live2dFps", 60);
        settings.setValue("character/live2dScale", 1.0);
        settings.sync();
    }

    Live2DMoodPreset preset;
    if (!loadConfiguredMoodPreset(&preset))
        QSKIP("本机没有当前角色/模型的心情预设数据（Documents 下，不入库），跳过");

    const QString happyMood = preset.representativeMoodForArchetype(QStringLiteral("happy"));
    QVERIFY2(!happyMood.isEmpty(), "happy 原型没有任何别名，无法测换心情");

    Live2DCharacterWindow window;
    window.loadModel(modelName);
    QVERIFY2(window.isModelLoaded(), "模型装载失败，无法验证换心情");
    QVERIFY2(window.isMoodPresetEnabled(), "情绪预设没装载成功");

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QCoreApplication::processEvents();

    // 冻住帧循环（hideEvent 停表）后再自己渲帧：基准帧/对照帧/心情帧之间的差别里，
    // 只允许剩下"真的改了参数"这一项，不能再掺进定时器带来的任意时间推进。
    window.hide();
    QCoreApplication::processEvents();

    window.reloadContent(QStringLiteral("default")); // 显式回到 neutral（默认别名）
    QVERIFY2(window.renderFrameNow(), "中立基准帧渲染失败");
    const QImage neutralFrame = window.renderedImage();
    QVERIFY2(!neutralFrame.isNull(), "中立基准帧为空");

    // 对照帧：什么都不改，再渲一帧。它与基准帧的差 = 呼吸/眨眼/物理在"一帧"里的漂移
    QVERIFY2(window.renderFrameNow(), "中立对照帧渲染失败");
    const QImage controlFrame = window.renderedImage();

    // 换 happy。⚠️ 必须等过渡走完 + 等帧循环真的渲出那一帧（见 waitForMoodSettle 的说明）：
    // 不然 moodFrame 拿到的还是中性帧，下面的像素差会是 0 —— 而参数读回又会读到半路的值。
    window.reloadContent(happyMood);
    QVERIFY2(waitForMoodSettle(&window), "换心情后等不到过渡走完/渲不出帧");
    const QImage moodFrame = window.renderedImage();
    QVERIFY2(!moodFrame.isNull(), "心情帧为空");

    QCOMPARE(moodFrame.size(), neutralFrame.size());
    constexpr int kChannelDelta = 8; // 抗 8bit 量化/抗锯齿的通道噪声
    const qint64 drift = countDifferingPixels(neutralFrame, controlFrame, kChannelDelta);
    const qint64 moodDiff = countDifferingPixels(controlFrame, moodFrame, kChannelDelta);
    QVERIFY2(drift >= 0 && moodDiff >= 0, "两帧尺寸不一致，无法比较");
    const qint64 total = static_cast<qint64>(moodFrame.width()) * moodFrame.height();
    qInfo("心情渲染验证[%s]：neutral→%s 逐通道差 >%d 的像素 = %lld/%lld（%.3f%%）；"
          "同一心情相邻两帧的漂移(对照) = %lld（%.3f%%）",
          qPrintable(modelName), qPrintable(happyMood), kChannelDelta, moodDiff, total,
          static_cast<double>(moodDiff) * 100.0 / static_cast<double>(total), drift,
          static_cast<double>(drift) * 100.0 / static_cast<double>(total));

    /*阈值：2000 像素远高于"什么都不改时的漂移"（紧邻两帧只有几百），
       又远低于"笑起来的眼睛/嘴巴/腮红"该有的量级（数万像素）。
       再乘 2 倍对照，是防"某次漂移恰好很大"把结论蒙对。*/
    QVERIFY2(moodDiff > 2000,
             qPrintable(QStringLiteral("换心情只差 %1 个像素，看不出表情变化").arg(moodDiff)));
    QVERIFY2(moodDiff > drift * 2,
             qPrintable(QStringLiteral("换心情的像素差 %1 没有明显超过同心情相邻帧的漂移 %2，"
                                       "无法证明变化来自心情")
                            .arg(moodDiff)
                            .arg(drift)));

    // 参数读回：预设写什么，模型里就该读到什么
    const QHash<QString, float> expected = preset.parametersForMood(happyMood);
    const QStringList readBack = readBackParameters();
    for (const QString &parameterId : readBack)
    {
        QVERIFY2(expected.contains(parameterId),
                 qPrintable(QStringLiteral("预设 %1 里没有 %2，读回断言写错了")
                                .arg(happyMood, parameterId)));
        const float actual = window.parameterValue(parameterId);
        qInfo("参数读回[%s]：%s 预设 %.3f 实际 %.3f", qPrintable(happyMood),
              qPrintable(parameterId), double(expected.value(parameterId)), double(actual));
        QVERIFY2(qAbs(actual - expected.value(parameterId)) <= 0.02f,
                 qPrintable(QStringLiteral("参数 %1 读回 %2，不等于预设值 %3（覆盖没生效）")
                                .arg(parameterId)
                                .arg(double(actual))
                                .arg(double(expected.value(parameterId)))));
    }

    /*把"没落到模型上"的参数全列出来（**只报不断言**）。上面断言的 5 项之外，还有三类
      注定对不上，校准时必须先知道它们，否则会去改一个永远不会生效的数字：
        ① ParamEyeLOpen/ParamEyeROpen：眨眼驱动器每帧绝对赋值，预设在这两项上是死的；
        ② ParamMouthForm：moc 自身范围 [-1,0]，正数被夹成 0（本模型的"笑"只能靠眼睛）；
        ③ ParamAngleX/Y/Z 与 ParamBodyAngleX：呼吸的加性摆动（读回 = 预设 + 摆动）。
      这条日志是把"死参数"变成可看见证据的地方。*/
    QStringList notLanded;
    for (auto it = expected.constBegin(); it != expected.constEnd(); ++it)
    {
        const float actual = window.parameterValue(it.key());
        if (qAbs(actual - it.value()) > 0.05f)
        {
            notLanded << QStringLiteral("%1=%2(want %3)")
                             .arg(it.key())
                             .arg(double(actual), 0, 'f', 2)
                             .arg(double(it.value()), 0, 'f', 2);
        }
    }
    notLanded.sort();
    qInfo("参数未落地清单[%s]（读回 ≠ 预设，含驱动器/范围夹取）：%s", qPrintable(happyMood),
          notLanded.isEmpty() ? "none" : qPrintable(notLanded.join(QStringLiteral(", "))));
}

/*「还没装载模型时施加心情」也必须安全 —— 这是启动期的**真实时序**：
   Dialog 先 show，AI 的第一句回复可能早于立绘窗口的首次布局（那时画布/dpr 都还没定）。

   两条观察量：
     ① 不崩、也不登记任何画布（没有画布时渲出来的帧尺寸是错的，宁可不渲）；
     ② 心情要**记住**，模型装载完成后补上 —— 丢掉这一句的心情就等于"她对第一句话没反应"。*/
void TestLive2DWindow::applyingMoodWithoutModelIsSafe()
{
    Live2DMoodPreset preset;
    if (!loadConfiguredMoodPreset(&preset))
        QSKIP("本机没有当前角色/模型的心情预设数据（Documents 下，不入库），跳过");

    const QString happyMood = preset.representativeMoodForArchetype(QStringLiteral("happy"));
    QVERIFY2(!happyMood.isEmpty(), "happy 原型没有别名");

    Live2DCharacterWindow window;
    QVERIFY2(!window.isModelLoaded(), "本用例要从「还没装载模型」的状态开始");

    // ① 未装载模型：施加心情什么都不该发生（不崩、不装载、不登记画布）
    window.reloadContent(happyMood);
    QVERIFY2(!window.isModelLoaded(), "施加心情不该顺手装载模型");
    QVERIFY2(window.contentSize().isEmpty(), "还没装载模型就登记了画布");
    QVERIFY2(!window.renderFrameNow(), "没有模型却渲染成功了");
    qInfo("未装载模型时施加心情[%s]：无异常、无画布登记", qPrintable(happyMood));

    const QString modelName = preferredModelName();
    if (!QFileInfo::exists(modelDirFor(modelName)))
        QSKIP("本机没有用户配置的模型（禁二传，不入库），本用例只验到「不崩」为止");

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QCoreApplication::processEvents();

    // ② 心情被记住：模型装载完成后立刻生效（不需要再发一次心情）
    QVERIFY2(window.loadModel(modelName), "模型装载失败");
    QVERIFY2(window.renderFrameNow(), "装载后的帧渲染失败");
    const QHash<QString, float> expected = preset.parametersForMood(happyMood);
    const QString probe = QStringLiteral("ParamEyeLSmile");
    const float actual = window.parameterValue(probe);
    qInfo("装载后补上心情[%s]：%s 预设 %.3f 实际 %.3f", qPrintable(happyMood), qPrintable(probe),
          double(expected.value(probe)), double(actual));
    QVERIFY2(qAbs(actual - expected.value(probe)) <= 0.02f,
             qPrintable(QStringLiteral("装载模型前收到的心情 %1 没有被补上：%2 读回 %3，"
                                       "预设是 %4")
                            .arg(happyMood)
                            .arg(probe)
                            .arg(double(actual))
                            .arg(double(expected.value(probe)))));
}

/***残留情绪的回归测试**（本阶段这个 bug 的核心）。

   `setParameter` 只能增改单条、删不掉：先来 happy（eyeLSmile=0.9、cheek=0.5…），
   再切回 neutral 时，旧条目仍留在覆盖表里每帧施加 —— 屏幕上就是"切了中立，
   她还在笑"。所以这里必须断言：切回 neutral 后读回值**全部**回到中立值。

  参数读回只挑不归驱动器管的参数，理由见 readBackParameters。*/
void TestLive2DWindow::switchingMoodDoesNotAccumulate()
{
    const QString modelName = preferredModelName();
    const QString dir = modelDirFor(modelName);
    if (!QFileInfo::exists(dir))
        QSKIP("本机没有用户配置的模型（禁二传，不入库），跳过情绪残留验证");

    Live2DMoodPreset preset;
    if (!loadConfiguredMoodPreset(&preset))
        QSKIP("本机没有当前角色/模型的心情预设数据（Documents 下，不入库），跳过");

    const QString happyMood = preset.representativeMoodForArchetype(QStringLiteral("happy"));
    const QString neutralMood = preset.representativeMoodForArchetype(QStringLiteral("neutral"));
    QVERIFY2(!happyMood.isEmpty() && !neutralMood.isEmpty(), "happy/neutral 缺别名");

    Live2DCharacterWindow window;
    window.loadModel(modelName);
    QVERIFY2(window.isModelLoaded(), "模型装载失败，无法验证情绪残留");
    QVERIFY2(window.isMoodPresetEnabled(), "情绪预设没装载成功");

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QCoreApplication::processEvents();
    window.hide();
    QCoreApplication::processEvents();

    const QStringList readBack = readBackParameters();
    const QHash<QString, float> happyExpected = preset.parametersForMood(happyMood);

    // ① 先确认 happy 真的落到了模型上（否则"切回中立"这条断言毫无意义 —— 什么都没变过）
    // 等过渡走完再读：覆盖值是逐帧过渡的，刚切完读到的是上一个心情的值（见 waitForMoodSettle）
    window.reloadContent(happyMood);
    QVERIFY2(waitForMoodSettle(&window), "换到 happy 后等不到过渡走完");
    QVERIFY2(window.renderFrameNow(), "happy 帧渲染失败");
    int happyApplied = 0;
    for (const QString &parameterId : readBack)
    {
        const float actual = window.parameterValue(parameterId);
        const float expectedValue = happyExpected.value(parameterId);
        if (qAbs(actual - expectedValue) <= 0.02f)
            ++happyApplied;
        qInfo("残留验证①[%s]：%s 预设 %.3f 实际 %.3f", qPrintable(happyMood),
              qPrintable(parameterId), double(expectedValue), double(actual));
    }
    QVERIFY2(happyApplied == readBack.size(),
             qPrintable(QStringLiteral("%1 只有 %2/%3 个参数落到模型上，先修装载路径")
                            .arg(happyMood)
                            .arg(happyApplied)
                            .arg(readBack.size())));

    // ② 切回 neutral：读回值必须**全部**回到中立，一个都不许残留
    window.reloadContent(neutralMood);
    QVERIFY2(waitForMoodSettle(&window), "切回 neutral 后等不到过渡走完");
    QVERIFY2(window.renderFrameNow(), "neutral 帧渲染失败");
    const QHash<QString, float> neutralExpected =
        preset.parametersForArchetype(QStringLiteral("neutral"));
    for (const QString &parameterId : readBack)
    {
        const float actual = window.parameterValue(parameterId);
        const float expectedValue = neutralExpected.value(parameterId);
        qInfo("残留验证②[%s]：%s 中立项 %.3f 实际 %.3f", qPrintable(neutralMood),
              qPrintable(parameterId), double(expectedValue), double(actual));
        QVERIFY2(qAbs(actual - expectedValue) <= 0.02f,
                 qPrintable(QStringLiteral("切回 %1 后参数 %2 读回 %3，没有回到中立值 %4 —— "
                                           "上一种情绪的覆盖条目还留在表里（残留情绪）")
                                .arg(neutralMood)
                                .arg(parameterId)
                                .arg(double(actual))
                                .arg(double(expectedValue))));
    }
}

/*==================== 情绪 × 眨眼：必须是合成，不能互相抢写 ====================

  **这个问题长什么样**（用户看到的就是"mood-sleepy.png 里眼睛睁着"）：
  情绪的睁闭眼值原本和其他参数一样在 LoadParameters 之后写绝对覆盖，
  但 `CubismEyeBlink` 在 OnLateUpdate 里对同一批参数**绝对赋值**（0~1 的眨眼进度），
  于是每一帧的最终值都是"眨眼说的算" —— 情绪那几项（sleepy 0.05 / surprised 1.7 /
  sad 0.55 / cry 0.35 / angry 0.85 / excited 1.25）全部被丢掉。
  实测：sleepy 帧与 neutral 帧逐像素相同、读回恒为 1.00。

  **修法**：情绪值改成**乘数**，在 OnLateUpdate **之后**乘到眨眼刚写下的值上
  （final = mood × blink）。不关掉眨眼 —— 眨眼是"她还活着"的唯一线索
  （本模型没有身体待机动作，只有呼吸/眨眼/物理）。

  这条测试要证的正是"合成"的三件事，任何一件都对应一种错的实现：
    (a) 闭眼真的落到了屏幕上（像素证据）：**相邻两帧**之间只换心情，
        眼睛闭下去造成的像素差必须远超同一呼吸/物理在相邻帧里本来就会造成的漂移；
    (b) 闭眼心情的最终值 ≈ 预设值 × 眨眼原始值 —— 证明是**乘法**，不是"被覆盖"也不是"被夹"；
        （旧实现下这个等式必然不成立：最终值恒等于眨眼原始值。）
    (c) 眨眼在采样窗口里仍然变化（blinkMin < blinkMax）—— 证明没有被情绪钉死。
        同一窗口里 neutral 也必须变化，否则"眨眼活着"这个前提本身没被验证。

  ⚠️ 像素对照为什么必须是**相邻帧**：两次独立的长采样之间，呼吸/物理已经跑过几百帧，
  相位完全不同，末帧逐像素差异里"背景摆动"占绝对多数（实测 5.3 万像素，比闭眼本身还大），
  用它证明"闭眼看得见"根本不成立 —— 这条错误设计在开发时真的踩到过，
  当时的读数（9.16% 像素差）在**关掉合成之后依然出现**，一测就露馅。
  相邻帧对照把时间推进限制在"一帧"内，漂移被压到几百像素量级，闭眼的贡献才是主导项。*/
void TestLive2DWindow::moodEyeOpennessComposesWithBlink()
{
    const QString modelName = preferredModelName();
    const QString dir = modelDirFor(modelName);
    if (!QFileInfo::exists(dir))
        QSKIP("本机没有用户配置的模型（禁二传，不入库），跳过眨眼合成验证");

    Live2DMoodPreset preset;
    if (!loadConfiguredMoodPreset(&preset))
        QSKIP("本机没有当前角色/模型的心情预设数据（Documents 下，不入库），跳过");

    const QString sleepyMood = preset.representativeMoodForArchetype(QStringLiteral("sleepy"));
    const QString neutralMood = preset.representativeMoodForArchetype(QStringLiteral("neutral"));
    QVERIFY2(!sleepyMood.isEmpty() && !neutralMood.isEmpty(), "sleepy/neutral 缺别名");

    // 眨眼是**时间**上的事件：采样窗口必须足够长到必然覆盖一次眨眼。
    // 60fps 的帧率只决定定时器间隔；采样窗口的虚拟时间由 sampleEyeOpenness 的
    // 每帧等待（60ms）× 帧数（200）≈ 12s 决定，> 眨眼间隔上限（2 × 默认 4s = 7s）。
    {
        QSettings settings(m_tempConfigPath, QSettings::IniFormat);
        settings.setValue("character/live2dFps", 60);
        settings.setValue("character/live2dScale", 1.0);
        settings.sync();
    }

    Live2DCharacterWindow window;
    window.loadModel(modelName);
    QVERIFY2(window.isModelLoaded(), "模型装载失败");
    QVERIFY2(window.isMoodPresetEnabled(), "情绪预设没装载成功");
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QCoreApplication::processEvents();
    // 冻住定时器：采样只推进"我们自己要求的那几帧"，不再掺进任意时间推进
    window.hide();
    QCoreApplication::processEvents();

    constexpr int kSamples = 130; // 100ms × 130 ≈ 13s 虚拟时间，稳过一次眨眼（上限 7s）
    const QString eyeParameter = QStringLiteral("ParamEyeLOpen");
    const float sleepyMoodValue = preset.parametersForMood(sleepyMood).value(eyeParameter, 1.0f);
    QVERIFY2(sleepyMoodValue < 0.5f,
             qPrintable(QStringLiteral("原型 sleepy 的 %1 = %2，不是一个「近乎闭眼」的值，"
                                       "本用例的前提不成立")
                            .arg(eyeParameter)
                            .arg(double(sleepyMoodValue))));

    /*眨眼参数的真实 ID 取自模型声明的眼睛语义名（不硬编码 ParamEyeLOpen/ROpen）*/
    QStringList eyeParameterIds;
    for (const QString &semantic :
         {QStringLiteral("eyeLOpen"), QStringLiteral("eyeROpen")})
    {
        const QString id = preset.parameters().value(semantic).id;
        QVERIFY2(!id.isEmpty(), "parameter-map 里没有眼睛参数，无法验证眨眼合成");
        eyeParameterIds.append(id);
    }

    // ① 中立基准（也是"眨眼在动"的对照：它必须动，否则本测试的前提不成立）
    const EyeBlinkSamples neutral =
        sampleEyeOpenness(&window, eyeParameter, neutralMood, kSamples);
    QVERIFY2(neutral.frames > 0, "中立采样一帧都没渲出来");
    QVERIFY2(neutral.blinkMax - neutral.blinkMin > 0.05f,
             qPrintable(QStringLiteral("采样窗口里眨眼原始值几乎没变（%1~%2）——"
                                       "要么眨眼没在跑，要么窗口太短，本用例的前提不成立")
                            .arg(double(neutral.blinkMin))
                            .arg(double(neutral.blinkMax))));

    /*② 相邻帧对照：同一时刻的呼吸/物理相位基本不变，只切换**眼睛的乘数**。

       为什么用"切换乘数"而不是"切换心情"：换心情会同时改头身角度/嘴，
       那些本来就让画面变（实测相邻帧差 5.1 万像素），于是"闭眼有没有落到屏幕上"
       被淹没在一个与眼睛无关的大数字里 —— 这条错误设计在开发时真的踩到过。
       关掉乘数（=1.0）正是**旧行为**：眨眼自己说了算，情绪完全压不住眼睛。

       ⚠️ "相邻帧"这个前提**必须逐次校验**（见文件上方 kEyeProbeMaxGapMs 的实测记录）：
        1) 眨眼必须全程处于**全睁**（interval 态）：落在眨眼里时开度差会从 (1.0→0.05)
           缩成 (0.5→0.02)，实测读数从 1218 掉到 353；
        2) 两帧之间真实流逝的时间必须够短：时间步长取自墙钟，被抢占时一帧能走几十毫秒的
           正常量，呼吸/待机动作随之跳一大步 —— 2026-09-29 复现的失败里，两帧眨眼原始值
           **都是 1.0000**（条件 1 成立）却仍有 712 像素漂移，全部来自这一条。
        所以下面用"取一对 → 校验两条 → 不合格就重取"的**条件式等待**，
        超时才报错；而不是"渲两帧然后假设它们是相邻的"。*/
    const QHash<QString, float> openEyes = eyeMultiplierFrom(eyeParameterIds, 1.0f);
    const QHash<QString, float> moodEyes = eyeMultiplierFrom(eyeParameterIds, sleepyMoodValue);

    /*⚠️ 先等中立那一段的过渡彻底走完再动乘数。
       渲染器现在每帧都会用"当前覆盖表"重推一次睁闭眼乘数（为了让它与过渡中的覆盖值
       同一帧，见 OffscreenRenderer 的 applyMoodBlend）；若覆盖表还在过渡中，
       renderEyeProbe 刚设好的乘数会在同一次 tick 里被半路值覆盖掉 ——
       实测症状就是"眼睛压到 0.05 之后画面只差 0 个像素"。*/
    QVERIFY2(waitForMoodSettle(&window, 0, 2), "中立过渡没走完，无法做相邻帧对照");

    QImage blinkOpenA;
    QImage blinkOpenB;
    float blinkA = 1.0f;
    float blinkB = 1.0f;
    //实际取到的那一对帧之间的墙钟间隔（毫秒）+ 尝试次数：报进日志，是"它们真的相邻"的证据
    qint64 pairGapMs = -1;
    int pairAttempts = 0;
    {
        QElapsedTimer pairClock;
        QElapsedTimer overall;
        overall.start();
        while (true)
        {
            ++pairAttempts;
            pairClock.start();
            if (!renderEyeProbe(&window, openEyes, eyeParameter, &blinkOpenA, nullptr, &blinkA))
            {
                QTest::qWait(20); //帧没渲出来：等一会儿再重试，别把 CPU 打满
                continue;
            }
            if (!renderEyeProbe(&window, openEyes, eyeParameter, &blinkOpenB, nullptr, &blinkB))
            {
                QTest::qWait(20);
                continue;
            }
            pairGapMs = pairClock.elapsed();
            const bool blinkOpen = blinkA >= kEyeProbeBlinkOpen && blinkB >= kEyeProbeBlinkOpen;
            if (blinkOpen && pairGapMs <= kEyeProbeMaxGapMs)
                break; //这一对帧真的可比：全睁 + 时间够短
            /*不合格：多半是落进了一次眨眼（0~7s 随机一次），或者这一对被抢占了很久。
               用 qWait 让时间过去（眨眼状态机要真的过时间才会走完），再取下一对。
               ⚠️ 不能紧循环重试：不推进时间的话眨眼永远停在闭着的相位上。*/
            QTest::qWait(50);
            if (overall.elapsed() > kEyeProbeDeadlineMs)
            {
                QVERIFY2(false,
                         qPrintable(QStringLiteral(
                                        "%1s 内找不到一对可比的相邻帧：最后一次 blink=%2~%3"
                                        "（要求都 ≥%4）、两帧间隔 %5ms（要求 ≤%6ms）——"
                                        "眨眼卡死或渲染一直在被抢占")
                                        .arg(kEyeProbeDeadlineMs / 1000)
                                        .arg(double(blinkA))
                                        .arg(double(blinkB))
                                        .arg(double(kEyeProbeBlinkOpen))
                                        .arg(pairGapMs)
                                        .arg(kEyeProbeMaxGapMs)));
            }
        }
    }

    /*闭眼帧必须也取自"眨眼全睁"的那一刻，并且与刚刚那一对帧的距离同样要近
       （"最终值 = 乘数 × 眨眼原始值"这个逐帧等式、以及像素对照都要求这一点）。
       条件式等待 + 超时，同 A/B 那一对。*/
    QImage eyesClosed;
    float closedComposed = 1.0f;
    float closedBlink = 1.0f;
    int closedAttempts = 0;
    qint64 closedGapMs = -1;
    {
        QElapsedTimer closedClock;
        QElapsedTimer overall;
        overall.start();
        while (true)
        {
            ++closedAttempts;
            closedClock.start();
            if (!renderEyeProbe(&window, moodEyes, eyeParameter, &eyesClosed, &closedComposed,
                                &closedBlink))
            {
                QTest::qWait(20);
                continue;
            }
            /*眨眼在闭眼帧渲染期间翻开也不行：那会让"最终值 = 乘数 × 眨眼原始值"里的
               眨眼原始值与实际渲出的帧错开。再读一次当前眨眼值确认这一刻它没动。*/
            const float blinkAfterProbe = window.blinkValue();
            closedGapMs = closedClock.elapsed();
            const bool blinkStillOpen = closedBlink >= kEyeProbeBlinkOpen &&
                                        blinkAfterProbe >= kEyeProbeBlinkOpen;
            if (blinkStillOpen && closedGapMs <= kEyeProbeMaxGapMs)
                break;
            QTest::qWait(50);
            if (overall.elapsed() > kEyeProbeDeadlineMs)
            {
                QVERIFY2(false,
                         qPrintable(QStringLiteral(
                                        "%1s 内取不到一对可比的闭眼帧：blink=%2（渲染后 %3）"
                                        "（要求都 ≥%4）、间隔 %5ms（要求 ≤%6ms）")
                                        .arg(kEyeProbeDeadlineMs / 1000)
                                        .arg(double(closedBlink))
                                        .arg(double(blinkAfterProbe))
                                        .arg(double(kEyeProbeBlinkOpen))
                                        .arg(closedGapMs)
                                        .arg(kEyeProbeMaxGapMs)));
            }
        }
    }
    /*校准用的显式乘数用完必须放手（并让眼睛重新跟随心情）：
       否则它会一直压制覆盖表，把后面 (c) 那一段" sleepy 心情下眨眼还活着"的采样
       变成"眼睛被钉在 0.05"的错误读数。*/
    window.clearEyeOpennessMultiplierOverride();
    QVERIFY2(window.renderFrameNow(), "解除显式乘数后的一帧渲染失败");

    QVERIFY2(!blinkOpenA.isNull() && !blinkOpenB.isNull() && !eyesClosed.isNull(),
             "相邻帧渲染失败");
    QCOMPARE(eyesClosed.size(), blinkOpenA.size());

    constexpr int kChannelDelta = 8;
    const qint64 drift = countDifferingPixels(blinkOpenA, blinkOpenB, kChannelDelta);
    const qint64 eyeDiff = countDifferingPixels(blinkOpenB, eyesClosed, kChannelDelta);
    const qint64 total = static_cast<qint64>(eyesClosed.width()) * eyesClosed.height();
    qInfo("眨眼合成验证[%s]：相邻帧对照 —— 眼睛乘数不动时的漂移 = %lld 像素（%.3f%%，眨眼 "
          "%.4f→%.4f）；把眼睛从 1.0 压到 %.4f 后 = %lld 像素（%.3f%%，眨眼原始值 %.4f，"
          "眼睛读回 %.4f）；倍数 = %.0fx",
          qPrintable(modelName), drift,
          static_cast<double>(drift) * 100.0 / static_cast<double>(total), double(blinkA),
          double(blinkB), double(sleepyMoodValue), eyeDiff,
          static_cast<double>(eyeDiff) * 100.0 / static_cast<double>(total), double(closedBlink),
          double(closedComposed),
          drift > 0 ? static_cast<double>(eyeDiff) / static_cast<double>(drift) : -1.0);
    /*"这一对帧真的相邻"的**证据**：两条前提各自的实际读数 + 取到它们花了多少次尝试。
       没有这行日志，失败时无法区分"眼睛真的没生效"与"这次取到的帧根本不可比"。*/
    qInfo("EYEPROBE adjacency: pair attempts=%d gap=%lld ms blink=%.4f/%.4f | closed attempts=%d"
          " gap=%lld ms blink=%.4f | limits: blink>=%.3f gap<=%d ms",
          pairAttempts, pairGapMs, double(blinkA), double(blinkB), closedAttempts, closedGapMs,
          double(closedBlink), double(kEyeProbeBlinkOpen), kEyeProbeMaxGapMs);

    /*(a) 闭眼真的落到屏幕上。先定标：实测 atri 上"眼睛 1.0 → 0.05"只改变 **1212** 像素
       （0.207% 画布）—— 闭眼参数动的是眼睑那一小块，不是半张脸，所以绝对量级本来就不大；
       而**旧行为**（乘数不生效）下同一个切换只改变 **1** 个像素。两条一起断言：
         - 绝对下限 500：比"完全没生效"（0~1 像素）高三个数量级，又不假装眼睛有半张脸大；
         - 相对倍数 5x：挡住"背景摆动恰好很大"把结论蒙对（空闲实测漂移 0~1 像素）。
       上面已经用条件式等待把"这一对帧真的相邻 + 眨眼全睁"钉住了，所以 5x 这条是在
       **可比的两帧**上断言，而不是靠"没有 wait 就等于时间没走"这个曾经被 26 线程
       满负载打破的假设（那次 drift=712、eyeDiff=1223，比值只有 2x）。
       修改眼睛参数建模（例如换成眼睑面积大得多的模型）可能让这个绝对值变化，
       但"远超同类相邻帧漂移"这条与模型无关，是主要判据。*/
    QVERIFY2(eyeDiff > 500,
             qPrintable(QStringLiteral("眼睛乘数压到 %1 之后画面只差 %2 个像素 ——"
                                       "闭眼没有落到屏幕上（旧行为：眨眼把情绪盖掉）")
                            .arg(double(sleepyMoodValue))
                            .arg(eyeDiff)));
    QVERIFY2(eyeDiff > drift * 5,
             qPrintable(QStringLiteral("闭眼造成的像素差 %1 没有明显超过相邻帧漂移 %2"
                                       "（这对帧间隔 %3ms、眨眼 %4~%5）——"
                                       "无法证明变化来自眼睛而不是背景摆动")
                            .arg(eyeDiff)
                            .arg(drift)
                            .arg(pairGapMs)
                            .arg(double(blinkA))
                            .arg(double(blinkB))));

    /*(b) 是乘积：这一刻的最终值 ≈ 乘数 × 这一刻的眨眼原始值。
       两张帧是相邻渲染的（眨眼进度只差几个百分点，落在下面的容差里），
       所以这是**逐帧**等式。旧实现下最终值恒等于眨眼原始值（0.05 对不上 ~1），必然失败。*/
    const float product = sleepyMoodValue * closedBlink;
    qInfo("眨眼合成验证：%s 乘数 %.4f × 眨眼原始值 %.4f = %.4f，实测最终值 %.4f（差 %.4f）",
          qPrintable(eyeParameter), double(sleepyMoodValue), double(closedBlink),
          double(product), double(closedComposed), double(qAbs(closedComposed - product)));
    QVERIFY2(qAbs(closedComposed - product) <= 0.02f,
             qPrintable(QStringLiteral("最终值 %1 ≠ 乘数 %2 × 眨眼原始值 %3 = %4 —— "
                                       "说明不是「乘在眨眼结果上」")
                            .arg(double(closedComposed))
                            .arg(double(sleepyMoodValue))
                            .arg(double(closedBlink))
                            .arg(double(product))));
    /*把"被压住"钉在数值上：闭眼帧的最终值必须**明显小于**眨眼全睁。
       旧实现下它等于眨眼原始值（这一刻约 1）；合成后 = 0.05 × 它 ≤ 0.05。*/
    QVERIFY2(closedComposed <= 0.25f,
             qPrintable(QStringLiteral("闭眼帧的最终值到 %1（眨眼原始值 %2）——"
                                       "眼睛并没有被压到近乎闭合")
                            .arg(double(closedComposed))
                            .arg(double(closedBlink))));

    /*(c) 眨眼还活着：**闭眼心情下再采一段**，眨眼原始值必须仍在变化。
       它证明"乘数只缩放、不冻结" —— 若为了让闭眼好看把眨眼中性化/关掉，
       这里会退化成一条常量（而 (a)(b) 那两条都察觉不到这种错解法）。
       用同一个 12s 窗口：比它短就不足以稳过"眨眼间隔 0~7s"这个随机性。*/
    const EyeBlinkSamples sleepy =
        sampleEyeOpenness(&window, eyeParameter, sleepyMood, kSamples);
    QVERIFY2(sleepy.frames > 0, "闭眼心情下采样一帧都没渲出来");
    QVERIFY2(sleepy.blinkMax - sleepy.blinkMin > 0.05f,
             qPrintable(QStringLiteral("闭眼心情下眨眼原始值被钉死了（%1~%2）——"
                                       "眨眼必须继续按自己的节奏走")
                            .arg(double(sleepy.blinkMin))
                            .arg(double(sleepy.blinkMax))));
    qInfo("眨眼合成验证：闭眼心情下眨眼原始值区间 %.4f~%.4f、眼睛最终值区间 %.4f~%.4f"
          "（各 %d 帧采样）",
          double(sleepy.blinkMin), double(sleepy.blinkMax), double(sleepy.composedMin),
          double(sleepy.composedMax), sleepy.frames);
}

/*不碰眼睛的心情（neutral / happy 都不写 eyeLOpen）必须对眨眼**零影响**：
  最终值就是眨眼原始值本身，区间也必须与眨眼区间一致。

  为什么这条不能省：把乘数逻辑写成"对覆盖表里所有参数一律相乘"也能让上面那条测试通过
  （乘数 1.0 的乘法看不出错），但那样会造出每帧累积的连乘（见
  Live2DOffscreenRenderer::applyEyeOpennessMultiplier 的说明）。
  这里用"最终值 == 眨眼原始值"把"不该动的绝不动"钉死。*/
void TestLive2DWindow::moodIgnoringEyesLeavesBlinkUnchanged()
{
    const QString modelName = preferredModelName();
    const QString dir = modelDirFor(modelName);
    if (!QFileInfo::exists(dir))
        QSKIP("本机没有用户配置的模型（禁二传，不入库），跳过眨眼不受影响验证");

    Live2DMoodPreset preset;
    if (!loadConfiguredMoodPreset(&preset))
        QSKIP("本机没有当前角色/模型的心情预设数据（Documents 下，不入库），跳过");

    const QString neutralMood = preset.representativeMoodForArchetype(QStringLiteral("neutral"));
    const QString happyMood = preset.representativeMoodForArchetype(QStringLiteral("happy"));
    QVERIFY2(!neutralMood.isEmpty() && !happyMood.isEmpty(), "neutral/happy 缺别名");

    // 这两个原型都不写 eyeLOpen —— 前提被数据破坏时这条测试就失去意义了
    for (const QString &archetype : {QStringLiteral("neutral"), QStringLiteral("happy")})
    {
        /*⚠️ 查的是**原始差异表**（moods.json 里这个原型真正写了什么），
           不是 parametersForArchetype() 的解析结果 —— 后者是"整组替换"，
           每个我们拥有的参数都会有一条，拿它判断"这个原型碰不碰眼睛"永远为真。*/
        const QHash<QString, float> delta = preset.archetypeDeltas().value(archetype);
        QVERIFY2(!delta.contains(QStringLiteral("eyeLOpen")) &&
                     !delta.contains(QStringLiteral("eyeROpen")),
                 qPrintable(QStringLiteral("原型 %1 现在会写 eyeLOpen/eyeROpen 了，"
                                           "本用例的前提（不碰眼睛）不再成立，请更新断言")
                                .arg(archetype)));
    }

    {
        QSettings settings(m_tempConfigPath, QSettings::IniFormat);
        settings.setValue("character/live2dFps", 60);
        settings.setValue("character/live2dScale", 1.0);
        settings.sync();
    }

    Live2DCharacterWindow window;
    window.loadModel(modelName);
    QVERIFY2(window.isModelLoaded() && window.isMoodPresetEnabled(), "模型/预设装载失败");
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QCoreApplication::processEvents();
    window.hide();
    QCoreApplication::processEvents();

    const QString eyeParameter = QStringLiteral("ParamEyeLOpen");

    // ①② 交替切换 neutral / happy：两者都不碰眼睛，切换本身也不该改变任何东西
    /*采样窗口故意取短（30ms × 40 帧 ≈ 1.2s 虚拟时间）：这条要证的是"乘数恒等"
       —— 它每一帧都必须成立，不需要等到眨眼发生；而"眨眼仍会走到全睁"也必然成立，
      因为眨眼状态机的 interval 态（值恰好 1.0）占了 >90% 的时间，窗口只要不是 0 就能撞上。
      真正需要 13s 窗口的"眨眼活着"证据在 moodEyeOpennessComposesWithBlink 里。*/
    const EyeBlinkSamples neutral =
        sampleEyeOpenness(&window, eyeParameter, neutralMood, 40, 30);
    const EyeBlinkSamples happy = sampleEyeOpenness(&window, eyeParameter, happyMood, 40, 30);
    QVERIFY2(neutral.frames > 0 && happy.frames > 0, "采样一帧都没渲出来");

    qInfo("眨眼不受影响验证[%s]：neutral 最终 %.4f~%.4f / 眨眼 %.4f~%.4f；%s 最终 %.4f~%.4f / "
          "眨眼 %.4f~%.4f",
          qPrintable(modelName), double(neutral.composedMin), double(neutral.composedMax),
          double(neutral.blinkMin), double(neutral.blinkMax), qPrintable(happyMood),
          double(happy.composedMin), double(happy.composedMax), double(happy.blinkMin),
          double(happy.blinkMax));

    for (const EyeBlinkSamples *sample : {&neutral, &happy})
    {
        // 乘数恒等：最终值与眨眼原始值必须逐项一致（同一个采样窗口）
        QVERIFY2(qAbs(sample->composedMin - sample->blinkMin) <= 1e-3f &&
                     qAbs(sample->composedMax - sample->blinkMax) <= 1e-3f,
                 qPrintable(QStringLiteral("最终值 %.4f~%.4f 与眨眼原始值 %.4f~%.4f 不一致 ——"
                                           "不碰眼睛的心情不该改动眨眼")
                                .arg(double(sample->composedMin))
                                .arg(double(sample->composedMax))
                                .arg(double(sample->blinkMin))
                                .arg(double(sample->blinkMax))));
    }

    /*眨眼仍在正常范围里走完一个循环（上限 1.0 = 全睁）。
       这条同时挡住"把眨眼中性化"这种错解法：那样最大最终值会永远停在某个常数上。*/
    QVERIFY2(neutral.blinkMax >= 0.99f && happy.blinkMax >= 0.99f,
             qPrintable(QStringLiteral("采样窗口里眨眼没有一次全睁（neutral %1 / %2 %3）——"
                                       "眨眼被改坏了")
                            .arg(double(neutral.blinkMax))
                            .arg(happyMood)
                            .arg(double(happy.blinkMax))));
}

/*校准素材：14 个情绪原型各出一张**固定区域**的脸部裁切图到
  `build2/tests/live2d-probe/mood-<原型>.png`。

  **只出图、不判断好坏**：这些数值是未校准的初稿，判读由人来做（照 2 倍放大看，
  见「判断锐度必须放大看」那条纪律）。所以这里不断言"表情对不对"，
  只钉住"14 张图尺寸与裁切区完全一致"——否则并排目视时根本对不上。

  顺带把"预设值 vs 模型读回"打出来：被 Core **夹取**的值（写了但越界）会被列出来，
  目视校准时必须知道哪几项其实不是他写的那个数，不然会去改一个永远不会生效的数字。
  （眼睛的睁闭不在此列 —— 它是乘数合成值，见下面 deviations 处的说明。）*/
void TestLive2DWindow::rendersMoodArchetypeCalibrationSheet()
{
    const QString modelName = preferredModelName();
    const QString dir = modelDirFor(modelName);
    if (!QFileInfo::exists(dir))
        QSKIP("本机没有用户配置的模型（禁二传，不入库），跳过校准图");

    // 固定 60fps/1.0x：裁切区尺寸必须可复现（本用例只出对照图，不量性能）
    {
        QSettings settings(m_tempConfigPath, QSettings::IniFormat);
        settings.setValue("character/live2dFps", 60);
        settings.setValue("character/live2dScale", 1.0);
        settings.sync();
    }

    Live2DMoodPreset preset;
    if (!loadConfiguredMoodPreset(&preset))
        QSKIP("本机没有当前角色/模型的心情预设数据（Documents 下，不入库），跳过");

    Live2DCharacterWindow window;
    window.loadModel(modelName);
    QVERIFY2(window.isModelLoaded(), "模型装载失败，无法出校准图");
    QVERIFY2(window.isMoodPresetEnabled(), "情绪预设没装载成功");

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QCoreApplication::processEvents();
    // 冻住帧循环：14 张图之间不再有定时器插进来的任意时间推进（只留每次渲染自身的一帧）
    window.hide();
    QCoreApplication::processEvents();

    // 先用 neutral 出一帧，从它的**人物包围盒**定脸部裁切区；14 张图共用这一个 QRect
    const QString neutralMood = preset.representativeMoodForArchetype(QStringLiteral("neutral"));
    QVERIFY2(!neutralMood.isEmpty(), "neutral 没有别名");
    window.reloadContent(neutralMood);
    // 等过渡走完：这张帧要用来定裁切区，不能是"上一个心情 → neutral"的半路帧
    QVERIFY2(waitForMoodSettle(&window), "neutral 过渡没走完");
    QVERIFY2(window.renderFrameNow(), "neutral 帧渲染失败");
    const QImage neutralFrame = window.renderedImage();
    QVERIFY2(!neutralFrame.isNull(), "neutral 帧为空");
    QRect figureBounds;
    QVERIFY2(opaqueBounds(neutralFrame, 32, &figureBounds), "neutral 帧里量不到人物");
    const QRect roi = faceRegionOfInterest(figureBounds).intersected(neutralFrame.rect());
    QVERIFY2(!roi.isEmpty(), "脸部裁切区是空的");

    const QString outDir = QDir(QCoreApplication::applicationDirPath())
                               .absoluteFilePath(QStringLiteral("../live2d-probe"));
    QVERIFY2(QDir().mkpath(outDir), qPrintable(QStringLiteral("建不出输出目录 %1").arg(outDir)));

    qInfo("MOOD calibration: model=%s frame=%dx%d figure=%d,%d %dx%d roi=%d,%d %dx%d outDir=%s",
          qPrintable(modelName), neutralFrame.width(), neutralFrame.height(),
          figureBounds.x(), figureBounds.y(), figureBounds.width(), figureBounds.height(), roi.x(),
          roi.y(), roi.width(), roi.height(), qPrintable(outDir));

    QStringList savedPaths;
    int mismatchedParams = 0;
    for (const QString &archetype : preset.archetypeNames())
    {
        const QString moodName = preset.representativeMoodForArchetype(archetype);
        QVERIFY2(!moodName.isEmpty(),
                 qPrintable(QStringLiteral("原型 %1 没有别名，出不了这一格").arg(archetype)));

        window.reloadContent(moodName);
        /*等过渡走完再出这一格：这 14 张图是给人做**目视校准**的素材，
           半路帧（上一个原型 → 这个原型）会让校准结论完全错掉。
           代价是每格多等约 0.2s（过渡时长），14 格多约 3s，可以接受。*/
        QVERIFY2(waitForMoodSettle(&window),
                 qPrintable(QStringLiteral("原型 %1 的过渡没走完").arg(archetype)));
        QVERIFY2(window.renderFrameNow(), qPrintable(QStringLiteral("原型 %1 渲染失败").arg(archetype)));
        const QImage frame = window.renderedImage();
        QVERIFY2(!frame.isNull(), "渲染帧为空");
        // 同一模型、同一画布：尺寸必须完全一致，裁切区才能共用
        QCOMPARE(frame.size(), neutralFrame.size());
        const QImage crop = frame.copy(roi);
        QCOMPARE(crop.size(), roi.size());

        const QString path =
            QDir(outDir).absoluteFilePath(QStringLiteral("mood-%1.png").arg(archetype));
        QVERIFY2(crop.save(path), qPrintable(QStringLiteral("写不出 %1").arg(path)));
        savedPaths.append(path);

        /*预设值 vs 读回：把"被驱动器/范围盖掉的参数"暴露出来
          （目视校准时必须知道哪几项其实不是他写的那个数）。

          ⚠️ 眼睛的睁闭**排除在偏差清单外**：它们是"乘数 × 眨眼进度"的合成值，
          这一帧恰好眨眼（0~0.05 而不是 0.05）是**正确行为**，不是没落地。
          拿它当偏差会让人误以为 sleepy 坏了 —— 眼睛的真实性由
          moodEyeOpennessComposesWithBlink 用帧像素证明，这里不再重复判断。
          （被 Core 夹取的值仍然会出现在清单里：夹取是"你写的数没生效"的另一种形态。）*/
        const QHash<QString, float> expected = preset.parametersForMood(moodName);
        const QString eyeLId = preset.parameters().value(QStringLiteral("eyeLOpen")).id;
        const QString eyeRId = preset.parameters().value(QStringLiteral("eyeROpen")).id;
        QStringList deviations;
        for (auto it = expected.constBegin(); it != expected.constEnd(); ++it)
        {
            if (it.key() == eyeLId || it.key() == eyeRId)
                continue;
            const float actual = window.parameterValue(it.key());
            if (qAbs(actual - it.value()) > 0.05f)
            {
                deviations.append(QStringLiteral("%1=%2(want %3)")
                                      .arg(it.key())
                                      .arg(double(actual), 0, 'f', 2)
                                      .arg(double(it.value()), 0, 'f', 2));
                ++mismatchedParams;
            }
        }
        qInfo("MOOD archetype=%s alias=%s crop=%dx%d file=%s deviations=%s", qPrintable(archetype),
              qPrintable(moodName), crop.width(), crop.height(), qPrintable(path),
              deviations.isEmpty() ? "none" : qPrintable(deviations.join(QStringLiteral(", "))));
    }

    QVERIFY2(savedPaths.size() == preset.archetypeNames().size(),
             qPrintable(QStringLiteral("只出了 %1 张图，原型有 %2 个")
                            .arg(savedPaths.size())
                            .arg(preset.archetypeNames().size())));
    qInfo("MOOD calibration summary: %d files, roi=%d,%d %dx%d (identical region), "
          "requested-vs-actual deviations=%d entries (see per-archetype lines above)",
          savedPaths.size(), roi.x(), roi.y(), roi.width(), roi.height(), mismatchedParams);
}

/*[配置键] character/live2dMoodBlendMs 必须真的被读出来，并且被夹到安全范围。

  为什么是窗口层用例、而且要用**真行为**验证：这个键只有窗口知道（它读 config.ini 的
  settingsPath()），渲染器只拿到一个毫秒数。所以"读 + 夹取"这条线只能在这里钉。
  又因为不许改用户的真实 config.ini，全程走 initTestCase 建立的 MANDARIN_CONFIG_INI
  临时文件（与 live2dFps/live2dScale 同一套机制）。

  观察量取"窗口报出来的过渡时长"本身，而不是"再去量一次过渡曲线"：
  后者要在窗口层跑几十帧、还得先装载模型，成本高得多，而"配置 → 时长"这件事
  在渲染器级用例（moodBlendDurationIsConfigurable）里已经证明会改变真实过渡长度了。

  夹取的三条都要验，因为"夹取"是**行为**不是装饰：
    - 0（或负）会让过渡消失（参数一帧跳过去，正是要修的用户症状）→ 抬到下限；
    - 超大值（例如 1 小时）会让表情"永远在半路上"→ 压到上限；
    - 非数字/缺键 → 用默认值（与 fps/scale 的既有做法一致）。*/
void TestLive2DWindow::moodBlendDurationComesFromConfig()
{
    const auto durationForConfiguredValue = [this](const QString &rawValue) {
        QSettings settings(m_tempConfigPath, QSettings::IniFormat);
        if (rawValue.isNull())
            settings.remove(QStringLiteral("character/live2dMoodBlendMs"));
        else
            settings.setValue(QStringLiteral("character/live2dMoodBlendMs"), rawValue);
        settings.sync();
        //每次新建窗口：配置只在构造/装载时读一次（这是刻意的，帧循环里绝不读盘）
        Live2DCharacterWindow window;
        return window.moodBlendDurationMs();
    };

    const int configured = durationForConfiguredValue(QStringLiteral("450"));
    const int zeroClamped = durationForConfiguredValue(QStringLiteral("0"));
    const int negativeClamped = durationForConfiguredValue(QStringLiteral("-100"));
    const int hugeClamped = durationForConfiguredValue(QStringLiteral("3600000"));
    const int missingDefault = durationForConfiguredValue(QString());
    const int garbageDefault = durationForConfiguredValue(QStringLiteral("abc"));

    qInfo("MOODBLEND config: 450→%d, 0→%d, -100→%d, 3600000→%d, 缺键→%d, 非数字→%d", configured,
          zeroClamped, negativeClamped, hugeClamped, missingDefault, garbageDefault);

    //① 配置值真的被读出来（硬编码 200 的实现会在这里失败）
    QVERIFY2(configured == 450,
             qPrintable(QStringLiteral("character/live2dMoodBlendMs=450 读出来是 %1，"
                                       "配置键没有被读（时长是硬编码的？）")
                            .arg(configured)));
    //② 0 / 负数必须被抬高：否则过渡被关掉，换心情又会"顿一下"
    QVERIFY2(zeroClamped >= 50,
             qPrintable(QStringLiteral("过渡时长配成 0 之后报出来 %1 —— 过渡被关掉了，"
                                       "换心情会一帧跳过去")
                            .arg(zeroClamped)));
    QVERIFY2(negativeClamped == zeroClamped,
             qPrintable(QStringLiteral("过渡时长配成 -100 与 0 的夹取结果不一致（%1 vs %2）")
                            .arg(negativeClamped)
                            .arg(zeroClamped)));
    //③ 超大值必须被压住：1 小时的过渡 = 表情永远走不到目标
    QVERIFY2(hugeClamped <= 5000,
             qPrintable(QStringLiteral("过渡时长配成 3600000 之后报出来 %1 —— 没有上限，"
                                       "表情会永远停在半路")
                            .arg(hugeClamped)));
    QVERIFY2(hugeClamped > zeroClamped,
             qPrintable(QStringLiteral("上限（%1）不该低于下限（%2）").arg(hugeClamped).arg(zeroClamped)));
    //④ 缺键/非数字回默认值（默认 200ms，与头文件/文档一致）
    QVERIFY2(missingDefault == 200,
             qPrintable(QStringLiteral("缺键时过渡时长是 %1，默认值应当是 200")
                            .arg(missingDefault)));
    QVERIFY2(garbageDefault == 200,
             qPrintable(QStringLiteral("非数字时过渡时长是 %1，默认值应当是 200")
                            .arg(garbageDefault)));

    //收尾：把这个键移除，免得影响同一进程里后面的用例（临时文件是共用的）
    {
        QSettings settings(m_tempConfigPath, QSettings::IniFormat);
        settings.remove(QStringLiteral("character/live2dMoodBlendMs"));
        settings.sync();
    }
}

/*[接线] 窗口的 SetSpeaking（= main.cpp 里 Dialog::requestSpeakState 的落点）
  必须一路走到渲染器，让嘴巴随时间开合，并在停止后回落到心情值。

  为什么这条不能省（渲染器级已有三条 SPEAK 用例）：
  那些用例直接调 renderer.setSpeaking()，证明的是**数学**；
  而用户路径多两跳 —— Dialog 的信号 → 基类槽（虚函数）→ 派生类 → 渲染器。
  少了这里，一个"槽忘了转给渲染器"的改动在渲染器级测试里是全绿的。

  ⚠️ 帧时间来自墙钟，所以必须 QTest::qWait（见 waitForMoodSettle 的说明）。*/
void TestLive2DWindow::setSpeakingDrivesMouthFlapThroughWindow()
{
    const QString modelName = preferredModelName();
    const QString dir = modelDirFor(modelName);
    if (!QFileInfo::exists(dir))
        QSKIP("本机没有用户配置的模型（禁二传，不入库），跳过说话接线验证");

    Live2DMoodPreset preset;
    if (!loadConfiguredMoodPreset(&preset))
        QSKIP("本机没有当前角色/模型的心情预设数据（Documents 下，不入库），跳过");

    Live2DCharacterWindow window;
    window.loadModel(modelName);
    QVERIFY2(window.isModelLoaded(), "模型装载失败，无法验证说话接线");
    QVERIFY2(window.isMoodPresetEnabled(), "情绪预设没装载成功");

    // 模型没声明嘴巴参数时这条测试没有意义（换模型可能不同）
    if (preset.parameters().value(QStringLiteral("mouthOpen")).id.isEmpty())
        QSKIP("本机的 parameter-map 里没有 mouthOpen，跳过说话接线验证");

    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QCoreApplication::processEvents();
    window.hide();
    QCoreApplication::processEvents();

    /*用一个**开口量为 0** 的心情的原型：这样"张嘴"一定来自扑动，
       不会与心情自己的开口量混在一起（surprised/sleepy/cry/excited 都会预设开口量）。*/
    const QString neutralMood = preset.representativeMoodForArchetype(QStringLiteral("neutral"));
    QVERIFY2(!neutralMood.isEmpty(), "neutral 没有别名");
    const float neutralMouthOpen =
        preset.parametersForArchetype(QStringLiteral("neutral"))
            .value(QStringLiteral("ParamMouthOpenY"), -1.0f);
    QVERIFY2(qAbs(neutralMouthOpen) <= 0.05f,
             qPrintable(QStringLiteral("neutral 原型的开口量是 %1（不是 0），"
                                       "本用例的前提（张嘴来自扑动）不成立")
                            .arg(double(neutralMouthOpen))));

    window.reloadContent(neutralMood);
    QVERIFY2(waitForMoodSettle(&window), "neutral 过渡没走完");

    // ① 不说话：开口量应当停在心情值上（不动）
    QVector<float> idle;
    for (int index = 0; index < 15; ++index)
    {
        QTest::qWait(20);
        QVERIFY2(window.renderFrameNow(), "静默帧渲染失败");
        idle.append(window.parameterValue(QStringLiteral("ParamMouthOpenY")));
    }
    const float idleMin = *std::min_element(idle.constBegin(), idle.constEnd());
    const float idleMax = *std::max_element(idle.constBegin(), idle.constEnd());
    QVERIFY2(idleMax - idleMin <= 1e-4f,
             qPrintable(QStringLiteral("没说话时开口量就在 %1~%2 之间变（应当纹丝不动）")
                            .arg(double(idleMin))
                            .arg(double(idleMax))));

    // ② 说话：走真实窗口路径（这就是 main.cpp 那条 connect 会调到的槽）
    window.SetSpeaking(true);
    QVector<float> speaking;
    for (int index = 0; index < 40; ++index)
    {
        QTest::qWait(20);
        QVERIFY2(window.renderFrameNow(), "说话帧渲染失败");
        speaking.append(window.parameterValue(QStringLiteral("ParamMouthOpenY")));
    }
    const float speakingMin = *std::min_element(speaking.constBegin(), speaking.constEnd());
    const float speakingMax = *std::max_element(speaking.constBegin(), speaking.constEnd());
    qInfo("SPEAK window[%s]: idle span=%.5f → speaking %s span=%.4f",
          qPrintable(modelName), double(idleMax - idleMin),
          qPrintable(QStringLiteral("%1~%2").arg(double(speakingMin)).arg(double(speakingMax))),
          double(speakingMax - speakingMin));
    QVERIFY2(speakingMax - speakingMin > 0.05f,
             qPrintable(QStringLiteral("窗口收到 SetSpeaking(true) 后开口量只在 %1~%2 之间 ——"
                                       "信号没有走到渲染器（嘴没动）")
                            .arg(double(speakingMin))
                            .arg(double(speakingMax))));

    // ③ 停止说话：回落到心情值（neutral 是 0）
    window.SetSpeaking(false);
    QVector<float> stopped;
    for (int index = 0; index < 45; ++index)
    {
        QTest::qWait(20);
        QVERIFY2(window.renderFrameNow(), "停止说话后的帧渲染失败");
        stopped.append(window.parameterValue(QStringLiteral("ParamMouthOpenY")));
    }
    const int tailBegin = stopped.size() * 3 / 4;
    float tailMin = stopped[tailBegin];
    float tailMax = stopped[tailBegin];
    for (int index = tailBegin; index < stopped.size(); ++index)
    {
        tailMin = std::min(tailMin, stopped[index]);
        tailMax = std::max(tailMax, stopped[index]);
    }
    qInfo("SPEAK window stop: tail span=%.5f (%.5f~%.5f), mood=%.5f",
          double(tailMax - tailMin), double(tailMin), double(tailMax), double(neutralMouthOpen));
    QVERIFY2(tailMax - tailMin <= 0.02f,
             qPrintable(QStringLiteral("SetSpeaking(false) 后开口量仍在 %1~%2 之间摆")
                            .arg(double(tailMin))
                            .arg(double(tailMax))));
    QVERIFY2(qAbs(tailMax - neutralMouthOpen) <= 0.02f,
             qPrintable(QStringLiteral("停止说话后开口量停在 %1，不等于心情值 %2")
                            .arg(double(tailMax))
                            .arg(double(neutralMouthOpen))));
}

QTEST_MAIN(TestLive2DWindow)
#include "test_live2dwindow.moc"
