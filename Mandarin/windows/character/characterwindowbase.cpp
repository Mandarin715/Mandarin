#include "characterwindowbase.h"

#include "../../GlobalConstants.h"

#include "../../utils/DragHelper.h"

#include <QAbstractAnimation>
#include <QBitmap>
#include <QDebug>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFontMetrics>
#include <QGraphicsOpacityEffect>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPropertyAnimation>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QVector>

#ifdef Q_OS_LINUX
#include <X11/Xlib.h>
#include <X11/Xutil.h> //必须包含这个处理图像转换
#include <X11/extensions/shape.h>
#endif

CharacterWindowBase::CharacterWindowBase(QWidget *parent)
    : QWidget(parent)
{
    /*窗口设置*/
    //无边框
    setAttribute(Qt::WA_TranslucentBackground);
    setAcceptDrops(true);
    Qt::WindowFlags flags = Qt::Tool | Qt::FramelessWindowHint |
                            Qt::WindowStaysOnTopHint;
#ifdef Q_OS_LINUX
    //避免窗口管理器限制拖拽范围（如屏幕边缘约束）。
    flags |= Qt::X11BypassWindowManagerHint;
#endif
    setWindowFlags(flags);
    //窗口拖拽
    new DragHelper(this);

    // 内心独白的生命周期由本类统一管理：显示后 20 秒自动淡出。
    m_innerThoughtTimer = new QTimer(this);
    m_innerThoughtTimer->setSingleShot(true);
    m_innerThoughtTimer->setInterval(20000);
    connect(m_innerThoughtTimer, &QTimer::timeout, this,
            &CharacterWindowBase::HideInnerThought);
}

CharacterWindowBase::~CharacterWindowBase()
{
    // 内心气泡是以本窗口为父的 QLabel 子对象、定时器同样以 this 为父，
    // 随 QObject 析构自动销毁，这里无需手工清理。
}

#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
void CharacterWindowBase::ApplyInteractiveRegion(const QRegion &region)
{
#ifdef Q_OS_LINUX
    Display *display = XOpenDisplay(nullptr);
    if (!display)
        return;

    Window window_id = static_cast<Window>(this->winId());

    const int count = region.rectCount();
    if (count <= 0)
    {
        XShapeCombineRectangles(display, window_id, ShapeInput, 0, 0, nullptr, 0,
                                ShapeSet, YXBanded);
        XCloseDisplay(display);
        return;
    }

    auto rects = region.begin();
    QVector<XRectangle> xrects;
    xrects.resize(count);
    for (int i = 0; i < count; ++i)
    {
        const QRect &rect = rects[i];
        xrects[i].x = static_cast<short>(rect.x());
        xrects[i].y = static_cast<short>(rect.y());
        xrects[i].width = static_cast<unsigned short>(rect.width());
        xrects[i].height = static_cast<unsigned short>(rect.height());
    }

    XShapeCombineRectangles(display, window_id, ShapeInput, 0, 0, xrects.data(),
                            count, ShapeSet, YXBanded);
    XCloseDisplay(display);
#else
    setMask(region);
#endif
}

void CharacterWindowBase::ApplyInteractiveRegionFromImage()
{
    if (m_scaledImg.isNull())
        return;

    QRegion region(QBitmap::fromImage(m_scaledImg.createAlphaMask()));
    region.translate(m_scaledImgTopLeft);
    ApplyInteractiveRegion(region);
}

void CharacterWindowBase::ApplyInteractiveRegionFullWindow()
{
    ApplyInteractiveRegion(QRegion(QRect(0, 0, width(), height())));
}
#endif

/*子类渲染完成后登记 alpha 图与位置，并重算内心气泡位置*/
void CharacterWindowBase::updateRenderedImage(const QImage &image,
                                              const QPoint &topLeft)
{
    m_scaledImg = image;
    m_scaledImgTopLeft = topLeft;
    RepositionInnerThoughtBubble();
}

/*立绘大小（百分比）*/
void CharacterWindowBase::SetTachieSize(int size)
{
    // 原 Tachie::SetTachieSize 的开头（含 "NowTachie 为空则直接返回" 的守卫）留在子类
    // relayoutContent() 里，因为"有没有内容"只有子类知道。
    m_tachieSizePercent = (size <= 0) ? 100 : size;
    qInfo() << "设置立绘大小为" << m_tachieSizePercent;
    relayoutContent();
}

