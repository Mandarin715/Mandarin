#ifndef DIALOG_H
#define DIALOG_H

#include "AiProvider.h"
#include "../../utils/AudioEnvelope.h"
#include "../../utils/SearchProvider.h"
#include "../../utils/FaceDetector.h"
#include "ZcJsonLib.h"
#include <QDateTime>
#include <QEvent>
#include <QJsonArray>
#include <QJsonObject>
#include <QMoveEvent>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <atomic>

class QCamera;
class QVideoSink;
class QMediaCaptureSession;
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
class reminder;
class ChatLogStore;

class Dialog : public QWidget
{
    Q_OBJECT

  public:
    explicit Dialog(QWidget *parent = nullptr);
    ~Dialog();

  public slots:
    void ToggleVisible();
    void handleFileDrop(QStringList paths);
    void VitsGetAndPlay(QString text);
    void ReloadGeneralConfig();
    void ReloadSpeechInputConfig();
    void ReloadScreenCaptureConfig();
    void ReloadCameraPerceptionConfig();
    void ReloadSearchConfig();
    void ReloadAppLauncherConfig();
    void ReloadContinuousHotkeyConfig();
    bool handleSpeechHotkeyEvent(quint32 vkCode, bool isKeyDown, bool isKeyUp);

  private slots:
    void on_pushButton_next_clicked();
    void on_pushButton_history_clicked();
    void on_pushButton_reminder_clicked();
    void on_pushButton_screenCapture_clicked();
    void on_pushButton_input_pressed();
    void on_pushButton_input_released();
    void on_checkBox_autoInput_toggled(bool checked);
    void rewindToHistoryIndex(int historyIndex);
    void deleteHistoryItem(int historyIndex);
    void deleteReminder(const QString &id);
    void clearAllReminders();
    void refreshReminderWindow();

  signals:
    void requestSetCharTachie(QString TachieName);
    void requestShowInnerThought(QString text);
    void requestHideInnerThought();
    /*TTS 是否在播（true = 开始/继续播，false = 停止或出错）。
       立绘据此让嘴巴开合 —— 是"纸片人开合"，不做音素口型、也不看音量。
       为什么用布尔而不是把音频数据送出去：需求要的就是"在说话"这一个信号，
       音频分析要么引依赖、要么在播放线程里做活儿，得不偿失。*/
    void requestSpeakState(bool speaking);

    /*TTS 这一拍的**响度电平**（0~1），与 requestSpeakState 成对：
       那个说"在不在播"，这个说"这一拍有多响"。
       立绘据此让开口量跟着真实响度走 —— 于是句子之间的停顿（电平为 0）嘴会闭上，
       这就是用户要的"停顿闭嘴"（盲扑动做不到：它不知道音频里有没有声音）。

       为什么只送一个数而不是音频数据：
         - 没有解码器/依赖：包络就在 Dialog 里从 TTS 返回的 **WAV（16 位 PCM）** 字节直接算出来
           （vits-simple-api 的默认格式，实测），不必把音频交给窗口层；
         - 窗口层与渲染层因此不需要知道采样率/位深/声道 —— 音频格式的事到此为止。

       **只有真的算出包络时才发这个信号**：算不出来（例如用户把 vits 的 format 配成 mp3）
       就一个都不发，渲染器据此保持今天逐位相同的盲扑动（回退契约）。*/
    void requestSpeakLevel(float level);

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
    reminder *reminderWin = nullptr;
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
    void loadContextHistory();   //加载上下文历史（chat.jsonl，含旧版迁移）
    void syncChatLogFromView();  //回退/删除后按内存视图重写 chat.jsonl
    ChatLogStore *m_chatLog = nullptr;
    void stopPendingConversationState();
    bool isHistoryOpen = false;
    bool m_reminderOpen = false;
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

