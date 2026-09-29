#ifndef AUDIOENVELOPE_H
#define AUDIOENVELOPE_H

#include <QByteArray>
#include <QVector>

/*一段 TTS 音频的**响度包络**：只有电平，没有任何解码依赖。

  为什么要有它（用户诉求）：TTS 在播时嘴巴按"盲扑动"开合，于是句子之间换气/停顿的
  静音段里嘴照样在动 —— 用户要的是"说话时嘴动、**停顿闭嘴**"。

  为什么不需要解码器：本项目的 vits-simple-api 默认返回 **WAV（16 位 PCM）**
  （实测两次独立确认：RIFF 头，32kHz 与 22kHz 各一次）。于是包络可以直接从
  RIFF 头 + `data` 块算出来，不必引 QtMultimedia 的解码器，也不必把音频交给渲染层。

  **不可解析时一律"没有包络"**（`isValid() == false`），调用方据此退回盲扑动。
  这条契约比"尽量算点东西出来"重要得多：用户把 vits 的 `format` 配成 mp3 时，
  功能必须**安静地退化成今天的样子** —— 不报错、不静音、不崩。

  数据流：Dialog 在收到 TTS 字节时算一次（它握着字节），播放中按
  `QMediaPlayer::position()` 每 50ms 问一次 `levelAtMs()`，只把一个 0~1 的数发给立绘。
  音频格式（采样率/位深/声道）就到此为止，绝不外泄到窗口层。*/
class AudioEnvelope
{
  public:
    /*从 TTS 返回的原始字节解析。
      不是"RIFF/WAVE + 16 位 PCM + 非空 data 块"时返回 `isValid()==false` 的空包络。*/
    static AudioEnvelope fromWavBytes(const QByteArray &bytes);

    /*是否算出了包络。false = 调用方必须回退到盲扑动。*/
    bool isValid() const { return m_valid; }

    /*每个电平窗口多少毫秒（包络的时间分辨率）。无效时为 0。*/
    int windowMs() const { return m_windowMs; }

    /*电平窗口个数。无效时为 0。*/
    int levelCount() const { return m_levels.size(); }

    /*采样率（Hz）。无效时为 0。*/
    int sampleRate() const { return m_sampleRate; }

    /*按窗口序号取电平（0~1）。越界或无效一律返回 0。*/
    float levelAt(int index) const;

    /*按**播放位置**（毫秒）取电平：内部换算成窗口序号并夹取到首/尾。
      无效时恒返回 0 —— 调用方不需要先判 isValid（"没有包络"与"电平为 0"在
      数值上等价，区别只在"要不要进入包络模式"，那由 isValid 决定）。*/
    float levelAtMs(qint64 positionMs) const;

  private:
    bool m_valid = false;
    int m_windowMs = 0;
    int m_sampleRate = 0;
    QVector<float> m_levels;
};

#endif // AUDIOENVELOPE_H
