#include "AudioEnvelope.h"

#include <algorithm>
#include <cmath>
#include <cstring>

/*===== 参数选择与理由（每一个数都是行为，改前先看这段）=====

  窗口 20ms：包络的时间分辨率。为什么不是 10ms 或 50ms：
    - 50ms 会把一个短促的音节（约 80~150ms）糊成一个台阶，"嘴跟不上字"；
    - 20ms 与语音的音节量级相称，而 Dialog 的采样节拍是 50ms —— 每个采样点最多跨
      2~3 个窗口，分辨率高于节拍，不会因为窗口太粗而丢掉形状；再细就只是白烧 CPU
      （一段 5s / 32kHz 的语音每细一倍就多算一倍样本）。

  参考值取 **95 分位**（不是最大值、不是平均值）：这是"一个重音不许把整句压扁"的解法。
    最大值：某个爆破音/齿音的一窗 RMS 可能是常态的两三倍，整句都被压到 0.3 以下；
    平均值：一句里静音多的时候参考值被拉低，静音段反而被抬起来（不再为 0）；
    95 分位：刚好落在"响亮的常态"上，单窗离群值被排到分位之外。
    实测（合成信号）——低声段 RMS 0.0707 + 一个满幅重音 0.707：
      最大值参考 ⇒ 低声段电平 0.09（嘴基本不动）；95 分位 ⇒ 1.0（正常张合）。

  软噪声门 kNoiseFloor = 0.06（相对参考值）：低于它的窗口电平**精确归零**。
    为什么需要：真实 TTS 的"静音"不是数字零，而是很低的本底（换气、编码残留）。
    没有门限时静音段电平约 0.02~0.05，嘴会留着一条缝一直在微微抖 ——
    而用户要的正是"停顿闭嘴"。0.06 只吃掉"比常态低 24dB 以上"的部分，
    正常语音的弱音节（约 0.2~0.5 参考值）不受影响。*/
namespace
{
constexpr int kWindowMs = 20;
constexpr double kReferencePercentile = 0.95;
constexpr float kNoiseFloor = 0.06f;
/*参考值低于这个数就没有可归一化的东西（整段数字静音）：**没有包络**，
  而不是"一串 0" —— 一串 0 会让渲染器进入包络模式、整句把嘴冻在心情值上。*/
constexpr float kMinUsableReference = 1e-6f;

quint16 readU16(const char *p)
{
    return quint16(quint8(p[0])) | quint16(quint16(quint8(p[1])) << 8);
}

quint32 readU32(const char *p)
{
    return quint32(quint8(p[0])) | (quint32(quint8(p[1])) << 8) | (quint32(quint8(p[2])) << 16)
           | (quint32(quint8(p[3])) << 24);
}

/*在 RIFF/WAVE 里找出 16 位定点 PCM 的 fmt 信息与 data 块。

  为什么只认 16 位定点 PCM：vits-simple-api 默认返回的就是它（实测 RIFF 头，
  32kHz 与 22kHz 各一次）。其余格式（8 位/24 位/浮点/ADPCM/mp3）一律"没有包络" ——
  按错误的位深去读会得到一堆看似合理、实则毫无意义的电平，那比"没有"更糟：
  用户看到的会是"她说话时嘴在乱抖，停顿也不闭"。

  只遍历一次：fmt 一般紧跟在 RIFF/WAVE 之后、data 之前，遇到 data 就停。*/
bool parseRiff16BitPcm(const QByteArray &bytes, int *sampleRate, int *channels, int *dataOffset,
                       int *dataSize)
{
    const int total = bytes.size();
    if (total < 44) //最小合法头：44 字节
        return false;
    const char *p = bytes.constData();
    if (std::memcmp(p, "RIFF", 4) != 0 || std::memcmp(p + 8, "WAVE", 4) != 0)
        return false;

    bool haveFormat = false;
    int formatTag = 0;
    int channelCount = 0;
    int rate = 0;
    int bits = 0;
    int offset = 12;
    while (offset + 8 <= total)
    {
        const char *id = p + offset;
        const quint32 chunkSize = readU32(p + offset + 4);
        const qint64 body = qint64(offset) + 8;
        const qint64 remaining = qint64(total) - body;
        const qint64 usable = std::min<qint64>(qint64(chunkSize), remaining);

        if (std::memcmp(id, "fmt ", 4) == 0)
        {
            if (usable < 16)
                return false;
            formatTag = readU16(p + body);
            channelCount = readU16(p + body + 2);
            rate = int(readU32(p + body + 4));
            bits = readU16(p + body + 14);
            haveFormat = true;
        }
        else if (std::memcmp(id, "data", 4) == 0)
        {
            if (!haveFormat || usable <= 0)
                return false; //data 在 fmt 之前 / data 块是空的：没有可用的包络
            *sampleRate = rate;
            *channels = channelCount;
            *dataOffset = int(body);
            *dataSize = int(usable);
            break;
        }

        /*块按偶数字节对齐（RIFF 规范）。chunkSize 来自**外部字节**，可以任意大 ——
          夹到 total，并保证每次都至少前进一个块头，避免死循环。*/
        const qint64 next = body + qint64(chunkSize) + qint64(chunkSize & 1u);
        offset = int(std::min<qint64>(std::max<qint64>(next, body), qint64(total)));
    }

    if (!haveFormat || *dataSize <= 0)
        return false;
    return formatTag == 1 && bits == 16 && *channels > 0 && *sampleRate > 0;
}

/*p 分位数（0~1）。空表返回 0。约定是"升序第 ceil(p*n) 个"：
  p=0.95、n=100 ⇒ 第 95 个（下标 94）—— 那一个离群的最大值当不上参考。*/
float percentile(QVector<float> values, double p)
{
    if (values.isEmpty())
        return 0.0f;
    std::sort(values.begin(), values.end());
    const int index = std::clamp(int(std::ceil(p * double(values.size()))) - 1, 0,
                                 int(values.size()) - 1);
    return values[index];
}
} // namespace

