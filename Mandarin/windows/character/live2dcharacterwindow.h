#ifndef LIVE2DCHARACTERWINDOW_H
#define LIVE2DCHARACTERWINDOW_H

#include "characterwindowbase.h"

#include "../../utils/Live2DMoodPreset.h"
#include "../../utils/Live2DOffscreenRenderer.h"

#include <QImage>
#include <QString>

class QTimer;
class QShowEvent;
class QHideEvent;
class QPaintEvent;

/*Live2D 立绘渲染器：窗口层（穿透/交互区/拖拽/位置/气泡/拖放）全部继承自 CharacterWindowBase，
  本类只负责「离屏渲染成帧 → 登记给窗口层」这一段。

  v1 范围：只做渲染与窗口形状，不含表情/动作/口型（那些是后续阶段）。*/
class Live2DCharacterWindow : public CharacterWindowBase
{
    Q_OBJECT

  public:
    explicit Live2DCharacterWindow(QWidget *parent = nullptr);
    ~Live2DCharacterWindow() override;

    /*装载模型（按模型名找目录 + 入口文件）。只在启动/换模型时调用一次 ——
      **换心情不再走这条路**，否则"心情名找不到同名模型"会被静默忽略（这就是本来的 bug）。
      返回是否装载成功；失败时保留上一版画布，不做任何清理（避免闪烁成空白窗口）。*/
    bool loadModel(const QString &modelName);

    /*模型是否装载成功（main.cpp 据此决定要不要回退到 PNG 立绘）*/
    bool isModelLoaded() const { return m_modelLoaded; }

    /*强制立刻走一遍完整帧管线（渲染 + 登记 + 低频交互区）：配置变更后要求立即重绘，
      以及测试里量「真实全链路成本」都靠它。返回是否成功渲染出一帧。*/
    bool renderFrameNow();

    /*该点（窗口逻辑坐标）是否算命中人物（= 不透明，可交互、可拖动）。
      抽成公开方法是因为「鼠标按下是否被接受」这条观察量对非 popup 控件不可靠
      （QWidget::mousePressEvent 并不 accept），而这类判定本身就是值得直接验证的行为。*/
    bool acceptsClickAt(const QPoint &logicalPoint) const;

    /*以下三个访问器把基类的 protected 内容暴露成只读接口，供启动期回退判定与测试断言使用*/
    QSize contentSize() const override; //当前**逻辑**画布尺寸（等于窗口尺寸）
    QImage renderedImage() const { return m_scaledImg; }
    QString modelDir() const { return m_modelDir; }

    /*读模型参数的当前值。与上面几个访问器同样的理由：情绪预设到底有没有真的落到模型上，
      只有"读回参数"能证明（两帧像素差无法区分"换了心情"和"呼吸多走了一拍"）。
      没装载模型或参数不存在时返回 0。*/
    float parameterValue(const QString &parameterId) const;

    /*眨眼驱动器本帧写下的原始开眼度（0 = 闭紧、1 = 全睁）。
      情绪闭眼是**乘在它上面**的（见 Live2DOffscreenRenderer::blinkValue 的说明）：
      要证伪"情绪把眨眼钉死了"就必须能看见这个值本身。*/
    float blinkValue() const;

    /*单独设定情绪睁闭眼乘数（默认 1.0 = 眨眼自己说了算）。
      正常路径不需要它 —— applyMood 会从预设里自动带上；它是给"关掉情绪对眨眼的干预"
      与校准实验用的（见 Live2DOffscreenRenderer::setEyeOpennessMultiplier）。*/
    void setEyeOpennessMultiplierForTest(const QHash<QString, float> &moodValues);

    /*用完上面那份显式乘数后放手：让眼睛重新跟随当前心情的覆盖表。
       记号（"显式"）不放手的话，那份乘数会把之后所有心情的眼睛都钉住。*/
    void clearEyeOpennessMultiplierOverride();

    /*情绪预设是否可用（两份 JSON 装载成功）。不可用时情绪功能自关：不施加任何覆盖。*/
    bool isMoodPresetEnabled() const { return m_moodPreset.isEnabled(); }