    /*---------- TTS 响度包络（见 requestSpeakLevel） ----------
       包络按**序号**存：并发最多 3 句在途，谁先回来不重要，播放时按游标取。
       "没有这一项"就是"这一句没有包络"（用户配成 mp3 的情形）—— 一个电平都不发。*/
    QMap<int, AudioEnvelope> m_vitsEnvelopes;
    int m_vitsPlayingSeq = -1;          // 当前在播的序号（取包络用）
    QTimer *m_vitsLevelTimer = nullptr; // 采样节拍：把 position() 换成渲染器要的那一个数
    /*50ms 一拍。与语音音节量级相称（一个音节 80~150ms ⇒ 每音节 2~3 个电平），
       且低于渲染帧率的量级：中间几帧沿用上一个电平，由渲染器的攻击/释放平滑掉。
       再快只是重复问同一个窗口（包络自己的窗口是 20ms），再慢就会漏掉短促的音节。*/
    static constexpr int kVitsLevelSampleMs = 50;
    /*算出并记下这一句的包络（有就存、没有就算了 —— 算不出来不是错误路径）。*/
    void prepareVitsEnvelope(int seq, const QByteArray &bytes);
    /*把当前播放位置换算成电平发出去（没有包络时什么都不发）。*/
    void emitVitsSpeakLevel();
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
    void applyRecognizedText(const QString &recognizedText); // 识别结果过滤/上屏/自动发送
    std::atomic_bool m_asrBusy{false}; // 工作线程识别在途，防并发/防误删
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
    void showVisionFailureMessage(const QString &message); // 视觉分析失败：显式报错，不再静默转聊天
    static QStringList screenCaptureTriggerKeywords();
    bool doSubmitCurrentInput(const QString &userInput);
    bool isChatBusy() const; // 任一轮对话/分类/搜索/视觉/主动对话在途
    // 主动对话
    QTimer *m_proactiveTimer = nullptr;
    QDateTime m_lastProactiveSpeakTime;
    bool m_userAway = false;
    bool m_proactiveEnabled = false;
    bool m_proactiveInFlight = false;
    int m_proactiveCooldownSec = 600;   // 冷却期，默认 10 分钟
    int m_currentCooldownSec = 600;     // 当前生效冷却（每次说话后随机化 0.8~1.5×）
    int m_idleGreetThresholdMs = 10 * 60 * 1000; // 空闲问候阈值（触发后随机化 10~20 分钟）
    QStringList m_recentProactiveLines; // 最近主动发言（防重复，最多 5 条）
    int m_proactiveDwellSec = 10;       // 窗口驻留确认，默认 10 秒
    int64_t m_proactivePendingHwnd = 0; // 待确认的窗口句柄（比标题更稳定）
    int64_t m_proactiveHandledHwnd = 0; // 已消费窗口切换事件的句柄
    QString m_proactivePendingTitle;
    int m_proactiveDwellCount = 0;      // 驻留倒计时
    AiProvider *m_activeProactiveAi = nullptr;
    void startProactiveTimer();
    void initProactiveAgent();
    void initClipboardMonitor();
    bool m_clipboardCooldown = false;
    // 日程提醒
    struct Schedule {
        QString id;
        QDateTime time;   // 触发时间
        QString text;     // 提醒内容
        int repeatSec = 0; // 0=一次性，>0=循环周期（秒）
        bool triggered = false;
    };
    QList<Schedule> m_schedules;
    void loadSchedules();
    void saveSchedules() const;
    bool tryParseSchedule(const QString &input, Schedule &out) const; // 规则解析
    void checkSchedules();                                            // 每秒轮询
    bool fireSchedule(const Schedule &s, bool missed);                // 触发提醒（返回是否发声）
    void catchUpMissedSchedules();                                    // 启动补触发
    void checkProactiveWindow();
    void checkProactiveUserPresence();
    bool doProactiveSpeak(const QString &windowTitle, const QString &contextHint,
                          bool forced = false, bool isReminder = false);

    // 摄像头感知（v1 仅 Windows：平时摄像头关，仅"怀疑"短开验证人脸）
    FaceDetector *m_faceDetector = nullptr;
    bool m_cameraPerceptionEnabled = false;
    QString m_cameraPerceptionDevice;   // 所选摄像头描述（空=自动选第一个）
    int m_cameraVerifyIdleSec = 600;    // 怀疑阈值：无输入秒数触发验证
    int m_cameraAwayRecheckSec = 180;   // 离开期间短检间隔
    bool m_cameraFallbackToKeyboard = false; // 摄像头不可用/被拒 → 回退键鼠推断
    bool m_cameraVerifying = false;     // 正在短开相机验证
    QCamera *m_presenceCamera = nullptr;
    QVideoSink *m_presenceVideoSink = nullptr;
    QMediaCaptureSession *m_presenceCaptureSession = nullptr;
    QTimer *m_cameraTimeoutTimer = nullptr; // 验证突发安全超时（3s 无帧则放弃）
    int m_cameraValidateSeq = 0;        // 校验序号，防过期回调
    QDateTime m_lastCameraVerifyTime;   // 上次验证时间（怀疑间隔 gating）
    QDateTime m_lastAwayRecheckTime;    // 离开期间上次短检时间
    void initCameraPerception();
    void checkPresenceByKeyboard(quint32 idleMs); // 摄像头不可用时回退键鼠推断
    void beginCameraValidation();
    void finishCameraValidation(bool facePresent);

    // 联网搜索
    SearchProvider *m_searchProvider = nullptr;
    bool m_searchEnabled = false;
    bool m_searchAutoSearch = false;
    bool m_searchInFlight = false;
    QString m_pendingSearchUserMessage;
    QString m_lastSearchQuery; // 上次搜索词（供"再搜搜"这类追问式重搜沿用）
    bool m_lastTurnWasSearch = false; // 上一轮是否由搜索触发（供否定重搜）
    void executeSearch(const QString &query, const QString &userMessage);
    static QStringList searchTriggerKeywords();
    static QString extractSearchQuery(const QString &userInput);
    void doSubmitWithSearchContext(const QString &userMessage,
                                   const QString &searchSummary);
};

#endif //DIALOG_H
