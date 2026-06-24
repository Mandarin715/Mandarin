#ifndef DIALOG_H
#define DIALOG_H

#include "AiProvider.h"
#include <QEvent>
#include <QJsonArray>
#include <QJsonObject>
#include <QMoveEvent>
#include <QStringList>
#include <QTimer>
#include <QWidget>

class QAudioOutput;
class QAudioSource;
class QMediaPlayer;
class QNetworkAccessManager;
class QIODevice;
class QTemporaryFile;
class WakeWordDetector;

namespace Ui
{
class Dialog;
}

class history;

class Dialog : public QWidget
{
    Q_OBJECT

  public:
    explicit Dialog(QWidget *parent = nullptr);
    ~Dialog();

  public slots:
    void ToggleVisible();
    void VitsGetAndPlay(QString text);
    void ReloadGeneralConfig();
    void ReloadSpeechInputConfig();
    void ReloadScreenCaptureConfig();
    void ReloadAppLauncherConfig();
    void ReloadContinuousHotkeyConfig();
    bool handleSpeechHotkeyEvent(quint32 vkCode, bool isKeyDown, bool isKeyUp);

  private slots:
    void on_pushButton_next_clicked();
    void on_pushButton_history_clicked();
    void on_pushButton_screenCapture_clicked();
    void on_pushButton_input_pressed();
    void on_pushButton_input_released();
    void on_checkBox_autoInput_toggled(bool checked);
    void rewindToHistoryIndex(int historyIndex);
    void deleteHistoryItem(int historyIndex);

  signals:
    void requestSetCharTachie(QString TachieName);

  public slots:
    void ReloadAIConfig();

  private:
    /*初始化*/
    virtual void paintEvent(QPaintEvent *event) override;
    Ui::Dialog *ui = nullptr;
    history *historyWin = nullptr;
    /*按键事件*/
    //鼠标
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    bool nativeEvent(const QByteArray &eventType, void *message,
                     qintptr *result) override;
    void wheelEvent(QWheelEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    QPoint lastPos;  //记录鼠标位置
    QList<int> keys; //按键按键获取
    /*主逻辑*/
    void initWindow();
    //历史
    void handleWheelUp();
    void handleWheelDown();
    void loadContextHistory(); //加载上下文历史
    void saveContextHistory() const;
    void stopPendingConversationState();
    bool isHistoryOpen = false;
    QStringList m_contextHistory;

    QString buildUserMessageWithContext(
        const QString &input) const; //构建用户消息，包含上下文

    void appendHistoryLine(const QString &line); //添加历史记录行
    void tryStartNextVitsRequest();              //添加到Vits请求
    AiProvider *ai = nullptr;                    //用于AI交互

    QString m_lastUserInput;
    QString m_streamRawReply;
    QString m_streamDisplayedChinese;
    QTimer *m_streamDisplayTimer = nullptr; // 流式显示防抖定时器
    bool m_isSpeechRecording = false;
    bool m_isSpeechRecognizing = false;
    bool m_globalSpeechHotkeyEnabled = false; //全局录音热键是否启用
    bool m_globalSpeechHotkeyPressed = false; //当前热键是否处于按下录音中
    quint32 m_globalSpeechHotkeyNativeKey = 0; //Ela绑定得到的原生按键值
    // 连续对话模式独立快捷键
    bool m_continuousHotkeyEnabled = false;
    quint32 m_continuousHotkeyNativeKey = 0;
    int m_continuousAudioDelayMs = 2500;
    bool m_continuousMode = false;
    QTimer *m_continuousSilenceTimer = nullptr;
    void enterContinuousMode();
    void exitContinuousMode();
    bool isAllVitsDone() const;
    bool m_streamVitsEnabled = false;
    bool m_streamVitsSentenceSplitEnabled = true;
    int m_streamSynthCursor = 0;
    QStringList m_vitsPendingTexts;
    QList<QTemporaryFile *> m_vitsReadyFiles;
    bool m_vitsRequestInFlight = false;
    QNetworkAccessManager *m_vitsManager = nullptr;
    QMediaPlayer *m_vitsPlayer = nullptr;
    QAudioOutput *m_vitsAudioOutput = nullptr;
    QTemporaryFile *m_vitsTempFile = nullptr;
    void tryStartNextVitsPlayback();
    bool submitCurrentInput();
    // 记忆功能
    QJsonObject m_memoryData;
    // 系统提示词缓存（避免每次发消息重复构建）
    QString m_cachedSystemPrompt;
    QString m_cachedCharacterForPrompt;
    bool m_memoryDirty = true;
    void loadMemory();
    void saveMemory() const;
    QString buildMemoryContext() const;
    void extractAndStoreMemory(const QString &userInput, const QString &aiReply);
    void compressContextHistory();
    // 语音输入（QAudioSource直录PCM，录音+静音检测同一音源）
    QAudioSource *m_speechAudioSource = nullptr;
    QIODevice *m_speechAudioDevice = nullptr;
    QByteArray m_capturedAudioData;
    void startSpeechRecording();
    void startSpeechRecordingFromHotkey();
    void stopSpeechRecording();
    QString speechRecordFilePath() const;
    QString recognizeSpeechFromFile(const QString &filePath);
    QString requestBaiduAccessToken(const QString &apiKey,
                                    const QString &secretKey);
    void releaseSpeechHotkeyResources();
    // 语音唤醒
    WakeWordDetector *m_wakeWordDetector = nullptr;
    bool m_wakeWordEnabled = false;
    // 静音检测：100ms轮询+帧计数器，25帧(2.5秒)无声音自动停止录音
    QTimer *m_silencePollTimer = nullptr;
    int m_silentFrameCount = 0;
    static constexpr float kSilenceThreshold = 0.005f;
    static constexpr int kSilencePollMs = 100;
    static constexpr int kSilenceFrameMax = 25;
    void initWakeWord();
    void startWakeWord();
    void stopWakeWord();
    void onWakeWordDetected(const QString &keyword);
    // 应用调用
    QJsonArray m_cachedAppCommands;
    // 多模态屏幕捕获
    bool m_screenCaptureEnabled = false;
    bool m_visionInFlight = false;
    QNetworkAccessManager *m_visionManager = nullptr;
    QByteArray captureScreenToJpeg();
    void captureAndAnalyzeScreen();
    void analyzeScreenWithVision(const QByteArray &imageBase64,
                                  const QString &userMessage);
    static QStringList screenCaptureTriggerKeywords();
    bool doSubmitCurrentInput(const QString &userInput);
};

#endif //DIALOG_H
