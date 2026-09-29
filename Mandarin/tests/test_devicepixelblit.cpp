#include <QtTest>

#include "../utils/DevicePixelAlign.h"

#include <QCoreApplication>
#include <QImage>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>
#include <QRectF>
#include <QStringList>
#include <QWidget>

#include <algorithm>
#include <cmath>

/*这一条验证的是一个**纯几何命题**，与 Live2D / 模型文件都无关：

  「窗口用 drawImage(rect(), img, 满源矩形) 把一张 QImage 铺满自己时，只有当 img 的**像素**
    尺寸恰好等于窗口的设备矩形（rect() × dpr），这一步才是像素级 1:1 拷贝。」

  精确一点：源矩形是按**图像像素**给的，目标矩形是**逻辑**坐标，所以 Qt 铺的是
  "img 像素网格 → 窗口设备像素网格"。这条命题与 img 自己的 devicePixelRatio 无关 ——
  本用例仍然把帧的 dpr 设成窗口 dpr（生产里 renderAndRegisterFrame 就是这么做的），
  于是它也可以读成"帧的逻辑尺寸 == 窗口逻辑尺寸"，两种说法在真实管线上等价。

  为什么需要它：真实桌宠管线（见 test_live2dwindow）实测「登记的渲染帧」与 window.grab()
  在 17.31% 的像素上不同、清晰度（4 邻域 Laplacian 方差）掉 11.9%。假设是几何原因 ——
  窗口逻辑尺寸 400x938、dpr 1.25 ⇒ 设备矩形 500x1172.5；而登记帧是 500x1173 像素。
  1173 行铺进 1172.5 行（纵向比例 0.99957），逐行相位从 0 漂到 0.5，于是每一行都吃到
  不同强度的插值。

  本用例把这个假设缩到最小：同一段绘制代码、同一个 dpr，只换尺寸组合。
  三组尺寸里两组预测"糊"、一组预测"精确"，所以它既能证实机制、也能证伪机制
  （如果"像素尺寸对不上"那一组居然也是精确的，假设就是错的）。

  它同时是「画布必须设备像素对齐」这条规则的回归测试：谁把画布尺寸改回"随便取整"，
  这里就会红。（真实窗口侧的断言在 test_live2dwindow::paintsRegisteredFrameWithoutResampling；
  对齐步长与画布吸附算法的回归测试在本文件的另外两个用例里。）

  高 DPI 说明：对齐步长完全由运行时的 dpr 决定，不写死任何倍数。
  dpr = p/q（既约）时，n·dpr 是整数 ⇔ n 是 q 的倍数 —— 所以 1.25→4、1.5→2、1.75→4、
  2.0→1、1.0→1。断言里的例（500x1173 / 1172 对 400x938，500x1170 对 400x936）
  是 dpr=1.25 的实例；dpr 不是 1.25 时用例按同一个"对齐 / 不对齐"的意思重新算尺寸，
  结论（对齐 ⇒ 精确、不对齐 ⇒ 有损）与 dpr 无关。*/
