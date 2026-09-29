#include <QtTest>

#include "../GlobalConstants.h"
#include "../windows/character/live2dcharacterwindow.h"

#include <QBitmap>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
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
        window.reloadContent(modelName);
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
    window.reloadContent(modelName);
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
        /*只写临时配置。窗口在 reloadContent() 里读它（applyRenderScaleFromConfig）。*/
        {
            QSettings settings(m_tempConfigPath, QSettings::IniFormat);
            settings.setValue("character/live2dFps", 60);
            settings.setValue("character/live2dScale", scale);
            settings.sync();
        }

        log << QStringLiteral("SHARPNESS scale=%1 --------").arg(scale);

        Live2DCharacterWindow window;
        window.reloadContent(modelName);
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

QTEST_MAIN(TestLive2DWindow)
#include "test_live2dwindow.moc"
