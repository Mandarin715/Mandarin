#include <QtTest>

#include "../GlobalConstants.h"
#include "../utils/Live2DOffscreenRenderer.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QSet>
#include <QSettings>
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

    /*---------- 心情过渡与说话嘴巴（本阶段新增） ---------- */
    /*过渡的四条观察量：起点 / 终点 / 途中不跳 / 同心情幂等*/
    void moodBlendStartsFromPreviousValue();
    void moodBlendReachesTargetExactly();
    void moodBlendHasNoSingleFrameJump();
    void applyingSameMoodTwiceDoesNotRestartBlend();
    /*过渡时长必须真的来自参数（硬编码 200ms 的实现过不了）*/
    void moodBlendDurationIsConfigurable();
    /*说话时嘴巴要动、不许越界、不许碰别人*/
    void speakingMouthFlapVariesAndStaysInRange();
    void speakingDoesNotDisturbOtherParameters();
    /*停止说话后扑动必须回落（不能突然闭嘴）*/
    void stoppingSpeechDecaysFlapToMoodValue();

  private:
    static QString modelDir();
    static QString outputDir();

    /*本机任意一个可用模型的目录（可能为空 = 本机没有模型）。*/
    static QString availableModelDir();
    /*在 availableModelDir() 里挑一个能装载的 model3.json（可能为空）。*/
    static QString availableModelJson(const QString &dir);
    /*装载本机任意一个模型。失败返回 false，调用方 QSKIP。
       **不写死模型名**：模型禁二传、不入库，写死就变成"在开发机上绿、换台机器全跳过"。*/
    static bool loadAnyModel(Live2DOffscreenRenderer *renderer, QString *error);

    /*这两个探测参数不在本模型里时 QSKIP（在函数里调用，不能抽成返回 bool 的普通函数：
       QSKIP 是宏，必须能 return 出调用方的 void 函数）。*/
    void requireBlendProbeParameters(Live2DOffscreenRenderer *renderer) const;
    /*嘴巴扑动用例的前提：模型必须声明 ParamMouthOpenY 与 ParamMouthForm（不存在就跳过）。*/
    void requireMouthParameters(Live2DOffscreenRenderer *renderer) const;

    /*这些用例量的是**参数过渡**，量的是模型的值，与像素无关，
       所以统一用 240x300 的小画布：渲染成本低一个数量级，采样帧数就能给足。
       画布一变 FBO 会重建一次，之后就稳定复用。*/
    static constexpr int kBlendProbeWidth = 240;
    static constexpr int kBlendProbeHeight = 300;

    /*本模型里**不归任何驱动器管**的参数（眼睛/眉毛/腮红/嘴形）：
       读回值就是覆盖表写下的值本身，可以用来量过渡曲线。
       姿势类参数（ParamAngleX/ParamBodyAngleX）绝对不能用 —— 呼吸驱动器每帧往它们身上
       **加**一个摆动量，读回值是"覆盖值 + 摆动"，会把过渡曲线淹掉。*/
    static const QString kMoodProbeParameter;   // ParamEyeLSmile：0 → 0.9
    static const QString kSecondProbeParameter; // ParamBrowLY
    static const QString kMouthOpenParameter;   // ParamMouthOpenY（只被嘴巴扑动驱动）
    static const QString kMouthFormParameter;   // ParamMouthForm（属于心情，绝不该被扑动碰）

    /*跑 n 帧真实时间：每帧之间等 waitMs 再渲一帧。
       **必须真的等**：帧的时间步长来自墙钟（renderFrame 内部 clock.restart()），
       紧循环连调 renderFrame() 的话两帧只隔几微秒 → 过渡永远走不完。
       values 非空时把每帧读到的 probeParameter 记下来。
       injectedDeltaSeconds > 0 时**注入固定步长**（走 setNextFrameDeltaForTest，仍是同一条
       夹取路径），于是"虚拟时间"由调用方钉住、与机器负载无关；此时不再 sleep（步长已经是
       注入的，等待没有意义，也让用例更快）。需要真实墙钟的用例传默认值 0 即可。*/
    static bool advanceFrames(Live2DOffscreenRenderer *renderer, const QString &probeParameter,
                              int frames, int waitMs, QVector<float> *values,
                              float injectedDeltaSeconds = 0.0f);

    /*一直推进到 probeParameter 落到 target（或帧数上限用完）。
       为什么要"等到"而不是"猜几帧"：每帧的墙钟时间 = waitMs + 渲染成本，
       而渲染成本随机器/画布/驱动变化（实测 240x300 画布上约 2~4ms，加上 20ms 等待
       约 24ms 一帧）。写死帧数的话，"起点/终点读数"会随机器而变 ——
       在一个更快的机器上先导阶段还没到目标，后续对照就失去意义。*/
    struct SettleResult
    {
        float value = 0.0f;
        int frames = 0;
        bool reached = false;
    };
    static SettleResult advanceUntilSettled(Live2DOffscreenRenderer *renderer,
                                            const QString &probeParameter, float target,
                                            int maxFrames, int waitMs);
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

/*探测参数的选择理由（这三个名字就是断言的核心前提）：
  - ParamEyeLSmile / ParamBrowLY：不归眨眼/呼吸/物理任何驱动器管，读回 = 覆盖值；
  - ParamMouthOpenY：ParamMouthForm 声明范围是 [-1,0]（0 已是"最笑"），
    而 OpenY 是 [0,1] 的开口量，嘴巴扑动只能动它。*/