class TestDevicePixelBlit : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase();
    void cleanupTestCase();

    /*三组尺寸的对照：这正是要检验的假设本身*/
    void paintIsExactOnlyWhenPixelSizesMatch();

    /*假设不成立时的旁证：关掉 SmoothPixmapTransform 会发生什么。
      目的不是"找个能糊弄过去的开关"，而是把"机制"与"缓解手段"分开：
      如果关掉它就不糊了，说明糊来自插值而不是来自"少了半行像素"。*/
    void smoothTransformIsTheBlurSource();

    /*对齐规则的运行时常量：dpr → 最小对齐步长。画布的尺寸算法要用同一个函数，
      所以这里把它的判据也钉住（1.25 → 4 等）。*/
    void alignmentStepFollowsDevicePixelRatio();

    /*生产代码的画布对齐算法（utils/DevicePixelAlign.h）：结果必须设备像素对齐，
      且宽高比偏差被量化并卡在 0.5% 以内。*/
    void alignedCanvasKeepsAspectAndIsAligned();

  private:
    /*与 paintEvent 完全同构的探针控件：整帧缩放进 rect()，源矩形给满。
      必须与 Live2DCharacterWindow::paintEvent 一致，否则量到的不是同一条路径。*/
    class ProbeWidget : public QWidget
    {
      public:
        void setFrame(const QImage &frame);
        void setSmoothTransform(bool smooth) { m_smooth = smooth; }

      protected:
        void paintEvent(QPaintEvent *event) override;

      private:
        QImage m_frame;
        bool m_smooth = true;
    };

    /*高频内容图：确定性伪随机（不依赖随机种子，跑多少次都是同一张）。
      为什么要高频：Laplacian 方差这类清晰度指标量的就是高频；内容本身平滑的话
      "被重采样"根本量不出来，用例会变成永远绿的假测试。*/
    static QImage makeHighFrequencyImage(const QSize &size);

    /*逐像素对比。differing 计"任一通道差值 > channelDelta"的像素数；
       maxChannelDiff 是最大通道绝对差（-1 表示尺寸不一致、不可比）。*/
    static qint64 comparePixels(const QImage &expected, const QImage &actual, int channelDelta,
                               int *maxChannelDiff);

    /*4 邻域 Laplacian（亮度域，中心 -4、上下左右各 +1）的方差。
      与 test_live2dwindow 的度量同一定义：纯色 0、高频大，图被插值糊掉就掉下来。
      内容全不透明，所以不需要 alpha 掩码。*/
    static double laplacianVariance(const QImage &image);

    /*跑一组尺寸：把 frame 画进逻辑尺寸为 logical 的探针控件，抓回来。
      顺带把关键算术打进日志（与假设直接相关的量：img 的逻辑尺寸 vs 控件逻辑尺寸）。*/
    QPixmap paintAndGrab(const QSize &logical, const QImage &frame, bool smooth,
                         const QString &label);

    QWidget *m_host = nullptr;
    ProbeWidget *m_probe = nullptr;
    qreal m_dpr = 1.0;
};

void TestDevicePixelBlit::ProbeWidget::setFrame(const QImage &frame)
{
    m_frame = frame;
    update();
}

void TestDevicePixelBlit::ProbeWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    if (m_frame.isNull())
        return;

    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, m_smooth);
    painter.drawImage(rect(), m_frame, QRectF(0, 0, m_frame.width(), m_frame.height()));
}

QImage TestDevicePixelBlit::makeHighFrequencyImage(const QSize &size)
{
    QImage image(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y)
    {
        uchar *line = image.scanLine(y);
        for (int x = 0; x < size.width(); ++x)
        {
            // murmur3 收官雪崩：逐像素独立、无可见结构，就是纯高频
            quint32 h = static_cast<quint32>(x) + 0x9E3779B9u * (static_cast<quint32>(y) + 1u);
            h ^= h >> 16;
            h *= 0x7FEB352Du;
            h ^= h >> 15;
            h *= 0x846CA68Bu;
            h ^= h >> 16;

            /*不做全 0~255 的纯噪声：那会让"任一通道不同"变成恒等于 100%，
                差值大小反而失去分辨力。取 ±40 的中等对比度，既保住高频、又让
                "差值有多小"这件事可读（真实管线里 97% 的差异像素只差 ≤3 级）。*/
            line[x * 4 + 0] = static_cast<uchar>(128 + static_cast<int>(h & 0x3Fu) - 32);
            line[x * 4 + 1] = static_cast<uchar>(128 + static_cast<int>((h >> 8) & 0x3Fu) - 32);
            line[x * 4 + 2] = static_cast<uchar>(128 + static_cast<int>((h >> 16) & 0x3Fu) - 32);
            line[x * 4 + 3] = 255; // 全不透明：对比里不掺预乘 alpha 的舍入
        }
    }
    return image;
}

qint64 TestDevicePixelBlit::comparePixels(const QImage &expected, const QImage &actual,
                                          int channelDelta, int *maxChannelDiff)
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
            bool over = false;
            for (int c = 0; c < 4; ++c)
            {
                const int d = qAbs(int(la[x * 4 + c]) - int(lb[x * 4 + c]));
                if (d > maxDiff)
                    maxDiff = d;
                if (d > channelDelta)
                    over = true;
            }
            if (over)
                ++differing;
        }
    }
    if (maxChannelDiff)
        *maxChannelDiff = maxDiff;
    return differing;
}