/*TTS 播放状态（说话开关）。

  PNG 路径的默认实现**故意什么都不做**：一张静止立绘没有可以开合的嘴，
  强行做点什么（换图/抖一下）反而会引入用户没要求的行为。
  这里不打日志也是刻意的 —— 每次播放开始/结束都打一条只会刷屏。
  Live2D 路径覆写它（见 Live2DCharacterWindow::SetSpeaking）。*/
void CharacterWindowBase::SetSpeaking(bool speaking)
{
    Q_UNUSED(speaking);
}

void CharacterWindowBase::contextMenuEvent(QContextMenuEvent *event)
{
    emit requestToggleVisible(); //发出信号
}
//鼠标按下
void CharacterWindowBase::mousePressEvent(QMouseEvent *event)
{
    const QPoint pos = event->pos();
    const QPoint imgPos = pos - m_scaledImgTopLeft;
    const QRect imageBounds(QPoint(0, 0), m_scaledImg.size());
    if (m_scaledImg.isNull() || !imageBounds.contains(imgPos))
    {
        event->ignore();
        return;
    }

    const int alpha = m_scaledImg.pixelColor(imgPos).alpha();
    if (alpha < 10)
    {
        event->ignore();
        return;
    }

#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
    //拖动时扩大输入区域，避免鼠标离开形状区域后丢失拖拽。
    ApplyInteractiveRegionFullWindow();
#endif

    QWidget::mousePressEvent(event);
}

//鼠标抬起
void CharacterWindowBase::mouseReleaseEvent(QMouseEvent *event)
{
    QWidget::mouseReleaseEvent(event);

#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
    ApplyInteractiveRegionFromImage();
#endif

    //仅在初始化恢复完成后，且左键释放时保存一次位置。
    if (!m_tachiePosRestoreDone || event->button() != Qt::LeftButton)
        return;

    SaveTachieLoc(); //保存立绘位置
}

/*文件拖放到立绘上——提取文件路径发给 Dialog 处理*/
void CharacterWindowBase::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void CharacterWindowBase::dropEvent(QDropEvent *event)
{
    const QList<QUrl> urls = event->mimeData()->urls();
    if (urls.isEmpty())
        return;

    QStringList paths;
    for (const QUrl &url : urls) {
        if (url.isLocalFile())
            paths.append(url.toLocalFile());
    }
    if (!paths.isEmpty())
        emit requestFileDrop(paths);
}

//重置立绘位置
void CharacterWindowBase::ResetTachieLoc()
{
    this->move(0, 0);
    SaveTachieLoc(); //保存立绘位置
}

//保存立绘位置
void CharacterWindowBase::SaveTachieLoc()
{
    const QString charName = ReadNowSelectChar();
    if (charName.isEmpty() || charName == "未选择")
        return;

    QSettings settings(IniSettingPath, QSettings::IniFormat);
    settings.setValue(QString("tachie/%1/posX").arg(charName), this->x());
    settings.setValue(QString("tachie/%1/posY").arg(charName), this->y());
}
//读取设置立绘位置
void CharacterWindowBase::RestoreTachieLoc()
{
    const QString charName = ReadNowSelectChar();
    if (charName.isEmpty() || charName == "未选择")
    {
        m_tachiePosRestoreDone = false;
        return;
    }

    QSettings settings(IniSettingPath, QSettings::IniFormat);
    const QString keyX = QString("tachie/%1/posX").arg(charName);
    const QString keyY = QString("tachie/%1/posY").arg(charName);

    if (!settings.contains(keyX) || !settings.contains(keyY))
    {
        //没有历史位置时标记恢复完成，后续用户拖动可直接保存。
        m_tachiePosRestoreDone = true;
        return;
    }

    //恢复阶段不触发 mouseReleaseEvent 保存，直接移动即可。
    this->move(settings.value(keyX).toInt(), settings.value(keyY).toInt());
    m_tachiePosRestoreDone = true;
}

