#include "OfflineSpeechRecognizer.h"

#include "sherpa-onnx/c-api/cxx-api.h"

#include <QDebug>
#include <QDir>
#include <QFile>

OfflineSpeechRecognizer::OfflineSpeechRecognizer(QObject *parent)
    : QObject(parent)
{
}

OfflineSpeechRecognizer::~OfflineSpeechRecognizer()
{
    delete m_recognizer;
}

bool OfflineSpeechRecognizer::init(const QString &modelDir)
{
    using namespace sherpa_onnx::cxx;

    QDir dir(modelDir);
    if (!dir.exists())
    {
        qWarning() << "OfflineSpeechRecognizer: model directory not found:"
                   << modelDir;
        return false;
    }

    const QString modelFile = dir.filePath("model.int8.onnx");
    const QString tokensFile = dir.filePath("tokens.txt");

    if (!QFile::exists(modelFile))
    {
        qWarning() << "OfflineSpeechRecognizer: model.int8.onnx not found in"
                   << modelDir;
        return false;
    }
    if (!QFile::exists(tokensFile))
    {
        qWarning() << "OfflineSpeechRecognizer: tokens.txt not found in"
                   << modelDir;
        return false;
    }

    OfflineRecognizerConfig config;
    config.model_config.sense_voice.model = modelFile.toStdString();
    config.model_config.sense_voice.language = "auto"; // 自动检测语言
    config.model_config.sense_voice.use_itn = true;    // 开启逆文本正则（数字/日期/标点规范化）
    config.model_config.tokens = tokensFile.toStdString();
    config.model_config.num_threads = 1;

    m_recognizer = new OfflineRecognizer(OfflineRecognizer::Create(config));
    if (!m_recognizer || !m_recognizer->Get())
    {
        qWarning() << "OfflineSpeechRecognizer: failed to create recognizer";
        delete m_recognizer;
        m_recognizer = nullptr;
        return false;
    }

    m_initialized = true;
    qDebug() << "OfflineSpeechRecognizer: initialized, model=" << modelDir;
    return true;
}

bool OfflineSpeechRecognizer::isInitialized() const
{
    return m_initialized;
}

QString OfflineSpeechRecognizer::recognize(const QByteArray &rawPcmData,
                                           int sampleRate)
{
    using namespace sherpa_onnx::cxx;

    if (!m_initialized || !m_recognizer)
    {
        qWarning() << "OfflineSpeechRecognizer: not initialized";
        return QString();
    }

    // int16 PCM → float 样本
    const int16_t *rawSamples = reinterpret_cast<const int16_t *>(
        rawPcmData.constData());
    const int sampleCount = rawPcmData.size() / 2;
    if (sampleCount == 0)
        return QString();

    std::vector<float> samples(sampleCount);
    for (int i = 0; i < sampleCount; ++i)
        samples[i] = rawSamples[i] / 32768.0f;

    // 创建流 → 送入音频 → 解码 → 取结果
    OfflineStream stream = m_recognizer->CreateStream();
    stream.AcceptWaveform(sampleRate, samples.data(), sampleCount);

    m_recognizer->Decode(&stream);
    OfflineRecognizerResult result = m_recognizer->GetResult(&stream);

    const QString text = QString::fromStdString(result.text).trimmed();

    qDebug() << "OfflineSpeechRecognizer: recognized" << text;

    // SenseVoice 附加信息（调试用）
    if (!result.lang.empty())
        qDebug() << "  lang:" << QString::fromStdString(result.lang);
    if (!result.emotion.empty())
        qDebug() << "  emotion:" << QString::fromStdString(result.emotion);
    if (!result.event.empty())
        qDebug() << "  event:" << QString::fromStdString(result.event);

    return text;
}