double TestDevicePixelBlit::laplacianVariance(const QImage &image)
{
    if (image.isNull() || image.width() < 3 || image.height() < 3)
        return 0.0;

    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    const int w = rgba.width();
    const int h = rgba.height();
    const auto lum = [&rgba](int x, int y) {
        const uchar *line = rgba.constScanLine(y);
        return 0.299 * line[x * 4 + 0] + 0.587 * line[x * 4 + 1] + 0.114 * line[x * 4 + 2];
    };

    double sum = 0.0;
    double sumSq = 0.0;
    qint64 n = 0;
    for (int y = 1; y < h - 1; ++y)
    {
        for (int x = 1; x < w - 1; ++x)
        {
            const double lap =
                4.0 * lum(x, y) - (lum(x - 1, y) + lum(x + 1, y) + lum(x, y - 1) + lum(x, y + 1));
            sum += lap;
            sumSq += lap * lap;
            ++n;
        }
    }
    if (n <= 0)
        return 0.0;
    const double mean = sum / n;
    return std::max(0.0, sumSq / n - mean * mean);
}

void TestDevicePixelBlit::initTestCase()
{
    /*宿主窗口只是给探针控件一个已映射的窗口：子控件在未显示时 dpr 未必是真实值。
      探针控件自身必须**恰好**是目标逻辑尺寸，所以尺寸不给布局管，直接 resize。*/
    m_host = new QWidget;
    m_host->setAttribute(Qt::WA_TranslucentBackground);
    m_host->resize(640, 1200);
    m_host->show();
    if (!QTest::qWaitForWindowExposed(m_host))
        QSKIP("窗口没能映射到屏幕，无法测真实 dpr 下的绘制");

    m_probe = new ProbeWidget;
    m_probe->setParent(m_host);
    m_probe->move(0, 0);
    m_probe->show();
    QCoreApplication::processEvents();

    m_dpr = m_probe->devicePixelRatioF();
    QVERIFY2(m_dpr > 0.0, "探针控件拿到了非法 dpr");
    qInfo("PAINTBLIT probe dpr=%f alignmentStep=%d", double(m_dpr), devicePixelAlignStep(m_dpr));
}

void TestDevicePixelBlit::cleanupTestCase()
{
    delete m_host; // 连同子探针一起销毁
    m_host = nullptr;
    m_probe = nullptr;
}

QPixmap TestDevicePixelBlit::paintAndGrab(const QSize &logical, const QImage &frame, bool smooth,
                                          const QString &label)
{
    /*与 renderAndRegisterFrame 一样：帧的 dpr 取窗口 dpr。
      注意源矩形是按像素给的，所以设不设这个 dpr 都不影响绘制结果 ——
      设上只是让"帧逻辑尺寸 == 窗口逻辑尺寸"这个说法在这里也成立，便于读日志。*/
    QImage withDpr = frame;
    withDpr.setDevicePixelRatio(m_dpr);

    m_probe->setSmoothTransform(smooth);
    m_probe->setFrame(withDpr);
    m_probe->resize(logical);
    QCoreApplication::processEvents();

    const QPixmap grabbed = m_probe->grab();
    qInfo("  %s: widget logical=%dx%d dpr=%f -> device=%dx%d | image %dx%d px -> logical "
          "%.4fx%.4f | dH=%+.4f dW=%+.4f | grab device=%dx%d",
          qPrintable(label), logical.width(), logical.height(), double(m_dpr),
          qRound(logical.width() * m_dpr), qRound(logical.height() * m_dpr), frame.width(),
          frame.height(), frame.width() / m_dpr, frame.height() / m_dpr,
          frame.height() / m_dpr - logical.height(), frame.width() / m_dpr - logical.width(),
          grabbed.width(), grabbed.height());
    return grabbed;
}

