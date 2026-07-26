#ifndef DIALOG_H
#define DIALOG_H

#include "AiProvider.h"
#include "../../utils/SearchProvider.h"
#include "ZcJsonLib.h"
#include <QDateTime>
#include <QEvent>
#include <QJsonArray>
#include <QJsonObject>
#include <QMoveEvent>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QWidget>

class QAudioOutput;
class QAudioSource;
class QMediaPlayer;
class QNetworkAccessManager;
class QBuffer;
class QIODevice;
class WakeWordDetector;
class OfflineSpeechRecognizer;

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
    void ReloadSearchConfig();
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
    void requestShowInnerThought(QString text);
    void requestHideInnerThought();

  public slots:
    void ReloadAIConfig();          // 完整重载（角色切换/F5）
    void ReloadProviderConfig();     // 仅 API Key/BaseURL（LLM 页变更）
    void ReloadCharacterConfig();    // 角色 prompt/模型/上下文/记忆
    void ReloadMemoryConfig();       // 仅 memory.json

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
    void initServices(); // 延迟初始化
    void showTemporaryMessage(const QString &msg); // 显示临时消息，2.5s后自动清除：网络/音频/AI配置等重操作，首帧后再执行
    // 配置缓存重载：接受预加载的 config.json，避免构造函数中重复 I/O
    void reloadAIConfig(const ZcJsonLib &config);
    void reloadSpeechInputConfig(const ZcJsonLib &config);
    void reloadScreenCaptureConfig(const ZcJsonLib &config);
    void reloadSearchConfig(const ZcJsonLib &config);
    void reloadAppLauncherConfig(const ZcJsonLib &config);
    void reloadContinuousHotkeyConfig(const ZcJsonLib &config);
    //历史
    void loadContextHistory(); //加载上下文历史
    void saveContextHistory() const;
    void stopPendingConversationState();
    bool isHistoryOpen = false;
    QStringList m_contextHistory;
    QString m_lastHistoryDate;
    bool m_contextCompressionInFlight = false;
    quint64 m_contextGeneration = 0;

    QString buildUserMessageWithContext(
        const QString &input) const; //构建用户消息，包含上下文
    QString buildSystemPrompt(const QString &currentChar); //构建系统提示词（含缓存）

    void appendHistoryLine(const QString &line); //添加历史记录行
    void tryStartNextVitsRequest();              //添加到Vits请求
    void connectChatCallbacks(AiProvider *provider, quint64 generation);

    QString m_lastUserInput;
    QString m_streamRawReply;
    QString m_streamDisplayedChinese;
    QTimer *m_streamDisplayTimer = nullptr; // 流式显示防抖定时器
    bool m_isSpeechRecording = false;
    // 会话隔离：每轮对话创建独立 AiProvider
    quint64 m_chatGeneration = 0;
    AiProvider *m_activeChatAi = nullptr;
    quint64 m_proactiveGeneration = 0;
    void cancelActiveChat();
    bool m_globalSpeechHotkeyEnabled = false; //全局录音热键是否启用
    bool m_globalSpeechHotkeyPressed = false; //当前热键是否处于按下录音中
    quint32 m_globalSpeechHotkeyNativeKey = 0; //Ela绑定得到的原生按键值
    // 连续对话模式独立快捷键
    bool m_continuousHotkeyEnabled = false;
    quint32 m_continuousHotkeyNativeKey = 0;
    int m_continuousAudioDelayMs = 800;
    bool m_continuousMode = false;
    QTimer *m_continuousSilenceTimer = nullptr;
    void enterContinuousMode();
    void exitContinuousMode();
    bool isAllVitsDone() const;
    bool m_streamVitsEnabled = false;
    bool m_streamVitsSentenceSplitEnabled = true;
    int m_streamSynthCursor = 0;
    QStringList m_vitsPendingTexts;
    QMap<int, QBuffer *> m_vitsReadyFiles; // key=序号，保证并发乱序完成时按原文序播放
    QSet<int> m_vitsFailedSeqs;            // 失败的序号集合，播放时跳过
    QList<QNetworkReply *> m_vitsInFlightReplies; // 在途VITS请求，回溯时abort
    int m_vitsSeqNext = 0;   // 下一个待分配的合成序号
    int m_vitsSeqCursor = 0; // 下一个应播放的序号，保证即使乱序完成也按序播放
    static constexpr int kVitsMaxConcurrent = 3;
    quint64 m_vitsGeneration = 0;          // VITS 请求代际，abort 后递增
    bool m_vitsFinishScheduled = false;
    void resetVitsPipeline();
    void checkVitsPipelineFinished();
    void checkVitsServerReady();
    QNetworkAccessManager *m_vitsManager = nullptr;
    QMediaPlayer *m_vitsPlayer = nullptr;
    bool m_vitsServerReady = false;            // VITS 服务就绪后置 true
    QString m_cachedVitsApiUrl;
    QString m_cachedVitsModel;
    QString m_cachedVitsSpeaker;
    QAudioOutput *m_vitsAudioOutput = nullptr;
    QBuffer *m_vitsTempFile = nullptr;
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
    // IP 定位
    QString m_cachedLocation;
    QNetworkAccessManager *m_locationManager = nullptr;
    void fetchLocation();
    // AI 搜索意图分类
    bool m_classifierInFlight = false;
    void classifyAndSearch(const QString &userInput);
    // 语音输入（QAudioSource直录PCM，录音+静音检测同一音源）
    QAudioSource *m_speechAudioSource = nullptr;
    QIODevice *m_speechAudioDevice = nullptr;
    QByteArray m_capturedAudioData;
    void startSpeechRecording();
    void startSpeechRecordingFromHotkey();
    void stopSpeechRecording();
    void releaseSpeechHotkeyResources();
    // 离线语音识别
    OfflineSpeechRecognizer *m_speechRecognizer = nullptr;
    void initSpeechRecognizer();
    // 语音唤醒
    WakeWordDetector *m_wakeWordDetector = nullptr;
    bool m_wakeWordEnabled = false;
    // 静音检测：100ms轮询+帧计数器，25帧(2.5秒)无声音自动停止录音
    QTimer *m_silencePollTimer = nullptr;
    int m_silentFrameCount = 0;
    float m_silenceThreshold = 0.005f; // 静音 RMS 阈值，从配置读取
    int m_recordFrame = 0;             // 录音帧计数器（每轮重置）
    static constexpr int kSilencePollMs = 100;
    int m_silenceFrameMax = 15; // 静默帧上限，从配置读取，默认 1.5s
    void initWakeWord();
    void startWakeWord();
    void stopWakeWord();
    // 上下文历史延迟写入
    QTimer *m_contextSaveTimer = nullptr;
    bool m_contextDirty = false;
    void scheduleContextSave();
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
    // 主动对话
    QTimer *m_proactiveTimer = nullptr;
    QDateTime m_lastProactiveSpeakTime;
    bool m_userAway = false;
    bool m_proactiveEnabled = false;
    bool m_proactiveInFlight = false;
    int m_proactiveCooldownSec = 600;   // 冷却期，默认 10 分钟
    int m_proactiveDwellSec = 10;       // 窗口驻留确认，默认 10 秒
    int64_t m_proactivePendingHwnd = 0; // 待确认的窗口句柄（比标题更稳定）
    int64_t m_proactiveHandledHwnd = 0; // 已消费窗口切换事件的句柄
    QString m_proactivePendingTitle;
    int m_proactiveDwellCount = 0;      // 驻留倒计时
    AiProvider *m_activeProactiveAi = nullptr;
    void startProactiveTimer();
    void initProactiveAgent();
    void checkProactiveWindow();
    void checkProactiveUserPresence();
    bool doProactiveSpeak(const QString &windowTitle, const QString &contextHint);

    // 联网搜索
    SearchProvider *m_searchProvider = nullptr;
    bool m_searchEnabled = false;
    bool m_searchAutoSearch = false;
    bool m_searchInFlight = false;
    QString m_pendingSearchUserMessage;
    void executeSearch(const QString &query, const QString &userMessage);
    static QStringList searchTriggerKeywords();
    static QString extractSearchQuery(const QString &userInput);
    void doSubmitWithSearchContext(const QString &userMessage,
                                   const QString &searchSummary);
};

#endif //DIALOG_H
