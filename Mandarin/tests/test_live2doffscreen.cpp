#include <QtTest>

#include "../utils/Live2DOffscreenRenderer.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QThread>

/*Live2D 离屏渲染的端到端验证。

  这条测试是本阶段最关键的证伪手段：它用真实模型跑通
  「内存加载 moc3/贴图/物理 → 离屏 FBO → 读回 RGBA」整条链路，
  并把结果写成 PNG 供目视确认。

  模型不入库（授权禁二传），所以本机没有模型时**跳过**而不是失败；
  可用环境变量 MANDARIN_LIVE2D_MODEL_DIR 指定别的模型目录。*/
class TestLive2DOffscreen : public QObject
{
    Q_OBJECT

  private slots:
    void rendersMikuFirstFrame();
    void drivesParametersWithUpdaters();
    void reportsFrameCost();

  private:
    static QString modelDir();
    static QString outputDir();
};

QString TestLive2DOffscreen::modelDir()
{
    const QByteArray fromEnv = qgetenv("MANDARIN_LIVE2D_MODEL_DIR");
    if (!fromEnv.isEmpty())
        return QString::fromLocal8Bit(fromEnv);
    return QDir::homePath() + QStringLiteral("/Documents/Mandarin/Live2D/miku");
}

QString TestLive2DOffscreen::outputDir()
{
    // 测试产物统一放构建目录，绝不落进 build2/Release（否则会被打进便携包）
    return QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(
        QStringLiteral("../live2d-probe"));
}

/*渲染第一帧，验证确实画出了东西，并验证水印可由参数关闭；两版 PNG 落盘供目视确认*/
void TestLive2DOffscreen::rendersMikuFirstFrame()
{
    const QString dir = modelDir();
    const QString modelJsonName = QStringLiteral("miku.model3.json");
    if (!QFileInfo::exists(dir + QLatin1Char('/') + modelJsonName))
        QSKIP("本机没有初音模型（禁二传，不入库），跳过离屏渲染验证");

    const QSize targetSize(760, 900);

    Live2DOffscreenRenderer renderer;
    QString error;
    QVERIFY2(renderer.load(dir, modelJsonName, &error), qPrintable(error));
    QVERIFY(renderer.isLoaded());

    // 水印参数应由模型目录里的 水印.exp3.json 推出，而不是硬编码
    QCOMPARE(renderer.watermarkParamId(), QStringLiteral("Param137"));

    // 默认应为隐藏水印（该参数反相：1 = 隐藏，0 = 显示）
    const QImage watermarkHidden = renderer.renderFrame(targetSize);
    QVERIFY2(!watermarkHidden.isNull(), "renderFrame 返回了空图");
    QCOMPARE(watermarkHidden.size(), targetSize);

    int opaquePixels = 0;
    for (int y = 0; y < watermarkHidden.height(); ++y)
    {
        const uchar *line = watermarkHidden.constScanLine(y);
        for (int x = 0; x < watermarkHidden.width(); ++x)
        {
            if (line[x * 4 + 3] > 32)
                ++opaquePixels;
        }
    }
    QVERIFY2(opaquePixels > 1000,
             qPrintable(QStringLiteral("不透明像素只有 %1 个，模型基本没画出来")
                            .arg(opaquePixels)));

    QDir().mkpath(outputDir());
    const QString hiddenPng = outputDir() + QStringLiteral("/miku-watermark-hidden.png");
    QVERIFY2(watermarkHidden.save(hiddenPng),
             qPrintable(QStringLiteral("写不出 %1").arg(hiddenPng)));

    // 关键：读回参数**真实值**来判断覆盖是否生效。
    // 不能用"两帧像素不同"来证明 —— 两帧之间物理动画（头发/呼吸）本来就会变，
    // 像素差异无法区分"参数生效"和"动画推进"，是无效断言。
    QCOMPARE(renderer.parameterValue(QStringLiteral("Param137")), 1.0f);

    // 显示水印再渲染一版，供人眼比对（水印文字是烤进贴图的，只能靠目视确认）
    renderer.setWatermarkVisible(true);
    const QImage watermarkVisible = renderer.renderFrame(targetSize);
    QVERIFY(!watermarkVisible.isNull());
    const QString visiblePng = outputDir() + QStringLiteral("/miku-watermark-visible.png");
    QVERIFY2(watermarkVisible.save(visiblePng),
             qPrintable(QStringLiteral("写不出 %1").arg(visiblePng)));

    QCOMPARE(renderer.parameterValue(QStringLiteral("Param137")), 0.0f);
    qInfo("离屏渲染产物：隐藏水印=%s 显示水印=%s", qPrintable(hiddenPng),
          qPrintable(visiblePng));
}

/*参数驱动器（呼吸/眨眼/物理）真的接上了吗 —— 这是 Bug A（模型完全静止）的离屏钉子。

  只断言"两帧像素不同"是不够的：那既可能来自驱动器，也可能来自渲染噪声。
  这里直接读**参数真实值**跑一段时间看它有没有变，并覆盖三类驱动器：
    - 眨眼：ParamEyeLOpen / ParamEyeROpen（model3.json 的 EyeBlink 组）
    - 呼吸：ParamAngleX/Y/Z、ParamBodyAngleX、ParamBreath
    - 物理输出：模型的 physics3.json 里的输出参数（Parami / Param_Angle_Rotation_*），
      它们只有物理真的在跑并且每帧**恰好求值一次**才会跟着动
      （算两次会让输出以双倍速度漂移，算零次则一直不变）。

  没装模型就跳过（禁二传，不入库）。*/