const QString TestLive2DOffscreen::kMoodProbeParameter = QStringLiteral("ParamEyeLSmile");
const QString TestLive2DOffscreen::kSecondProbeParameter = QStringLiteral("ParamBrowLY");
const QString TestLive2DOffscreen::kMouthOpenParameter = QStringLiteral("ParamMouthOpenY");
const QString TestLive2DOffscreen::kMouthFormParameter = QStringLiteral("ParamMouthForm");

void TestLive2DOffscreen::requireBlendProbeParameters(Live2DOffscreenRenderer *renderer) const
{
    const QHash<QString, Live2DOffscreenRenderer::DeclaredRange> ranges =
        renderer->declaredParameterRanges();
    for (const QString &id : {kMoodProbeParameter, kSecondProbeParameter})
    {
        if (!ranges.contains(id))
        {
            QSKIP("本机模型里没有这个探测参数（换模型了？），跳过心情过渡验证");
        }
    }
}

void TestLive2DOffscreen::requireMouthParameters(Live2DOffscreenRenderer *renderer) const
{
    const QHash<QString, Live2DOffscreenRenderer::DeclaredRange> ranges =
        renderer->declaredParameterRanges();
    for (const QString &id : {kMouthOpenParameter, kMouthFormParameter})
    {
        if (!ranges.contains(id))
            QSKIP("本机模型里没有嘴巴参数（换模型了？），跳过说话嘴巴验证");
    }
}

/*本机有没有模型、有哪一个 —— 不写死模型名。
  这里按下面的顺序找一个能用的：
    1) MANDARIN_LIVE2D_MODEL_DIR（显式指定，最高优先）；
    2) config.ini 的 character/live2dModel（用户实际在用的那个）；
    3) 默认 miku 目录（兼容既有的 rendersMikuFirstFrame / reportsFrameCost）；
    4) 模型根目录下的**第一个**含 *.model3.json 的子目录（兜底）。
  找不到返回空 QString，调用方 QSKIP。*/
QString TestLive2DOffscreen::availableModelDir()
{
    const QByteArray fromEnv = qgetenv("MANDARIN_LIVE2D_MODEL_DIR");
    if (!fromEnv.isEmpty())
        return QString::fromLocal8Bit(fromEnv);

    QSettings settings(IniSettingPath, QSettings::IniFormat);
    const QString configured =
        settings.value(QStringLiteral("character/live2dModel")).toString().trimmed();
    if (!configured.isEmpty())
    {
        const QString configuredDir = QDir(Live2DModelRootPath).filePath(configured);
        if (!availableModelJson(configuredDir).isEmpty())
            return configuredDir;
    }

    const QString defaultDir = modelDir();
    if (!availableModelJson(defaultDir).isEmpty())
        return defaultDir;

    const QDir root(Live2DModelRootPath);
    const QStringList entries = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &entry : entries)
    {
        const QString candidate = root.filePath(entry);
        if (!availableModelJson(candidate).isEmpty())
            return candidate;
    }
    return QString();
}

QString TestLive2DOffscreen::availableModelJson(const QString &dir)
{
    if (dir.isEmpty())
        return QString();
    const QDir modelDir(dir);
    const QStringList found = modelDir.entryList({QStringLiteral("*.model3.json")}, QDir::Files);
    return found.isEmpty() ? QString() : found.first();
}

bool TestLive2DOffscreen::loadAnyModel(Live2DOffscreenRenderer *renderer, QString *error)
{
    if (renderer == nullptr)
        return false;
    const QString dir = availableModelDir();
    if (dir.isEmpty())
        return false;
    const QString json = availableModelJson(dir);
    if (json.isEmpty())
        return false;
    return renderer->load(dir, json, error);
}

bool TestLive2DOffscreen::advanceFrames(Live2DOffscreenRenderer *renderer,
                                        const QString &probeParameter, int frames, int waitMs,
                                        QVector<float> *values, float injectedDeltaSeconds)
{
    if (renderer == nullptr || frames <= 0)
        return false;
    const QSize targetSize(kBlendProbeWidth, kBlendProbeHeight);
    for (int index = 0; index < frames; ++index)
    {
        if (injectedDeltaSeconds > 0.0f)
            renderer->setNextFrameDeltaForTest(injectedDeltaSeconds);
        else
            QThread::msleep(static_cast<unsigned long>(waitMs > 0 ? waitMs : 0));
        if (renderer->renderFrame(targetSize).isNull())
            return false;
        if (values != nullptr)
            values->append(renderer->parameterValue(probeParameter));
    }
    return true;
}

