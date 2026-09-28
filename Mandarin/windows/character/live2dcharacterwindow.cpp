#include "live2dcharacterwindow.h"

#include "../../GlobalConstants.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHideEvent>
#include <QImage>
#include <QPainter>
#include <QPaintEvent>
#include <QScreen>
#include <QSettings>
#include <QShowEvent>
#include <QTimer>

#include <algorithm>
#include <cmath>

#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
//ApplyInteractiveRegionFromImage() 的非 Windows 分支要用到基类里的这套 X11 逻辑，
//派生类的编译单元必须看到同样的头（与 Tachie 保持一致）。
#ifdef Q_OS_LINUX
#include <X11/Xlib.h>
#include <X11/Xutil.h> //必须包含这个处理图像转换
#include <X11/extensions/shape.h>
#endif
#endif

namespace
{
/*探针渲染的采样步长：占比只需要精确到几个像素，隔点采样把扫描成本降到 1/4*/
constexpr int kProbeSampleStride = 2;
/*包围盒占比低于这个值就认为模型基本没画出来，退回默认尺寸*/
constexpr double kMinProbeCoverage = 0.01;
/*画布尺寸下限/上限：防止极端百分比或异常探针算出 1px 或巨大的窗口*/
constexpr int kMinCanvasSide = 32;
constexpr int kMaxCanvasSide = 8192;

int clampInt(int value, int low, int high)
{
    return std::min(std::max(value, low), high);
}

double clampDouble(double value, double low, double high)
{
    return std::min(std::max(value, low), high);
}
} // namespace

Live2DCharacterWindow::Live2DCharacterWindow(QWidget *parent)
    : CharacterWindowBase(parent)
{
    applyFrameRateFromConfig();

    m_frameTimer = new QTimer(this);
    //高帧率（280Hz 屏上可能配到 120~240fps）必须用精确定时器：
    //Windows 的默认粗粒度定时器给不出 ~7ms 这种间隔，会退化成 15.6ms 的倍数。
    m_frameTimer->setTimerType(Qt::PreciseTimer);
    m_frameTimer->setInterval(m_frameIntervalMs);
    connect(m_frameTimer, &QTimer::timeout, this, &Live2DCharacterWindow::onFrameTick);
}

Live2DCharacterWindow::~Live2DCharacterWindow()
{
    //帧循环必须先停：渲染器析构要销毁 GL 上下文，定时器再进来就会碰到半销毁状态。
    if (m_frameTimer)
        m_frameTimer->stop();
}

/*帧率：character/live2dFps，默认 60，夹取 [5,240]*/
void Live2DCharacterWindow::applyFrameRateFromConfig()
{
    QSettings settings(IniSettingPath, QSettings::IniFormat);
    const int fps =
        clampInt(settings.value("character/live2dFps", kDefaultFps).toInt(),
                 kMinFps, kMaxFps);
    m_frameIntervalMs = std::max(1, 1000 / fps);
    qInfo() << "Live2D 帧率配置:" << fps << "fps → 定时器间隔" << m_frameIntervalMs << "ms";
}

/*渲染缩放：character/live2dScale，默认 1.0，夹取 [0.5,2.0]。
  贴图是 6×4096 的大图，1.5x 在 1440p 上确实更清晰，所以留出这个档位。*/
void Live2DCharacterWindow::applyRenderScaleFromConfig()
{
    QSettings settings(IniSettingPath, QSettings::IniFormat);
    m_renderScale =
        clampDouble(settings.value("character/live2dScale", 1.0).toDouble(),
                    kMinRenderScale, kMaxRenderScale);
    qInfo() << "Live2D 渲染缩放:" << m_renderScale;
}

/*按「全局模型根目录 → 当前角色资源目录」的顺序找模型，取第一个含 <model>.model3.json 的目录*/
QString Live2DCharacterWindow::resolveModelDir(const QString &modelName) const
{
    const QString trimmed = modelName.trimmed();
    if (trimmed.isEmpty())
        return QString();

    QStringList candidates;
    candidates << QDir(Live2DModelRootPath).filePath(trimmed);
    const QString charName = ReadNowSelectChar();
    if (!charName.isEmpty() && charName != "未选择")
    {
        candidates << QDir(CharacterAssestPath)
                          .filePath(charName + "/Live2D/" + trimmed);
    }

    const QString modelJsonName = trimmed + ".model3.json";
    for (const QString &dir : candidates)
    {
        if (QFileInfo::exists(QDir(dir).filePath(modelJsonName)))
            return dir;
    }

    qWarning() << "未找到 Live2D 模型:" << trimmed << "已查找:" << candidates;
    return QString();
}

