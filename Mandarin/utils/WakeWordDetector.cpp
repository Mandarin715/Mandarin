#include "WakeWordDetector.h"

#include "sherpa-onnx/c-api/cxx-api.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QMediaDevices>

#include <cmath>

WakeWordDetector::WakeWordDetector(QObject *parent) : QObject(parent)
{
}

WakeWordDetector::~WakeWordDetector()
{
    stop();
    delete m_spotter;
    delete m_stream;
}

bool WakeWordDetector::init(const QString &modelDir, const QStringList &keywords,
                             float threshold)
{
    using namespace sherpa_onnx::cxx;

    QDir dir(modelDir);
    if (!dir.exists())
    {
        qWarning() << "WakeWordDetector: model directory not found:" << modelDir;
        return false;
    }

    // 使用模型内置关键词文件（已有ppinyin格式的keywords.txt）
    const QString kwFilePath = dir.filePath("keywords.txt");
    if (!QFile::exists(kwFilePath))
    {
        qWarning() << "WakeWordDetector: keywords.txt not found in model dir";
        return false;
    }
    qDebug() << "WakeWordDetector: using" << kwFilePath;

    KeywordSpotterConfig config;

    // 特征配置
    config.feat_config.sample_rate = 16000;
    config.feat_config.feature_dim = 80;

    // 模型配置
    config.model_config.transducer.encoder =
        dir.filePath("encoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx")
            .toStdString();
    config.model_config.transducer.decoder =
        dir.filePath("decoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx")
            .toStdString();
    config.model_config.transducer.joiner =
        dir.filePath("joiner-epoch-12-avg-2-chunk-16-left-64.int8.onnx")
            .toStdString();
    config.model_config.tokens =
        dir.filePath("tokens.txt").toStdString();
    config.model_config.num_threads = 1;

    // 关键词配置
    config.keywords_file = kwFilePath.toStdString();
    config.keywords_threshold = threshold;

    m_spotter = new KeywordSpotter(KeywordSpotter::Create(config));
    if (!m_spotter)
    {
        qWarning() << "WakeWordDetector: failed to create keyword spotter";
        return false;
    }

    m_stream = new OnlineStream(m_spotter->CreateStream());
    m_initialized = true;
    qDebug() << "WakeWordDetector: initialized, threshold=" << threshold;
    return true;
}

void WakeWordDetector::start()
{
    if (!m_initialized || m_running)
        return;

    QAudioFormat format;
    format.setSampleRate(16000);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);

    QAudioDevice device = QMediaDevices::defaultAudioInput();
    if (device.isNull())
    {
        qWarning() << "WakeWordDetector: no audio input device";
        return;
    }

    if (!device.isFormatSupported(format))
    {
        qWarning() << "WakeWordDetector: 16kHz mono format not supported, using nearest";
        format = device.preferredFormat();
    }

    m_audioSource = new QAudioSource(device, format, this);
    m_audioDevice = m_audioSource->start();
    if (!m_audioDevice)
    {
        qWarning() << "WakeWordDetector: failed to start audio capture";
        return;
    }

    m_running = true;
    m_stopRequested = false;
    qDebug() << "WakeWordDetector: started listening";

    // 持续读取音频数据
    connect(m_audioDevice, &QIODevice::readyRead, this,
            &WakeWordDetector::processAudio);
}

void WakeWordDetector::stop()
{
    m_stopRequested = true;
    if (m_audioSource)
    {
        m_audioSource->stop();
        delete m_audioSource;
        m_audioSource = nullptr;
        m_audioDevice = nullptr;
    }
    m_running = false;
    qDebug() << "WakeWordDetector: stopped";
}

bool WakeWordDetector::isRunning() const
{
    return m_running;
}

void WakeWordDetector::processAudio()
{
    if (!m_running || !m_audioDevice || !m_spotter || !m_stream)
        return;

    // 读取可用音频数据
    while (m_audioDevice->bytesAvailable() > 0 && !m_stopRequested)
    {
        QByteArray data = m_audioDevice->read(3200); // 100ms at 16kHz 16-bit mono
        if (data.isEmpty())
            break;

        // 转换为float样本（sherpa-onnx需要float格式），同时计算RMS
        const int16_t *rawSamples = reinterpret_cast<const int16_t *>(
            data.constData());
        int32_t numSamples = data.size() / 2;
        std::vector<float> samples(numSamples);
        double sumSq = 0.0;
        for (int32_t i = 0; i < numSamples; ++i)
        {
            const float s = rawSamples[i] / 32768.0f;
            samples[i] = s;
            sumSq += static_cast<double>(s) * s;
        }
        const float rms =
            numSamples > 0 ? static_cast<float>(std::sqrt(sumSq / numSamples)) : 0.0f;
        emit audioLevel(rms);

        // 送入sherpa-onnx stream
        m_stream->AcceptWaveform(16000, samples.data(), numSamples);

        // 检测
        while (m_spotter->IsReady(m_stream))
        {
            m_spotter->Decode(m_stream);
            auto result = m_spotter->GetResult(m_stream);
            if (!result.keyword.empty())
            {
                QString kw = QString::fromStdString(result.keyword);
                qDebug() << "WakeWordDetector: detected keyword:" << kw;
                emit wakeWordDetected(kw);
                m_spotter->Reset(m_stream);
            }
        }
    }
}
