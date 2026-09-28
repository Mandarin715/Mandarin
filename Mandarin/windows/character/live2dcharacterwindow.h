#ifndef LIVE2DCHARACTERWINDOW_H
#define LIVE2DCHARACTERWINDOW_H

#include "characterwindowbase.h"

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

    /*帧循环节拍（毫秒），由 character/live2dFps 推出*/
    int frameIntervalMs() const { return m_frameIntervalMs; }
    /*实际提交给渲染器的分辨率（逻辑尺寸 × 有效 dpr）*/
    QSize renderSize() const;

  public slots:
    /*contentName 为模型名（如 miku）；找不到时保留当前模型不重载*/
    void reloadContent(const QString &contentName) override;

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
    /*探针取样次数与间隔：要覆盖整段待机动作（本模型 2.667s 一循环）。
       20 次 × 150ms ≈ 3s > 一个循环，够把动作走满一遍；只做一次布局，成本可接受。*/
    static constexpr int kProbeSamples = 20;
    static constexpr int kProbeSampleIntervalMs = 150;
    /*人物高度目标占比：留出 (1-ratio)/2 的上下余量。
       0.84 → 上下各 8% 余量，人物在屏幕上的尺寸只比原来小 16%，同时杜绝边缘裁切。
       （取 0.84 而不是贴着 0.88：探针是在一段动作上求并集，真实帧的姿势仍会有
       几个像素的出入，余量要留得比"刚好"更宽一点才稳。）*/
    static constexpr double kTargetFigureHeightRatio = 0.84;
    static constexpr int kRegionRefreshInterval = 10;
    static constexpr int kDefaultFps = 60;
    static constexpr int kMinFps = 5;
    static constexpr int kMaxFps = 240;
    static constexpr double kMinRenderScale = 0.5;
    static constexpr double kMaxRenderScale = 2.0;
    static constexpr int kMaxRenderSide = 2048;       //渲染长边上限（护 VRAM 与读回带宽）
    static constexpr double kMaxScreenHeightRatio = 0.85; //逻辑窗口高度上限：可用屏高的 85%
    static constexpr double kMinCanvasSide = 32;
    static constexpr double kMaxCanvasSide = 8192;
    static constexpr double kFallbackFigureAspect = 3.0 / 4.0; //探针失败时的兜底宽高比
    static constexpr double kFallbackHeightRatio = kTargetFigureHeightRatio;

    void onFrameTick();              //定时器回调：渲染一帧并登记
    void refreshInteractiveRegion(); //低频重算交互区（构建 QBitmap 很贵，不能每帧做）
    bool renderAndRegisterFrame();   //渲染一帧并登记 alpha 图，失败返回 false

    /*按模型名在两个候选目录里找模型，返回含可用 model3.json 的目录，找不到返回空*/
    QString resolveModelDir(const QString &modelName) const;

    /*在 modelDir 里挑出要用的 model3.json 文件名（不假定它等于 <模型名>.model3.json：
       实机模型常对不上，如 atri/atri_8.model3.json）。找不到返回空。*/
    static QString resolveModelJsonName(const QString &modelDir, const QString &modelName);

    /*读 config.ini 的帧率/缩放（各自带默认值与安全夹取）*/
    void applyFrameRateFromConfig();
    void applyRenderScaleFromConfig();

    /*探针渲染：量出人物可见范围在绘制输出空间里的跨度（见 Live2DOffscreenRenderer::
      probeFigureMetrics），据此定画布宽高比与显示比例。*/
    void probeFigureMetrics();

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

    /*重入保护：高帧率 + 慢帧时不能让定时器把渲染排队堆起来；也用于 paintEvent 里的重排*/
    bool m_renderingFrame = false;

    /*人物几何（探针实测）
      m_figureAspect：人物可见范围在**目标画布比例**下的宽/高。画布按它定宽高比，
      人物才不会横向溢出。
      m_figureSpanY：人物可见范围在**探针（1:1）比例**下的高度跨度。渲染器的缩放是在
      探针空间里算的，所以显示比例要用它，见 relayoutContent。
      m_displayRatio：人物高度占画布高度的目标比例，交给渲染器 setDisplayHeightRatio。*/
    double m_figureAspect = kFallbackFigureAspect;
    double m_figureSpanY = kFallbackHeightRatio;
    double m_displayRatio = kFallbackHeightRatio;

    /*最近一帧对应的**逻辑**画布尺寸。
      为什么不实时用 m_scaledImg.size()/dpr 推：窗口一旦映射到屏幕，devicePixelRatioF()
      会变（本例 1.0 → 1.25），同一个 QImage 用新 dpr 去除就会算出另一个逻辑尺寸，
      导致 contentSize() 与 size() 打架。直接把布局时定下来的尺寸记下来最可靠。*/
    QSize m_logicalCanvasSize;

    /*reloadContent 在窗口还没映射时只记模型名，真正的首次布局推迟到 showEvent
      （那时 devicePixelRatioF() 才是真实值）。*/
    QString m_pendingModelName;
};

#endif // LIVE2DCHARACTERWINDOW_H