void Live2DCharacterWindow::reloadContent(const QString &contentName)
{
    const QString modelName = contentName.trimmed();
    const QString dir = resolveModelDir(modelName);
    if (dir.isEmpty())
    {
        //保留已装载的模型：内容名可能是心情名，不该因为找不到就黑掉立绘。
        return;
    }

    applyRenderScaleFromConfig();

    const QString modelJsonName = modelName + ".model3.json";
    QString error;
    if (!m_renderer.load(dir, modelJsonName, &error))
    {
        qWarning() << "Live2D 模型装载失败:" << dir << error;
        //装载失败时上一版画布还留着，直接沿用（不置空，避免闪烁成空白窗口）。
        return;
    }

    m_modelDir = dir;
    m_modelJsonName = modelJsonName;
    m_modelLoaded = true;
    m_frameIndex = 0;
    m_renderingFrame = false;
    qInfo() << "Live2D 模型已装载:" << modelJsonName << "于" << dir;

    // 窗口映射到屏幕之前 devicePixelRatioF() 给不出真实值（本机未映射时是 1.0，
    // 实际 1.25），此时用错 dpr 定画布/渲染分辨率会同时错两处（屏幕尺寸与像素密度）。
    // 所以未映射就先只记下模型名，真正的首次布局交给 showEvent —— 那时 dpr 才是真的。
    if (isVisible())
        relayoutContent();
    else
        m_pendingModelName = modelName;
}

/*画布尺寸启发式（v1）：
   1) 先按正方形探针渲一帧，量出人物占探针高度的比例（figureHeightRatio）与人物宽高比；
   2) 逻辑人物高 = 基准高 900 × (m_tachieSizePercent/100)；
   3) 逻辑画布高 = 逻辑人物高 / figureHeightRatio，逻辑画布宽 = 逻辑画布高 × 人物宽高比 ——
      这样人物既不变形、又基本填满画布，没有大片空白；
   4) 逻辑尺寸再夹进屏幕可用高度/宽度的 85%（2560x1440@125% 只有 2048x1152 逻辑像素，
      不夹的话大立绘会被屏幕裁掉）。m_tachieSizePercent 沿用 PNG 路径的「立绘大小」语义。

   注意：character/live2dScale **只放大渲染分辨率**，不改变这里的逻辑尺寸
   （逻辑尺寸直接决定屏幕上的大小，见 renderAndRegisterFrame）。*/
void Live2DCharacterWindow::relayoutContent()
{
    if (!m_renderer.isLoaded())
        return;

    // 重排期间置位，避免 paintEvent 在自检「画布 vs 窗口」时递归进来
    m_renderingFrame = true;

    //探针必须在定尺寸之前跑：宽高比与占比都来自它
    probeFigureMetrics();

    const int targetFigureHeight =
        std::max(1, static_cast<int>(std::lround(kBaseCanvasHeight *
                                                (m_tachieSizePercent / 100.0))));
    int canvasHeight = clampInt(
        static_cast<int>(std::lround(targetFigureHeight /
                                     std::max(1e-3, m_figureHeightRatio))),
        static_cast<int>(kMinCanvasSide), static_cast<int>(kMaxCanvasSide));
    int canvasWidth = clampInt(
        static_cast<int>(std::lround(canvasHeight * m_figureAspect)),
        static_cast<int>(kMinCanvasSide), static_cast<int>(kMaxCanvasSide));

    //屏幕夹取：先按可用高度夹，再按可用宽度夹，始终保比例（不拉伸人物）。
    if (QScreen *screen = QGuiApplication::primaryScreen())
    {
        const QRect available = screen->availableGeometry();
        const int maxHeight =
            std::max(1, static_cast<int>(available.height() * kMaxScreenHeightRatio));
        const int maxWidth =
            std::max(1, static_cast<int>(available.width() * kMaxScreenHeightRatio));
        if (canvasHeight > maxHeight)
        {
            canvasHeight = maxHeight;
            canvasWidth = clampInt(
                static_cast<int>(std::lround(canvasHeight * m_figureAspect)),
                static_cast<int>(kMinCanvasSide), maxWidth);
        }
        if (canvasWidth > maxWidth)
        {
            canvasWidth = maxWidth;
            canvasHeight = clampInt(
                static_cast<int>(std::lround(canvasWidth / m_figureAspect)),
                static_cast<int>(kMinCanvasSide), maxHeight);
        }
    }

    const QSize canvasSize(canvasWidth, canvasHeight);
    resize(canvasSize);
    m_logicalCanvasSize = canvasSize;

    if (!renderAndRegisterFrame())
    {
        //渲染失败不覆盖已经登记的好帧，窗口保持上一帧的样子。
        qWarning() << "Live2D 首帧渲染失败，画布:" << canvasSize;
        m_renderingFrame = false;
        return;
    }
    m_frameIndex = 0;
    refreshInteractiveRegion();
    m_renderingFrame = false;
}