void TestLive2DOffscreen::drivesParametersWithUpdaters()
{
    const QString dir = modelDir();
    if (!QFileInfo::exists(dir + QStringLiteral("/miku.model3.json"))
        && !QFileInfo::exists(dir + QStringLiteral("/atri_8.model3.json")))
        QSKIP("本机没有模型（禁二传，不入库），跳过参数驱动器验证");

    const QString modelJsonName = QFileInfo::exists(dir + QStringLiteral("/miku.model3.json"))
                                      ? QStringLiteral("miku.model3.json")
                                      : QStringLiteral("atri_8.model3.json");

    Live2DOffscreenRenderer renderer;
    QString error;
    QVERIFY2(renderer.load(dir, modelJsonName, &error), qPrintable(error));

    const QSize targetSize(600, 700);
    QVERIFY(!renderer.renderFrame(targetSize).isNull());

    /*要观察的参数：眨眼 / 呼吸用官方样例那套名字；物理输出取该模型 physics3.json
      里真实存在的参数名（多试几个，至少有一个会动）。*/
    const QStringList watched = {
        QStringLiteral("ParamEyeLOpen"),   QStringLiteral("ParamEyeROpen"),
        QStringLiteral("ParamAngleX"),     QStringLiteral("ParamAngleY"),
        QStringLiteral("ParamAngleZ"),     QStringLiteral("ParamBodyAngleX"),
        QStringLiteral("ParamBreath"),     QStringLiteral("Parami"),
        QStringLiteral("Param_Angle_Rotation_1_ArtMesh48"),
        QStringLiteral("Param_Angle_Rotation_1_ArtMesh79"),
    };

    QHash<QString, float> first;
    for (const QString &id : watched)
        first.insert(id, renderer.parameterValue(id));

    // 真的让时间过去：呼吸周期 3.2~15.5s，眨眼间隔 1~4s，取样 4s 足够覆盖
    constexpr int kFrames = 40;
    for (int i = 0; i < kFrames; ++i)
    {
        QThread::msleep(100);
        QVERIFY(!renderer.renderFrame(targetSize).isNull());
    }

    int movedParameters = 0;
    for (const QString &id : watched)
    {
        const float before = first.value(id);
        const float after = renderer.parameterValue(id);
        if (!qFuzzyCompare(before, after))
        {
            ++movedParameters;
            qInfo("参数驱动器验证：%s %f → %f", qPrintable(id), before, after);
        }
    }

    QVERIFY2(movedParameters >= 2,
             qPrintable(QStringLiteral("跑完 %1 帧后只有 %2 个参数变化，参数驱动器没接上"
                                       "（呼吸/眨眼/物理至少要有两个在动）")
                            .arg(kFrames)
                            .arg(movedParameters)));
}

/*测每帧渲染成本，给"帧循环能跑多少 fps / 要不要降分辨率"提供真实数据，并作为性能守门。
  为什么必须有这条：窗口层要按帧重绘，帧成本直接决定帧率档位与是否需要降采样。
  实测值随机器/驱动不同，所以这里只设一个**宽松退步警戒线**，真实数字打到日志里。*/
void TestLive2DOffscreen::reportsFrameCost()
{
    const QString dir = modelDir();
    const QString modelJsonName = QStringLiteral("miku.model3.json");
    if (!QFileInfo::exists(dir + QLatin1Char('/') + modelJsonName))
        QSKIP("本机没有初音模型（禁二传，不入库），跳过帧成本测量");

    Live2DOffscreenRenderer renderer;
    QString error;
    QVERIFY2(renderer.load(dir, modelJsonName, &error), qPrintable(error));

    struct Result
    {
        QSize size;
        double msPerFrame;
    };
    QVector<Result> results;

    const int frameCount = 30;
    for (const QSize &size : {QSize(760, 900), QSize(380, 450)})
    {
        (void)renderer.renderFrame(size); // 首帧含 GL 目标重建，不计入
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < frameCount; ++i)
            (void)renderer.renderFrame(size);
        const double msPerFrame = static_cast<double>(timer.elapsed()) / frameCount;
        results.append({size, msPerFrame});
        qInfo("帧成本：%dx%d 平均 %.1f ms/帧（约 %.1f fps）", size.width(), size.height(),
              msPerFrame, msPerFrame > 0.0 ? 1000.0 / msPerFrame : 0.0);
    }

    // 退步警戒：只要不是崩坏级变慢就放过（真实性能靠上面日志观察，不在这里卡死阈值）
    for (const Result &r : results)
    {
        QVERIFY2(r.msPerFrame < 500.0,
                 qPrintable(QStringLiteral("%1x%2 每帧 %.1f ms，慢到不可用")
                                .arg(r.size.width())
                                .arg(r.size.height())
                                .arg(r.msPerFrame)));
    }
}

QTEST_MAIN(TestLive2DOffscreen)
#include "test_live2doffscreen.moc"
