#include "dialog.h"
#include "history/history.h"
#include "ui_dialog.h"

#include "../../GlobalConstants.h"

#include "../../utils/CustomScrollBinder.h"
#include "../../utils/DragHelper.h"

#include "ZcJsonLib.h"
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>

#include <QAudioDevice>
#include <QAudioInput>
#include <QAudioOutput>
#include <QBuffer>
#include <QEventLoop>
#include <QGraphicsOpacityEffect>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMediaDevices>
#include <QMediaFormat>
#include <QMediaPlayer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QParallelAnimationGroup>
#include <QPermissions>
#include <QPropertyAnimation>
#include <QPixmap>
#include <QScreen>
#include <QTemporaryFile>
#include <QUrlQuery>
#include <QUuid>
#include <QWheelEvent>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#ifdef Q_OS_LINUX
#include <X11/Xlib.h>
#endif

namespace
{
//对话框尺寸配置范围，避免配置文件被手动写入过小或过大的值。
constexpr int kDefaultDialogWidth = 650;
constexpr int kDefaultDialogHeight = 200;
constexpr int kMinDialogWidth = 320;
constexpr int kMinDialogHeight = 120;
constexpr int kMaxDialogWidth = 1600;
constexpr int kMaxDialogHeight = 900;

#ifdef Q_OS_WIN
//Windows低级键盘钩子需要静态回调，这里保存当前接收热键的Dialog实例。
HHOOK g_speechHotkeyHook = nullptr;
Dialog *g_speechHotkeyOwner = nullptr;

LRESULT CALLBACK SpeechHotkeyHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0 && g_speechHotkeyOwner)
    {
        const KBDLLHOOKSTRUCT *info =
            reinterpret_cast<KBDLLHOOKSTRUCT *>(lParam);
        const bool isKeyDown =
            (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        const bool isKeyUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
        if (info &&
            g_speechHotkeyOwner->handleSpeechHotkeyEvent(info->vkCode, isKeyDown,
                                                         isKeyUp))
            return 1;
    }
    return CallNextHookEx(g_speechHotkeyHook, nCode, wParam, lParam);
}
#endif
} // namespace

/*寻找句子分割点*/
static int findNextSentenceEnd(const QString &text, int start)
{
    for (int i = qMax(0, start); i < text.size(); ++i)
    {
        const QChar ch = text.at(i);
        if (ch == QChar('.') || ch == QChar('!') || ch == QChar('?') ||
            ch == QChar('\n') || ch == QStringLiteral("。").at(0) ||
            ch == QStringLiteral("！").at(0) || ch == QStringLiteral("？").at(0) ||
            ch == QStringLiteral("、").at(0) || ch == QStringLiteral("；").at(0) ||
            ch == QChar(';'))
            return i;
    }
    return -1;
}

/*窗口的绘制*/
void Dialog::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    QRectF rect(5, 5, this->width() - 10, this->height() - 10);
    path.addRoundedRect(rect, 15, 15);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillPath(path, QBrush(Qt::white));
    QColor color(0, 0, 0, 50);
    for (int i = 0; i < 5; i++)
    {
        QPainterPath shadowPath;
        shadowPath.setFillRule(
            Qt::WindingFill); //使用圆角矩形而不是普通矩形绘制阴影
        QRectF shadowRect((5 - i), (5 - i), this->width() - (5 - i) * 2,
                          this->height() - (5 - i) * 2);
        shadowPath.addRoundedRect(shadowRect, 15, 15); //添加圆角矩形路径
        color.setAlpha(50 - qSqrt(i) * 22);            //增加透明度效果，模拟阴影逐渐变淡
        painter.setPen(color);
        painter.drawPath(shadowPath); //绘制阴影路径
    }
}

/*初始化窗口*/
void Dialog::initWindow()
{
    /*窗口初始化*/
    Qt::WindowFlags flags = Qt::FramelessWindowHint | Qt::Tool |
                            Qt::WindowStaysOnTopHint;
#ifdef Q_OS_LINUX
    //避免窗口管理器限制拖拽范围（如屏幕边缘约束）。
    flags |= Qt::X11BypassWindowManagerHint;
#endif
    setWindowFlags(flags);
    setWindowOpacity(0.95);
    setAttribute(Qt::WA_TranslucentBackground);
    /*内容初始化*/
    ui->pushButton_next->hide();
    ui->verticalScrollBar->hide();
    new CustomScrollBinder(ui->textEdit, ui->verticalScrollBar, 5,
                           this); //TextEdit的滚动条
    new DragHelper(this);         //给窗口添加拖拽功能
    ui->textEdit->installEventFilter(this);
    ui->textEdit->viewport()->installEventFilter(this);
    //初始隐藏语音输入相关控件，后续根据配置决定是否显示
    ui->pushButton_input->hide();
    ui->pushButton_screenCapture->hide();
    ui->checkBox_autoInput->hide();
}

/*重载通用设置*/
void Dialog::ReloadGeneralConfig()
{
    QSettings settings(IniSettingPath, QSettings::IniFormat);
    //限制读取到的尺寸，防止异常配置导致窗口不可用。
    const int dialogWidth =
        qBound(kMinDialogWidth,
               settings.value("general/DialogWidth", kDefaultDialogWidth).toInt(),
               kMaxDialogWidth);
    const int dialogHeight = qBound(
        kMinDialogHeight,
        settings.value("general/DialogHeight", kDefaultDialogHeight).toInt(),
        kMaxDialogHeight);

    resize(dialogWidth, dialogHeight);

    if (historyWin)
    {
        //历史记录框贴在对话框上方，宽度需要跟随对话框变化。
        historyWin->resize(dialogWidth, historyWin->height());
        if (historyWin->isVisible())
            historyWin->move(x(), y() - historyWin->height());
    }
}

/*打开关闭历史记录*/
void Dialog::handleWheelUp()
{
    if (!isHistoryOpen)
        ui->pushButton_history->click();
}
void Dialog::handleWheelDown()
{
    if (isHistoryOpen)
        ui->pushButton_history->click();
}

/*加载上下文历史*/
void Dialog::loadContextHistory()
{
    m_contextHistory.clear();
    const QString contextPath = ReadCharacterContextPath();
    if (contextPath.isEmpty())
        return;

    ZcJsonLib contextConfig(contextPath);
    const QJsonArray historyArray =
        contextConfig.value("history", QJsonValue(QJsonArray())).toArray();
    for (const QJsonValue &value : historyArray)
    {
        const QString line = value.toString();
        if (!line.isEmpty())
            m_contextHistory.append(line);
    }
}

/*构建用户消息，包含上下文*/
QString Dialog::buildUserMessageWithContext(const QString &input) const
{
    if (m_contextHistory.isEmpty())
        return input;

    return QStringLiteral(
               "以下是你和用户最近的对话，请延续上下文并保持人设一致：\n") +
           m_contextHistory.join("\n") + QStringLiteral("\n\n用户当前输入：") +
           input;
}

/*添加历史记录行*/
void Dialog::appendHistoryLine(const QString &line)
{
    if (line.isEmpty())
        return;
    m_contextHistory.append(line);
}

/*保存上下文历史*/
void Dialog::saveContextHistory() const
{
    const QString contextPath = ReadCharacterContextPath();
    if (contextPath.isEmpty())
        return;

    const QFileInfo fileInfo(contextPath);
    QDir().mkpath(fileInfo.absolutePath());

    QJsonArray historyArray;
    for (const QString &line : m_contextHistory)
        historyArray.append(line);

    ZcJsonLib contextConfig(contextPath);
    contextConfig.setValue("history", QJsonValue(historyArray));
}