/*探针渲染：按正方形画布渲一帧，用非透明像素包围盒量人物占比与宽高比。

  不能用模型自然尺寸替代：renderFrame 的投影矩阵会把模型铺满目标画布
  （见 Live2DOffscreenRenderer::renderFrame），画布本身不携带"模型多宽多高"的信息，
  而模型画布宽高只有渲染器内部知道 —— 所以只能实测，不为渲染器新增 API。*/
void Live2DCharacterWindow::probeFigureMetrics()
{
    const QSize probeSize(kProbeCanvasHeight, kProbeCanvasHeight);
    const QImage probe = m_renderer.renderFrame(probeSize);
    if (probe.isNull())
    {
        qWarning() << "Live2D 探针渲染失败，退回默认尺寸";
        m_figureAspect = kFallbackFigureAspect;
        m_figureHeightRatio = kFallbackHeightRatio;
        return;
    }

    //统一成 Format_RGBA8888 后逐行读，比逐像素 pixelColor() 快一个量级。
    const QImage rgba = probe.convertToFormat(QImage::Format_RGBA8888);

    int minX = rgba.width();
    int minY = rgba.height();
    int maxX = -1;
    int maxY = -1;
    qint64 opaqueCount = 0;
    for (int y = 0; y < rgba.height(); y += kProbeSampleStride)
    {
        const uchar *line = rgba.constScanLine(y);
        for (int x = 0; x < rgba.width(); x += kProbeSampleStride)
        {
            if (line[x * 4 + 3] <= 32)
                continue;
            ++opaqueCount;
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
        }
    }

    const qint64 probedPixels =
        static_cast<qint64>(rgba.width() / kProbeSampleStride) *
        static_cast<qint64>(rgba.height() / kProbeSampleStride);
    if (maxX < minX || maxY < minY ||
        opaqueCount < static_cast<qint64>(probedPixels * kMinProbeCoverage))
    {
        qWarning() << "Live2D 探针几乎没画出内容，退回默认尺寸";
        m_figureAspect = kFallbackFigureAspect;
        m_figureHeightRatio = kFallbackHeightRatio;
        return;
    }

    const int figureWidth = maxX - minX + 1;
    const int figureHeight = maxY - minY + 1;
    m_figureAspect = static_cast<double>(figureWidth) / static_cast<double>(figureHeight);
    //采样步长会引入 ±stride 的误差，占比夹一下防止极端值把画布撑爆
    m_figureHeightRatio =
        clampDouble(static_cast<double>(figureHeight) / rgba.height(), 0.05, 1.0);
    qInfo() << "Live2D 探针: 人物" << figureWidth << "x" << figureHeight
            << " 宽高比" << m_figureAspect << " 高度占比" << m_figureHeightRatio;
}

/*每帧：渲染 → 登记；交互区只在低频节拍上重算（QBitmap 构造成本太高，不能进帧热路径）*/
void Live2DCharacterWindow::onFrameTick()
{
    //重入保护：240fps 下如果某帧偶发变慢（首帧重建 FBO、抢占），
    //事件循环可能积压 timeout，直接丢掉这一拍比排队重渲更稳。
    if (m_renderingFrame)
        return;

    if (!renderAndRegisterFrame())
        return;

    if (++m_frameIndex >= kRegionRefreshInterval)
    {
        m_frameIndex = 0;
        refreshInteractiveRegion();
    }

    // 请求重绘：帧循环是唯一会周期触发绘制的地方。
    // renderFrameNow() 故意不带 update()，这样「量全链路成本」的循环不会被绘制刷屏拖慢，
    // 调用方需要立刻上屏时自己 update()。
    update();
}