TestLive2DOffscreen::SettleResult TestLive2DOffscreen::advanceUntilSettled(
    Live2DOffscreenRenderer *renderer, const QString &probeParameter, float target,
    int maxFrames, int waitMs)
{
    SettleResult result;
    if (renderer == nullptr || maxFrames <= 0)
        return result;
    for (int index = 0; index < maxFrames; ++index)
    {
        if (!advanceFrames(renderer, probeParameter, 1, waitMs, nullptr))
            return result;
        result.frames = index + 1;
        result.value = renderer->parameterValue(probeParameter);
        if (result.value == target)
        {
            result.reached = true;
            return result;
        }
    }
    return result;
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

/*==================== 心情过渡（mood blend） ====================

  用户看到的症状：换心情（尤其是切回 neutral）时"顿一下" —— 参数从 0.9 一帧跳到 0。

  这一组用例把「过渡」这件事拆成四条彼此独立的观察量，每条都对应一种错的实现：
    (a) 起点：刚换心情的那几帧参数仍在**上一个值附近**（一帧就跳过去的实现在这里失败）；
    (b) 终点：过了过渡时长后参数**精确等于**目标值（纯指数逼近的实现在这里失败 ——
        指数永远到不了，读回的永远是个"接近但不等于"的数）；
    (c) 途中：逐帧变化量有上界（退回"一帧跳完"的实现在这里失败）；
    (d) 幂等：同一个心情再施加一次**不重启**过渡（"每次拿目标值重置当前值"的实现会被
        这条抓住：参数被拽回起点、出现一个反向跳变）。

  为什么量参数而不是量像素：过渡是**时间**上的曲线，像素差里混着呼吸/眨眼/物理的漂移，
  分不出"过渡走了多少"和"背景动了多少"。参数读回是直接证据。

  ⚠️ 帧的时间步长来自墙钟（renderFrame 内部的 clock.restart()），所以每帧之间**必须真的等**；
  紧循环连调 renderFrame() 只隔几微秒，过渡永远走不完，这条测试会假通过。
  这也是为什么这里统一用 240x300 的小画布：把省下的渲染时间换成采样帧数。

  这一组是**渲染器级**用例（不需要窗口/心情预设数据）：过渡的数学与"心情"这个词无关，
  用一组显式的覆盖值就能把曲线量清楚，模型也只要求"本机有任意一个模型"。*/
void TestLive2DOffscreen::moodBlendStartsFromPreviousValue()
{
    Live2DOffscreenRenderer renderer;
    QString error;
    if (!loadAnyModel(&renderer, &error))
        QSKIP("本机没有 Live2D 模型（禁二传，不入库），跳过心情过渡验证");
    requireBlendProbeParameters(&renderer);
    QVERIFY(!renderer.renderFrame(QSize(kBlendProbeWidth, kBlendProbeHeight)).isNull());

    // 过渡给足 1s：这条要观察的是"起点"，时长越长起点读数越清晰
    renderer.setMoodBlendDurationMs(1000);
    renderer.setParameterOverrides({{kMoodProbeParameter, 0.9f}});
    /*先导阶段：**推进到真的到达**，不靠"猜几帧"。
       帧时间 = 等待 + 渲染成本，随机器/画布/驱动变化，写死帧数会让"起点读数"随机器而变 ——
       先导阶段没到 0.9 的话，下面 afterOneFrame 的对照就失去意义。*/
    const SettleResult leadIn =
        advanceUntilSettled(&renderer, kMoodProbeParameter, 0.9f, 300, 20);
    QVERIFY2(leadIn.reached,
             qPrintable(QStringLiteral("先导阶段：%1 在 %2 帧内都没到 0.9（当前 %3）")
                            .arg(kMoodProbeParameter)
                            .arg(leadIn.frames)
                            .arg(double(leadIn.value))));
    const float before = leadIn.value;

    /*换心情：**一帧都不渲**就立刻读一次。
       这一条在旧实现上也会通过（覆盖值要等下一帧 tick 才写进模型），留着它是为了把
       "当帧就跳"这种更粗暴的实现也钉住 —— 真正的判据是紧接着的下一帧。*/
    renderer.setParameterOverrides({{kMoodProbeParameter, 0.0f}});
    const float immediate = renderer.parameterValue(kMoodProbeParameter);
    QVERIFY2(qAbs(immediate - before) <= 0.15f,
             qPrintable(QStringLiteral("换心情当帧 %1 就跳到了 %2（上一帧 %3）")
                            .arg(kMoodProbeParameter)
                            .arg(double(immediate))
                            .arg(double(before))));

    QVERIFY(advanceFrames(&renderer, kMoodProbeParameter, 1, 20, nullptr));
    const float afterOneFrame = renderer.parameterValue(kMoodProbeParameter);
    qInfo("BLEND start[%s]: before=%.4f immediate=%.4f afterOneFrame=%.4f",
          qPrintable(kMoodProbeParameter), double(before), double(immediate),
          double(afterOneFrame));
    /*1s 过渡、一帧约 20ms ⇒ 这一帧最多走 2% 的量程（0.018），给 6 倍余量。
       旧实现下 afterOneFrame 直接是 0.0000 —— 与 before 差 0.9，必然失败。*/
    QVERIFY2(qAbs(afterOneFrame - before) <= 0.12f,
             qPrintable(QStringLiteral("换心情后**第一帧** %1 就从 %2 跳到 %3 —— "
                                       "参数是整组原子替换，过渡没有发生")
                            .arg(kMoodProbeParameter)
                            .arg(double(before))
                            .arg(double(afterOneFrame))));
}

void TestLive2DOffscreen::moodBlendReachesTargetExactly()
{
    Live2DOffscreenRenderer renderer;
    QString error;
    if (!loadAnyModel(&renderer, &error))
        QSKIP("本机没有 Live2D 模型（禁二传，不入库），跳过心情过渡验证");
    requireBlendProbeParameters(&renderer);
    QVERIFY(!renderer.renderFrame(QSize(kBlendProbeWidth, kBlendProbeHeight)).isNull());

    constexpr int kBlendMs = 300;
    renderer.setMoodBlendDurationMs(kBlendMs);

    // 起点：0
    renderer.setParameterOverrides({{kMoodProbeParameter, 0.0f}});
    const SettleResult atZero =
        advanceUntilSettled(&renderer, kMoodProbeParameter, 0.0f, 200, 20);
    QVERIFY2(atZero.reached,
             qPrintable(QStringLiteral("起点没落到 0（当前 %1）").arg(double(atZero.value))));

    /*换到 0.9，等远超过渡时长（300ms 过渡，等 2.4s 的墙钟时间）。
       给足时间是为了断言"**精确等于**"：线性逼近 + 收敛 epsilon 必须落在目标上，
       而指数逼近（value += (target-value)*k*dt）即使等再久也差一点（实测 0.8666）。*/
    renderer.setParameterOverrides({{kMoodProbeParameter, 0.9f}});
    QVERIFY(advanceFrames(&renderer, kMoodProbeParameter, 60, 20, nullptr));
    const float arrived = renderer.parameterValue(kMoodProbeParameter);
    qInfo("BLEND arrival[%s]: target=0.9000 actual=%.9f", qPrintable(kMoodProbeParameter),
          double(arrived));

    /***精确**相等：线性逼近 + 收敛 epsilon 必须落在目标上。
       指数逼近（value += (target-value)*k*dt）在这里永远差一点点，
       所以这条断言就是"必须 snap"的那颗钉子。*/
    QVERIFY2(qAbs(arrived - 0.9f) <= 1e-5f,
             qPrintable(QStringLiteral("过渡时长过后 %1 = %.6f，不等于目标 0.900000 —— "
                                       "逼近必须收敛到目标（snap），不能只是无限接近")
                            .arg(kMoodProbeParameter)
                            .arg(double(arrived))));
}

void TestLive2DOffscreen::moodBlendHasNoSingleFrameJump()
{
    Live2DOffscreenRenderer renderer;
    QString error;
    if (!loadAnyModel(&renderer, &error))
        QSKIP("本机没有 Live2D 模型（禁二传，不入库），跳过心情过渡验证");
    requireBlendProbeParameters(&renderer);
    QVERIFY(!renderer.renderFrame(QSize(kBlendProbeWidth, kBlendProbeHeight)).isNull());

    renderer.setMoodBlendDurationMs(400);
    renderer.setParameterOverrides({{kMoodProbeParameter, 0.0f}});

    QVector<float> samples;
    // 400ms 过渡、每帧约 12ms ⇒ 约 33 帧走完；两个方向各采 50 帧把整个过渡覆盖住
    QVERIFY(advanceFrames(&renderer, kMoodProbeParameter, 50, 12, &samples));
    renderer.setParameterOverrides({{kMoodProbeParameter, 0.9f}});
    QVERIFY(advanceFrames(&renderer, kMoodProbeParameter, 50, 12, &samples));

    QVERIFY2(samples.size() >= 2, "采样帧数不足");
    float maxStep = 0.0f;
    int maxStepIndex = -1;
    for (int index = 1; index < samples.size(); ++index)
    {
        const float step = qAbs(samples[index] - samples[index - 1]);
        if (step > maxStep)
        {
            maxStep = step;
            maxStepIndex = index;
        }
    }
    const float totalRange = 0.9f;
    qInfo("BLEND no-jump[%s]: frames=%d maxStep=%.4f at=%d (range=%.2f)",
          qPrintable(kMoodProbeParameter), samples.size(), double(maxStep), maxStepIndex,
          double(totalRange));

    /*阈值 = 量程的 1/3。为什么这么取：
       - 时间制过渡下每帧位移 ≈ 量程 × dt/过渡时长。400ms 过渡里最坏的一帧即使吃掉
         60ms（本机实测帧时间被抢占时也到不了这个数）也只有 0.9 × 0.15 = 0.135，
         离 0.30 有一倍余量，所以不会因为线程抖动偶发失败；
       - 而"一帧跳完"的实现第一帧就位移 0.9，是阈值的 3 倍，必然被抓到。
       噪声不参与：这个参数不归任何驱动器管（见 kMoodProbeParameter 的说明）。*/
    QVERIFY2(maxStep <= totalRange / 3.0f,
             qPrintable(QStringLiteral("逐帧最大变化 %.4f（第 %1 帧）超过量程的 1/3 —— "
                                       "参数在某一帧整块跳过去了，过渡没有铺开")
                            .arg(double(maxStep))
                            .arg(maxStepIndex)));
}

void TestLive2DOffscreen::applyingSameMoodTwiceDoesNotRestartBlend()
{
    Live2DOffscreenRenderer renderer;
    QString error;
    if (!loadAnyModel(&renderer, &error))
        QSKIP("本机没有 Live2D 模型（禁二传，不入库），跳过心情过渡验证");
    requireBlendProbeParameters(&renderer);
    QVERIFY(!renderer.renderFrame(QSize(kBlendProbeWidth, kBlendProbeHeight)).isNull());

    // 1s 过渡：留出大片"过渡途中"的窗口，重启与不重启的行为差异非常明显
    renderer.setMoodBlendDurationMs(1000);
    renderer.setParameterOverrides({{kMoodProbeParameter, 0.0f}});
    QVERIFY(advanceFrames(&renderer, kMoodProbeParameter, 3, 20, nullptr));

    renderer.setParameterOverrides({{kMoodProbeParameter, 0.9f}});
    QVERIFY(advanceFrames(&renderer, kMoodProbeParameter, 8, 20, nullptr)); // 走到途中的某点
    const float midway = renderer.parameterValue(kMoodProbeParameter);
    QVERIFY2(midway > 0.02f && midway < 0.85f,
             qPrintable(QStringLiteral("采样点 %1 不在过渡途中（%2），本用例的前提不成立")
                            .arg(kMoodProbeParameter)
                            .arg(double(midway))));

    /***同一个**心情再施加一次。它必须什么都不做：不能把当前值重置回 0（那会让参数
       反向跳一下，屏幕上就是一次小回退），也不能重新计时。
       检测手段是把下一帧与"当帧读到的值"对比 —— 正常推进是单调向 0.9 靠近，
       而"重启过渡"的实现会让它掉回起点附近甚至 0。*/
    renderer.setParameterOverrides({{kMoodProbeParameter, 0.9f}});
    const float rightAfterRepeat = renderer.parameterValue(kMoodProbeParameter);
    QVERIFY(advanceFrames(&renderer, kMoodProbeParameter, 1, 20, nullptr));
    const float nextFrame = renderer.parameterValue(kMoodProbeParameter);
    qInfo("BLEND same-mood[%s]: midway=%.4f rightAfterRepeat=%.4f nextFrame=%.4f",
          qPrintable(kMoodProbeParameter), double(midway), double(rightAfterRepeat),
          double(nextFrame));

    // 不跳变
    QVERIFY2(qAbs(nextFrame - rightAfterRepeat) <= 0.10f,
             qPrintable(QStringLiteral("重复施加同一个心情后 %1 从 %2 跳到 %3")
                            .arg(kMoodProbeParameter)
                            .arg(double(rightAfterRepeat))
                            .arg(double(nextFrame))));
    // 而且仍然是"朝目标推进"，不是被重置回起点
    QVERIFY2(nextFrame >= rightAfterRepeat - 1e-4f,
             qPrintable(QStringLiteral("重复施加同一个心情后 %1 反而倒退了（%2 → %3）—— "
                                       "过渡被重新开始了")
                            .arg(kMoodProbeParameter)
                            .arg(double(rightAfterRepeat))
                            .arg(double(nextFrame))));
    QVERIFY2(nextFrame > midway * 0.6f,
             qPrintable(QStringLiteral("重复施加同一个心情后 %1 掉回 %2（途中是 %3）—— "
                                       "过渡被重置了")
                            .arg(kMoodProbeParameter)
                            .arg(double(nextFrame))
                            .arg(double(midway))));
}

void TestLive2DOffscreen::moodBlendDurationIsConfigurable()
{
    Live2DOffscreenRenderer renderer;
    QString error;
    if (!loadAnyModel(&renderer, &error))
        QSKIP("本机没有 Live2D 模型（禁二传，不入库），跳过心情过渡验证");
    requireBlendProbeParameters(&renderer);
    QVERIFY(!renderer.renderFrame(QSize(kBlendProbeWidth, kBlendProbeHeight)).isNull());

    /*同一段过渡、同一套采样节奏，只改过渡时长 → 观察到的过渡长度必须跟着变。
       只断言"能不能到目标"是不够的：一个**硬编码** 200ms 的实现也能到目标，
       但它对配置改动的反应是零 —— 那样用户就调不动手感（与 fps/scale 同款诉求）。

       这条同时是 Config 键的**行为级**钉子：窗口把 character/live2dMoodBlendMs
       喂给 setMoodBlendDurationMs（另有窗口级用例验证这条线），
       而"喂进来的数字真的改变过渡长度"由这里证明。*/
    const auto framesToReachTarget = [&](int blendMs) {
        renderer.setMoodBlendDurationMs(blendMs);
        // 每次都从明确的起点开始（同一组覆盖值 → 不触发过渡）
        renderer.setParameterOverrides({{kMoodProbeParameter, 0.0f}});
        if (!advanceFrames(&renderer, kMoodProbeParameter, 4, 10, nullptr))
            return -1;

        QElapsedTimer clock;
        clock.start();
        renderer.setParameterOverrides({{kMoodProbeParameter, 0.9f}});
        for (int frame = 1; frame <= 400; ++frame)
        {
            if (!advanceFrames(&renderer, kMoodProbeParameter, 1, 10, nullptr))
                return -1;
            if (qAbs(renderer.parameterValue(kMoodProbeParameter) - 0.9f) <= 1e-5f)
            {
                qInfo("BLEND duration probe: %dms → %d frames / %lld ms", blendMs, frame,
                      clock.elapsed());
                return frame;
            }
        }
        return 0; // 400 帧都没到目标
    };

    const int fastFrames = framesToReachTarget(200);
    const int slowFrames = framesToReachTarget(600);
    qInfo("BLEND duration: 200ms→%d frames, 600ms→%d frames (同一采样节奏)", fastFrames,
          slowFrames);

    QVERIFY2(fastFrames > 0 && slowFrames > 0, "过渡在 3s 内都没走到目标");
    /*600ms 是 200ms 的 3 倍，时间制实现下帧数应当也约 3 倍。
       给 2 倍这条下界：帧时间里有 sleep(10) 之外的渲染成本与系统抖动，
       但"3 倍 vs 1 倍"这个量级差绝不会被抖动吃掉；而硬编码时长时两者必然相等。*/
    QVERIFY2(slowFrames >= fastFrames * 2,
             qPrintable(QStringLiteral("过渡时长 200ms 用了 %1 帧、600ms 只用了 %2 帧 —— "
                                       "观察到的过渡长度没有跟着配置变（时长可能是硬编码的）")
                            .arg(fastFrames)
                            .arg(slowFrames)));
}

/*==================== 说话时的嘴巴扑动（anime paper-doll 口型） ====================

  用户要的是"她说话时嘴在动"这一条最朴素的观感：不是音素级 lip-sync，也不是音量驱动，
  而是**动画片纸片人**那种上下开合。所以判据不是"嘴型对不对"，而是三件事：
    (a) 说话时开口量**随时间变化**（不规则、3~5 次/秒量级），且始终落在模型声明的
        [min,max] 里 —— 越过声明范围会被 Core 夹回去，屏幕上表现为"卡在上下限"；
    (b) 不打扰任何别的东西：其他被覆盖的参数一动不动，`ParamMouthForm`（嘴形，属于心情）
        绝对不许被碰 —— 它是嘴的**形状**，与开合是两件事，碰了就会与心情打架；
    (c) 停止说话后扑动**回落到心情自己的值**（不是啪一下闭嘴）。

  ⚠️ 帧时间来自墙钟，所以必须 QTest 式地"真的等"；紧循环连调 renderFrame 不会推进扑动。

  这一组同样是**渲染器级**用例：扑动的数学与"哪句台词"无关，用一组显式覆盖值就能量清楚。*/
void TestLive2DOffscreen::speakingMouthFlapVariesAndStaysInRange()
{
    Live2DOffscreenRenderer renderer;
    QString error;
    if (!loadAnyModel(&renderer, &error))
        QSKIP("本机没有 Live2D 模型（禁二传，不入库），跳过说话嘴巴验证");
    requireMouthParameters(&renderer);
    requireBlendProbeParameters(&renderer);

    const QHash<QString, Live2DOffscreenRenderer::DeclaredRange> ranges =
        renderer.declaredParameterRanges();
    const Live2DOffscreenRenderer::DeclaredRange mouthRange = ranges.value(kMouthOpenParameter);
    QVERIFY2(mouthRange.max > mouthRange.min,
             qPrintable(QStringLiteral("%1 的声明范围是 [%2,%3]，量不出任何东西")
                            .arg(kMouthOpenParameter)
                            .arg(double(mouthRange.min))
                            .arg(double(mouthRange.max))));

    QVERIFY(!renderer.renderFrame(QSize(kBlendProbeWidth, kBlendProbeHeight)).isNull());

    /*心情：嘴形取一个明确的非中立值，用来证明"扑动不碰嘴形"；
       开口量取范围中点（= 说话时围着它上下开合，与预设值"surprised 张嘴"这类情形一致）。
       另外两项（眼笑/眉毛）是"不该被动到"的旁观者。*/
    const float moodMouthOpen = mouthRange.min + (mouthRange.max - mouthRange.min) * 0.5f;
    const QHash<QString, float> mood = {{kMoodProbeParameter, 0.35f},
                                        {kMouthOpenParameter, moodMouthOpen},
                                        {kMouthFormParameter, -0.7f},
                                        {kSecondProbeParameter, 0.25f}};
    renderer.setMoodBlendDurationMs(150);
    renderer.setParameterOverrides(mood);
    const SettleResult settled =
        advanceUntilSettled(&renderer, kMouthOpenParameter, moodMouthOpen, 200, 20);
    QVERIFY2(settled.reached,
             qPrintable(QStringLiteral("心情的开口量没落到 %1（当前 %2）")
                            .arg(double(moodMouthOpen))
                            .arg(double(settled.value))));

    /*说话：注入固定步长的 60 帧 ⇒ **虚拟时间恰好 1.0s**（1/60 步长）。
      为什么不用真实等待：帧增量有 0.1s 的夹取上限，机器一被抢占，"墙钟间隔"与"虚拟时间"
      就脱钩（每帧都吃满 100ms），扑动的相位推进随之变慢、反转次数掉下来 —— 这正是本用例
      此前负载下偶发变红的原因。步长由调用方钉住后，反转次数只由频率决定。*/
    renderer.setSpeaking(true);
    QVector<float> samples;
    QVERIFY(advanceFrames(&renderer, kMouthOpenParameter, 60, 0, &samples, 1.0f / 60.0f));
    QVERIFY2(samples.size() >= 20, "采样帧数不足，量不出扑动");

    float minValue = samples.first();
    float maxValue = samples.first();
    int alternations = 0;
    int previousTrend = 0;
    for (int index = 0; index < samples.size(); ++index)
    {
        minValue = std::min(minValue, samples[index]);
        maxValue = std::max(maxValue, samples[index]);
        if (index == 0)
            continue;
        const int trend = (samples[index] > samples[index - 1]) ? 1 : -1;
        if (previousTrend != 0 && trend != previousTrend)
            ++alternations;
        previousTrend = trend;
    }

    QStringList printSamples;
    for (int index = 0; index < samples.size(); index += 4)
        printSamples << QString::number(double(samples[index]), 'f', 4);
    qInfo("SPEAK flap[%s]: frames=%d min=%.4f max=%.4f span=%.4f alternations=%d "
          "declared=[%.3f,%.3f] samples(every 4th)=%s",
          qPrintable(kMouthOpenParameter), samples.size(), double(minValue), double(maxValue),
          double(maxValue - minValue), alternations, double(mouthRange.min),
          double(mouthRange.max), qPrintable(printSamples.join(QStringLiteral(","))));

    /*(a1) 真的在动。为什么用**相对声明量程**的比例而不是绝对值：
       换模型时量程可能完全不同（0~1 与 0~2 都是常见的），绝对阈值会变成"只对本机模型有效"。
       15% 的量程：本机 atri 上实测开合跨度约 0.55（55% 量程），留了 3 倍余量。*/
    QVERIFY2(maxValue - minValue >= (mouthRange.max - mouthRange.min) * 0.15f,
             qPrintable(QStringLiteral("说话时 %1 只在 %2~%3 之间动（量程 %4~%5）—— 嘴巴没动")
                            .arg(kMouthOpenParameter)
                            .arg(double(minValue))
                            .arg(double(maxValue))
                            .arg(double(mouthRange.min))
                            .arg(double(mouthRange.max))));
    /*(a2) 是"开合"而不是"慢漂移"：方向必须反复改。
       虚拟时间 1.0s、频率 3~5 次/秒 ⇒ 反转次数 = 2×频率×时间 = **6~10 次**。
       下界取 5：3Hz 的 6 次留了余量，而"慢漂移"只有 0~1 次、必然被抓。
       上界取 14：5Hz 的 10 次留了余量，同时"频率被误改回旧的 8~10Hz"会给出 16~20 次、
       也会被抓 —— 两侧都卡住，改错任何一个方向都过不去。*/
    QVERIFY2(alternations >= 5 && alternations <= 14,
             qPrintable(QStringLiteral("说话窗口里开口量换了 %1 次方向（期望 5~14，"
                                       "对应 3~5 次/秒 × 1.0s 虚拟时间）—— 不是开合振荡，"
                                       "或频率被改到了别的量级")
                            .arg(alternations)));
    /*(a3) 一个采样窗内的**打开量**应当是个温和的包络，不是"几乎一直全开"：
       开合比只统计"明显张开"的帧占比，太满（>80%）说明振幅被顶到了上限，
       这种嘴看起来是"张着不动"。*/
    const float openThreshold = moodMouthOpen + (maxValue - moodMouthOpen) * 0.3f;
    int openFrames = 0;
    for (const float value : samples)
    {
        if (value > openThreshold)
            ++openFrames;
    }
    const double openRatio = double(openFrames) / double(samples.size());
    qInfo("SPEAK flap openness: %d/%d frames above %.4f (ratio %.2f)", openFrames,
          samples.size(), double(openThreshold), openRatio);
    QVERIFY2(openRatio < 0.8, qPrintable(QStringLiteral("说话时嘴有 %1%% 的帧处于张开状态 ——"
                                                        "振幅顶在上限上，看着是张着嘴不动")
                                             .arg(openRatio * 100.0, 0, 'f', 1)));
    /*(a4) 永远不许越出模型声明的范围：越界会被 Core 夹回去，屏幕上就是"卡在上下限"。*/
    QVERIFY2(minValue >= mouthRange.min - 1e-4f && maxValue <= mouthRange.max + 1e-4f,
             qPrintable(QStringLiteral("说话时 %1 落到 [%2,%3] 之外（实测 %4~%5）")
                            .arg(kMouthOpenParameter)
                            .arg(double(mouthRange.min))
                            .arg(double(mouthRange.max))
                            .arg(double(minValue))
                            .arg(double(maxValue))));
}

void TestLive2DOffscreen::speakingDoesNotDisturbOtherParameters()
{
    Live2DOffscreenRenderer renderer;
    QString error;
    if (!loadAnyModel(&renderer, &error))
        QSKIP("本机没有 Live2D 模型（禁二传，不入库），跳过说话干扰验证");
    requireMouthParameters(&renderer);
    requireBlendProbeParameters(&renderer);

    QVERIFY(!renderer.renderFrame(QSize(kBlendProbeWidth, kBlendProbeHeight)).isNull());

    constexpr float kMoodMouthOpen = 0.2f;
    constexpr float kMoodMouthForm = -0.7f;
    constexpr float kMoodSmile = 0.4f;
    constexpr float kMoodBrow = 0.25f;
    const QHash<QString, float> mood = {{kMoodProbeParameter, kMoodSmile},
                                        {kMouthOpenParameter, kMoodMouthOpen},
                                        {kMouthFormParameter, kMoodMouthForm},
                                        {kSecondProbeParameter, kMoodBrow}};
    renderer.setMoodBlendDurationMs(150);
    renderer.setParameterOverrides(mood);
    QVERIFY(advanceUntilSettled(&renderer, kMoodProbeParameter, kMoodSmile, 200, 20).reached);

    renderer.setSpeaking(true);
    /*说话 40 帧，每帧把"除嘴巴开合之外"的参数读一遍。
       注意**不能**把 ParamMouthOpenY 放进被检查的集合里（它本来就该动），
       但 ParamMouthForm 必须在 —— 它是嘴形，属于心情，扑动碰它就是与心情打架。*/
    const QStringList watched = {kMouthFormParameter, kMoodProbeParameter, kSecondProbeParameter};
    for (int frame = 0; frame < 40; ++frame)
    {
        QVERIFY(advanceFrames(&renderer, kMouthOpenParameter, 1, 15, nullptr));
        for (const QString &id : watched)
        {
            const float expected = mood.value(id);
            const float actual = renderer.parameterValue(id);
            QVERIFY2(qAbs(actual - expected) <= 1e-4f,
                     qPrintable(QStringLiteral("说话第 %1 帧：%2 = %3，不等于心情值 %4 —— "
                                               "扑动动了不该动的参数（嘴形属于心情）")
                                    .arg(frame)
                                    .arg(id)
                                    .arg(double(actual))
                                    .arg(double(expected))));
        }
    }
    qInfo("SPEAK isolation: %d frames，%s 全程等于心情值 %.3f（未被扑动触碰）", 40,
          qPrintable(kMouthFormParameter), double(kMoodMouthForm));
}

void TestLive2DOffscreen::stoppingSpeechDecaysFlapToMoodValue()
{
    Live2DOffscreenRenderer renderer;
    QString error;
    if (!loadAnyModel(&renderer, &error))
        QSKIP("本机没有 Live2D 模型（禁二传，不入库），跳过停止说话验证");
    requireMouthParameters(&renderer);
    requireBlendProbeParameters(&renderer);

    QVERIFY(!renderer.renderFrame(QSize(kBlendProbeWidth, kBlendProbeHeight)).isNull());

    /*心情故意给一个**非零**开口量：这样"扑动回落到 0"与"回落到心情值"能区分开
       —— 若实现是"停止说话就把嘴合死"，最终值会是 0 而不是这个数。*/
    constexpr float kMoodMouthOpen = 0.32f;
    const QHash<QString, float> mood = {{kMoodProbeParameter, 0.0f},
                                        {kMouthOpenParameter, kMoodMouthOpen},
                                        {kSecondProbeParameter, 0.0f}};
    constexpr int kBlendMs = 200;
    renderer.setMoodBlendDurationMs(kBlendMs);
    renderer.setParameterOverrides(mood);
    QVERIFY(advanceUntilSettled(&renderer, kMouthOpenParameter, kMoodMouthOpen, 200, 20).reached);

    renderer.setSpeaking(true);
    QVector<float> speakingSamples;
    QVERIFY(advanceFrames(&renderer, kMouthOpenParameter, 25, 20, &speakingSamples));
    const float speakingMin = *std::min_element(speakingSamples.constBegin(),
                                                speakingSamples.constEnd());
    const float speakingMax = *std::max_element(speakingSamples.constBegin(),
                                                speakingSamples.constEnd());
    QVERIFY2(speakingMax - speakingMin > 0.05f,
             qPrintable(QStringLiteral("说话阶段没量到扑动（%1~%2），本用例的前提不成立")
                            .arg(double(speakingMin))
                            .arg(double(speakingMax))));

    renderer.setSpeaking(false);
    /*等远超过渡时长（200ms 过渡 ⇒ 采 1.2s 的墙钟）。

       ⚠️ 判据必须看**尾部窗口**而不是整段的极差：衰减是"线性趋近 0"，
       整段极差里含着开头那几百毫秒的残余摆动（那是正确行为，不是没衰减）。
       取最后 1/4 段当"稳态窗口"，才是"扑动已经彻底退场"的证据。*/
    QVector<float> afterSamples;
    QVERIFY(advanceFrames(&renderer, kMouthOpenParameter, 60, 20, &afterSamples));
    const float afterMin = *std::min_element(afterSamples.constBegin(), afterSamples.constEnd());
    const float afterMax = *std::max_element(afterSamples.constBegin(), afterSamples.constEnd());
    const int tailBegin = afterSamples.size() * 3 / 4;
    float tailMin = afterSamples[tailBegin];
    float tailMax = afterSamples[tailBegin];
    for (int index = tailBegin; index < afterSamples.size(); ++index)
    {
        tailMin = std::min(tailMin, afterSamples[index]);
        tailMax = std::max(tailMax, afterSamples[index]);
    }
    qInfo("SPEAK stop[%s]: speaking span=%.4f（%s）→ stopped all=%.4f~%.4f, tail(last 1/4)"
          "=%.4f~%.4f span=%.4f, mood=%.4f",
          qPrintable(kMouthOpenParameter), double(speakingMax - speakingMin),
          qPrintable(QStringLiteral("%1~%2").arg(double(speakingMin)).arg(double(speakingMax))),
          double(afterMin), double(afterMax), double(tailMin), double(tailMax),
          double(tailMax - tailMin), double(kMoodMouthOpen));

    /*(a) 扑动真的退场了：稳态窗口里开口量不再摆动（不是"幅度只衰减一半"）。
       说话的幅度是"整段极差"，对比同一口径的稳态极差。*/
    QVERIFY2(tailMax - tailMin <= 0.02f,
             qPrintable(QStringLiteral("停止说话 1.2s 后开口量仍在 %1~%2 之间摆"
                                       "（稳态极差 %3）—— 扑动没有停")
                            .arg(double(tailMin))
                            .arg(double(tailMax))
                            .arg(double(tailMax - tailMin))));
    /*(b) 回落到**心情自己的值**，不是 0（"停止说话 = 闭嘴"是错的解法）。
       稳态窗口的上下界都要贴在心情值上，所以这条同时挡住"衰减到别的数"与"合死到 0"。*/
    QVERIFY2(qAbs(tailMax - kMoodMouthOpen) <= 0.02f && qAbs(tailMin - kMoodMouthOpen) <= 0.02f,
             qPrintable(QStringLiteral("停止说话后开口量停在 %1~%2，不等于心情值 %3 ——"
                                       "扑动没有干净地回落，或把嘴合死了")
                            .arg(double(tailMin))
                            .arg(double(tailMax))
                            .arg(double(kMoodMouthOpen))));
}

QTEST_MAIN(TestLive2DOffscreen)
#include "test_live2doffscreen.moc"