AudioEnvelope AudioEnvelope::fromWavBytes(const QByteArray &bytes)
{
    AudioEnvelope envelope;

    int sampleRate = 0;
    int channels = 0;
    int dataOffset = 0;
    int dataSize = 0;
    if (!parseRiff16BitPcm(bytes, &sampleRate, &channels, &dataOffset, &dataSize))
        return envelope; //不是 16 位 PCM：没有包络，调用方回退盲扑动

    const char *data = bytes.constData() + dataOffset;
    const int bytesPerFrame = channels * 2;
    const int frameCount = dataSize / bytesPerFrame;
    if (frameCount <= 0)
        return envelope;

    const int windowFrames = std::max(1, sampleRate * kWindowMs / 1000);
    const int levelCount =
        (frameCount + windowFrames - 1) / windowFrames; //最后一个窗口填不满也算一格

    /*逐个样本读（不 reinterpret_cast 成 qint16*）：data 块的偏移只保证块对齐，
      "此处恰好是 2 字节对齐"是碰巧而不是契约，强转会踩 UB。*/
    const auto sampleAt = [data, channels](int frame, int channel)
    {
        const int index = (frame * channels + channel) * 2;
        const quint16 raw =
            quint16(quint8(data[index])) | quint16(quint16(quint8(data[index + 1])) << 8);
        return float(qint16(raw)) / 32768.0f;
    };

    QVector<float> rms;
    rms.reserve(levelCount);
    for (int window = 0; window < levelCount; ++window)
    {
        const int begin = window * windowFrames;
        const int end = std::min(begin + windowFrames, frameCount);
        double sum = 0.0;
        for (int frame = begin; frame < end; ++frame)
        {
            for (int channel = 0; channel < channels; ++channel)
            {
                const double value = double(sampleAt(frame, channel));
                sum += value * value;
            }
        }
        const double count = double(end - begin) * double(channels);
        rms.append(float(std::sqrt(sum / (count > 0.0 ? count : 1.0))));
    }

    const float reference = percentile(rms, kReferencePercentile);
    if (reference <= kMinUsableReference)
        return envelope; //整段静音：归一化不出任何东西

    /*归一化 + 软噪声门。门的写法让"低于门限"精确落到 0（而不是留一个很小的正数）——
      渲染器把 0 当作"嘴回到心情值"，这是"停顿闭嘴"能成立的前提。*/
    constexpr float denominator = 1.0f - kNoiseFloor;
    QVector<float> levels;
    levels.reserve(levelCount);
    for (const float value : rms)
    {
        const float normalised = (value / reference - kNoiseFloor) / denominator;
        levels.append(std::clamp(normalised, 0.0f, 1.0f));
    }

    envelope.m_valid = true;
    envelope.m_windowMs = kWindowMs;
    envelope.m_sampleRate = sampleRate;
    envelope.m_levels = std::move(levels);
    return envelope;
}

float AudioEnvelope::levelAt(int index) const
{
    if (!m_valid || index < 0 || index >= m_levels.size())
        return 0.0f;
    return m_levels[index];
}

float AudioEnvelope::levelAtMs(qint64 positionMs) const
{
    if (!m_valid || m_levels.isEmpty())
        return 0.0f;
    /*位置换算成窗口序号并夹取：播放器的 position() 在句首可能短暂为负、
      在句尾可能越过音频长度（缓冲/时钟误差），两者都不该是"没有电平"。*/
    const qint64 window = positionMs / qint64(m_windowMs);
    const qint64 clamped = std::clamp<qint64>(window, 0, qint64(m_levels.size()) - 1);
    return m_levels[int(clamped)];
}
