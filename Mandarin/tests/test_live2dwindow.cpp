#include <QtTest>

#include "../GlobalConstants.h"
#include "../windows/character/live2dcharacterwindow.h"

#include <QBitmap>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMouseEvent>
#include <QPixmap>
#include <QRegion>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <algorithm>

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

  private:
    static QString modelDir();
    static QString modelDirFor(const QString &name);
    /*用户实际在用的模型名（config.ini 的 character/live2dModel，当前是 atri）*/
    static QString preferredModelName();

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
    window.reloadContent(modelName);
    QVERIFY2(window.isModelLoaded(), "模型装载失败（Live2DCharacterWindow::reloadContent）");

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
        window.reloadContent(modelName);
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
    window.reloadContent(modelName);
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
        window.reloadContent(modelName);
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
        window.reloadContent(QString::fromUtf8(c.dirName));
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

  三种配置都量：用户实际在用的档位（120fps，缩放 1.0 与 1.5 各一档 —— 这是"8.33ms 帧预算
  到底够不够"的唯一依据）与**代码默认值** 60fps / 1.0x（「弱机器开箱行为」的真实成本）。
  三种配置都写进**临时**配置文件（见 initTestCase），用户的 config.ini 全程只读。
  注：帧率档位只影响定时器间隔，不影响这里的每帧成本 —— 两条 1.0x 行本就该给出同一个数字，
  分列出来是为了让"1.0x 该是多少"在日志里直接可查，不必自己推。*/
void TestLive2DWindow::reportsFullPipelineFrameCost()
{
    const QString dir = modelDir();
    const QString modelJsonName = QStringLiteral("miku.model3.json");
    if (!QFileInfo::exists(dir + QLatin1Char('/') + modelJsonName))
        QSKIP("本机没有初音模型（禁二传，不入库），跳过全链路帧成本测量");

    const struct
    {
        const char *label;
        int fps;
        double scale;
    } configurations[] = {
        {"用户档位 120fps/1.5x", 120, 1.5},
        {"用户档位 120fps/1.0x", 120, 1.0},
        {"代码默认值 60fps/1.0x", 60, 1.0},
    };

    for (const auto &config : configurations)
    {
        /*只写临时配置；不需要 RAII —— 临时目录随测试进程一起消失，
          而且下一个用例会自己再写一次，用户真实配置从头到尾没参与。*/
        {
            QSettings settings(m_tempConfigPath, QSettings::IniFormat);
            settings.setValue("character/live2dFps", config.fps);
            settings.setValue("character/live2dScale", config.scale);
            settings.sync();
        }

        Live2DCharacterWindow window;
        window.reloadContent(QStringLiteral("miku"));
        QVERIFY2(window.isModelLoaded(), "模型装载失败，无法量帧成本");

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

        qInfo("全链路帧成本[%s]：逻辑 %dx%d，渲染 %dx%d，平均 %.2f ms/帧（约 %.1f fps），节拍 %d ms",
              config.label, logical.width(), logical.height(), physical.width(),
              physical.height(), msPerFrame, fps, window.frameIntervalMs());

        // 只做宽松退步警戒：真实数字靠上面日志观察，不在测试里卡死阈值。
        QVERIFY2(msPerFrame < 200.0,
                 qPrintable(QStringLiteral("[%1] 全链路每帧 %2 ms，慢到不可用")
                                .arg(QString::fromUtf8(config.label))
                                .arg(msPerFrame)));
    }
}

QTEST_MAIN(TestLive2DWindow)
#include "test_live2dwindow.moc"