/*停止当前对话的残留状态*/
void Dialog::stopPendingConversationState()
{
    //清空当前轮次缓存，避免回溯后旧流式结果继续写入界面或历史
    m_lastUserInput.clear();
    m_streamRawReply.clear();
    m_streamDisplayedChinese.clear();
    m_streamVitsEnabled = false;
    m_streamSynthCursor = 0;
    m_vitsPendingTexts.clear();
    m_vitsRequestInFlight = false;

    for (QTemporaryFile *file : m_vitsReadyFiles)
    {
        if (file)
            file->deleteLater();
    }
    m_vitsReadyFiles.clear();

    if (m_vitsTempFile)
    {
        m_vitsTempFile->deleteLater();
        m_vitsTempFile = nullptr;
    }

    if (m_vitsPlayer)
        m_vitsPlayer->stop();

    m_isSpeechRecognizing = false;
}

/*构建窗口*/
Dialog::Dialog(QWidget *parent)
    : QWidget(parent), ui(new Ui::Dialog)
{
    ui->setupUi(this);
    initWindow();

    /*AI初始化*/
    ai = new AiProvider(this);
    ai->setStreamEnabled(true);

    /*Vits初始化*/
    m_vitsManager = new QNetworkAccessManager(this);
    m_visionManager = new QNetworkAccessManager(this);
    m_vitsPlayer = new QMediaPlayer(this);
    m_vitsAudioOutput = new QAudioOutput(this);
    m_vitsPlayer->setAudioOutput(m_vitsAudioOutput);
    //播放完成后播放下一条
    connect(m_vitsPlayer, &QMediaPlayer::playbackStateChanged, this,
            [this](QMediaPlayer::PlaybackState state)
            {
                if (state == QMediaPlayer::StoppedState)
                {
                    if (m_vitsTempFile)
                    {
                        m_vitsTempFile->deleteLater();
                        m_vitsTempFile = nullptr;
                    }
                    tryStartNextVitsPlayback();
                }
            });
    /*语音输入初始化*/
    m_speechRecorder = new QMediaRecorder(this);
    m_speechAudioInput = new QAudioInput(this);
    m_speechCaptureSession.setRecorder(m_speechRecorder);
    m_speechCaptureSession.setAudioInput(m_speechAudioInput);
    if (m_speechAudioInput)
        m_speechAudioInput->setDevice(QMediaDevices::defaultAudioInput());
    //录音结束后统一进入识别，再决定是否直接复用现有发送链路
    connect(m_speechRecorder, &QMediaRecorder::recorderStateChanged, this,
            [this](QMediaRecorder::RecorderState state)
            {
                if (state != QMediaRecorder::StoppedState || !m_isSpeechRecording)
                    return;

                m_isSpeechRecording = false;
                m_isSpeechRecognizing = true;
                const QString recognizedText =
                    recognizeSpeechFromFile(speechRecordFilePath()).trimmed();
                m_isSpeechRecognizing = false;
                if (recognizedText.isEmpty())
                    return;

                ui->label_name->setText(QStringLiteral("你"));
                ui->textEdit->setEnabled(true);
                ui->textEdit->setText(recognizedText);
                if (ui->checkBox_autoInput->isChecked())
                    submitCurrentInput();
            });
    ReloadAIConfig();
    ReloadGeneralConfig();
    ReloadSpeechInputConfig();
    ReloadScreenCaptureConfig();
    loadContextHistory();
    loadMemory();

    //接收分块回复
    connect(ai, &AiProvider::replyChunkReceived, [=](const QString &chunk)
            {
                m_streamRawReply += chunk; //追加

                /*提取中文*/
                const int firstSep = m_streamRawReply.indexOf('|'); //寻找第一个分隔符
                if (firstSep < 0)
                    return;
                const int secondSep =
                    m_streamRawReply.indexOf('|',
                                             firstSep + 1); //寻找第二个分隔符
                const int chineseEnd =
                    secondSep < 0
                        ? m_streamRawReply.size()
                        : secondSep; //如果没有找到第二个分隔符，就以当前字符串末尾为中文结束位置
                const QString chinesePartial = m_streamRawReply.mid(
                    firstSep + 1, chineseEnd - firstSep - 1); //提取中文部分
                //更新中文的显示部分
                if (!chinesePartial.isEmpty() &&
                    chinesePartial != m_streamDisplayedChinese)
                {
                    m_streamDisplayedChinese = chinesePartial;
                    ui->textEdit->setText(m_streamDisplayedChinese);
                }

                /*第二个分隔符处理*/
                if (m_streamVitsEnabled && m_streamVitsSentenceSplitEnabled &&
                    secondSep >= 0)
                {
                    const QString japanesePartial =
                        m_streamRawReply.mid(secondSep + 1); //提取日语的全部内容
                    if (!japanesePartial.isEmpty())
                    {
                        int sentenceEnd =
                            findNextSentenceEnd(japanesePartial,
                                                m_streamSynthCursor); //初始化首个句尾位置
                        while (sentenceEnd >= 0)
                        {
                            const QString sentence =
                                japanesePartial
                                    .mid(m_streamSynthCursor,
                                         sentenceEnd - m_streamSynthCursor + 1)
                                    .trimmed();                    //获取从上一次切分位置到当前句子结束位置的文本
                            m_streamSynthCursor = sentenceEnd + 1; //记录切分位置
                            if (!sentence.isEmpty())
                            {
                                VitsGetAndPlay(sentence); //发送到语音合成
                            }
                            sentenceEnd = findNextSentenceEnd(
                                japanesePartial,
                                m_streamSynthCursor); //继续查找下一句结束位置
                        }
                    }
                } });

    //接收完整回复
    connect(ai, &AiProvider::replyReceived, [=](const QString &reply)
            {
                const QString finalReply = m_streamRawReply.isEmpty()
                                               ? reply
                                               : m_streamRawReply; //确保使用完整结果
                //解析回复
                const QString mood = finalReply.section('|', 0, 0).trimmed();
                const QString chineseReply = finalReply.section('|', 1, 1).trimmed();
                const QString japaneseReply = finalReply.section('|', 2, 2).trimmed();

                //界面更新
                ui->pushButton_next->show();
                ui->textEdit->setText(chineseReply); //提取中文内容并显示
                //语音合成补漏或收尾生成
                if (m_streamVitsEnabled)
                {
                    if (m_streamVitsSentenceSplitEnabled)
                    {
                        //若最后一段不足一句（无句末标点），在结束回包时补一次合成。
                        const QString remainJapanese =
                            japaneseReply.mid(qMax(0, m_streamSynthCursor)).trimmed();
                        if (!remainJapanese.isEmpty())
                            VitsGetAndPlay(remainJapanese);
                    }
                    else
                    {
                        //关闭切分后，仅在完整日语输出后一次性生成语音。
                        if (!japaneseReply.isEmpty())
                            VitsGetAndPlay(japaneseReply);
                    }
                }
                emit requestSetCharTachie(mood); //提取心情并发出信号

                //历史记录写入
                const QString capturedUserInput = m_lastUserInput;
                if (!m_lastUserInput.isEmpty())
                {
                    appendHistoryLine(QStringLiteral("用户：") + m_lastUserInput);
                    m_lastUserInput.clear();
                }
                appendHistoryLine(QStringLiteral("角色：") + chineseReply);
                saveContextHistory();

                // 异步提取记忆（不阻塞界面）
                if (!capturedUserInput.isEmpty())
                    extractAndStoreMemory(capturedUserInput, chineseReply);

                //重置内容
                m_streamRawReply.clear();
                m_streamDisplayedChinese.clear();
                m_streamVitsEnabled = false;
                m_streamSynthCursor = 0; });
    //错误处理
    connect(ai, &AiProvider::errorOccurred, [=](const QString &error)
            {
                ui->pushButton_next->show();
                ui->textEdit->setText(error);
                ui->textEdit->setEnabled(false);
                m_lastUserInput.clear();
                m_streamRawReply.clear();
                m_streamDisplayedChinese.clear();
                m_streamVitsEnabled = false;
                m_streamSynthCursor = 0; });
}

