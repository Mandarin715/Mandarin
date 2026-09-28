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
#include <QThread>
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
/*画布尺寸下限/上限：防止极端百分比或异常探针算出 1px 或巨大的窗口*/
constexpr int kMinCanvasSide = 32;
constexpr int kMaxCanvasSide = 8192;

/*本类要读的 ini 路径。

  为什么不是直接用 IniSettingPath：**测试绝不允许改到用户的真实配置**。
  以前测试直接往 IniSettingPath 写 live2dFps/live2dScale 再"恢复"，一旦中途 QSKIP
  或崩溃就永久覆盖用户的值（实测已发生过，用户被改成 60/1）。所以配置读取支持一个
  环境变量重定向：测试把它指到 QTemporaryDir 里的临时文件，真实配置全程只读。*/
QString settingsPath()
{
    const QByteArray override = qgetenv("MANDARIN_CONFIG_INI");
    return override.isEmpty() ? IniSettingPath : QString::fromLocal8Bit(override);
}

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
    QSettings settings(settingsPath(), QSettings::IniFormat);
    m_renderScale =
        clampDouble(settings.value("character/live2dScale", 1.0).toDouble(),
                    kMinRenderScale, kMaxRenderScale);
    qInfo() << "Live2D 渲染缩放:" << m_renderScale;
}

/*在目录里挑出要用的 model3.json 文件名。

  为什么不能假定"<模型名>.model3.json"：实机模型常常对不上 —— 用户新加的
  `Live2D/atri/` 目录里入口文件叫 `atri_8.model3.json`。模型文件受授权保护、不能改名，
  所以查找必须宽松：

    1) 先看 `<模型名>.model3.json`（miku / 樱花miku 都是这种正命名，优先命中）；
    2) 没有就扫目录里的 `*.model3.json`：
       - 只有一个 → 直接用；
       - 有多个 → 优先"文件名以模型名开头"的那个，否则取第一个并警告列出候选；
       - 一个都没有 → 返回空（调用方沿用"找不到模型"的既有行为，回退 PNG 立绘）。

  返回空字符串表示该目录里没有可用的 model3.json。*/
QString Live2DCharacterWindow::resolveModelJsonName(const QString &modelDir,
                                                    const QString &modelName)
{
    if (modelDir.isEmpty() || modelName.isEmpty())
        return QString();

    const QDir dir(modelDir);
    const QString exactName = modelName + QStringLiteral(".model3.json");
    if (QFileInfo::exists(dir.filePath(exactName)))
        return exactName;

    const QStringList found = dir.entryList({QStringLiteral("*.model3.json")}, QDir::Files);
    if (found.isEmpty())
        return QString();
    if (found.size() == 1)
        return found.first();

    // 多个候选：优先文件名以模型名开头的那个（atri → atri_8.model3.json）
    for (const QString &fileName : found)
    {
        if (fileName.startsWith(modelName, Qt::CaseInsensitive))
        {
            qInfo() << "Live2D 模型目录里有多个 model3.json，按前缀选中:" << fileName
                    << "候选:" << found;
            return fileName;
        }
    }

    qWarning() << "Live2D 模型目录里有多个 model3.json 且没有一个以模型名开头，取第一个:"
               << found.first() << "候选:" << found;
    return found.first();
}