    /*帧循环节拍（毫秒），由 character/live2dFps 推出*/
    int frameIntervalMs() const { return m_frameIntervalMs; }
    /*心情过渡时长（毫秒），由 character/live2dMoodBlendMs 推出（已夹取）。
       暴露出来是为了让"配置真的被读到并被夹取"这条能直接断言 ——
       只看帧循环是看不出过渡时长的。*/
    int moodBlendDurationMs() const { return m_moodBlendDurationMs; }
    /*实际提交给渲染器的分辨率（逻辑尺寸 × 有效 dpr）*/
    QSize renderSize() const;

    /***把**下一帧**的时间步长钉死（秒）——只生效一帧，走渲染器同一条夹取路径。

       ⚠️ 这是给**测试**用的确定性阀门，不是生产路径（生产路径的探针也用它，
       见 renderer.setNextFrameDeltaSeconds 的三条说明）。它存在的理由：

       相邻帧对比（moodEyeOpennessComposesWithBlink）量的是"两帧之间背景动了多少"，
       而帧步长默认取自**墙钟**：机器一被抢占，一帧就吃掉几十毫秒，呼吸/待机动作随之
       跳一大步，于是"背景漂移"与"要观察的信号"变成同量级 —— 实测负载下漂移从 0~5 像素
       涨到四位数，用例必然红，而那是测量方式的问题，不是被测行为的问题。
       钉住步长之后，同一对帧的漂移只由"注入的 2×delta"决定，与被抢占与否无关。*/
    void setNextFrameDeltaSecondsForTest(float seconds);

    /*回到墙钟路径（并重置时间基准，免得"钉住的这一段"被算进解冻后的第一帧）。*/
    void clearNextFrameDeltaForTest();

  public slots:
    /*按**心情名**应用情绪预设（基类契约：PNG 路径用同一个入口按名切换内容）。
      心情名 = 角色 Tachie/ 下 PNG 的文件名；经 moods.json 映射成一组参数覆盖值。
      找不到/非法的名字回退 neutral（绝不会把上一种情绪留在屏幕上）。
      没装载模型时调用也是安全的：只记住心情，模型装载成功后补上。*/
    void reloadContent(const QString &contentName) override;

    /*TTS 播放状态：转给渲染器，让嘴巴做"纸片人"开合（见
       Live2DOffscreenRenderer::setSpeaking）。基类默认实现什么都不做（PNG 路径）。*/
    void SetSpeaking(bool speaking) override;

    /*TTS 这一拍的响度电平（0~1）：转给渲染器，让开口量跟着**真实响度**走，
       于是句子之间的停顿（电平为 0）嘴会回到心情值上 —— 用户要的就是这个
       （见 Live2DOffscreenRenderer::setSpeechLevel）。基类默认实现什么都不做。*/
    void SetSpeechLevel(float level) override;

  protected:
    void relayoutContent() override; //按 m_tachieSizePercent 重算逻辑画布并渲染首帧

    /*把登记好的渲染帧画到半透明窗口上。
      为什么必须有它：窗口层（基类）只负责登记 alpha 图 + 命中判定，**不负责绘制**
      —— PNG 路径是 QLabel 拿着 pixmap 自己在画。Live2D 帧没有控件承载，
      少了这个 paintEvent 就是「窗口有形状、可点击，却什么都看不见」。*/
    void paintEvent(QPaintEvent *event) override;

    /*命中判定必须按**本类实际的绘制映射**来算。
      基类用的是 QImage::devicePixelRatio()，而本类把整帧缩放进 rect()，
      真实比例是 renderSize()/rect()（dpr 与 live2dScale 都掺在里面）。
      不覆写的话，开了 live2dScale 后点击位置会整体偏掉 —— 也就是"点不到人"。
      （基类的 DPR 折算对 PNG 路径仍然成立，这里只是不走它，免得两套映射打架。）*/
    void mousePressEvent(QMouseEvent *event) override;

    void showEvent(QShowEvent *event) override; //显示才跑帧循环，隐藏不烧 CPU/GPU
    void hideEvent(QHideEvent *event) override;