/*解构窗口*/
Dialog::~Dialog()
{
    releaseSpeechHotkeyResources();
    delete ui;
}

/*按键相关*/
void Dialog::keyPressEvent(QKeyEvent *event)
{
    keys.append(event->key());
}
void Dialog::keyReleaseEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Return)
        /*发送对话请求*/
        if (!keys.contains(Qt::Key_Shift)) //过滤Shift换行
            submitCurrentInput();
    keys.removeAll(event->key());
}

/*点击继续*/
void Dialog::on_pushButton_next_clicked()
{
    ui->label_name->setText("你");
    ui->textEdit->clear();
    ui->textEdit->setEnabled(true);
    ui->pushButton_next->hide();
}

/*开关窗口*/
void Dialog::ToggleVisible()
{
    setVisible(!isVisible());
}

/*重载ai配置*/
void Dialog::ReloadAIConfig()
{
    /*AI初始化*/
    ZcJsonLib CharConfig(ReadCharacterUserConfigPath());
    //读取当前角色的服务商选择
    QString serverSelect = CharConfig.value("serverSelect").toString();
    if (serverSelect == "DeepSeek")
        ai->setServiceType(AiProvider::DeepSeek);
    else if (serverSelect == "OpenAI")
        ai->setServiceType(AiProvider::OpenAI);
    else if (serverSelect == "Custom")
        ai->setServiceType(AiProvider::Custom);
    else
    {
        serverSelect = "DeepSeek";
        ai->setServiceType(AiProvider::DeepSeek);
    }

    ZcJsonLib config(JsonSettingPath);
    QString apiKey = config.value("llm/" + serverSelect + "/ApiKey").toString();
    ai->setApiKey(apiKey);
    if (serverSelect == "Custom")
    {
        QString baseUrl = config.value("llm/Custom/BaseUrl").toString().trimmed();
        if (baseUrl.isEmpty())
            ai->setApiUrl(QString());
        else
            ai->setBaseUrl(baseUrl);
    }

    //读取当前角色的模型选择
    QString modelSelect = CharConfig.value("modelSelect").toString();
    ai->setModel(modelSelect);

    loadContextHistory();
    loadMemory();
}