/*按「全局模型根目录 → 当前角色资源目录」的顺序找模型，取第一个含可用 model3.json 的目录*/
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

    for (const QString &dir : candidates)
    {
        if (!resolveModelJsonName(dir, trimmed).isEmpty())
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

    //入口文件名以目录里的实际内容为准（可能不叫 <模型名>.model3.json）
    const QString modelJsonName = resolveModelJsonName(dir, modelName);
    if (modelJsonName.isEmpty())
    {
        qWarning() << "Live2D 目录里没有 model3.json:" << dir;
        return;
    }

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

/*画布尺寸启发式（v2）：

  1) 先按**正方形**探针渲一帧，由渲染器把人物包围盒换算回模型画布像素，得到人物宽高比；
  2) 逻辑人物高 = 基准高 900 × (m_tachieSizePercent/100)；
  3) 画布高 = 人物高 / kTargetFigureHeightRatio（人物只占画布高的 88%，上下各留 6%）；
     画布宽 = 画布高 × 人物宽高比 —— 画布与人物包围盒同宽高比，两条边同时留出余量；
  4) 逻辑尺寸再夹进屏幕可用高度/宽度的 85%（2560x1440@125% 只有 2048x1152 逻辑像素，
     不夹的话大立绘会被屏幕裁掉），夹取时**始终保比例**。

  为什么 v1（画布 = 人物包围盒 / 占比、零余量）会裁掉人物：
  画布宽 = 画布高 × 人物宽高比 且 画布高 = 人物高 / 人物高占比，两式合起来等价于
  "画布恰好等于人物包围盒"，人物四边全部贴边；再加上模型画布里人物本身是偏的，
  右边自然先被切掉。现在多了 12% 的余量，且渲染器会把人物**搬回画布中心**再做等比缩放，
  偏置不再体现为某一侧被切。

  注意：character/live2dScale **只放大渲染分辨率**，不改变这里的逻辑尺寸
  （逻辑尺寸直接决定屏幕上的大小，见 renderAndRegisterFrame）。*/
void Live2DCharacterWindow::relayoutContent()
{
    if (!m_renderer.isLoaded())
        return;

    // 重排期间置位，避免 paintEvent 在自检「画布 vs 窗口」时递归进来
    m_renderingFrame = true;

    // 探针必须在定尺寸之前跑：宽高比与显示比例都来自它
    probeFigureMetrics();

    const int targetFigureHeight =
        std::max(1, static_cast<int>(std::lround(kBaseCanvasHeight *
                                                (m_tachieSizePercent / 100.0))));
    int canvasHeight = clampInt(
        static_cast<int>(std::lround(targetFigureHeight /
                                     std::max(1e-3, kTargetFigureHeightRatio))),
        static_cast<int>(kMinCanvasSide), static_cast<int>(kMaxCanvasSide));
    int canvasWidth = clampInt(
        static_cast<int>(std::lround(canvasHeight * m_figureAspect)),
        static_cast<int>(kMinCanvasSide), static_cast<int>(kMaxCanvasSide));

    /*屏幕夹取：先按可用高度夹，再按可用宽度夹，**始终保比例**（不拉伸人物）。
      夹取后按同一个比例回算另一条边 —— 画布宽高比不能被破坏，否则渲染器那套
      "宽度与高度各算一次缩放取较大者"就会退化成单边受限，人物在另一边贴边。*/
    if (QScreen *screen = QGuiApplication::primaryScreen())
    {
        const QRect available = screen->availableGeometry();
        const int maxHeight =
            std::max(1, static_cast<int>(available.height() * kMaxScreenHeightRatio));
        const int maxWidth =
            std::max(1, static_cast<int>(available.width() * kMaxScreenHeightRatio));
        if (canvasHeight > maxHeight)
        {
            const double shrink = static_cast<double>(maxHeight) / canvasHeight;
            canvasHeight = maxHeight;
            canvasWidth = clampInt(
                static_cast<int>(std::lround(canvasWidth * shrink)),
                static_cast<int>(kMinCanvasSide), maxWidth);
        }
        if (canvasWidth > maxWidth)
        {
            const double shrink = static_cast<double>(maxWidth) / canvasWidth;
            canvasWidth = maxWidth;
            canvasHeight = clampInt(
                static_cast<int>(std::lround(canvasHeight * shrink)),
                static_cast<int>(kMinCanvasSide), maxHeight);
        }
    }

    const QSize canvasSize(canvasWidth, canvasHeight);

    /*显示比例：先按探针的解析换算给一个初值，渲染后再用**真实帧**实测校正。

      为什么还要实测校正：解析式要同时依赖"探针空间跨度"和"投影分支的 W/H 换算"，
      任何一处偏差都会让人物偏大或偏小。实测校正不依赖这些假设 —— 第一帧量出人物
      实际占画布多少，就能精确算出还需要放大/缩小多少倍。*/
    const double stretch = static_cast<double>(std::max(1, canvasHeight)) /
                           static_cast<double>(std::max(1, canvasWidth));
    m_displayRatio = clampDouble(kTargetFigureHeightRatio /
                                     std::max(1e-6, m_figureSpanY / stretch),
                                 0.05, 1.0);
    m_renderer.setDisplayHeightRatio(static_cast<float>(m_displayRatio));

    resize(canvasSize);
    m_logicalCanvasSize = canvasSize;

    /*余量自检：把「画布只比人物大一点点」这种本该被测试抓到的情况直接打到日志里，
      省得只有测试失败时才知道裁了。真值来自渲染帧（见 probeFigureMetrics 的探针）。*/
    const double marginRatio =
        std::max(0.0, 1.0 - kTargetFigureHeightRatio) / 2.0;
    qInfo() << "Live2D 画布:" << canvasSize << " 目标人物高" << targetFigureHeight
            << " 人物占比" << kTargetFigureHeightRatio << " 每侧余量约"
            << static_cast<int>(std::lround(marginRatio * 100)) << "% | 人物宽高比"
            << m_figureAspect << " 显示比例" << m_displayRatio;

    if (!renderAndRegisterFrame())
    {
        //渲染失败不覆盖已经登记的好帧，窗口保持上一帧的样子。
        qWarning() << "Live2D 首帧渲染失败，画布:" << canvasSize;
        m_renderingFrame = false;
        return;
    }

    /*实测校正：量出人物在这张真实帧里占画布多少，把显示比例精确修正到目标占比。

      为什么要有这一步：解析换算要同时押中"探针空间跨度"与"投影分支的 W/H 换算"，
      任何一处偏差都会让人物偏大偏小（实测解析值只能做到 50% 而非目标 84%）。
      缩放因子与占比成正比，所以量一次就能一步到位，不需要反复试探。
      只做一轮：第二轮与第一轮同帧同尺寸，收敛是确定的；多轮只会白白多渲染一帧。*/
    {
        QRect figureBounds;
        const QImage frame = m_scaledImg;
        if (!frame.isNull() && opaqueBoundsInFrame(&figureBounds))
        {
            // 占比用同一张帧的像素算，避免逻辑/物理像素换算又引入一次误差
            const double measuredFraction =
                static_cast<double>(figureBounds.height()) / frame.height();
            if (measuredFraction > 1e-3)
            {
                const double corrected =
                    clampDouble(m_displayRatio * (kTargetFigureHeightRatio / measuredFraction),
                                0.05, 1.0);
                qInfo() << "Live2D 占比校正: 实测占比" << measuredFraction << " 目标"
                        << kTargetFigureHeightRatio << " 显示比例" << m_displayRatio << "→"
                        << corrected;
                m_displayRatio = corrected;
                m_renderer.setDisplayHeightRatio(static_cast<float>(m_displayRatio));
                if (!renderAndRegisterFrame())
                    qWarning() << "Live2D 校正帧渲染失败，沿用上一帧";
            }
        }
    }

    m_frameIndex = 0;
    refreshInteractiveRegion();
    m_renderingFrame = false;
}

/*量当前登记帧（m_scaledImg）里人物可见部分的像素包围盒。
  layout 时用它实测校正显示比例。占比一律用同一张帧的像素算，
  不做逻辑/物理像素换算 —— 那会再引入一次 dpr 取整误差。*/
bool Live2DCharacterWindow::opaqueBoundsInFrame(QRect *bounds) const
{
    if (bounds)
        *bounds = QRect();
    if (m_scaledImg.isNull())
        return false;

    const QImage rgba = m_scaledImg.convertToFormat(QImage::Format_RGBA8888);
    int minX = rgba.width();
    int minY = rgba.height();
    int maxX = -1;
    int maxY = -1;
    for (int y = 0; y < rgba.height(); ++y)
    {
        const uchar *line = rgba.constScanLine(y);
        for (int x = 0; x < rgba.width(); ++x)
        {
            if (line[x * 4 + 3] <= 32)
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

/*探针：量出人物**可见部分**在绘制输出空间里的范围（多次取样求并集），
  再由它推出宽高比与显示比例交给渲染器定位。

  为什么要取样求并集：呼吸/物理会让姿势移动，单帧范围会在动作过程中被超出；
  并集是"这段时间里人物占过的最大范围"，画布按它留余量才稳。

  为什么渲染器现在能量准：模型变换按帧**无状态**重建（不再被 SetWidth 累积放大），
  换算也补上了投影缩放；而且范围保留在**输出空间的浮点**里，不做模型画布像素取整
  （本模型画布是 1x1，取整会把人物范围毁成 1x1）。*/
void Live2DCharacterWindow::probeFigureMetrics()
{
    m_renderer.clearFigureSpan();
    for (int i = 0; i < kProbeSamples; ++i)
    {
        // 取样之间真的让时间过去，呼吸/物理才会走到不同相位（i=0 不睡，省一次等待）
        if (i > 0)
        {
            QThread::msleep(static_cast<unsigned long>(kProbeSampleIntervalMs));
            QCoreApplication::processEvents();
        }
        // 探针固定 1:1：renderFrame 的投影留边会让非正方形探针的换算失真
        const QImage probe = m_renderer.renderFrame(QSize(kProbeCanvasSide, kProbeCanvasSide));
        if (probe.isNull())
        {
            qWarning() << "Live2D 探针渲染失败，退回默认尺寸";
            m_figureAspect = kFallbackFigureAspect;
            m_displayRatio = kFallbackHeightRatio;
            m_renderer.clearFigureSpan();
            return;
        }
        // probeFigureMetrics 会把本次可见范围并进渲染器的 FigureSpan
        (void)m_renderer.probeFigureMetrics(probe);
    }

    /*人物可见范围只在探针里测一次；这里拿到的是**探针（1:1）空间**的跨度。
      渲染器的缩放也在这个空间里算（relayoutContent 会用同一个值定 displayHeightRatio），
      所以这里直接用原始 span，不再做 W/H 换算 —— 换算统一放在 relayoutContent。*/
    const Live2DOffscreenRenderer::FigureMetrics raw = m_renderer.figureMetrics(
        kProbeCanvasSide, kProbeCanvasSide);
    if (!raw.valid)
    {
        qWarning() << "Live2D 探针几乎没画出内容，退回默认尺寸";
        m_figureAspect = kFallbackFigureAspect;
        m_figureSpanY = kFallbackHeightRatio;
        m_displayRatio = kFallbackHeightRatio;
        m_renderer.clearFigureSpan();
        return;
    }

    /*画布宽高比：用"换算到目标比例后"的可见范围宽高比。
      先用探针比例自身做一次换算（此时 stretch=1 的等效情形），得到目标比例下的比例。*/
    const int probeW = std::max(1, static_cast<int>(std::lround(kBaseCanvasHeight * 0.6)));
    const Live2DOffscreenRenderer::FigureMetrics corrected =
        m_renderer.figureMetrics(probeW, kBaseCanvasHeight);
    m_figureAspect = clampDouble(corrected.valid ? corrected.boundsAspect : raw.boundsAspect,
                                 0.05, 20.0);
    m_figureSpanY = std::max(1e-6, static_cast<double>(raw.spanY));

    qInfo() << "Live2D 探针:" << kProbeSamples << "次取样，人物探针空间跨度" << raw.spanX << "x"
            << raw.spanY << " 探针比例" << raw.boundsAspect << " 目标比例" << m_figureAspect;
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
