#include <QtTest>

#include "../utils/AudioEnvelope.h"
#include "SyntheticWav.h"

#include <QByteArray>

/*TTS 音频的**响度包络**（用户诉求：说话时嘴动、句子之间的停顿要闭嘴）。

  为什么这套用例值得单独存在：包络是"停顿闭嘴"这件事的**唯一数据来源**。
  它算错的两种方式都很安静、也很难从画面上反推：
    - 归一化用最大值当参考 → 某一声重音就把整句压扁，嘴从此"张不开"；
    - 参考值取整个文件 → 一句里大半是静音时，包络整体被抬高，静音段不再为 0。
  这两条分别由 singleLoudSampleDoesNotSquashTheEnvelope 与
  wavEnvelopeFollowsLoudAndSilentHalves 钉住。

  另一半是**回退契约**：不能解析 PCM 时（用户把 vits 的 format 配成 mp3）必须
  "没有包络"交回盲扑动，绝不许报错、崩、或者给出一堆垃圾电平
  —— 由 nonPcmBufferYieldsNoEnvelope 与 silentBufferYieldsNoEnvelope 钉住。*/
class TestAudioEnvelope : public QObject
{
    Q_OBJECT

  private slots:
    /*头条：前半句有声、后半句静音 → 电平必须"高 → 精确 0"（静音段闭嘴的定量前提）*/
    void wavEnvelopeFollowsLoudAndSilentHalves();
    /*参考值必须是**高分位**而不是最大值：一个重音不许把整句压扁*/
    void singleLoudSampleDoesNotSquashTheEnvelope();
    /*全静音算不出参考值 → 无包络（否则嘴会整句冻在心情值上）*/
    void silentBufferYieldsNoEnvelope();
    /*非 PCM（mp3/8 位/浮点/截断/空）→ 无包络、不崩、电平恒 0*/
    void nonPcmBufferYieldsNoEnvelope();
};

/*一个窗口多少毫秒 —— 与 Dialog 的采样节拍同量级（50ms 采样、20ms 窗口 =
  每个采样点最多跨 2~3 个窗口，包络的时间分辨率因此高于采样节拍）。*/
static constexpr int kExpectedWindowMs = 20;

void TestAudioEnvelope::wavEnvelopeFollowsLoudAndSilentHalves()
{
    /*1.0s 有声（峰值 0.6 ⇒ RMS ≈ 0.424）+ 1.0s 数字静音。
        22050Hz、20ms 一窗 ⇒ 每窗 441 帧，共 44100 帧 = 100 窗（前 50 有声、后 50 静音）。*/
    const QByteArray wav = buildPcm16Wav(22050, 1, {{1000, 0.6f}, {1000, 0.0f}});
    const AudioEnvelope envelope = AudioEnvelope::fromWavBytes(wav);

    QVERIFY2(envelope.isValid(), "合成出来的合法 16 位 PCM WAV 解析不出包络");
    QCOMPARE(envelope.sampleRate(), 22050);
    QCOMPARE(envelope.windowMs(), kExpectedWindowMs);
    QCOMPARE(envelope.levelCount(), 100);

    /*有声段：归一化后应当接近满电平（参考值就取自这一段，所以它就是 1.0 那一档）*/
    QVERIFY2(envelope.levelAtMs(100) > 0.9f,
             qPrintable(QStringLiteral("有声段 100ms 处电平只有 %1（应当接近 1）")
                            .arg(double(envelope.levelAtMs(100)))));
    QVERIFY2(envelope.levelAtMs(950) > 0.9f,
             qPrintable(QStringLiteral("有声段末尾 950ms 处电平只有 %1（应当接近 1）")
                            .arg(double(envelope.levelAtMs(950)))));

    /*静音段：必须是**精确的 0**，不是"接近 0"。
        这一条就是用户诉求的定量形式：句间停顿里电平为 0，嘴才会停在心情值上。
        窗口 0~49 是有声、50~99 是静音；1001ms 落在第 50 窗。*/
    QCOMPARE(envelope.levelAtMs(1001), 0.0f);
    QCOMPARE(envelope.levelAtMs(1500), 0.0f);
    QCOMPARE(envelope.levelAtMs(1990), 0.0f);

    /*超范围的位置要**夹取**（播放器 position 偶尔会越过末尾），不许越界读、不许崩*/
    QCOMPARE(envelope.levelAtMs(-500), envelope.levelAtMs(0));
    QCOMPARE(envelope.levelAtMs(100000), 0.0f);

    int loudWindows = 0;
    for (int index = 0; index < envelope.levelCount(); ++index)
    {
        if (envelope.levelAt(index) > 0.5f)
            ++loudWindows;
        QVERIFY2(envelope.levelAt(index) >= 0.0f && envelope.levelAt(index) <= 1.0f,
                 qPrintable(QStringLiteral("第 %1 窗电平 %2 越出 [0,1]")
                                .arg(index)
                                .arg(double(envelope.levelAt(index)))));
    }
    QCOMPARE(loudWindows, 50);
    qInfo("ENVELOPE loud/silent: windowMs=%d levels=%d loudWindows=%d "
          "level@100ms=%.4f level@1500ms=%.4f",
          envelope.windowMs(), envelope.levelCount(), loudWindows,
          double(envelope.levelAtMs(100)), double(envelope.levelAtMs(1500)));
}