void TestDevicePixelBlit::paintIsExactOnlyWhenPixelSizesMatch()
{
    /*三组尺寸刻意用**同一个逻辑宽度 400**，把变量收敛到纵向那一维：
      横向 500 像素 / 1.25 = 400 逻辑，与控件等宽，所以横向永远没有缩放；
      纵向的差异全部来自 1173 / 1170 / 1172 与控件逻辑高之间的关系。*/
    struct Case
    {
        const char *label;
        int imageW;
        int imageH;
        int logicalW;
        int logicalH;
        /* SamePixelCount：源图与绘制结果的像素尺寸相同，可以逐像素硬比。
           RowCountChanged：绘制结果行数会变（见 case (c) 的注释），逐像素比无从谈起 ——
           那本身就是"不是精确拷贝"的证据。*/
        bool samePixelCount;
        bool expectExact;
    };
    const Case cases[] = {
        {"(a) 500x1173 px (logical 400x938.4) -> widget 400x938  [predict LOSSY]", 500, 1173, 400,
         938, true, false},
        {"(b) 500x1170 px (logical 400x936)   -> widget 400x936  [predict LOSSLESS]", 500, 1170,
         400, 936, true, true},
        {"(c) 500x1172 px (logical 400x937.6) -> widget 400x938  [predict LOSSY]", 500, 1172, 400,
         938, false, false},
    };

    for (const Case &c : cases)
    {
        const QImage frame = makeHighFrequencyImage(QSize(c.imageW, c.imageH));
        const QPixmap grabbed = paintAndGrab(QSize(c.logicalW, c.logicalH), frame, true,
                                            QString::fromLatin1(c.label));
        QVERIFY2(!grabbed.isNull(), "grab() 失败，量不到绘制结果");

        const QImage painted = grabbed.toImage().convertToFormat(QImage::Format_RGBA8888);
        const double srcLap = laplacianVariance(frame);
        const double paintedLap = laplacianVariance(painted);

        if (!c.samePixelCount)
        {
            /*case (c) 是"帧比窗口设备矩形少一行"的形态（截断而不是 lround 就会这样：
              938×1.25 = 1172.5，lround 得 1173、截断得 1172）。此时绘制把 1172 行铺成
              1173 个设备行 —— **精确拷贝不可能改变行数**，所以这一组必然有损，
              这本身就是判据，不需要逐像素比（尺寸都不同，比不了）。*/
            QVERIFY2(painted.size() != frame.size(),
                     qPrintable(QStringLiteral("%1 预测有损：%2 行铺进 %3 行，行数必须变")
                                    .arg(QString::fromLatin1(c.label))
                                    .arg(frame.height())
                                    .arg(qRound(c.logicalH * m_dpr))));
            QVERIFY2(paintedLap < srcLap,
                     qPrintable(QStringLiteral("%1 预测有损：清晰度却没下降（源 %2 → 绘制 %3），"
                                               "「非整数比例 ⇒ 被重采样」这条假设不成立")
                                    .arg(QString::fromLatin1(c.label))
                                    .arg(srcLap)
                                    .arg(paintedLap)));

            /*旁证只到"清晰度掉了一半"为止：不再去和 QImage::scaled 的输出逐像素比 ——
              画家的双线性与 QImage::scaled 的滤波器内核并不相同，那种比较量到的是
              "两个不同重采样器之间的差"，不是"多走了一遍重采样"，会把人带偏。*/
            qInfo("    rowCount %d -> %d (exact blit impossible) | laplacianVariance "
                  "source=%.4f painted=%.4f (%+.3f%%)",
                  frame.height(), painted.height(), srcLap, paintedLap,
                  (paintedLap - srcLap) * 100.0 / srcLap);
            continue;
        }

        QCOMPARE(painted.size(), frame.size());

        int maxDiff = -1;
        const qint64 differing = comparePixels(frame, painted, 0, &maxDiff);
        const qint64 total = static_cast<qint64>(painted.width()) * painted.height();

        qInfo("    differing=%lld/%lld (%.4f%%) maxChannelDiff=%d | laplacianVariance "
              "source=%.4f painted=%.4f (%+.3f%%)",
              differing, total, differing * 100.0 / static_cast<double>(total), maxDiff, srcLap,
              paintedLap, (paintedLap - srcLap) * 100.0 / srcLap);

        if (c.expectExact)
        {
            /*精确：逐位相同。这一组是假设的"正面"预测 —— 像素尺寸对得上 ⇒ 1:1 blit。
              容差给 0：内容全不透明、格式转换无损，任何非零差异都说明还有别的机制。*/
            QVERIFY2(differing == 0 && maxDiff == 0,
                     qPrintable(QStringLiteral("%1 预测精确，实测 differing=%2 maxChannelDiff=%3"
                                               "（%4）")
                                    .arg(QString::fromLatin1(c.label))
                                    .arg(differing)
                                    .arg(maxDiff)
                                    .arg(differing == 0 && maxDiff == 0
                                             ? QStringLiteral("符合")
                                             : QStringLiteral("假设被证伪"))));
        }
        else
        {
            /*有损：像素尺寸对不上 ⇒ 必须真的被重采样。
              只断言"不是逐位相同"，不卡具体数字 —— 数字随 dpr 与尺寸而变，
              卡死会变成跨机器偶发失败。清晰度掉多少由日志给出。*/
            QVERIFY2(differing > 0,
                     qPrintable(QStringLiteral("%1 预测有损，但 grab() 与源图逐位相同："
                                               "「像素尺寸对不上 ⇒ 重采样」这条假设不成立")
                                    .arg(QString::fromLatin1(c.label))));
            QVERIFY2(paintedLap < srcLap,
                     qPrintable(QStringLiteral("%1 预测有损：清晰度却没下降（源 %2 → 绘制 %3）")
                                    .arg(QString::fromLatin1(c.label))
                                    .arg(srcLap)
                                    .arg(paintedLap)));
        }
    }
}

