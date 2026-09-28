#include <QtTest>

#include "../GlobalConstants.h"
#include "../windows/character/live2dcharacterwindow.h"

#include <QBitmap>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QMouseEvent>
#include <QPixmap>
#include <QRegion>
#include <QSettings>

/*Live2D 立绘窗口的端到端验证：真正要证伪的是「窗口只是个空白矩形」——
  也就是渲染帧确实被登记进窗口层、确实被画出来、并且穿透/命中判定确实来自模型 alpha。

  只用**公开 API** 断言（isModelLoaded / contentSize / renderedImage / renderSize / mask），
  不翻私有成员、不假设渲染器内部实现。

  模型不入库（授权禁二传），本机没有模型时**跳过**而不是失败；
  可用环境变量 MANDARIN_LIVE2D_MODEL_DIR 指定别的模型目录。

  关于坐标系（踩过的坑，写在这里免得再踩）：窗口的 paintEvent 是按**设备像素**把帧画满的，
  所以「图像像素坐标」与「窗口设备无关坐标」一一对应；而 renderedImage() 给的是物理像素帧，
  两者之间要按 QImage::devicePixelRatio() 换算。测试里凡是跨这两种量纲的地方都显式换算。*/
class TestLive2DWindow : public QObject
{
    Q_OBJECT

  private slots:
    void shapesWindowFromRenderedModel();
    void reportsFullPipelineFrameCost();

  private:
    static QString modelDir();
    /*扫描一帧，返回不透明像素数与一个确定不透明的点（画布中部，避开边缘羽化）。
      点在图像**像素**坐标系里。*/
    static int findOpaquePoint(const QImage &frame, QPoint *opaquePoint);
};

QString TestLive2DWindow::modelDir()
{
    const QByteArray fromEnv = qgetenv("MANDARIN_LIVE2D_MODEL_DIR");
    if (!fromEnv.isEmpty())
        return QString::fromLocal8Bit(fromEnv);
    return QDir(Live2DModelRootPath).filePath(QStringLiteral("miku"));
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

/*装载模型 → 窗口按渲染帧定尺寸 → 帧被画出来 → 形状/命中判定确实来自 alpha*/
void TestLive2DWindow::shapesWindowFromRenderedModel()
{
    const QString dir = modelDir();
    const QString modelJsonName = QStringLiteral("miku.model3.json");
    if (!QFileInfo::exists(dir + QLatin1Char('/') + modelJsonName))
        QSKIP("本机没有初音模型（禁二传，不入库），跳过立绘窗口验证");

    Live2DCharacterWindow window;
    window.reloadContent(QStringLiteral("miku"));
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
    const QString grabPath = QDir(QCoreApplication::applicationDirPath())
                                 .absoluteFilePath(QStringLiteral("../live2d-probe/window-grab.png"));
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

    // 塑形的等价观察量①：绘制结果里确实有人物（不是空白矩形，也不是满屏不透明底板）。
    // 统计口径放在窗口自己的绘制结果上，避免和渲染帧的物理像素量纲混淆。
    QPoint hitPoint;
    const int paintedOpaque = findOpaquePoint(painted, &hitPoint);
    const qint64 paintedPixels = static_cast<qint64>(painted.width()) * painted.height();
    QVERIFY2(paintedOpaque > 1000, "窗口绘制结果里几乎没有不透明像素（帧没被画出来）");
    QVERIFY2(paintedOpaque < paintedPixels * 0.9,
             "窗口绘制结果几乎全是不透明，等于一块矩形背板而不是人物");
    QVERIFY2(hitPoint.x() >= 0, "窗口绘制结果中部找不到不透明像素");
    QVERIFY2(hitPoint.x() < window.width() && hitPoint.y() < window.height(),
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

/*量「真实全链路」每帧成本：renderFrame() + 登记 alpha 图 + 交互区节拍（每 10 帧一次）。

  为什么必须量整条链路而不能只看 renderFrame：高刷屏下帧预算只有几毫秒，
  控件侧的开销（QImage 拷贝、updateRenderedImage）与周期性的 QBitmap mask
  完全可能反超渲染本身。这条测试是把「能不能上高帧率」变成数字的地方。

  两种配置都量：用户 config.ini 里的实际值，以及**代码默认值** 60fps / 1.0x
  —— 后者才是「弱机器开箱行为」的真实成本。
  config.ini 是全局状态，测完必须原样还原（程序没在跑才动它）。*/
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
        {"config.ini 实际值", -1, -1.0}, // -1 = 不覆盖，用用户当前配置
        {"代码默认值", 60, 1.0},
    };

    for (const auto &config : configurations)
    {
        // RAII：无论断言怎么退出，都把被改过的 config.ini 键还原回去
        struct ConfigGuard
        {
            QSettings settings{IniSettingPath, QSettings::IniFormat};
            bool touched = false;
            bool hadFps = false;
            bool hadScale = false;
            QVariant oldFps;
            QVariant oldScale;

            ~ConfigGuard()
            {
                if (!touched)
                    return;
                if (hadFps)
                    settings.setValue("character/live2dFps", oldFps);
                else
                    settings.remove("character/live2dFps");
                if (hadScale)
                    settings.setValue("character/live2dScale", oldScale);
                else
                    settings.remove("character/live2dScale");
                settings.sync();
            }
        } guard;

        if (config.fps > 0)
        {
            guard.hadFps = guard.settings.contains("character/live2dFps");
            guard.hadScale = guard.settings.contains("character/live2dScale");
            guard.oldFps = guard.settings.value("character/live2dFps");
            guard.oldScale = guard.settings.value("character/live2dScale");
            guard.settings.setValue("character/live2dFps", config.fps);
            guard.settings.setValue("character/live2dScale", config.scale);
            guard.settings.sync();
            guard.touched = true;
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