/*渲染一帧并登记 alpha 图。失败（空图）返回 false，调用方据此跳过本帧更新。

  分辨率与屏幕尺寸的分离（关键）：
  - 逻辑尺寸 = size()（窗口尺寸），决定桌宠在屏幕上的实际大小；
  - 渲染分辨率 = 逻辑尺寸 × 设备像素比 × character/live2dScale，长边夹在 kMaxRenderSide 内；
  - 帧的 devicePixelRatio 取**窗口自己的 dpr**（不是 dpr×scale）：这样 Qt 画的时候
    逻辑矩形就是窗口 rect()，多出来的像素由平滑下采样消化，既拿到超采样清晰度，
    又让基类的命中判定（逻辑坐标 ÷ 该 dpr）天然对得上。
  读回成本随面积增长（dpr 1.25 × scale 1.5 ≈ 1.9 倍像素），这正是全链路帧成本要量化的取舍。*/
bool Live2DCharacterWindow::renderAndRegisterFrame()
{
    if (!m_renderer.isLoaded())
        return false;

    const QSize logicalSize = size();
    if (logicalSize.isEmpty())
        return false;

    const qreal windowDpr = devicePixelRatioF();
    QSize physicalSize(
        static_cast<int>(std::lround(logicalSize.width() * windowDpr * m_renderScale)),
        static_cast<int>(std::lround(logicalSize.height() * windowDpr * m_renderScale)));

    const int longSide = std::max(physicalSize.width(), physicalSize.height());
    if (longSide > kMaxRenderSide)
    {
        const double shrink = static_cast<double>(kMaxRenderSide) / longSide;
        physicalSize.setWidth(
            std::max(1, static_cast<int>(std::lround(physicalSize.width() * shrink))));
        physicalSize.setHeight(
            std::max(1, static_cast<int>(std::lround(physicalSize.height() * shrink))));
    }

    m_renderingFrame = true;
    QImage frame = m_renderer.renderFrame(physicalSize);
    m_renderingFrame = false;

    if (frame.isNull())
        return false; //渲染失败就跳过，不覆盖上一张好帧（防闪烁）

    // 关键：交给 Qt 的比例是**窗口 dpr**，超采样的那部分靠缩放消化
    frame.setDevicePixelRatio(windowDpr > 0.0 ? windowDpr : 1.0);

    //帧覆盖整个窗口，偏移为 0。
    updateRenderedImage(frame, QPoint(0, 0));
    return true;
}

QSize Live2DCharacterWindow::renderSize() const
{
    return m_scaledImg.isNull() ? QSize() : m_scaledImg.size();
}

/*公开的「立刻走一遍完整帧管线」：配置变更后强制重绘，也让测试能量真实全链路成本*/
bool Live2DCharacterWindow::renderFrameNow()
{
    if (!renderAndRegisterFrame())
        return false;

    if (++m_frameIndex >= kRegionRefreshInterval)
    {
        m_frameIndex = 0;
        refreshInteractiveRegion();
    }
    return true;
}

/*重算交互区。Windows 与 Tachie 保持同一行为：不裁剪窗口形状，只清 mask ——
  硬裁半透明边缘会露出"略微缩小/边缘异常"，穿透/可点击交给 alpha 命中判定
  （本类的 acceptsClickAt / mousePressEvent）。
  注意纯 Windows 构建里基类的 ApplyInteractiveRegion* 根本没有定义，所以这里必须同条件编译。*/
void Live2DCharacterWindow::refreshInteractiveRegion()
{
#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
    ApplyInteractiveRegionFromImage();
#else
    clearMask();
#endif
}

/*把登记好的帧画到窗口上。

  画法：整帧缩放进窗口 rect()，源矩形给满。帧的 devicePixelRatio 存的是**窗口 dpr**
  （见 renderAndRegisterFrame），于是设备像素上是精确 1:1；m_renderScale>1 时多出来的
  像素由 Qt 平滑下采样，清晰度增益照样落在屏幕上，而桌宠的屏幕尺寸不变。*/