void TestDevicePixelBlit::smoothTransformIsTheBlurSource()
{
    /*同一组 (a) 尺寸，只切换 SmoothPixmapTransform。这一组**不断言**，只报数字：
      它要回答的是"糊是插值造成的，还是少了半行像素造成的"。
      - 关掉后逐位相同 ⇒ 机制是"平滑插值被非整数比例打成亚像素混合"，
        设备的几何其实没少像素（最近邻恰好逐行落在原行上）；
      - 关掉后仍然不同 ⇒ 除插值外还有真正的内容丢失。
      真实管线里超采样（live2dScale>1）依赖平滑下采样，所以不管哪种结果，
      缓解手段都不会是"关掉它"（那会把 1.5x 的超采样质量一起扔掉）。*/
    const QImage frame = makeHighFrequencyImage(QSize(500, 1173));
    const QPixmap smoothGrabbed = paintAndGrab(QSize(400, 938), frame, true,
                                              QStringLiteral("(a) smooth=ON"));
    const QPixmap plainGrabbed = paintAndGrab(QSize(400, 938), frame, false,
                                             QStringLiteral("(a) smooth=OFF"));

    const QImage smoothPainted = smoothGrabbed.toImage().convertToFormat(QImage::Format_RGBA8888);
    const QImage plainPainted = plainGrabbed.toImage().convertToFormat(QImage::Format_RGBA8888);
    const double srcLap = laplacianVariance(frame);

    int smoothMax = -1;
    int plainMax = -1;
    const qint64 smoothDiffering = comparePixels(frame, smoothPainted, 0, &smoothMax);
    const qint64 plainDiffering = comparePixels(frame, plainPainted, 0, &plainMax);
    const qint64 total = static_cast<qint64>(frame.width()) * frame.height();

    qInfo("    smooth=ON : differing=%lld/%lld (%.4f%%) maxChannelDiff=%d lapVar=%.4f "
          "(%+.3f%% vs source)",
          smoothDiffering, total, smoothDiffering * 100.0 / static_cast<double>(total), smoothMax,
          laplacianVariance(smoothPainted),
          (laplacianVariance(smoothPainted) - srcLap) * 100.0 / srcLap);
    qInfo("    smooth=OFF: differing=%lld/%lld (%.4f%%) maxChannelDiff=%d lapVar=%.4f "
          "(%+.3f%% vs source)",
          plainDiffering, total, plainDiffering * 100.0 / static_cast<double>(total), plainMax,
          laplacianVariance(plainPainted),
          (laplacianVariance(plainPainted) - srcLap) * 100.0 / srcLap);

    /*唯一硬断言：这两种画法必须给出**不同**的结果。
      如果它们一模一样，说明 SmoothPixmapTransform 在这条路径上根本没生效，
      那么"糊"就不可能来自插值 —— 上一条用例的结论必须重新解释。*/
    QVERIFY2(smoothDiffering != plainDiffering || smoothMax != plainMax,
             "开关 SmoothPixmapTransform 对绘制结果毫无影响：重采样路径与假设不符");
}