  private:
    /*画布尺寸启发式的基准高、探针参数与各类夹取范围*/
    static constexpr int kBaseCanvasHeight = 900;
    static constexpr int kProbeCanvasSide = 512;
    /*探针取样次数与"取样之间推进多少虚拟时间"。
       20 次 × 150ms：要覆盖整段待机动作（本模型 2.667s 一循环）。
       ⚠️ 这个 150ms 是**虚拟时间**，不是墙钟睡眠：渲染器的帧步长上限是 100ms
       （Live2DOffscreenRenderer 的 kMaxFrameDeltaSeconds），所以每个取样实际推进 100ms、
       20 次共 ≈1.9s —— 与"以前真的 msleep(150)"时渲染器看到的步长**逐位相同**
       （150ms 的墙钟间隔本来就会被夹成 100ms）。不把它调大（例如 200ms）是故意的：
       测量模式下人物的纵向范围已经饱和到 1.977/2.0，把窗口拉长只会让并集顶到测量帧边界、
       把"人物到底多大"量成"被裁掉的大小"。*/
    static constexpr int kProbeSamples = 20;
    static constexpr int kProbeSampleIntervalMs = 150;
    /*人物目标占比：**横竖同一个值**，于是画布四边留出 (1-ratio)/2 的余量。
       0.84 → 四边各 8% 余量，人物在屏幕上的尺寸只比"贴边铺满"小 16%，同时杜绝边缘裁切。
       为什么横竖必须同值：画布宽高比取的是人物真实宽高比，两个方向的余量才会一样宽；
       以前只盯纵向（横向占比 = 纵向占比 × 人物宽高比，实测只有 0.36），
       人物就缩在画布中间、两侧空出一大片。
       （取 0.84 而不是贴着 0.88：探针是在一段动作上求并集，真实帧的姿势仍会有
       几个像素的出入，余量要留得比"刚好"更宽一点才稳。）*/
    static constexpr double kTargetFigureRatio = 0.84;
    /*实测校正：最多做几轮"量占比 → 按比例修正"。
       两个方向的缩放在渲染器里是**解耦**的（横向 fitX 不影响纵向几何），按比例修正一次
       就是精确解，所以正常情况第一轮就收敛；留到两轮只是兜底（例如首帧渲染与测量之间
       姿势恰好跳了一下）。绝不允许更多轮：再多只会在两个解之间来回摆。*/
    static constexpr int kMaxCorrectionRounds = 2;
    /*判定"占比已经到位"的容差（占画布的比例）*/
    static constexpr double kCorrectionTolerance = 0.02;
    /*一次测量取几帧求并集、帧间隔多少毫秒。
       为什么要并集：待机动作/呼吸会让姿势移动，单帧包围盒可能恰好偏松，按它校正会把
       人物放得比"整段动作都装得下"更大，动作一摆就贴边。取 2 帧（间隔 120ms）是
       成本与稳健性的折中：多渲一帧约几毫秒，比裁掉人物便宜得多。
       ⚠️ 与探针同理：这 120ms 现在也是**虚拟时间**（不再 msleep），实际推进量同样是
       被夹取的 100ms —— 与以前逐位相同。*/
    static constexpr int kMeasuredFramePasses = 2;
    static constexpr int kMeasuredFrameIntervalMs = 120;
    static constexpr int kRegionRefreshInterval = 10;
    static constexpr int kDefaultFps = 60;
    static constexpr int kMinFps = 5;
    static constexpr int kMaxFps = 240;
    /*心情过渡：默认 200ms；下限 50ms、上限 3000ms。
       下限的理由：过渡比"一帧"还短就等于没过渡（参数在一帧里跳过去，正是要修的症状）。
       上限的理由：过渡长过几秒就不是"过渡"，而是"表情永远在半路上"，
       而且会让"AI 说话的心情"迟迟落不到脸上。两个值都远松于可用区间，只挡明显配错的数。*/
    static constexpr int kDefaultMoodBlendMs = 200;
    static constexpr int kMinMoodBlendMs = 50;
    static constexpr int kMaxMoodBlendMs = 3000;
    static constexpr double kMinRenderScale = 0.5;
    static constexpr double kMaxRenderScale = 2.0;
    static constexpr int kMaxRenderSide = 2048;       //渲染长边上限（护 VRAM 与读回带宽）
    static constexpr double kMaxScreenHeightRatio = 0.85; //逻辑窗口高度上限：可用屏高的 85%
    static constexpr double kMinCanvasSide = 32;
    static constexpr double kMaxCanvasSide = 8192;
    static constexpr double kMinDisplayRatio = 0.05;
    static constexpr double kMaxDisplayRatio = 1.0;
    /*人物宽高比的可用范围：探针彻底失败时才用兜底值（3:4）*/
    static constexpr double kMinFigureAspect = 0.05;
    static constexpr double kMaxFigureAspect = 20.0;
    static constexpr double kFallbackFigureAspect = 3.0 / 4.0; //探针失败时的兜底宽高比