void Live2DCharacterWindow::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    if (m_scaledImg.isNull())
        return; //还没有帧：只画背景（透明）

    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(rect(), m_scaledImg,
                      QRectF(0, 0, m_scaledImg.width(), m_scaledImg.height()));

    // 画布尺寸与窗口不一致时按当前 dpr 重排重渲，然后交给下一帧绘制。
    // 为什么需要：窗口未映射到屏幕前 devicePixelRatioF() 给不出真实值（本机 1.0 vs 实际 1.25），
    // 首帧画布是按错 dpr 定的；映射后尺寸/分辨率都不会自己变好，必须在真实的 dpr 下再排一次。
    // （顺带也覆盖了用户改窗口尺寸、跨屏 dpr 变化等情况。）
    if (!m_renderingFrame && !m_logicalCanvasSize.isEmpty() && m_logicalCanvasSize != size())
    {
        qInfo() << "Live2D 画布" << m_logicalCanvasSize << "与窗口" << size()
                << "不一致，按 dpr" << devicePixelRatioF() << "重排";
        relayoutContent();
    }
}

/*当前内容的**逻辑**尺寸（= 窗口尺寸）。

  不能实时用 m_scaledImg.size()/dpr 推：窗口映射到屏幕后 devicePixelRatioF() 会变
  （本例 1.0 → 1.25），同一个 QImage 用新 dpr 去除会算出另一个逻辑尺寸，
  与 size() 打架。所以直接返回布局时定下的画布尺寸。*/
QSize Live2DCharacterWindow::contentSize() const
{
    return m_logicalCanvasSize;
}

/*命中判定：把逻辑坐标按**实际绘制映射**折回帧像素再查 alpha。

  paintEvent 是把整帧缩放进 rect()（在设备像素上是 rect() × dpr），所以
  帧像素 ↔ 逻辑坐标的比例是 renderSize() / rect()，而不是图像自带的 devicePixelRatio。
  两处必须始终一致，否则（尤其在 live2dScale > 1 时）点击位置会整体偏移。*/
bool Live2DCharacterWindow::acceptsClickAt(const QPoint &logicalPoint) const
{
    if (m_scaledImg.isNull() || width() <= 0 || height() <= 0)
        return false;

    const int frameX = static_cast<int>(
        std::lround(static_cast<double>(logicalPoint.x()) * m_scaledImg.width() / width()));
    const int frameY = static_cast<int>(
        std::lround(static_cast<double>(logicalPoint.y()) * m_scaledImg.height() / height()));
    if (frameX < 0 || frameY < 0 || frameX >= m_scaledImg.width() ||
        frameY >= m_scaledImg.height())
    {
        return false;
    }
    return m_scaledImg.pixelColor(frameX, frameY).alpha() >= 10;
}

void Live2DCharacterWindow::mousePressEvent(QMouseEvent *event)
{
    if (!acceptsClickAt(event->pos()))
    {
        event->ignore(); //透明区域：穿透
        return;
    }

    /*命中就按基类的"按下"语义放行：拖动期间临时把输入区放大到整窗，
      再交给 QWidget 处理（DragHelper 的 eventFilter 会借这次按下启动拖拽）。
      **不能再调基类的 mousePressEvent**：它用的是 QImage::devicePixelRatio() 那套映射，
      在本类（整帧缩放进 rect()）下会算出另一个像素、判成透明而 ignore。*/
#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
    ApplyInteractiveRegionFullWindow();
#endif
    QWidget::mousePressEvent(event);
}

void Live2DCharacterWindow::showEvent(QShowEvent *event)
{
    CharacterWindowBase::showEvent(event);
    if (!m_renderer.isLoaded())
        return;

    // 窗口已映射：此刻 devicePixelRatioF() 才是真实值，首次布局在这里做。
    if (!m_pendingModelName.isEmpty())
    {
        m_pendingModelName.clear();
        relayoutContent();
    }
    else if (!m_logicalCanvasSize.isEmpty() && m_logicalCanvasSize != size())
    {
        // 跨屏/DPI 变化导致窗口尺寸变了，按新 dpr 重排
        relayoutContent();
    }

    if (m_frameTimer)
    {
        m_frameIndex = 0;
        m_frameTimer->start();
    }
}

void Live2DCharacterWindow::hideEvent(QHideEvent *event)
{
    //隐藏时停帧循环：否则每帧数毫秒的渲染会白白烧 CPU/GPU。
    if (m_frameTimer)
        m_frameTimer->stop();
    CharacterWindowBase::hideEvent(event);
}