/*重载语音输入配置*/
void Dialog::ReloadSpeechInputConfig()
{
    ZcJsonLib config(JsonSettingPath);
    const bool speechEnabled =
        config.value("speechInput/Enable", false).toBool();
    const bool autoSend =
        config.value("speechInput/AutoSend", false).toBool();
    const bool globalHotkeyEnable =
        config.value("speechInput/GlobalHotkey/Enable", false).toBool();
    const quint32 globalHotkeyNativeKey = static_cast<quint32>(
        config.value("speechInput/GlobalHotkey/NativeKey", 0).toInteger());

    ui->pushButton_input->setVisible(speechEnabled);
    ui->pushButton_input->setEnabled(speechEnabled);
    ui->checkBox_autoInput->setVisible(speechEnabled);
    ui->checkBox_autoInput->blockSignals(true);
    ui->checkBox_autoInput->setChecked(autoSend);
    ui->checkBox_autoInput->blockSignals(false);

    //配置变化后重新安装热键，避免旧按键继续占用。
    releaseSpeechHotkeyResources();
    m_globalSpeechHotkeyNativeKey = globalHotkeyNativeKey;
    m_globalSpeechHotkeyEnabled =
        globalHotkeyEnable && globalHotkeyNativeKey != 0;

#ifdef Q_OS_WIN
    if (m_globalSpeechHotkeyEnabled)
    {
        //Windows全局热键用低级键盘钩子，松开按键时结束录音。
        g_speechHotkeyOwner = this;
        g_speechHotkeyHook =
            SetWindowsHookExW(WH_KEYBOARD_LL, SpeechHotkeyHookProc, nullptr, 0);
    }
#endif

#ifdef Q_OS_MACOS
    if (m_globalSpeechHotkeyEnabled)
    {
        qWarning() << "Global speech hotkey is not supported on macOS yet";
        m_globalSpeechHotkeyEnabled = false;
    }
#endif

#ifdef Q_OS_LINUX
    if (m_globalSpeechHotkeyEnabled)
    {
        Display *display = XOpenDisplay(nullptr);
        if (display)
        {
            //忽略大小写和小键盘锁定状态，避免锁定键影响热键触发。
            const Window targetWindow = static_cast<Window>(winId());
            const int modifiers[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
            for (int modifier : modifiers)
                XGrabKey(display, static_cast<int>(m_globalSpeechHotkeyNativeKey),
                         modifier, targetWindow, True, GrabModeAsync,
                         GrabModeAsync);
            XSync(display, False);
            XCloseDisplay(display);
        }
    }
#endif
}

/*显示历史记录*/
void Dialog::on_pushButton_history_clicked()
{
    if (!historyWin)
    {
        historyWin = new history(this);
        connect(historyWin, &history::jumpToHistory, this,
                &Dialog::rewindToHistoryIndex);
    }

    //刷新历史记录内容
    historyWin->clearHistory();
    for (int i = 0; i < m_contextHistory.size(); ++i)
    {
        const QString &line = m_contextHistory.at(i);
        if (line.startsWith(QStringLiteral("用户：")))
            historyWin->addChildWindow(i, QStringLiteral("你"), line.mid(3));
        else if (line.startsWith(QStringLiteral("角色：")))
            historyWin->addChildWindow(i, QStringLiteral("她"), line.mid(3));
        else
            historyWin->addChildWindow(i, QStringLiteral("记录"), line);
    }

    //历史记录框宽度跟随对话框，保持上下窗口对齐。
    historyWin->resize(width(), historyWin->height());
    historyWin->move(this->x(), this->y() - historyWin->height());

    if (!isHistoryOpen)
    {
        historyWin->show();
        historyWin->raise();
        isHistoryOpen = true;

        //显示历史记录窗口动画效果
        QGraphicsOpacityEffect *opacityEffect =
            new QGraphicsOpacityEffect(historyWin);
        historyWin->setGraphicsEffect(opacityEffect);

        QRect startRect = historyWin->geometry();
        QRect endRect = startRect;
        startRect.moveTop(startRect.top() + 20);
        historyWin->setGeometry(startRect);
        opacityEffect->setOpacity(0.0);

        QPropertyAnimation *opacityAnim =
            new QPropertyAnimation(opacityEffect, "opacity");
        opacityAnim->setDuration(150);
        opacityAnim->setStartValue(0.0);
        opacityAnim->setEndValue(1.0);

        QPropertyAnimation *moveAnim =
            new QPropertyAnimation(historyWin, "geometry");
        moveAnim->setDuration(150);
        moveAnim->setStartValue(startRect);
        moveAnim->setEndValue(endRect);

        QParallelAnimationGroup *group = new QParallelAnimationGroup(historyWin);
        group->addAnimation(opacityAnim);
        group->addAnimation(moveAnim);
        group->start(QAbstractAnimation::DeleteWhenStopped);
    }
    else
    {
        isHistoryOpen = false;

        QRect startRect = historyWin->geometry();
        QRect endRect = startRect;
        endRect.moveTop(endRect.top() + 20);

        //隐藏历史记录动画效果
        QGraphicsOpacityEffect *opacityEffect =
            qobject_cast<QGraphicsOpacityEffect *>(historyWin->graphicsEffect());
        if (!opacityEffect)
        {
            opacityEffect = new QGraphicsOpacityEffect(historyWin);
            historyWin->setGraphicsEffect(opacityEffect);
        }

        QPropertyAnimation *opacityAnim =
            new QPropertyAnimation(opacityEffect, "opacity");
        opacityAnim->setDuration(150);
        opacityAnim->setStartValue(1.0);
        opacityAnim->setEndValue(0.0);

        QPropertyAnimation *moveAnim =
            new QPropertyAnimation(historyWin, "geometry");
        moveAnim->setDuration(150);
        moveAnim->setStartValue(startRect);
        moveAnim->setEndValue(endRect);

        QParallelAnimationGroup *group = new QParallelAnimationGroup(historyWin);
        group->addAnimation(opacityAnim);
        group->addAnimation(moveAnim);
        connect(group, &QParallelAnimationGroup::finished, historyWin,
                &QWidget::hide);
        group->start(QAbstractAnimation::DeleteWhenStopped);
    }
}

/*回退历史*/
void Dialog::rewindToHistoryIndex(int historyIndex)
{
    if (historyIndex < 0 || historyIndex >= m_contextHistory.size())
        return;

    //先停掉当前会话残留，再把历史截断到目标位置
    stopPendingConversationState();
    m_contextHistory = m_contextHistory.mid(0, historyIndex + 1);
    saveContextHistory();

    const QString selectedLine = m_contextHistory.at(historyIndex);
    if (selectedLine.startsWith(QStringLiteral("用户：")))
    {
        ui->label_name->setText(QStringLiteral("你"));
        ui->textEdit->setEnabled(true);
        ui->textEdit->setText(selectedLine.mid(3));
        ui->pushButton_next->hide();
    }
    else if (selectedLine.startsWith(QStringLiteral("角色：")))
    {
        ui->label_name->setText(QStringLiteral("她"));
        ui->textEdit->setEnabled(false);
        ui->textEdit->setText(selectedLine.mid(3));
        ui->pushButton_next->show();
    }
    else
    {
        ui->label_name->setText(QStringLiteral("记录"));
        ui->textEdit->setEnabled(false);
        ui->textEdit->setText(selectedLine);
        ui->pushButton_next->show();
    }

    if (historyWin && isHistoryOpen)
        on_pushButton_history_clicked();
}

/*移动窗口*/
void Dialog::moveEvent(QMoveEvent *event)
{
    if (historyWin && historyWin->isVisible())
    {
        QPoint offset = event->pos() - lastPos;
        historyWin->move(historyWin->pos() + offset);
    }
    lastPos = event->pos();
    QWidget::moveEvent(event);
}

/*滚动窗口*/
void Dialog::wheelEvent(QWheelEvent *event)
{
    if (event->angleDelta().y() > 0)
        handleWheelUp();
    else if (event->angleDelta().y() < 0)
        handleWheelDown();
    QWidget::wheelEvent(event);
}

/*拦截普通的滚动*/
bool Dialog::eventFilter(QObject *watched, QEvent *event)
{
    if ((watched == ui->textEdit || watched == ui->textEdit->viewport()) &&
        event->type() == QEvent::Wheel)
    {
        QWheelEvent *wheelEvent = static_cast<QWheelEvent *>(event);
        if (wheelEvent->angleDelta().y() > 0)
            handleWheelUp();
        else if (wheelEvent->angleDelta().y() < 0)
            handleWheelDown();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

bool Dialog::nativeEvent(const QByteArray &eventType, void *message,
                         qintptr *result)
{
#ifdef Q_OS_LINUX
    Q_UNUSED(eventType)
    Q_UNUSED(result)
    if (m_globalSpeechHotkeyEnabled && message)
    {
        XEvent *event = static_cast<XEvent *>(message);
        if (event->type == KeyPress &&
            handleSpeechHotkeyEvent(event->xkey.keycode, true, false))
            return true;
        if (event->type == KeyRelease &&
            handleSpeechHotkeyEvent(event->xkey.keycode, false, true))
            return true;
    }
#else
    Q_UNUSED(eventType)
    Q_UNUSED(message)
    Q_UNUSED(result)
#endif
    return QWidget::nativeEvent(eventType, message, result);
}

/*追加待合成文本*/
void Dialog::VitsGetAndPlay(QString text)
{
    qDebug() << "请求合成文本：" << text;

    m_vitsPendingTexts.append(text);
    tryStartNextVitsRequest();
}

/*启动下一个Vits请求*/
void Dialog::tryStartNextVitsRequest()
{
    if (!m_vitsManager || !m_vitsPlayer)
        return;
    if (m_vitsRequestInFlight || m_vitsPendingTexts.isEmpty())
        return;

    const QString text = m_vitsPendingTexts.takeFirst();
    if (text.isEmpty())
        return;

    /*请求地址构建*/
    //获取地址
    ZcJsonLib config(JsonSettingPath);
    QString apiUrl = config.value("vits/ApiUrl").toString();
    //获取选中模型
    ZcJsonLib charConfig(ReadCharacterUserConfigPath());
    QString modelAndSpeaker = charConfig.value("vitsMasSelect").toString();
    QString model =
        modelAndSpeaker.section(" - ", 0, 0).trimmed().toLower(); //提取模型名
    QString speaker =
        modelAndSpeaker.section(" - ", 2, 2).trimmed(); //提取说话人
    //构建请求 URL，对文本进行 URL 编码
    QString urlString = QString(apiUrl + "/voice/%2?id=%3&text=%1")
                            .arg(QString(QUrl::toPercentEncoding(text)))
                            .arg(QString(QUrl::toPercentEncoding(model)))
                            .arg(QString(QUrl::toPercentEncoding(speaker)));
    qInfo() << "语音合成请求到" << modelAndSpeaker;

    m_vitsRequestInFlight = true;
    QNetworkRequest request(urlString);
    //发送 GET 请求
    QNetworkReply *reply = m_vitsManager->get(request);
    //连接信号处理响应
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]()
                     {
                         m_vitsRequestInFlight = false;

                         if (reply->error() == QNetworkReply::NoError)
                         {
                             QByteArray audioData = reply->readAll();
                             if (!audioData.isEmpty())
                             {
                                 QTemporaryFile *readyFile =
                                     new QTemporaryFile(QDir::tempPath() + "/vits_XXXXXX.mp3", this);
                                 if (readyFile->open())
                                 {
                                     readyFile->write(audioData);
                                     readyFile->flush();
                                     //合成完成先入就绪队列，播放端按顺序播放
                                     m_vitsReadyFiles.append(readyFile);
                                     tryStartNextVitsPlayback();
                                 }
                                 else
                                 {
                                     readyFile->deleteLater();
                                 }
                             }
                         }

                         reply->deleteLater();
                         //当前请求结束后立即尝试合成下一句，实现“合成前置”。
                         tryStartNextVitsRequest(); });
}

/*启动下一个Vits播放*/
void Dialog::tryStartNextVitsPlayback()
{
    if (!m_vitsPlayer)
        return;
    if (m_vitsPlayer->playbackState() != QMediaPlayer::StoppedState)
        return;
    if (m_vitsReadyFiles.isEmpty())
        return;

    if (m_vitsTempFile)
    {
        m_vitsTempFile->deleteLater();
        m_vitsTempFile = nullptr;
    }

    m_vitsTempFile = m_vitsReadyFiles.takeFirst();
    if (!m_vitsTempFile)
        return;

    //播放严格串行：只有播放器空闲才取下一句。
    m_vitsPlayer->setSource(QUrl::fromLocalFile(m_vitsTempFile->fileName()));
    m_vitsPlayer->play();
}

/*提交当前输入*/
bool Dialog::submitCurrentInput()
{
    ui->label_name->setText("她");
    ui->textEdit->setEnabled(false);
    ui->pushButton_next->hide();

    QTextCursor cursor = ui->textEdit->textCursor();
    if (cursor.hasSelection())
        cursor.clearSelection();
    if (ui->textEdit->toPlainText().endsWith('\n') && cursor.position() > 0)
        cursor.deletePreviousChar();

    const QString userInput = ui->textEdit->toPlainText().trimmed();
    if (userInput.isEmpty())
    {
        ui->textEdit->clear();
        ui->textEdit->setEnabled(true);
        ui->label_name->setText(QStringLiteral("你"));
        return false;
    }

    // 屏幕捕获关键词检测
    if (m_screenCaptureEnabled)
    {
        const QString lowerInput = userInput.toLower();
        const QStringList triggers = screenCaptureTriggerKeywords();
        bool triggered = false;
        for (const QString &kw : triggers)
        {
            if (lowerInput.contains(kw))
            {
                triggered = true;
                break;
            }
        }
        if (triggered)
        {
            m_lastUserInput = userInput;
            ui->textEdit->setText(QStringLiteral("正在分析屏幕内容……"));
            captureAndAnalyzeScreen();
            return true;
        }
    }

    return doSubmitCurrentInput(userInput);
}

/*执行提交逻辑（供屏幕捕获回调复用）*/
bool Dialog::doSubmitCurrentInput(const QString &userInput)
{
    QDir dir(ReadCharacterTachiePath());
    QStringList nameFilters;
    nameFilters << "*.png" << "*.jpg" << "*.jpeg";
    QStringList fileNames = dir.entryList(nameFilters, QDir::Files);
    QString nameListStr;
    for (const QString &fileName : fileNames)
        nameListStr += fileName.section('.', 0, 0) + ", ";

    ZcJsonLib roleConfig(CharacterAssestPath + "/" + ReadNowSelectChar() +
                         "/config.json");
    const QString characterPrompt =
        roleConfig.value("prompt").toString().trimmed();
    QString systemPrompt;

    // 注入用户记忆
    const QString memoryContext = buildMemoryContext();
    if (!memoryContext.isEmpty())
        systemPrompt += memoryContext + QStringLiteral("\n\n");

    if (!characterPrompt.isEmpty())
        systemPrompt += QStringLiteral("角色设定：") + characterPrompt +
                        QStringLiteral("\n请始终保持该设定进行回复。\n\n");
    systemPrompt +=
        QStringLiteral("你是一个桌宠 AI，输出内容必须严格按照以下格式：\n"
                       "心情|中文|日语\n\n"
                       "要求：\n"
                       "1. 心情必须从以下列表中选择：") +
        nameListStr + "\n" +
        QStringLiteral("2. 中文是桌宠此刻想表达的内容\n"
                       "3. 日语是中文内容的对应翻译\n"
                       "4. 输出中不能有多余内容或解释，严格用“|”分隔\n\n"
                       "示例输出：\n"
                       "快乐|今天的天气真好呀！|今日はいい天気ですね！\n"
                       "生气|为什么一直打扰我！|なんでずっと邪魔するの！");
    ai->setSystemPrompt(systemPrompt);

    m_lastUserInput = userInput;
    ZcJsonLib charConfig(ReadCharacterUserConfigPath());
    m_streamVitsEnabled = charConfig.value("vitsEnable").toBool();
    ZcJsonLib config(JsonSettingPath);
    m_streamVitsSentenceSplitEnabled =
        config.value("vits/SentenceSplit", true).toBool();
    m_streamRawReply.clear();
    m_streamDisplayedChinese.clear();
    m_streamSynthCursor = 0;
    m_vitsPendingTexts.clear();
    for (QTemporaryFile *file : m_vitsReadyFiles)
    {
        if (file)
            file->deleteLater();
    }
    m_vitsReadyFiles.clear();
    m_vitsRequestInFlight = false;
    if (m_vitsTempFile)
    {
        m_vitsTempFile->deleteLater();
        m_vitsTempFile = nullptr;
    }
    if (m_vitsPlayer)
        m_vitsPlayer->stop();
    ai->chat(buildUserMessageWithContext(userInput));
    ui->textEdit->setText("……");
    return true;
}

/*长按语言输入相关*/
void Dialog::on_pushButton_input_pressed()
{
    startSpeechRecording();
}
void Dialog::on_pushButton_input_released()
{
    stopSpeechRecording();
}

/*自动发送开关*/
void Dialog::on_checkBox_autoInput_toggled(bool checked)
{
    ZcJsonLib config(JsonSettingPath);
    config.setValue("speechInput/AutoSend", checked);
}

/*开始录音*/
void Dialog::startSpeechRecording()
{
    if (!ui->pushButton_input->isVisible())
        return;

    startSpeechRecordingFromHotkey();
}

void Dialog::startSpeechRecordingFromHotkey()
{
    if (!ui->textEdit->isEnabled() || m_isSpeechRecording ||
        m_isSpeechRecognizing || !m_speechRecorder || !m_speechAudioInput)
        return;

#ifdef Q_OS_MACOS
    auto *app = QCoreApplication::instance();
    if (app)
    {
        const QMicrophonePermission microphonePermission;
        const Qt::PermissionStatus status =
            app->checkPermission(microphonePermission);

        if (status == Qt::PermissionStatus::Denied)
        {
            ui->textEdit->setText(QStringLiteral("麦克风权限未开启，请在系统设置中允许 ZcChat2 使用麦克风"));
            return;
        }

        if (status == Qt::PermissionStatus::Undetermined)
        {
            ui->textEdit->setText(QStringLiteral("正在请求麦克风权限……"));
            app->requestPermission(microphonePermission, this,
                                   [this](const QPermission &permission)
                                   {
                                       if (permission.status() ==
                                           Qt::PermissionStatus::Granted)
                                           startSpeechRecordingFromHotkey();
                                       else
                                           ui->textEdit->setText(QStringLiteral(
                                               "麦克风权限未开启，请在系统设置中允许 ZcChat2 使用麦克风"));
                                   });
            return;
        }
    }
#endif

    if (QMediaDevices::defaultAudioInput().isNull())
    {
        ui->textEdit->setText(QStringLiteral("未检测到可用麦克风"));
        return;
    }

    //录音文件统一落到临时目录，识别完成后直接读取
    m_speechAudioInput->setDevice(QMediaDevices::defaultAudioInput());
    QDir().mkpath(QFileInfo(speechRecordFilePath()).absolutePath());

    QMediaFormat format;
    format.setAudioCodec(QMediaFormat::AudioCodec::AAC);
    m_speechRecorder->setMediaFormat(format);
    m_speechRecorder->setAudioSampleRate(16000);
    m_speechRecorder->setAudioChannelCount(1);
    m_speechRecorder->setQuality(QMediaRecorder::HighQuality);
    m_speechRecorder->setOutputLocation(
        QUrl::fromLocalFile(speechRecordFilePath()));

    m_isSpeechRecording = true;
    ui->textEdit->setText(QStringLiteral("录音中……"));
    m_speechRecorder->record();
}

/*结束录音*/
void Dialog::stopSpeechRecording()
{
    if (!m_isSpeechRecording || !m_speechRecorder)
        return;
    ui->label_name->setText(QStringLiteral("你"));
    m_speechRecorder->stop();
}

/*录音文件路径*/
QString Dialog::speechRecordFilePath() const
{
    return QDir(QDir::tempPath()).filePath("ZcChat2/speech_input.m4a");
}

/*获取百度 Token*/
QString Dialog::requestBaiduAccessToken(const QString &apiKey,
                                        const QString &secretKey)
{
    if (apiKey.trimmed().isEmpty() || secretKey.trimmed().isEmpty())
        return QString();

    QNetworkAccessManager manager;
    QUrl url("https://aip.baidubce.com/oauth/2.0/token");
    QUrlQuery query;
    query.addQueryItem("grant_type", "client_credentials");
    query.addQueryItem("client_id", apiKey);
    query.addQueryItem("client_secret", secretKey);
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QEventLoop loop;
    QNetworkReply *reply = manager.post(request, QByteArray());
    QString accessToken;
    connect(reply, &QNetworkReply::finished, &loop, [&]()
            {
                if (reply->error() == QNetworkReply::NoError)
                {
                    const QJsonDocument doc =
                        QJsonDocument::fromJson(reply->readAll());
                    accessToken =
                        doc.object().value("access_token").toString().trimmed();
                }
                reply->deleteLater();
                loop.quit(); });
    loop.exec();
    return accessToken;
}

/*识别录音文件*/
QString Dialog::recognizeSpeechFromFile(const QString &filePath)
{
    QFile file(filePath);
    if (!file.exists() || file.size() <= 0)
        return QString();

    ZcJsonLib config(JsonSettingPath);
    const QString apiKey =
        config.value("speechInput/Baidu/ApiKey").toString().trimmed();
    const QString secretKey =
        config.value("speechInput/Baidu/SecretKey").toString().trimmed();
    const QString accessToken = requestBaiduAccessToken(apiKey, secretKey);
    if (accessToken.isEmpty())
    {
        ui->textEdit->setText(QStringLiteral("百度语音识别配置不完整或 Token 获取失败"));
        return QString();
    }

    if (!file.open(QIODevice::ReadOnly))
    {
        ui->textEdit->setText(QStringLiteral("无法读取录音文件"));
        return QString();
    }
    const QByteArray audioData = file.readAll();
    file.close();
    if (audioData.isEmpty())
        return QString();

    //沿用旧项目的百度短语音识别请求格式，直接提交 m4a 的 Base64 数据
    QJsonObject payload{
        {"format", "m4a"},
        {"rate", 16000},
        {"channel", 1},
        {"token", accessToken},
        {"cuid", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"speech", QString::fromLatin1(audioData.toBase64())},
        {"len", audioData.size()},
    };

    QNetworkAccessManager manager;
    QNetworkRequest request(QUrl("https://vop.baidu.com/server_api"));
    request.setRawHeader("Content-Type", "application/json");
    request.setRawHeader("Accept", "application/json");

    QEventLoop loop;
    QNetworkReply *reply =
        manager.post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QString recognizedText;
    connect(reply, &QNetworkReply::finished, &loop, [&]()
            {
                if (reply->error() == QNetworkReply::NoError)
                {
                    const QJsonDocument doc =
                        QJsonDocument::fromJson(reply->readAll());
                    const QJsonArray result =
                        doc.object().value("result").toArray();
                    if (!result.isEmpty())
                        recognizedText = result.first().toString().trimmed();
                }
                reply->deleteLater();
                loop.quit(); });
    loop.exec();

    if (recognizedText.isEmpty())
        ui->textEdit->setText(QStringLiteral("没有识别到有效语音"));
    return recognizedText;
}

bool Dialog::handleSpeechHotkeyEvent(quint32 vkCode, bool isKeyDown, bool isKeyUp)
{
    if (!m_globalSpeechHotkeyEnabled || m_globalSpeechHotkeyNativeKey == 0)
        return false;

    //按下开始录音，重复KeyDown不重复启动。
    if (isKeyDown && vkCode == m_globalSpeechHotkeyNativeKey)
    {
        if (!m_globalSpeechHotkeyPressed)
        {
            m_globalSpeechHotkeyPressed = true;
            QMetaObject::invokeMethod(this, [this]()
                                      { startSpeechRecordingFromHotkey(); }, Qt::QueuedConnection);
        }
        return true;
    }

    //松开同一个热键才停止录音，保持“长按说话”的交互。
    if (m_globalSpeechHotkeyPressed &&
        isKeyUp && vkCode == m_globalSpeechHotkeyNativeKey)
    {
        m_globalSpeechHotkeyPressed = false;
        QMetaObject::invokeMethod(this, [this]()
                                  { stopSpeechRecording(); }, Qt::QueuedConnection);
        return true;
    }
    return false;
}

void Dialog::releaseSpeechHotkeyResources()
{
    //释放热键时如果仍在按住录音，先走一次停止逻辑。
    if (m_globalSpeechHotkeyPressed)
        stopSpeechRecording();
    m_globalSpeechHotkeyPressed = false;

#ifdef Q_OS_WIN
    if (g_speechHotkeyOwner == this)
    {
        if (g_speechHotkeyHook)
        {
            UnhookWindowsHookEx(g_speechHotkeyHook);
            g_speechHotkeyHook = nullptr;
        }
        g_speechHotkeyOwner = nullptr;
    }
#endif

#ifdef Q_OS_LINUX
    if (m_globalSpeechHotkeyNativeKey != 0)
    {
        Display *display = XOpenDisplay(nullptr);
        if (display)
        {
            const Window targetWindow = static_cast<Window>(winId());
            const int modifiers[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
            for (int modifier : modifiers)
                XUngrabKey(display, static_cast<int>(m_globalSpeechHotkeyNativeKey),
                           modifier, targetWindow);
            XSync(display, False);
            XCloseDisplay(display);
        }
    }
#endif
}

/* 加载记忆文件 */
void Dialog::loadMemory()
{
    const QString memoryPath = ReadCharacterMemoryPath();
    if (memoryPath.isEmpty())
        return;

    QFile file(memoryPath);
    if (!file.exists())
    {
        m_memoryData = QJsonObject();
        // 创建初始空记忆文件，确保文件在首次使用时就被创建
        saveMemory();
        return;
    }

    if (!file.open(QIODevice::ReadOnly))
    {
        qWarning() << "loadMemory: failed to open file for reading" << memoryPath;
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    if (doc.isObject())
        m_memoryData = doc.object();
    else
        m_memoryData = QJsonObject();
}

/* 保存记忆文件 */
void Dialog::saveMemory() const
{
    const QString memoryPath = ReadCharacterMemoryPath();
    if (memoryPath.isEmpty())
        return;

    const QFileInfo fileInfo(memoryPath);
    if (!QDir().mkpath(fileInfo.absolutePath()))
    {
        qWarning() << "saveMemory: failed to create directory" << fileInfo.absolutePath();
        return;
    }

    QFile file(memoryPath);
    if (!file.open(QIODevice::WriteOnly))
    {
        qWarning() << "saveMemory: failed to open file for writing" << memoryPath;
        return;
    }

    const QJsonDocument doc(m_memoryData);
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();
}

/* 构建记忆上下文文本，用于注入系统提示词 */
QString Dialog::buildMemoryContext() const
{
    QString context;

    // 个人信息
    const QJsonObject personalInfo = m_memoryData.value("personal_info").toObject();
    if (!personalInfo.isEmpty())
    {
        context += QStringLiteral("关于用户的记忆：\n");
        for (auto it = personalInfo.begin(); it != personalInfo.end(); ++it)
        {
            context += QStringLiteral("- ") + it.key() +
                       QStringLiteral("：") + it.value().toString() +
                       QStringLiteral("\n");
        }
    }

    // 帮助摘要
    const QJsonArray helpSummaries =
        m_memoryData.value("help_summaries").toArray();
    if (!helpSummaries.isEmpty())
    {
        if (!context.isEmpty())
            context += QStringLiteral("\n");
        context += QStringLiteral("用户曾向你寻求过的帮助：\n");
        for (const QJsonValue &val : helpSummaries)
        {
            const QJsonObject summary = val.toObject();
            context += QStringLiteral("- ") +
                       summary.value("topic").toString() +
                       QStringLiteral("：") +
                       summary.value("summary").toString() +
                       QStringLiteral("\n");
        }
    }

    return context;
}

/* 从对话中异步提取记忆 */
void Dialog::extractAndStoreMemory(const QString &userInput,
                                    const QString &aiReply)
{
    if (userInput.trimmed().isEmpty() || aiReply.trimmed().isEmpty())
        return;

    // 创建独立的 AI 实例用于记忆提取（非流式）
    AiProvider *memoryAi = new AiProvider(this);
    memoryAi->setStreamEnabled(false);

    // 复用当前角色的 AI 配置
    ZcJsonLib charConfig(ReadCharacterUserConfigPath());
    QString serverSelect = charConfig.value("serverSelect").toString();
    if (serverSelect == "DeepSeek")
        memoryAi->setServiceType(AiProvider::DeepSeek);
    else if (serverSelect == "OpenAI")
        memoryAi->setServiceType(AiProvider::OpenAI);
    else if (serverSelect == "Custom")
        memoryAi->setServiceType(AiProvider::Custom);
    else
        memoryAi->setServiceType(AiProvider::DeepSeek);

    const ZcJsonLib config(JsonSettingPath);
    const QString apiKey =
        config.value("llm/" + serverSelect + "/ApiKey").toString();
    memoryAi->setApiKey(apiKey);
    if (serverSelect == "Custom")
    {
        const QString baseUrl =
            config.value("llm/Custom/BaseUrl").toString().trimmed();
        if (!baseUrl.isEmpty())
            memoryAi->setBaseUrl(baseUrl);
    }
    const QString modelSelect = charConfig.value("modelSelect").toString();
    memoryAi->setModel(modelSelect);

    // 记忆提取专用的系统提示词
    memoryAi->setSystemPrompt(QStringLiteral(
        "你是一个信息提取助手。你的任务是从对话中提取值得长期记忆的用户信息。\n"
        "你必须严格只返回JSON，不要包含任何其他文字、解释或markdown标记。"));

    // 构建包含对话内容和提取规则的聊天消息
    const QString extractionMessage = QStringLiteral(
        "分析以下对话，提取值得记忆的信息：\n\n"
        "用户：%1\n"
        "角色：%2\n\n"
        "请严格返回以下JSON格式（仅JSON，无其他内容）：\n"
        "{\"has_personal_info\":false,\"personal_info\":{},\"is_help\":false,"
        "\"help_summary\":\"\"}\n\n"
        "判断规则：\n"
        "1. has_personal_info：对话中是否包含用户的独特个人信息。\n"
        "   - 值得记忆：名字、昵称、年龄、职业、喜好、习惯、家庭成员、重要经历等\n"
        "   - 不需要记忆：日常寒暄、临时情绪、对天气/食物的随口评价\n"
        "2. personal_info：以键值对给出。例如{\"名字\":\"小明\",\"爱好\":\"编程\"}。\n"
        "   没有则为{}。\n"
        "3. is_help：用户是否在寻求帮助/建议/教学/问题解答。\n"
        "   普通闲聊、分享日常、表达情绪不算求助。\n"
        "4. help_summary：仅在is_help为true时填写，用一句话（30字内）概括。\n"
        "   格式：\"[问题类型]用户问题简述\"")
        .arg(userInput, aiReply);

    // 处理提取结果
    connect(memoryAi, &AiProvider::replyReceived, this,
            [this, memoryAi](const QString &reply)
            {
                // 尝试清理可能的 markdown 代码块包装
                QString jsonText = reply.trimmed();
                if (jsonText.startsWith("```"))
                {
                    const int firstNewline = jsonText.indexOf('\n');
                    if (firstNewline >= 0)
                        jsonText = jsonText.mid(firstNewline + 1);
                }
                if (jsonText.endsWith("```"))
                    jsonText = jsonText.left(jsonText.lastIndexOf("```")).trimmed();

                const QJsonDocument doc =
                    QJsonDocument::fromJson(jsonText.toUtf8());
                if (!doc.isObject())
                {
                    memoryAi->deleteLater();
                    return;
                }

                const QJsonObject result = doc.object();
                bool changed = false;

                // 处理个人信息
                if (result.value("has_personal_info").toBool(false))
                {
                    const QJsonObject newInfo =
                        result.value("personal_info").toObject();
                    QJsonObject existingInfo =
                        m_memoryData.value("personal_info").toObject();

                    for (auto it = newInfo.begin(); it != newInfo.end(); ++it)
                    {
                        const QString key = it.key().trimmed();
                        const QString value = it.value().toString().trimmed();
                        if (!key.isEmpty() && !value.isEmpty())
                        {
                            if (!existingInfo.contains(key) ||
                                existingInfo.value(key).toString() != value)
                            {
                                existingInfo[key] = value;
                                changed = true;
                            }
                        }
                    }

                    if (changed)
                        m_memoryData["personal_info"] = existingInfo;
                }

                // 处理帮助摘要
                if (result.value("is_help").toBool(false))
                {
                    const QString helpSummary =
                        result.value("help_summary").toString().trimmed();
                    if (!helpSummary.isEmpty())
                    {
                        QJsonArray helpSummaries =
                            m_memoryData.value("help_summaries").toArray();

                        // 去重检查
                        bool duplicate = false;
                        for (const QJsonValue &val : helpSummaries)
                        {
                            if (val.toObject().value("summary").toString() ==
                                helpSummary)
                            {
                                duplicate = true;
                                break;
                            }
                        }

                        if (!duplicate)
                        {
                            // 保留最近20条，避免膨胀
                            if (helpSummaries.size() >= 20)
                                helpSummaries.removeFirst();

                            QJsonObject newSummary;
                            newSummary["topic"] = helpSummary;
                            newSummary["summary"] = helpSummary;
                            newSummary["date"] =
                                QDate::currentDate().toString("yyyy-MM-dd");
                            helpSummaries.append(newSummary);
                            m_memoryData["help_summaries"] = helpSummaries;
                            changed = true;
                        }
                    }
                }

                if (changed)
                    saveMemory();

                memoryAi->deleteLater();
            });

    // 错误处理：静默失败，不影响用户体验
    connect(memoryAi, &AiProvider::errorOccurred, this,
            [memoryAi](const QString &error)
            {
                qWarning() << "Memory extraction AI error:" << error;
                memoryAi->deleteLater();
            });

    // 发送提取请求
    memoryAi->chat(extractionMessage);
}

/*截图按钮点击*/
void Dialog::on_pushButton_screenCapture_clicked()
{
    if (!ui->textEdit->isEnabled())
        return;

    const QString userInput = ui->textEdit->toPlainText().trimmed();
    const QString message = userInput.isEmpty()
        ? QStringLiteral("帮我看看屏幕上的内容")
        : userInput;

    ui->label_name->setText(QStringLiteral("她"));
    ui->textEdit->setEnabled(false);
    ui->pushButton_next->hide();
    ui->textEdit->setText(QStringLiteral("正在分析屏幕内容……"));
    m_lastUserInput = message;
    captureAndAnalyzeScreen();
}

/*重载屏幕捕获配置*/
void Dialog::ReloadScreenCaptureConfig()
{
    ZcJsonLib config(JsonSettingPath);
    m_screenCaptureEnabled =
        config.value("screenCapture/Enable", false).toBool();

    ui->pushButton_screenCapture->setVisible(m_screenCaptureEnabled);
    ui->pushButton_screenCapture->setEnabled(m_screenCaptureEnabled);
}

/*屏幕捕获触发关键词*/
QStringList Dialog::screenCaptureTriggerKeywords()
{
    return {
        QStringLiteral("看看屏幕"),
        QStringLiteral("看下屏幕"),
        QStringLiteral("帮我看看"),
        QStringLiteral("看看这个"),
        QStringLiteral("看下这个"),
        QStringLiteral("截图"),
        QStringLiteral("屏幕截图"),
        QStringLiteral("截屏"),
        QStringLiteral("看屏幕"),
        QStringLiteral("帮我看看这个"),
    };
}

/*截取屏幕并编码为JPEG base64*/
QByteArray Dialog::captureScreenToJpeg()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (!screen)
    {
        qWarning() << "Screen capture: no primary screen available";
        return QByteArray();
    }

    QPixmap pixmap = screen->grabWindow(0);
    if (pixmap.isNull())
    {
        qWarning() << "Screen capture: grabWindow returned null pixmap";
        return QByteArray();
    }

    // 缩放至最大1920px，保持宽高比
    QImage image = pixmap.toImage();
    const int maxDim = 1920;
    if (image.width() > maxDim || image.height() > maxDim)
    {
        image = image.scaled(maxDim, maxDim, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
    }

    QByteArray jpegData;
    QBuffer buffer(&jpegData);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", 70);
    buffer.close();

    return jpegData;
}

/*捕获屏幕并启动分析*/
void Dialog::captureAndAnalyzeScreen()
{
    if (m_visionInFlight)
        return;

    const QByteArray jpegData = captureScreenToJpeg();
    if (jpegData.isEmpty())
    {
        ui->textEdit->setText(QStringLiteral("屏幕捕获失败，请重试"));
        ui->textEdit->setEnabled(true);
        ui->label_name->setText(QStringLiteral("你"));
        ui->pushButton_next->show();
        m_lastUserInput.clear();
        return;
    }

    const QByteArray imageBase64 = jpegData.toBase64();
    const QString userMessage = m_lastUserInput.isEmpty()
        ? QStringLiteral("帮我看看屏幕上的内容")
        : m_lastUserInput;

    m_visionInFlight = true;
    analyzeScreenWithVision(imageBase64, userMessage);
}

/*发送截图到视觉AI分析*/
void Dialog::analyzeScreenWithVision(const QByteArray &imageBase64,
                                      const QString &userMessage)
{
    // 读取屏幕捕获专用API配置（独立于对话模型）
    ZcJsonLib config(JsonSettingPath);
    const QString serverSelect =
        config.value("screenCapture/Server", "Kimi").toString();
    const QString apiKey =
        config.value("screenCapture/ApiKey").toString();
    const QString model =
        config.value("screenCapture/Model", "moonshot-v1-8k-vision-preview").toString();

    if (apiKey.isEmpty())
    {
        qWarning() << "Vision API: no API key configured for screen capture";
        doSubmitCurrentInput(userMessage);
        return;
    }

    // 确定API端点
    QString apiUrl;
    if (serverSelect == "Kimi")
        apiUrl = QStringLiteral("https://api.moonshot.cn/v1/chat/completions");
    else if (serverSelect == "OpenAI")
        apiUrl = QStringLiteral("https://api.openai.com/v1/chat/completions");
    else if (serverSelect == "Custom")
    {
        const QString baseUrl =
            config.value("screenCapture/BaseUrl").toString().trimmed();
        if (baseUrl.isEmpty())
        {
            qWarning() << "Vision API: Custom server selected but no BaseUrl configured";
            doSubmitCurrentInput(userMessage);
            return;
        }
        apiUrl = baseUrl + "/v1/chat/completions";
    }
    else
    {
        qWarning() << "Vision API: unknown server" << serverSelect;
        doSubmitCurrentInput(userMessage);
        return;
    }

    // 构建多模态消息
    QJsonArray content;
    QJsonObject textPart;
    textPart["type"] = "text";
    textPart["text"] = QStringLiteral(
        "请分析这张屏幕截图的内容，用中文简要描述你能看到什么。"
        "如果屏幕上有代码，请说明代码的大致功能和结构。"
        "如果屏幕上有对话框、网页或文档，请总结其内容。"
        "请简洁直接，200字以内。");
    content.append(textPart);

    QJsonObject imagePart;
    imagePart["type"] = "image_url";
    QJsonObject imageUrlObj;
    imageUrlObj["url"] =
        QStringLiteral("data:image/jpeg;base64,") + QString::fromLatin1(imageBase64);
    imagePart["image_url"] = imageUrlObj;
    content.append(imagePart);

    QJsonObject userMsg;
    userMsg["role"] = "user";
    userMsg["content"] = content;

    QJsonObject sysMsg;
    sysMsg["role"] = "system";
    sysMsg["content"] = QStringLiteral("你是一个屏幕分析助手，用中文简洁回复。");

    QJsonArray messages;
    messages.append(sysMsg);
    messages.append(userMsg);

    QJsonObject body;
    body["model"] = model;
    body["messages"] = messages;
    body["max_tokens"] = 500;
    body["stream"] = false;

    QUrl url(apiUrl);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", ("Bearer " + apiKey).toUtf8());

    QNetworkReply *reply =
        m_visionManager->post(request,
                              QJsonDocument(body).toJson(QJsonDocument::Compact));

    // 处理响应
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, userMessage]()
            {
                reply->deleteLater();
                m_visionInFlight = false;

                if (reply->error() != QNetworkReply::NoError)
                {
                    qWarning() << "Vision API error:" << reply->errorString()
                               << "- falling back to text-only mode";
                    doSubmitCurrentInput(userMessage);
                    return;
                }

                const QJsonDocument responseDoc =
                    QJsonDocument::fromJson(reply->readAll());
                const QJsonObject responseObj = responseDoc.object();
                const QJsonArray choices =
                    responseObj.value("choices").toArray();

                QString visionResult;
                if (!choices.isEmpty())
                {
                    const QJsonObject firstChoice =
                        choices.first().toObject();
                    const QJsonObject message =
                        firstChoice.value("message").toObject();
                    visionResult = message.value("content").toString().trimmed();
                }

                if (visionResult.isEmpty())
                {
                    qWarning() << "Vision API returned empty content"
                               << "- falling back to text-only mode";
                    doSubmitCurrentInput(userMessage);
                    return;
                }

                // 将分析结果注入用户消息，走正常对话流程
                const QString enhancedInput = userMessage +
                    QStringLiteral("\n\n[当前屏幕截图的分析结果]：") +
                    visionResult;

                doSubmitCurrentInput(enhancedInput);
            });
}

/*视觉分析错误恢复*/
void Dialog::handleVisionError(const QString &errorMsg)
{
    ui->textEdit->setText(errorMsg);
    ui->textEdit->setEnabled(true);
    ui->label_name->setText(QStringLiteral("你"));
    ui->pushButton_next->show();
    m_lastUserInput.clear();
}