    void onFrameTick();              //定时器回调：渲染一帧并登记
    void refreshInteractiveRegion(); //低频重算交互区（构建 QBitmap 很贵，不能每帧做）
    bool renderAndRegisterFrame();   //渲染一帧并登记 alpha 图，失败返回 false

    /*换一整组参数覆盖并（有画布时）立刻出一帧。
      为什么整组替换、为什么立刻出帧：见 .cpp 里的说明（残留情绪 + 即时性）。*/
    void applyMood(const QString &moodName);

    /*按模型名在两个候选目录里找模型，返回含可用 model3.json 的目录，找不到返回空*/
    QString resolveModelDir(const QString &modelName) const;

    /*在 modelDir 里挑出要用的 model3.json 文件名（不假定它等于 <模型名>.model3.json：
       实机模型常对不上，如 atri/atri_8.model3.json）。找不到返回空。*/
    static QString resolveModelJsonName(const QString &modelDir, const QString &modelName);

    /*读 config.ini 的帧率/缩放（各自带默认值与安全夹取）*/
    void applyFrameRateFromConfig();
    void applyRenderScaleFromConfig();
    /*读 config.ini 的心情过渡时长（character/live2dMoodBlendMs）并转发给渲染器。
       与 fps/scale 同一套做法：默认值 + 安全夹取 + 只记一条日志（不每秒刷屏）。*/
    void applyMoodBlendFromConfig();

    /*探针渲染：量出人物可见范围在绘制输出空间里的跨度（见 Live2DOffscreenRenderer::
      probeFigureMetrics），据此定画布宽高比与目标占比。

      **同一份模型只探一次**：结果缓存在 m_figureAspect/m_figureSpanX/m_figureSpanY 里
      （它是模型的属性，与画布尺寸无关 —— 理由见 m_figureMetricsValid 的说明）。
      取样之间用**虚拟时间**（setNextFrameDeltaSeconds）推进，不再 msleep。*/
    void probeFigureMetrics();

    /*量当前画布上人物实际占多少（横/竖各一个 0~1 的比例）。
      在 kMeasuredFramePasses 个真实帧上求包围盒并集，避免"单帧偏松"把人物放大到贴边。
      量与校正都不依赖任何解析换算 —— 这是「人物必须真的填满画布」唯一的可信依据。*/
    bool measureFigureOccupancy(double *fractionX, double *fractionY);

    /*量当前登记帧（m_scaledImg，物理像素）里人物可见部分的包围盒。
      layout 时用它实测校正显示比例，不依赖解析换算。*/
    bool opaqueBoundsInFrame(QRect *bounds) const;

    Live2DOffscreenRenderer m_renderer;
    QTimer *m_frameTimer = nullptr; //渲染节拍

    bool m_modelLoaded = false;
    QString m_modelDir;       //当前模型的目录（bubble 定位/调试用）
    QString m_modelJsonName;  //形如 miku.model3.json
    int m_frameIndex = 0;     //帧计数：每 kRegionRefreshInterval 帧重算一次交互区

    /*渲染参数（均可由 config.ini 覆盖）*/
    int m_frameIntervalMs = 1000 / kDefaultFps;
    double m_renderScale = 1.0;
    /*心情过渡时长（毫秒；已夹取）。构造时从 config.ini 读一次，之后不再读盘。*/
    int m_moodBlendDurationMs = kDefaultMoodBlendMs;

    /*重入保护：高帧率 + 慢帧时不能让定时器把渲染排队堆起来；也用于 paintEvent 里的重排*/
    bool m_renderingFrame = false;