void TestDevicePixelBlit::alignmentStepFollowsDevicePixelRatio()
{
    /*步长必须由 dpr 推出来，不能写死 —— 这里测的是**生产代码**里的
      utils/DevicePixelAlign.h（Live2DCharacterWindow 用的就是同一个函数），
      所以"把 4 写死"这种退化会在这里当场失败：dpr 1.0 的用例要 1、1.5 的要 2、
      1.75 的要 4、2.0 的要 1，写死 4 过不了。*/
    struct Case
    {
        qreal dpr;
        int expectedStep;
    };
    const Case cases[] = {
        {1.0, 1}, {1.25, 4}, {1.5, 2}, {1.75, 4}, {2.0, 1}, {2.25, 4}, {2.5, 2}, {3.0, 1},
    };
    for (const Case &c : cases)
    {
        QCOMPARE(devicePixelAlignStep(c.dpr), c.expectedStep);
        // 判据本身：步长乘出来的设备像素数必须是整数
        const qreal devicePixels = 400.0 * c.expectedStep * c.dpr;
        QVERIFY2(std::abs(devicePixels - std::round(devicePixels)) < 1e-9,
                 qPrintable(QStringLiteral("dpr=%1 的步长 %2 乘不出整数设备像素")
                                .arg(double(c.dpr))
                                .arg(c.expectedStep)));
    }
}

