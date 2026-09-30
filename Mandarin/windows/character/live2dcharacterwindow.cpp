#include "live2dcharacterwindow.h"

#include "../../GlobalConstants.h"

#include "../../utils/DevicePixelAlign.h"

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
    applyMoodBlendFromConfig();

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

/*帧率：character/live2dFps，默认 60，夹取 [5,240]
  经 settingsPath() 读取（不是 IniSettingPath）：测试要把配置重定向到临时文件，
  漏掉这一处会让"测试写进临时文件的帧率"被静默忽略、实际读的是用户真实配置 ——
  那样跟帧率有关的断言就是在依赖用户本机的值。*/
void Live2DCharacterWindow::applyFrameRateFromConfig()
{
    QSettings settings(settingsPath(), QSettings::IniFormat);
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

void Live2DCharacterWindow::applyMoodBlendFromConfig()
{
    /*心情过渡时长：character/live2dMoodBlendMs，默认 200ms，夹取 [50,3000]。
       为什么要夹（两条都是真实会发生的误配）：
         - 0/负数 → 过渡被关掉，参数一帧跳过去，用户看到的还是"换心情顿一下"（本阶段要修的症状）；
         - 极大值（例如手抖多打几个 0）→ 表情永远停在半路，AI 说的心情迟迟落不到脸上。
       非数字（ok==false）与缺键一样回默认值 —— 与 fps/scale 的既有做法保持一致。*/
    QSettings settings(settingsPath(), QSettings::IniFormat);
    bool ok = false;
    const int raw = settings.value("character/live2dMoodBlendMs", kDefaultMoodBlendMs)
                        .toString()
                        .toInt(&ok);
    m_moodBlendDurationMs =
        clampInt(ok ? raw : kDefaultMoodBlendMs, kMinMoodBlendMs, kMaxMoodBlendMs);
    /*转发给渲染器：过渡是在渲染器的帧循环里按帧时间做的（见
       Live2DOffscreenRenderer::setMoodBlendDurationMs）—— 窗口层不做插值，
       它只负责"把用户配的数安全地交下去"。未装载模型时这次调用会安全地落空，
       装载后 loadModel() 再调一次补上。*/
    m_renderer.setMoodBlendDurationMs(m_moodBlendDurationMs);
    qInfo() << "Live2D 心情过渡时长:" << m_moodBlendDurationMs << "ms（配置原值:" << raw
            << "合法:" << ok << "）";
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

/*装载模型：模型名 → 目录 → 入口文件 → 渲染器。

  为什么与「应用心情」分成两个入口（本来的 bug 就在这）：`reloadContent(心情名)` 曾经被当成
  模型名，AI 每句话发来的心情（`高兴`/`睡觉`…）在模型根目录与角色目录下都找不到同名模型，
  于是这里 `dir.isEmpty()` 直接 return —— **心情变化被静默吃掉**，立绘永远一副表情。
  现在 `reloadContent` 只负责换情绪预设（见下），模型名的解析只走这个函数。*/
bool Live2DCharacterWindow::loadModel(const QString &modelName)
{
    const QString name = modelName.trimmed();
    const QString dir = resolveModelDir(name);
    if (dir.isEmpty())
        return false;

    applyRenderScaleFromConfig();
    /*过渡时长也要在这里补一次：构造时渲染器还没装载模型，setMoodBlendDurationMs 会落空
       （见 applyMoodBlendFromConfig 的说明）。装载成功后必须让用户配的值真的生效。*/
    applyMoodBlendFromConfig();

    //入口文件名以目录里的实际内容为准（可能不叫 <模型名>.model3.json）
    const QString modelJsonName = resolveModelJsonName(dir, name);
    if (modelJsonName.isEmpty())
    {
        qWarning() << "Live2D 目录里没有 model3.json:" << dir;
        return false;
    }

    QString error;
    if (!m_renderer.load(dir, modelJsonName, &error))
    {
        qWarning() << "Live2D 模型装载失败:" << dir << error;
        //装载失败时上一版画布还留着，直接沿用（不置空，避免闪烁成空白窗口）。
        return false;
    }

    m_modelDir = dir;
    m_modelJsonName = modelJsonName;
    m_modelLoaded = true;
    m_frameIndex = 0;
    m_renderingFrame = false;
    /*人物几何的缓存必须在这里失效：渲染器刚重建了模型实例，旧模型量到的可见范围
       对新模型没有意义。（这是**唯一**的失效点 —— 换立绘大小不重探，见
       probeFigureMetrics 的缓存说明。）
       自检重排的"同一组合只试一次"记录也一起清掉：换模型是一次全新的事件。*/
    m_figureMetricsValid = false;
    m_figureMetricsReuseLogged = false;
    m_relayoutAttempted = false;
    m_relayoutGaveUpLogged = false;
    qInfo() << "Live2D 模型已装载:" << modelJsonName << "于" << dir;

    /*情绪预设：路径要靠模型名才推得出来（…/Live2D/<模型名>/presets/moods.json），
      所以只能在这里装载。失败时功能整个自关 —— 不施加任何覆盖、也不残留上一种情绪。*/
    m_moodPreset.load(name);

    /*待机摆动数据（…/presets/idle.json）与情绪预设**分开**装载：它是可选的，缺了只是不做
       摆动（角色照样呼吸/眨眼/跟着心情变表情），见 Live2DMoodPreset::loadIdle。
       整组交给渲染器（空表 = 关掉），于是"没配数据"与"配了数据"走同一条路径。
       ⚠️ 放在这一句之后：它依赖上面 load() 解析出来的 parameter-map（语义名 → 参数 ID）。*/
    m_moodPreset.loadIdle();
    m_renderer.setIdleSway(m_moodPreset.idleSwayEntries());

    // 窗口映射到屏幕之前 devicePixelRatioF() 给不出真实值（本机未映射时是 1.0，
    // 实际 1.25），此时用错 dpr 定画布/渲染分辨率会同时错两处（屏幕尺寸与像素密度）。
    // 所以未映射就先只记下模型名，真正的首次布局交给 showEvent —— 那时 dpr 才是真的。
    if (isVisible())
        relayoutContent();
    else
        m_pendingModelName = name;

    /*心情在**布局之后**才施加：画布的宽高比/人物占比是由探针量出来的，探针必须在
      "默认状态"下跑，否则换一次模型得到的画布会随当前心情而变（同一套参数算出两种画布）。
      重排已经出过一帧，这里再出一帧带上情绪，代价是一帧渲染。*/
    applyMood(m_currentMoodName);
    return true;
}

/*换一整组参数覆盖，有画布时立刻出一帧。

  **整组替换**是关键：`setParameter` 只能增改单条，切回 neutral 时上一种情绪的条目仍在表里，
  每帧继续施加 —— 于是"换成中立"看起来什么都没发生（残留情绪的成因）。
  这里交给 setParameterOverrides 一次换掉整张表。

  三条安全边界（都在旧实现上踩过或可能踩到）：
  1. 预设不可用（两份 JSON 缺失/非法）→ 什么都不施加（功能自关），且**清空**覆盖表，
     保证不会留下上一帧的残留情绪；
  2. 还没装载模型 → 只记住心情名，覆盖表照写（渲染器侧会在装载后自然带上），
     但绝不渲染 —— 那时窗口还没有画布，渲出来的帧是错尺寸的；
  3. 窗口还没有画布（未映射/dpr 未定）→ 同样只记覆盖，等首次布局的那一帧自然带上。*/
void Live2DCharacterWindow::applyMood(const QString &moodName)
{
    const QString trimmed = moodName.trimmed();
    m_currentMoodName = trimmed.isEmpty() ? QStringLiteral("default") : trimmed;

    if (!m_moodPreset.isEnabled())
    {
        // 预设不可用 = 情绪功能自关：清掉可能存在的覆盖，别把上一种情绪留在屏幕上
        m_renderer.clearParameterOverrides();
        return;
    }

    m_renderer.setParameterOverrides(m_moodPreset.parametersForMood(m_currentMoodName));

    if (!m_modelLoaded || m_logicalCanvasSize.isEmpty())
        return; //还没有画布：等下一帧/首次布局时自然带上，这里不能渲一个错尺寸的帧

    /*⚠️ 这里**故意不立刻渲染**（曾经是 renderAndRegisterFrame() + update()）。

       为什么要去掉：换心情是跟着 AI 的回复走的，而这条路径跑在**主线程**上，
       一次同步渲染实测约 4ms（240x300 是 2~4ms，真实画布更大）。它插在帧循环的
       两个 8ms 节拍**之间**，于是那一拍的间隔被撑长 —— 用户看到的"顿一下"里
       有一半是这次卡顿。而定时器最迟 8ms 后就会带着新参数渲一帧，
       把工作交给它，延迟的代价小到看不见，却把主线程的那 4ms 还了回去。

       注意与"过渡"的分工：这里只是不再抢帧；表情的变化本身由
       renderer.setParameterOverrides 的逐帧过渡负责（见 applyMoodBlend），
       所以下一拍渲出来的那一帧正好是过渡的第一帧，不会漏掉任何东西。
       交互区同理：下一拍（≤8ms）的 refreshInteractiveRegion 会跟着新剪影更新。*/
    update(); //请求重绘：帧循环到点自己会渲，这里只保证窗口被标记为脏
}

void Live2DCharacterWindow::reloadContent(const QString &contentName)
{
    //基类契约：按内容名切换（PNG 路径按名换图，Live2D 路径按名换情绪）
    applyMood(contentName);
}

/*TTS 播放状态 → 渲染器的说话扑动。

  这里**不**顺手渲染一帧：说话状态是"接下来每一帧都要变"的东西，
  而帧循环最迟 8ms 就会带着它渲下一帧 —— 与 applyMood 去掉同步渲染同一个理由
  （那 4ms 主线程卡顿正是用户感到的"顿"）。*/
void Live2DCharacterWindow::SetSpeaking(bool speaking)
{
    m_renderer.setSpeaking(speaking);
}

/*TTS 响度电平 → 渲染器的包络驱动。

  与 SetSpeaking 一样**不**顺手渲染一帧：电平每 50ms 就有一个新值，
  帧循环最迟 8ms 就会带着它渲下一帧（同步渲染那 4ms 主线程卡顿正是要避免的）。*/
void Live2DCharacterWindow::SetSpeechLevel(float level)
{
    m_renderer.setSpeechLevel(level);
}

float Live2DCharacterWindow::parameterValue(const QString &parameterId) const
{
    return m_renderer.parameterValue(parameterId);
}

float Live2DCharacterWindow::blinkValue() const
{
    return m_renderer.blinkValue();
}

void Live2DCharacterWindow::setEyeOpennessMultiplierForTest(const QHash<QString, float> &values)
{
    m_renderer.setEyeOpennessMultiplier(values);
}

void Live2DCharacterWindow::clearEyeOpennessMultiplierOverride()
{
    m_renderer.clearEyeOpennessMultiplierOverride();
}

/*画布尺寸启发式（v3）：

  1) 在**测量模式**下按正方形探针渲一帧帧取样（变换固定，见 probeFigureMetrics），
     得到人物在一整段待机动作上占过的最大范围，以及它的**真实宽高比**；
  2) 逻辑人物高 = 基准高 900 × (m_tachieSizePercent/100)；
  3) 画布高 = 人物高 / kTargetFigureRatio（人物只占画布高的 84%，上下各留 8%）；
     画布宽 = 画布高 × 人物真实宽高比 —— 画布与人物的形状一致，左右也各留 8%；
  4) 逻辑尺寸再夹进屏幕可用高度/宽度的 85%（2560x1440@125% 只有 2048x1152 逻辑像素，
     不夹的话大立绘会被屏幕裁掉），夹取时**始终保比例**；
  5) 真正落定尺寸靠**实测校正**：量真实帧里人物占画布多少，按比例修正两个方向的目标占比。

  为什么 v2（画布宽 = 画布高 × 探针宽高比 × 画布高/画布宽）会把人物挤扁到 36%：
  那个"再乘画布高/画布宽"的换算本来是想补偿"投影分支的各向异性"，可投影分支早就被
  分轴摆放 Scale 覆盖掉了（CubismMatrix44::Scale 是直接赋值），于是它成了纯粹的误差 ——
  实测把 atri 的宽高比从 0.436 抬到 0.524，画布宽随之定成 779（真实只需 409），
  人物横向只占 36%，两侧各空 250 逻辑像素。

  为什么单靠"改画布宽度"修不好：v2 的渲染器只给一个**等比**摆放因子，人物的像素宽高比
  = 模型宽高比 × 画布宽高比，横向占比被钉死在 `目标占比 × 人物宽高比 / 画布宽高比` ——
  实测画布宽取 400/600/779/938/1200 时横向占比恒为 0.357，改宽度毫无作用。
  所以渲染器改成**横竖分别缩放**（见 setDisplayRatios 的说明），画布宽高比再由人物
  真实宽高比给出，两个方向才能同时到位且不拉伸。

  注意：character/live2dScale **只放大渲染分辨率**，不改变这里的逻辑尺寸
  （逻辑尺寸直接决定屏幕上的大小，见 renderAndRegisterFrame）。*/
void Live2DCharacterWindow::relayoutContent()
{
    if (!m_renderer.isLoaded())
        return;

    // 重排期间置位，避免 paintEvent 在自检「画布 vs 窗口」时递归进来
    m_renderingFrame = true;

    // 探针必须在定尺寸之前跑：宽高比与目标占比都来自它
    probeFigureMetrics();

    const int targetFigureHeight =
        std::max(1, static_cast<int>(std::lround(kBaseCanvasHeight *
                                                (m_tachieSizePercent / 100.0))));
    int canvasHeight = clampInt(
        static_cast<int>(std::lround(targetFigureHeight / kTargetFigureRatio)),
        static_cast<int>(kMinCanvasSide), static_cast<int>(kMaxCanvasSide));
    /*画布宽度 = 画布高度 × 人物**真实**宽高比：画布与人物同形状，四边余量一样宽。
      这是"人物填满画布"与"不拉伸"能同时成立的前提（分轴摆放把像素宽高比变成画布宽高比，
      所以画布宽高比必须等于人物真实宽高比）。*/
    int canvasWidth = clampInt(
        static_cast<int>(std::lround(canvasHeight * m_figureAspect)),
        static_cast<int>(kMinCanvasSide), static_cast<int>(kMaxCanvasSide));

    /*屏幕夹取：先按可用高度夹，再按可用宽度夹，**始终保比例**（不拉伸人物）。
      夹取后按同一个比例回算另一条边 —— 画布宽高比不能被破坏，否则人物会被横向或纵向
      拉伸（分轴摆放的像素宽高比就等于画布宽高比）。*/
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

    /*设备像素对齐（"上屏发糊"的根因修复，别删）：

      paintEvent 这样铺满窗口：drawImage(rect(), m_scaledImg, 满源矩形)。**源矩形给的是图像
      像素，目标矩形是逻辑坐标**，所以 Qt 是把 img 的像素网格铺到窗口的**设备**像素网格
      （rect() × dpr）上 —— 只有 img 的像素尺寸恰好等于 rect() × dpr 才是 1:1 拷贝。
      登记帧尺寸是 lround(canvas × dpr)，窗口设备矩形是 canvas × dpr，于是 canvas × dpr
      不是整数时两者必然差着零点几个像素：本机 atri 画布 400x938、dpr 1.25 ⇒ 登记帧 1173 行、
      窗口设备矩形 1172.5 行，1173 行被压进 1172.5 行（纵向比例 0.99957），逐行相位从 0 漂到 0.5。

      实测代价：登记帧 vs grab() 有 17.31% 的像素不同、清晰度（4 邻域 Laplacian 方差）−11.9%。
      纯 Qt 对照实验（test_devicepixelblit）把因果钉死：同样的绘制代码、同样的 dpr，
      **像素尺寸对不上** ⇒ 99.6% 像素不同、方差 −49%；**对得上** ⇒ 逐位相同、方差 +0.000%。
      回归断言：test_live2dwindow::paintsRegisteredFrameWithoutResampling。

      步长按 dpr 推（1.25→4、1.5→2、1.75→4、2.0→1、1.0→1），不写死 4。
      宽度偏差直接等于人物被拉伸的比例，已由 utils/DevicePixelAlign.h 压到 <0.3%，实际数字见下面的日志。*/
    const int alignStep = devicePixelAlignStep(devicePixelRatioF());
    const QSize idealCanvas(canvasWidth, canvasHeight);
    const QSize canvasSize = devicePixelAlignedCanvasSize(
        idealCanvas, m_figureAspect, alignStep, idealCanvas, kMinCanvasSide, kMaxCanvasSide);

    /*目标占比：横竖同一个值。分轴摆放让"目标占比"变成精确值（fit = 2*占比/跨度，
      而跨度来自同一把尺子的探针），所以这里不再需要解析式给初值 ——
      下面的实测校正只是兜底与验证。*/
    m_displayRatioX = kTargetFigureRatio;
    m_displayRatioY = kTargetFigureRatio;
    m_renderer.setDisplayRatios(static_cast<float>(m_displayRatioX),
                                static_cast<float>(m_displayRatioY));

    resize(canvasSize);
    m_logicalCanvasSize = canvasSize;
    // 记下这次布局用的 dpr：画布尺寸是"按这个 dpr 对齐"定的，dpr 一变对齐就失效
    m_layoutDpr = devicePixelRatioF();

    qInfo() << "Live2D 画布:" << canvasSize << " 目标人物高" << targetFigureHeight
            << " 目标占比" << kTargetFigureRatio << " | 设备像素对齐步长" << alignStep
            << "（未对齐的理想尺寸" << idealCanvas << "，宽高比偏差"
            << (static_cast<double>(canvasSize.width()) / canvasSize.height() - m_figureAspect) /
                   std::max(1e-6, m_figureAspect) * 100.0
            << "%）"
            << " | 人物真实宽高比" << m_figureAspect << " 探针跨度" << m_figureSpanX << "x"
            << m_figureSpanY;

    if (!renderAndRegisterFrame())
    {
        //渲染失败不覆盖已经登记的好帧，窗口保持上一帧的样子。
        qWarning() << "Live2D 首帧渲染失败，画布:" << canvasSize;
        m_renderingFrame = false;
        return;
    }

    /*实测校正：量出人物在这张真实帧里占画布多少，把两个方向的目标占比精确修正到目标值。

      为什么还要实测：解析式要押中"探针跨度"与"摆放缩放"是同一把尺子。分轴之后单帧几何
      已经很确定，但真实帧的姿势与探针并集之间仍可能有几像素出入，实测不依赖任何假设。
      为什么一次就能收敛：横向 fitX 与纵向 fitY 在渲染器里是**解耦**的（fitX 只影响 x），
      缩放因子与占比成正比，所以按比例修正一次就是精确解。
      为什么最多 kMaxCorrectionRounds 轮：再多没有意义 —— 若两轮后仍不收敛，说明出现了
      本轮修复没有设想的耦合（例如模型自身在换尺寸时改变姿势），此时**保留当前画布**并打
      警告：人物会略小于目标，但画布尺寸在进入校正前就已定下，绝不会因此越界或拉伸。*/
    double measuredX = 0.0;
    double measuredY = 0.0;
    for (int round = 0; round <= kMaxCorrectionRounds; ++round)
    {
        if (!measureFigureOccupancy(&measuredX, &measuredY))
        {
            qWarning() << "Live2D 占比校正：量不到人物，跳过（沿用按探针跨度排好的画布）";
            break;
        }

        const bool converged =
            std::abs(measuredX - kTargetFigureRatio) <= kCorrectionTolerance &&
            std::abs(measuredY - kTargetFigureRatio) <= kCorrectionTolerance;
        qInfo() << "Live2D 占比校正: 第" << round << "轮（0 = 校正前）实测 横向" << measuredX
                << "纵向" << measuredY << " 目标" << kTargetFigureRatio
                << " 容差" << kCorrectionTolerance;
        if (converged)
            break;

        if (round == kMaxCorrectionRounds)
        {
            qWarning() << "Live2D 占比校正未收敛：" << kMaxCorrectionRounds << "轮后实测 横向"
                       << measuredX << "纵向" << measuredY << "（目标" << kTargetFigureRatio
                       << "）。保留当前画布：人物可能比目标略小，但不会被拉伸、也不会越界。";
            break;
        }

        m_displayRatioX = clampDouble(
            m_displayRatioX * kTargetFigureRatio / std::max(1e-6, measuredX), kMinDisplayRatio,
            kMaxDisplayRatio);
        m_displayRatioY = clampDouble(
            m_displayRatioY * kTargetFigureRatio / std::max(1e-6, measuredY), kMinDisplayRatio,
            kMaxDisplayRatio);
        m_renderer.setDisplayRatios(static_cast<float>(m_displayRatioX),
                                    static_cast<float>(m_displayRatioY));
        if (!renderAndRegisterFrame())
        {
            qWarning() << "Live2D 校正帧渲染失败，沿用上一帧";
            break;
        }
    }

    m_frameIndex = 0;
    refreshInteractiveRegion();
    m_renderingFrame = false;
}

/*量"人物在这张画布上实际占多少"：在 kMeasuredFramePasses 个真实帧上求 alpha 包围盒的
  并集，再各自除以画布宽/高，得到横向与纵向的占比（0~1）。

  为什么用并集：姿势会动（待机/呼吸/物理），单帧的包围盒可能恰好偏松，按它校正会把人物
  放得比"整段动作都装得下"更大，动作一摆就贴到画布边。
  为什么占比一律用同一张帧的像素算：逻辑/物理像素换算会再引入一次 dpr 取整误差。*/
bool Live2DCharacterWindow::measureFigureOccupancy(double *fractionX, double *fractionY)
{
    QRect unionBounds;
    for (int pass = 0; pass < kMeasuredFramePasses; ++pass)
    {
        /*真的让时间过去一小段，姿势才会走到不同相位 —— 但走的是**虚拟时间**。
           与被删掉的 msleep(120) 等价：渲染器的步长上限是 100ms，睡 120ms 喂进去的
           也是 100ms 的步长；这里传同一个 120ms 走同一条夹取路径。
           顺带也去掉了这里的 processEvents()：重排中途让出主线程只会让 paintEvent
           有机会在"画布还没定完"的窗口期进来（见 renderAndRegisterFrame 的重入保护）。*/
        if (pass > 0)
            m_renderer.setNextFrameDeltaSeconds(kMeasuredFrameIntervalMs / 1000.0f);

        if (!renderAndRegisterFrame() || m_scaledImg.isNull())
            return false;

        QRect bounds;
        if (!opaqueBoundsInFrame(&bounds))
            return false;
        unionBounds = unionBounds.isNull() ? bounds : unionBounds.united(bounds);
    }

    if (unionBounds.isNull() || m_scaledImg.isNull() || m_scaledImg.width() <= 0 ||
        m_scaledImg.height() <= 0)
    {
        return false;
    }

    if (fractionX)
        *fractionX = static_cast<double>(unionBounds.width()) / m_scaledImg.width();
    if (fractionY)
        *fractionY = static_cast<double>(unionBounds.height()) / m_scaledImg.height();
    return true;
}

/*量当前登记帧（m_scaledImg，物理像素）里人物可见部分的像素包围盒。
  layout 的实测校正用它，不依赖任何解析换算；占比一律用同一张帧的像素算，
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
  再由它推出真实宽高比与摆放跨度交给渲染器定位。

  为什么要取样求并集：呼吸/物理会让姿势移动，单帧范围会在动作过程中被超出；
  并集是"这段时间里人物占过的最大范围"，画布按它留余量才稳。

  ⚠️ 为什么必须在**测量模式**下取样（setMeasureMode(true)，踩过的坑）：
  并集要成立，所有取样必须在同一个坐标系里。以前探针帧的摆放变换是由"上一次测量结果"
  推出来的（span 有效后就按并集中心/跨度缩放），而并集又喂给下一次测量 ——
  测量基准被自己的结果改写，并集变成"不同缩放下的框的并集"，只会越并越大。
  实测 atri：并集宽高比 0.436（真实）→ 0.498（虚高 14%），纵向范围顶到画布边缘
  饱和成 2.0。画布宽度正是按这个虚高的宽高比定的，于是画布比人物宽 2.3 倍。

  为什么测量模式要把人物缩到画布一半：人物的自然大小几乎占满输出空间（纵向 1.98/2.0），
  动作一摆就超出画布被裁，量到的"并集"会被裁成整幅画布。缩小后动作范围稳稳落在画布内。

  为什么渲染器现在能量准：摆放按帧**无状态**重建（不再被 SetWidth 累积放大、也不再依赖
  画布宽高比），分轴缩放让输出空间到帧像素的各向异性被抵消，范围保留在**输出空间的浮点**里
  不做模型画布像素取整（本模型画布是 1x1，取整会把人物范围毁成 1x1）。*/
void Live2DCharacterWindow::probeFigureMetrics()
{
    /*========== 缓存：探针量的是**模型**的属性，不是画布的 ==========

      为什么可以缓存（结构性理由，不是"看着差不多"）：
        ① 探针帧永远是固定的 512x512 正方形（kProbeCanvasSide），与真实画布尺寸无关；
        ② 取样走**测量模式**（setMeasureMode(true)），变换是固定的等比 kMeasureScale，
           与 m_displayRatioX/Y、与画布尺寸都无关（见 OffscreenUserModel::drawModel）；
        ③ 反解回"自然缩放"只除以那个固定的 fit（见 probeFigureMetrics 的 fold-back），
           没有任何一项来自画布。
      所以画布尺寸是这份结果的**输出**，不是它的输入 —— 换个立绘大小重探一遍是纯浪费。

      浪费有多大（修复前实测）：一次重排 = 3.0s 主线程冻结，其中 2.97s 是
      `msleep(150)` × 19 + `msleep(120)` × 1。而 SettingChild_Char 的立绘大小输入框是
      `textChanged` 驱动的 —— **每敲一个字符**就发一次 requestSetTachieSize，
      于是"改一次大小"实际是 N × 3.0s 的全应用冻结（实测敲 "150" 三个字符 = 9369ms），
      用户看到的就是"延迟几秒才变、之后持续卡顿"。

      失效点只有 loadModel()：换模型 / 重新装载（渲染器会重建模型实例）。*/
    if (m_figureMetricsValid)
    {
        if (!m_figureMetricsReuseLogged)
        {
            m_figureMetricsReuseLogged = true;
            qInfo() << "Live2D 探针 [LIVE2D-PROBE-CACHED] 已缓存（人物几何是模型的属性，与画布"
                       "尺寸无关）：宽高比"
                    << m_figureAspect << "跨度" << m_figureSpanX << "x" << m_figureSpanY
                    << "→ 本次重排不再重探（省下 20 次取样）";
        }
        return;
    }

    m_renderer.clearFigureSpan();
    m_renderer.setMeasureMode(true);

    bool probeOk = true;
    for (int i = 0; i < kProbeSamples; ++i)
    {
        /*取样之间推进**虚拟时间**，不再让墙钟睡眠（更不用 processEvents 把事件循环
           放进来 —— 重排中途让出主线程只会给自检重排制造可乘之机）。

           为什么推进量与被删掉的 msleep(150) 等价：渲染器的单帧步长上限是 100ms
           （Live2DOffscreenRenderer::kMaxFrameDeltaSeconds），以前"睡 150ms 再渲一帧"
           喂进去的也是被夹成 100ms 的步长。这里传的就是同一个 150ms，走的还是那条夹取
           路径 ⇒ 探针覆盖的虚拟时间段与修复前逐位相同（i=0 仍旧不推进，与以前一样）。
           好处：主线程一秒都不睡，而且取样相位不再随机器负载/渲染耗时漂移（确定性）。
           为什么不借机把窗口拉长去覆盖完整的 2.667s 循环：测量模式下人物的纵向范围已经
           饱和在 1.977/2.0，拉长窗口只会让并集顶到测量帧边界，把"人物多大"量成
           "被裁掉多大"。*/
        if (i > 0)
            m_renderer.setNextFrameDeltaSeconds(kProbeSampleIntervalMs / 1000.0f);

        const QImage probe = m_renderer.renderFrame(QSize(kProbeCanvasSide, kProbeCanvasSide));
        if (probe.isNull())
        {
            probeOk = false;
            break;
        }
        // probeFigureMetrics 会把本次可见范围并进渲染器的 FigureSpan
        (void)m_renderer.probeFigureMetrics(probe);
    }

    // 探针的下一帧就是真实画布的首帧，测量模式必须先关掉
    m_renderer.setMeasureMode(false);

    const Live2DOffscreenRenderer::FigureMetrics metrics = m_renderer.figureMetrics();
    if (!probeOk || !metrics.valid)
    {
        qWarning() << "Live2D 探针渲染失败或几乎没画出内容，退回默认尺寸";
        m_figureAspect = kFallbackFigureAspect;
        m_figureSpanX = 2.0;
        m_figureSpanY = 2.0;
        m_renderer.clearFigureSpan();
        /*⚠️ 探针失败**不置** m_figureMetricsValid：这是一次失败，不是"模型量不出东西"。
           下一次重排应当再试（否则偶发的一次 GL 失败会把兜底尺寸永久钉住）。
           失败路径没有睡眠，重试的代价是 20 帧渲染。*/
        return;
    }

    /*人物**真实**宽高比：探针是 1:1 正方形帧、测量模式又是等比变换，所以输出空间到帧像素
      是各向同性的 —— 这个宽高比就是人物"没有任何拉伸"时的形状，直接拿来定画布宽高比。
      （以前这里还要再乘一次画布高/画布宽去"补偿投影分支"，而那个分支从来就没生效过：
      见 Live2DOffscreenRenderer::figureMetrics 的说明。）*/
    m_figureAspect = clampDouble(metrics.boundsAspect, kMinFigureAspect, kMaxFigureAspect);
    m_figureSpanX = std::max(1e-6, static_cast<double>(metrics.spanX));
    m_figureSpanY = std::max(1e-6, static_cast<double>(metrics.spanY));

    /*纯 ASCII 标记 [LIVE2D-PROBE-RUN]：测试要数"探针真的跑了几次"，而控制台是 GBK、
       中文日志会变乱码，所以用它当可解析的锚点（probeFigureMetrics 是私有且非虚的，
       派生类覆写不了，日志是唯一不侵入实现的观察通道）。*/
    qInfo() << "Live2D 探针 [LIVE2D-PROBE-RUN]:" << kProbeSamples
            << "次取样（测量模式 + 虚拟时间），人物可见范围"
            << "宽" << m_figureSpanX << "高" << m_figureSpanY << "（满画布 = 2）→ 真实宽高比"
            << m_figureAspect;

    //量成功了才缓存：下一次重排（换立绘大小）直接用这三个数，不再取样
    m_figureMetricsValid = true;
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

    /*重入保护：**保存并恢复**，不是无条件置 false。
       为什么（这是一个真实存在的隐患，不是洁癖）：relayoutContent() 在开头就把
       m_renderingFrame 置位，用来挡住"重排期间 paintEvent 又来自检重排"；而重排内部
       要经本函数渲染好几帧。无条件置 false 会在第一次内部渲染之后就把外层那道保护
       提前解除，于是重排还没定完画布时 paintEvent 就有机会进来看到"画布 != 窗口"。
       保存-恢复让"是谁置位的、由谁解除"这条语义成立。*/
    const bool wasRenderingFrame = m_renderingFrame;
    m_renderingFrame = true;
    QImage frame = m_renderer.renderFrame(physicalSize);
    m_renderingFrame = wasRenderingFrame;

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

/*见头文件：测试用的确定性阀门，直接把下一帧的步长转给渲染器
   （仍然过渲染器自己的 kMaxFrameDeltaSeconds 夹取 —— 与生产路径同一把尺子）。*/
void Live2DCharacterWindow::setNextFrameDeltaSecondsForTest(float seconds)
{
    m_renderer.setNextFrameDeltaSeconds(seconds);
}

void Live2DCharacterWindow::clearNextFrameDeltaForTest()
{
    m_renderer.clearNextFrameDelta();
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

    // 画布尺寸与窗口不一致、或 dpr 变了时按当前 dpr 重排重渲，然后交给下一帧绘制。
    // 为什么需要：窗口未映射到屏幕前 devicePixelRatioF() 给不出真实值（本机 1.0 vs 实际 1.25），
    // 首帧画布是按错 dpr 定的；映射后尺寸/分辨率都不会自己变好，必须在真实的 dpr 下再排一次。
    // （顺带也覆盖了用户改窗口尺寸、跨屏 dpr 变化等情况。）
    // 为什么要单独看 dpr：画布尺寸是按"canvas × dpr 为整数"对齐定下来的，dpr 一变对齐就失效。
    // 跨屏/改缩放时 dpr 与逻辑尺寸**通常**一起变（物理尺寸不变 ⇒ 逻辑尺寸 = 物理 ÷ dpr 也变），
    // 但两者并不等价（系统只改缩放比例、窗口不挪的情况就只变 dpr），所以两个条件都看：
    // 多存一个 dpr 的代价可以忽略，漏掉一次重排的代价是"整帧被重采样、看不清"。
    const qreal currentDpr = devicePixelRatioF();
    const bool canvasMismatch =
        !m_logicalCanvasSize.isEmpty() &&
        (m_logicalCanvasSize != size() || qAbs(m_layoutDpr - currentDpr) > 1e-9);
    if (m_renderingFrame || !canvasMismatch)
        return;

    /*⚠️ 同一个 (窗口尺寸, dpr) 组合只试一次。
       为什么：这条自检跑在**每一次重绘**里。如果那个不一致是持久的（重排也修不好 ——
       例如窗口管理器把 resize 夹到别的尺寸），"每帧重绘都重排"就是每帧一次探针/一次
       实测校正，用户看到的正是**持续掉帧**：帧率被压到接近 0，而且看起来永远好不了。
       一次修不好就说明"重排"不是这个不一致的解，此时记一条日志、放手，画面继续按
       当前画布画（宁可尺寸不完美，也不能每帧卡一次）。
       放手不等于永久放弃：真正的变化（窗口被真正 resize、跨屏/改缩放导致 dpr 变化）
       会产生一个**新的组合**，那时必须重新尝试 —— 所以记录的是组合本身。*/
    const bool alreadyTriedThisPair = m_relayoutAttempted &&
                                      m_relayoutAttemptedForSize == size() &&
                                      qAbs(m_relayoutAttemptedForDpr - currentDpr) <= 1e-9;
    if (alreadyTriedThisPair)
    {
        if (!m_relayoutGaveUpLogged)
        {
            m_relayoutGaveUpLogged = true;
            qWarning() << "Live2D 画布" << m_logicalCanvasSize << "与窗口" << size() << "/ dpr"
                       << currentDpr << "重排一次后仍不一致：不再每次重绘都重排"
                       << "（窗口尺寸或 dpr 真正变化时会重新尝试）";
        }
        return;
    }

    qInfo() << "Live2D 画布" << m_logicalCanvasSize << "与窗口" << size() << "不一致，或 dpr 由"
            << m_layoutDpr << "变为" << currentDpr << "：按 dpr" << currentDpr << "重排";
    m_relayoutAttempted = true;
    m_relayoutAttemptedForSize = size();
    m_relayoutAttemptedForDpr = currentDpr;
    m_relayoutGaveUpLogged = false;
    relayoutContent();
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
        /*"自检重排只试一次"的记录要清掉：显示是一次全新的事件（跨屏拖动、隐藏再显示
           都可能带来新的 dpr/尺寸），不该被上一次的放手状态挡住。*/
        m_relayoutAttempted = false;
        m_relayoutGaveUpLogged = false;
        relayoutContent();
    }
    else if (!m_logicalCanvasSize.isEmpty() && m_logicalCanvasSize != size())
    {
        // 跨屏/DPI 变化导致窗口尺寸变了，按新 dpr 重排
        m_relayoutAttempted = false;
        m_relayoutGaveUpLogged = false;
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
