#ifndef SYNTHETICWAV_H
#define SYNTHETICWAV_H

#include <QByteArray>
#include <QVector>

#include <cmath>

/*===== 测试专用的合成 WAV（16 位 PCM RIFF/WAVE）=====

  为什么测试需要它：包络的正确性只能用**响度已知**的音频来证伪，而真实 TTS 字节来自
  本机的 vits-simple-api（测试既不能依赖外部服务，也不该把一段真实语音入库）。
  合成一段"前半有声、后半静音"的信号，就得到了一份形状完全已知的输入：
  包络必须在有声段高、在静音段为 0。

  为什么放在 tests/ 而不是 utils/：它不是产品代码。任何"测试专用的宽松构造"都不该进
  产品类 —— 一旦进去，那种宽松语义迟早会渗进生产路径。

  ⚠️ 只生成**能解析**的输入（合法 16 位 PCM）。"不能解析"的那些输入由用例自己
  改字节造出来（例如把 wFormatTag 改成 3 = IEEE float），这样"到底改坏了什么"一目了然。*/
struct SyntheticWavSegment
{
    int milliseconds = 0;   //这一段多长
    float amplitude = 0.0f; //峰值幅度（0~1，相对 16 位满量程）；0 = 数字静音
};

/*把若干段正弦/静音顺序拼成一份 WAV。
  正弦频率固定 220Hz、**相位跨段连续** —— 段边界不产生额外冲击，否则包络里会凭空
  多出一个尖峰，把"形状"断言搅浑。*/
inline QByteArray buildPcm16Wav(int sampleRate, int channels,
                                const QVector<SyntheticWavSegment> &segments)
{
    const double kPi = 3.14159265358979323846;
    QVector<qint16> samples;
    double phase = 0.0;
    const double phaseStep = 2.0 * kPi * 220.0 / double(sampleRate > 0 ? sampleRate : 1);
    const int channelCount = channels > 0 ? channels : 1;

    for (const SyntheticWavSegment &segment : segments)
    {
        const int frames = sampleRate * segment.milliseconds / 1000;
        for (int frame = 0; frame < frames; ++frame)
        {
            const double value = std::sin(phase) * double(segment.amplitude);
            phase += phaseStep;
            const qint16 quantised = qint16(std::lround(value * 32767.0));
            for (int channel = 0; channel < channelCount; ++channel)
                samples.append(quantised);
        }
    }

    const quint32 dataBytes = quint32(samples.size()) * 2;
    const quint16 blockAlign = quint16(channelCount * 2);
    QByteArray wav;
    wav.reserve(44 + int(dataBytes));

    const auto putTag = [&wav](const char *tag) { wav.append(tag, 4); };
    const auto putU16 = [&wav](quint16 value) {
        wav.append(char(quint8(value & 0xff)));
        wav.append(char(quint8((value >> 8) & 0xff)));
    };
    const auto putU32 = [&wav](quint32 value) {
        wav.append(char(quint8(value & 0xff)));
        wav.append(char(quint8((value >> 8) & 0xff)));
        wav.append(char(quint8((value >> 16) & 0xff)));
        wav.append(char(quint8((value >> 24) & 0xff)));
    };

    putTag("RIFF");
    putU32(36 + dataBytes);
    putTag("WAVE");
    putTag("fmt ");
    putU32(16);            //fmt 块长度：PCM 的规范值
    putU16(1);             //wFormatTag = 1 = PCM
    putU16(quint16(channelCount));
    putU32(quint32(sampleRate));
    putU32(quint32(sampleRate) * blockAlign); //nAvgBytesPerSec
    putU16(blockAlign);
    putU16(16);            //wBitsPerSample
    putTag("data");
    putU32(dataBytes);
    for (const qint16 sample : samples)
    {
        wav.append(char(quint8(quint16(sample) & 0xff)));
        wav.append(char(quint8((quint16(sample) >> 8) & 0xff)));
    }
    return wav;
}

#endif // SYNTHETICWAV_H
