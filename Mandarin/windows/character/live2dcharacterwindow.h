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
    static constexpr int kProbeCanvasHeight = 512;
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
    static constexpr double kFallbackHeightRatio = 1.0;

    void onFrameTick();              //定时器回调：渲染一帧并登记
    void refreshInteractiveRegion(); //低频重算交互区（构建 QBitmap 很贵，不能每帧做）
    bool renderAndRegisterFrame();   //渲染一帧并登记 alpha 图，失败返回 false

    /*按模型名在两个候选目录里找模型，返回含 <model>.model3.json 的目录，找不到返回空*/
    QString resolveModelDir(const QString &modelName) const;

    /*读 config.ini 的帧率/缩放（各自带默认值与安全夹取）*/
    void applyFrameRateFromConfig();
    void applyRenderScaleFromConfig();

    /*探针渲染：先按正方形画布渲一帧，量出人物到底占多高（见 m_figureHeightRatio）。
      为什么必须探测而不是用模型画布尺寸：投影矩阵会把模型铺满整个目标画布
      （见 Live2DOffscreenRenderer::renderFrame），画布本身不携带"模型多宽多高"的信息，
      而模型画布宽高只有渲染器内部知道 —— 所以只能实测，不为渲染器新增 API。*/
    void probeFigureMetrics();

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

    double m_figureAspect = kFallbackFigureAspect;     //人物宽/高（探针实测）
    double m_figureHeightRatio = kFallbackHeightRatio; //人物高 / 探针画布高

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