    /*人物几何（探针实测）
      m_figureAspect：人物**真实**宽高比（宽/高），画布就取这个宽高比 ——
      这样人物在四个方向留一样宽的余量，也不会被拉伸。
      m_figureSpanX / m_figureSpanY：人物可见范围在**自然缩放**的输出空间里的宽/高跨度。
      渲染器的摆放缩放是 fit = 2*目标占比/跨度，所以这两个值要跟探针同一把尺子（见
      Live2DOffscreenRenderer::probeFigureMetrics）。
      m_displayRatioX / m_displayRatioY：人物在画布 x/y 方向各占的目标比例，交给
      setDisplayRatios。两个方向分别给是"不拉伸还占满"的必要条件（见渲染器头注释）。*/
    double m_figureAspect = kFallbackFigureAspect;
    double m_figureSpanX = 2.0;
    double m_figureSpanY = 2.0;
    double m_displayRatioX = kTargetFigureRatio;
    double m_displayRatioY = kTargetFigureRatio;

    /*上面这三个量是不是已经由探针量出来过（**属于模型**，与画布尺寸无关）。
        为什么必须缓存：探针量的是模型的可见范围与真实宽高比 —— 探针帧是固定的
        512x512 正方形，测量模式用的是固定的等比变换（kMeasureScale），反解回"自然缩放"
        只除以那个固定值（见 Live2DOffscreenRenderer::probeFigureMetrics）。画布尺寸是从
        这份结果**推**出来的，不是它的输入。所以"换个立绘大小就重探一遍"是纯浪费：
        修复前实测一次布局 = 3.0s 主线程冻结，而输入框每敲一个字符就触发一次。
        失效点只有 loadModel()（换模型 / 重新装载）。*/
    bool m_figureMetricsValid = false;
    /*"探针已缓存、本次重排不再重探"只记一条日志：用户拖输入框时不能刷屏*/
    bool m_figureMetricsReuseLogged = false;

    /*最近一帧对应的**逻辑**画布尺寸。
      为什么不实时用 m_scaledImg.size()/dpr 推：窗口一旦映射到屏幕，devicePixelRatioF()
      会变（本例 1.0 → 1.25），同一个 QImage 用新 dpr 去除就会算出另一个逻辑尺寸，
      导致 contentSize() 与 size() 打架。直接把布局时定下来的尺寸记下来最可靠。*/
    QSize m_logicalCanvasSize;

    /*最近一次布局时窗口的 dpr。画布尺寸是按「canvas × dpr 为整数」对齐定下来的
      （见 relayoutContent 的说明与 utils/DevicePixelAlign.h），所以 dpr 一变对齐就失效、必须重排。
      为什么不只比 m_logicalCanvasSize 与 size()：dpr 与逻辑尺寸通常一起变，但并不等价
      （系统只改缩放比例、窗口不挪时只变 dpr），多存一个 dpr 是廉价保险。*/
    qreal m_layoutDpr = 0.0;

    /*paintEvent 自检重排的"同一组合只试一次"记录（见 paintEvent 的说明）。
      为什么要它：一旦"画布 vs 窗口/dpr"的不一致是**持久**的（重排也修不好，例如
      窗口管理器把 resize 夹掉了），每帧重绘都重排就是"每次重绘一次探针" —— 用户看到的
      是持续掉帧。记下"为哪个 (窗口尺寸, dpr) 组合试过"就能做到：同一组合只试一次、
      记一条日志就放手，而真正的尺寸/dpr 变化（新组合）照样会重新尝试。*/
    bool m_relayoutAttempted = false;
    QSize m_relayoutAttemptedForSize;
    qreal m_relayoutAttemptedForDpr = 0.0;
    bool m_relayoutGaveUpLogged = false;

    /*reloadContent 在窗口还没映射时只记模型名，真正的首次布局推迟到 showEvent
      （那时 devicePixelRatioF() 才是真实值）。*/
    QString m_pendingModelName;

    /*情绪预设与当前心情。
      m_moodPreset 由 loadModel 按模型名装载（路径 = CharacterAssestPath + CharSelect + 模型名）；
      装载失败时 isEnabled()==false，窗口不施加任何覆盖（功能自关，绝不崩）。
      m_currentMoodName 的初值是 "default"（Tachie 里就是中立那张）：模型装载完成后
      即使 AI 还没说话，也要有一份显式的 neutral 覆盖，而不是"表里什么都没有"。*/
    Live2DMoodPreset m_moodPreset;
    QString m_currentMoodName = QStringLiteral("default");
};

#endif // LIVE2DCHARACTERWINDOW_H