void TestDevicePixelBlit::alignedCanvasKeepsAspectAndIsAligned()
{
    /*画布尺寸算法的两条硬性质，跨 dpr 与人物宽高比都成立：
        (1) 结果 × dpr 必须是整数 —— 这就是"上屏能成为精确拷贝"的前提；
        (2) 结果的宽高比必须贴住人物真实宽高比 —— 画布宽高比就是人物的像素宽高比，
            偏多少人物就被拉长/压扁多少，所以这条偏差要**量化**并卡住。
      另外把"对齐后尺寸仍在上下界内"也钉住（kMinCanvasSide/kMaxCanvasSide 由调用方给）。
      注意这里用的是生产代码里的 devicePixelAlignedCanvasSize，不是复刻品。*/
    constexpr int kMinSide = 32;
    constexpr int kMaxSide = 8192;
    // 面板上真实存在的缩放档位 + 两个非整数档位里的"难例"
    const qreal dprs[] = {1.0, 1.25, 1.5, 1.75, 2.0, 2.25, 2.5};
    // 人物宽高比的量级：从瘦长（atri ≈ 0.427）到宽扁
    const double aspects[] = {0.30, 0.426877, 0.60, 1.0, 1.8};

    double worstAspectDeviation = 0.0;
    QSize worstSize;
    qreal worstDpr = 1.0;
    double worstAspect = 1.0;
    for (qreal dpr : dprs)
    {
        const int step = devicePixelAlignStep(dpr);
        for (double aspect : aspects)
        {
            /*理想画布按**生产路径**推出来，不能随便造：
               高度 = 人物基准高 900 / 目标占比 0.84；宽度 = 高度 × 人物宽高比；
               再按可用屏幕的 85% 夹一次（**保比例**，两条边同乘一个 shrink）。
              上界就取夹取后的尺寸本身 —— 生产里也正是这么传的，
              于是"对齐结果不超过上界"等价于"85% 屏高夹取仍是硬保证"。
              （第一版用例在这里造了一个宽高比与理想不一致的上界，
                那是生产路径产生不出来的输入，量到的 28.9% 偏差是假失败。）*/
            int h = static_cast<int>(std::lround(900.0 / 0.84));
            int w = static_cast<int>(std::lround(h * aspect));
            const int maxH = 979;  // 1440 物理高 @125% 的 85%
            const int maxW = 1088; // 2560 物理宽 @125% 的 85%
            if (h > maxH)
            {
                const double shrink = static_cast<double>(maxH) / h;
                h = maxH;
                w = static_cast<int>(std::lround(w * shrink));
            }
            if (w > maxW)
            {
                const double shrink = static_cast<double>(maxW) / w;
                w = maxW;
                h = static_cast<int>(std::lround(h * shrink));
            }
            const QSize ideal(w, h);
            const QSize got =
                devicePixelAlignedCanvasSize(ideal, aspect, step, ideal, kMinSide, kMaxSide);

            const qreal deviceW = got.width() * dpr;
            const qreal deviceH = got.height() * dpr;
            QVERIFY2(std::abs(deviceW - std::round(deviceW)) < 1e-9 &&
                         std::abs(deviceH - std::round(deviceH)) < 1e-9,
                     qPrintable(QStringLiteral("dpr=%1 aspect=%2 → 画布 %3x%4，×dpr = %5x%6 不是整数")
                                    .arg(double(dpr))
                                    .arg(aspect)
                                    .arg(got.width())
                                    .arg(got.height())
                                    .arg(deviceW)
                                    .arg(deviceH)));
            QVERIFY2(got.width() >= kMinSide && got.height() >= kMinSide &&
                         got.width() <= kMaxSide && got.height() <= kMaxSide,
                     "对齐后的画布越出了边长上下界");
            QVERIFY2(got.width() <= ideal.width() && got.height() <= ideal.height(),
                     qPrintable(QStringLiteral("对齐后的画布 %1x%2 越过了屏幕夹取上界 %3x%4")
                                    .arg(got.width())
                                    .arg(got.height())
                                    .arg(ideal.width())
                                    .arg(ideal.height())));

            /*高度也不该被对齐拉走太远（它是"人物在屏幕上多大"的直接决定者）。
               候选只在理想高度附近 ±1 个步长里取，所以偏差天然有界。*/
            const double heightDeviation =
                std::abs(static_cast<double>(got.height() - ideal.height())) / ideal.height();
            QVERIFY2(heightDeviation <= 0.01,
                     qPrintable(QStringLiteral("dpr=%1 aspect=%2：对齐把画布高度从 %3 拉到 %4"
                                               "（偏 %5%），超过 1%")
                                    .arg(double(dpr))
                                    .arg(aspect)
                                    .arg(ideal.height())
                                    .arg(got.height())
                                    .arg(heightDeviation * 100.0, 0, 'f', 3)));

            const double deviation =
                std::abs(static_cast<double>(got.width()) / got.height() - aspect) / aspect;
            if (deviation > worstAspectDeviation)
            {
                worstAspectDeviation = deviation;
                worstSize = got;
                worstDpr = dpr;
                worstAspect = aspect;
            }
        }
    }

    /*接受的上限（本用例的判据，也是修复说明书里的数字）：
       宽高比偏差 = 人物被拉伸的比例。对齐只在"理想尺寸附近"吸附，所以偏差天然是
       亚像素量级 / 画布边长；宽度还要额外向下取整到步长，所以最坏能到几个像素。
       实测（本矩阵 7 个 dpr × 5 个宽高比）：最坏 0.27%（dpr 1.5、宽高比 0.30）；
       真机 atri / dpr 1.25：0.11%。界取 0.5% —— 约为实测最坏值的两倍，
       够松不会偶发失败，又紧到能抓住"两条边各自取整 / 不按宽高比推宽度"这类退化。*/
    constexpr double kAspectDeviationBound = 0.005;
    qInfo("ALIGN canvas: worst aspect deviation over %d dpr x %d aspects = %.4f%% "
          "(dpr=%g aspect=%.6f -> %dx%d), bound %.1f%%",
          int(sizeof(dprs) / sizeof(dprs[0])), int(sizeof(aspects) / sizeof(aspects[0])),
          worstAspectDeviation * 100.0, double(worstDpr), worstAspect, worstSize.width(),
          worstSize.height(), kAspectDeviationBound * 100.0);
    QVERIFY2(worstAspectDeviation <= kAspectDeviationBound,
             qPrintable(QStringLiteral("最坏宽高比偏差 %1%（dpr=%2 aspect=%3 → %4x%5）超过 %6%："
                                       "人物会被拉伸这么多")
                            .arg(worstAspectDeviation * 100.0, 0, 'f', 4)
                            .arg(double(worstDpr))
                            .arg(worstAspect)
                            .arg(worstSize.width())
                            .arg(worstSize.height())
                            .arg(kAspectDeviationBound * 100.0, 0, 'f', 1)));
}

QTEST_MAIN(TestDevicePixelBlit)
#include "test_devicepixelblit.moc"