void TestAudioEnvelope::singleLoudSampleDoesNotSquashTheEnvelope()
{
    /*1980ms 低声（峰值 0.1 ⇒ RMS ≈ 0.0707）+ 20ms 满幅重音（RMS ≈ 0.707）。
        参考值若取**最大值**，低声段会被压到 0.1 以下、再被噪声门清零 ——
        整句的嘴几乎不动（正是"听不清她在说什么"的那种观感）。
        取 95 分位则落在低声段上（100 窗里只有 1 窗是重音），低声段归一化到接近 1。*/
    const QByteArray wav = buildPcm16Wav(22050, 1, {{1980, 0.1f}, {20, 1.0f}});
    const AudioEnvelope envelope = AudioEnvelope::fromWavBytes(wav);
    QVERIFY2(envelope.isValid(), "合成 WAV 解析不出包络");
    QCOMPARE(envelope.levelCount(), 100);

    const float quietLevel = envelope.levelAtMs(500);
    const float accentLevel = envelope.levelAtMs(1990);
    qInfo("ENVELOPE percentile: quiet=%.4f accent=%.4f", double(quietLevel),
          double(accentLevel));

    QVERIFY2(quietLevel > 0.9f,
             qPrintable(QStringLiteral("一个 20ms 的重音就把整句压到 %1 —— "
                                       "参考值取的是最大值，不是高分位")
                            .arg(double(quietLevel))));
    /*重音本身只许夹到 1.0，不许出现 >1 的"放大后的电平"（渲染器按 0~1 语义使用它）*/
    QVERIFY2(accentLevel <= 1.0f + 1e-6f,
             qPrintable(QStringLiteral("重音窗电平 %1 > 1").arg(double(accentLevel))));
    QVERIFY2(accentLevel >= quietLevel,
             qPrintable(QStringLiteral("重音窗电平 %1 反而低于低声段 %2")
                            .arg(double(accentLevel))
                            .arg(double(quietLevel))));
}

void TestAudioEnvelope::silentBufferYieldsNoEnvelope()
{
    /*全数字静音：任何归一化都除不出参考值来。
        此时**故意**给"没有包络"而不是"一串 0"：给 0 会让渲染器进入包络模式、
        整句把嘴冻在心情值上一动不动；交回盲扑动至少还是"在说话"的样子。
        （这也是"绝不许静音/绝不许崩"这条契约的一部分。）*/
    const AudioEnvelope envelope =
        AudioEnvelope::fromWavBytes(buildPcm16Wav(22050, 1, {{500, 0.0f}}));
    QVERIFY2(!envelope.isValid(), "全静音的音频不该给出包络（嘴会整句冻住）");
    QCOMPARE(envelope.levelAtMs(200), 0.0f);
    QCOMPARE(envelope.levelAt(0), 0.0f);
}

void TestAudioEnvelope::nonPcmBufferYieldsNoEnvelope()
{
    const QByteArray good = buildPcm16Wav(22050, 1, {{200, 0.5f}});

    /*① 完全不是音频：mp3 帧头 / 随便一段文本。
        用户把 vits 的 format 配成 mp3 时走的就是这条路 —— 必须安静地回退。*/
    const QByteArray mp3Like = QByteArray::fromHex("49443304000000000000") + "not audio at all";
    QVERIFY2(!AudioEnvelope::fromWavBytes(mp3Like).isValid(), "垃圾字节被当成了 WAV");
    QVERIFY2(!AudioEnvelope::fromWavBytes(QByteArray()).isValid(), "空字节被当成了 WAV");
    QVERIFY2(!AudioEnvelope::fromWavBytes(good.left(20)).isValid(),
             "只有半个 RIFF 头的字节被当成了 WAV");

    /*② 合法 RIFF/WAVE，但位深不是 16（8 位 PCM）—— 按 16 位读会得到一堆噪声电平*/
    QByteArray eightBit = good;
    eightBit[34] = 8; //wBitsPerSample 低字节
    QVERIFY2(!AudioEnvelope::fromWavBytes(eightBit).isValid(), "8 位 PCM 被当成了 16 位");

    /*③ 合法 RIFF/WAVE，但编码是 IEEE float（wFormatTag = 3）*/
    QByteArray floatFormat = good;
    floatFormat[20] = 3;
    QVERIFY2(!AudioEnvelope::fromWavBytes(floatFormat).isValid(), "浮点 PCM 被当成了定点");

    /*④ data 块长度为 0*/
    QByteArray emptyData = good;
    const int dataSizeOffset = 40;
    for (int index = 0; index < 4; ++index)
        emptyData[dataSizeOffset + index] = 0;
    QVERIFY2(!AudioEnvelope::fromWavBytes(emptyData).isValid(), "空 data 块被当成了有内容");

    /*无包络时的电平必须是 0，且任何位置查询都不许越界/崩*/
    const AudioEnvelope none = AudioEnvelope::fromWavBytes(mp3Like);
    QCOMPARE(none.levelAtMs(-1), 0.0f);
    QCOMPARE(none.levelAtMs(0), 0.0f);
    QCOMPARE(none.levelAtMs(1'000'000), 0.0f);
    QCOMPARE(none.levelAt(-1), 0.0f);
    QCOMPARE(none.levelAt(999), 0.0f);
    QCOMPARE(none.levelCount(), 0);
    qInfo("ENVELOPE non-PCM: mp3/empty/truncated/8bit/float/empty-data 全部回退（无包络）");
}

QTEST_MAIN(TestAudioEnvelope)
#include "test_audioenvelope.moc"
