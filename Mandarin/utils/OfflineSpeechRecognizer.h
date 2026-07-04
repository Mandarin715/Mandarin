#ifndef OFFLINESPEECHRECOGNIZER_H
#define OFFLINESPEECHRECOGNIZER_H

#include <QObject>
#include <QString>

namespace sherpa_onnx
{
namespace cxx
{
class OfflineRecognizer;
} // namespace cxx
} // namespace sherpa_onnx

/**
 * @brief 离线语音识别封装，基于 sherpa-onnx SenseVoice 模型。
 *
 * 使用方式（非流式，整段录音→识别）：
 *   recognizer.init(modelDir);
 *   QString text = recognizer.recognize(pcmInt16Data, 16000);
 *
 * 模型要求：SenseVoice ONNX 模型 (.int8.onnx) + tokens.txt
 */
class OfflineSpeechRecognizer : public QObject
{
    Q_OBJECT

  public:
    explicit OfflineSpeechRecognizer(QObject *parent = nullptr);
    ~OfflineSpeechRecognizer();

    /**
     * @brief 初始化识别器，加载模型
     * @param modelDir 包含 model.int8.onnx 和 tokens.txt 的目录
     * @return 是否初始化成功
     */
    bool init(const QString &modelDir);

    /// 是否已成功初始化
    bool isInitialized() const;

    /**
     * @brief 识别一段 PCM 音频（16kHz, 16-bit, mono, little-endian）
     * @param rawPcmData 原始 int16 PCM 字节
     * @param sampleRate 采样率，默认 16000
     * @return 识别结果文本；失败返回空字符串
     */
    QString recognize(const QByteArray &rawPcmData, int sampleRate = 16000);

  private:
    sherpa_onnx::cxx::OfflineRecognizer *m_recognizer = nullptr;
    bool m_initialized = false;
};

#endif // OFFLINESPEECHRECOGNIZER_H