/*内心独白气泡：立绘头顶右上 45°，半透明淡入→停留→语音播完淡出*/
void CharacterWindowBase::ShowInnerThought(QString text)
{
    text = text.trimmed();
    if (text.isEmpty())
        return;

    // 先清理上一个气泡
    HideInnerThought();

    // 恢复原来的父窗口内 QLabel 绘制方式，确保半透明背景稳定显示。
    auto *bubble = new QLabel(text, this);
    bubble->setTextFormat(Qt::PlainText);
    bubble->setWordWrap(true);
    bubble->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    QFont bubbleFont = bubble->font();
    bubbleFont.setPixelSize(13);
    bubble->setFont(bubbleFont);
    bubble->setStyleSheet(
        "color: #555; background: rgba(255,255,255,200); "
        "border: 1px solid rgba(180,180,180,120); "
        "border-radius: 12px; padding: 8px 14px;");

    // 在 Tachie 画布范围内自动换行，避免长文本横向越界。
    const QRect available = rect().adjusted(10, 10, -10, -10);

    constexpr int kHorizontalPadding = 30;
    constexpr int kVerticalPadding = 18;
    const int maxBubbleWidth = qMax(130, qMin(320, available.width()));
    const int maxTextWidth = maxBubbleWidth - kHorizontalPadding;
    const QFontMetrics metrics(bubbleFont);
    const int naturalWidth = metrics.boundingRect(text).width();
    const int textWidth = qBound(100, naturalWidth, maxTextWidth);
    const QRect textRect = metrics.boundingRect(
        QRect(0, 0, textWidth, qMax(100, available.height())),
        Qt::TextWordWrap | Qt::TextWrapAnywhere | Qt::AlignLeft, text);
    bubble->setFixedSize(qMin(maxBubbleWidth,
                              qMax(130, textRect.width() + kHorizontalPadding)),
                         qMax(36, textRect.height() + kVerticalPadding));

    m_innerThoughtBubble = bubble;
    RepositionInnerThoughtBubble();
    bubble->show();

    // 淡入
    auto *effect = new QGraphicsOpacityEffect(bubble);
    bubble->setGraphicsEffect(effect);
    effect->setOpacity(0.0);
    auto *fadeIn = new QPropertyAnimation(effect, "opacity", bubble);
    fadeIn->setDuration(400);
    fadeIn->setStartValue(0.0);
    fadeIn->setEndValue(1.0);
    fadeIn->start(QAbstractAnimation::DeleteWhenStopped);

    m_innerThoughtTimer->start();
}

/*将气泡定位到立绘头部附近，并限制在 Tachie 画布内*/
void CharacterWindowBase::RepositionInnerThoughtBubble()
{
    if (!m_innerThoughtBubble)
        return;

    const QPoint head(width() / 2, static_cast<int>(height() * 0.30));
    constexpr int kCanvasMargin = 10;
    constexpr int kHorizontalOffset = 50;
    constexpr int kVerticalOffset = 35;

    int x = head.x() + kHorizontalOffset;
    int y = head.y() - kVerticalOffset - m_innerThoughtBubble->height();

    // 右侧空间不足时翻转到立绘左上方。
    if (x + m_innerThoughtBubble->width() >
        width() - kCanvasMargin)
    {
        x = head.x() - kHorizontalOffset -
            m_innerThoughtBubble->width();
    }

    // 顶部空间不足时改放到头部下方。
    if (y < kCanvasMargin)
        y = head.y() + kVerticalOffset;

    const int minX = kCanvasMargin;
    const int minY = kCanvasMargin;
    const int maxX = qMax(minX, width() - kCanvasMargin -
                                    m_innerThoughtBubble->width());
    const int maxY = qMax(minY, height() - kCanvasMargin -
                                    m_innerThoughtBubble->height());

    m_innerThoughtBubble->move(qBound(minX, x, maxX),
                               qBound(minY, y, maxY));
}

/*隐藏内心独白气泡：淡出后销毁*/
void CharacterWindowBase::HideInnerThought()
{
    if (m_innerThoughtTimer)
        m_innerThoughtTimer->stop();

    if (!m_innerThoughtBubble)
        return;

    QWidget *bubble = m_innerThoughtBubble;
    m_innerThoughtBubble = nullptr;

    auto *eff = qobject_cast<QGraphicsOpacityEffect *>(bubble->graphicsEffect());
    if (!eff)
    {
        delete bubble;
        return;
    }

    auto *fadeOut = new QPropertyAnimation(eff, "opacity", bubble);
    fadeOut->setDuration(400);
    fadeOut->setStartValue(eff->opacity());
    fadeOut->setEndValue(0.0);
    QObject::connect(fadeOut, &QPropertyAnimation::finished, bubble, &QObject::deleteLater);
    fadeOut->start(QAbstractAnimation::DeleteWhenStopped);
}
