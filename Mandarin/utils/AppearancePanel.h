#pragma once

#include <QApplication>
#include <QFrame>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QLabel>
#include <QVariant>
#include <QCursor>
#include <QTimer>
#include <QShowEvent>
#include <functional>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

class AppearanceDragHandle : public QLabel
{
  public:
    explicit AppearanceDragHandle(QWidget *parent) : QLabel(QStringLiteral("装扮"), parent)
    {
        setObjectName(QStringLiteral("appearanceDragHandle"));
        setCursor(Qt::SizeAllCursor);
        setToolTip(QStringLiteral("按住左键拖动"));
        setStyleSheet(QStringLiteral("background: #e8e8e8; border-radius: 4px; padding: 6px; font-weight: bold;"));
    }

  protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton)
        {
            m_dragging = true;
            m_offset = event->globalPosition().toPoint() - window()->pos();
            event->accept();
        }
        else
            QLabel::mousePressEvent(event);
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_dragging && event->buttons().testFlag(Qt::LeftButton))
        {
            window()->move(event->globalPosition().toPoint() - m_offset);
            event->accept();
        }
        else
            QLabel::mouseMoveEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton)
        {
            m_dragging = false;
            event->accept();
        }
        else
            QLabel::mouseReleaseEvent(event);
    }

  private:
    bool m_dragging = false;
    QPoint m_offset;
};

// A tool window keeps its contents open across selections and focus changes.
class AppearancePanel : public QFrame
{
  public:
    explicit AppearancePanel(QWidget *owner = nullptr,
                             std::function<void()> onDismiss = {})
        : QFrame(owner, Qt::Tool | Qt::FramelessWindowHint), m_onDismiss(std::move(onDismiss))
    {
        setObjectName(QStringLiteral("appearancePanel"));
        qApp->installEventFilter(this);
#ifdef Q_OS_WIN
        // Qt application filters cannot see clicks in the desktop or other apps.
        m_outsideClickTimer.setInterval(20);
        connect(&m_outsideClickTimer, &QTimer::timeout, this, [this]() {
            const bool down = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
            if (isVisible() && down && !m_rightWasDown)
            {
                const QPoint point = QCursor::pos();
                // Qt widgets use the event filter below, so polling cannot race their press.
                if (!QApplication::widgetAt(point) && !geometry().contains(point))
                    dismissForOutsideRightClick();
            }
            m_rightWasDown = down;
            if (!isVisible()) m_outsideClickTimer.stop();
        });
#endif
    }
    ~AppearancePanel() override { qApp->removeEventFilter(this); }
    void dismissForOutsideRightClick()
    {
        if (m_onDismiss) m_onDismiss();
        close();
    }

  protected:
    void showEvent(QShowEvent *event) override
    {
        QFrame::showEvent(event);
#ifdef Q_OS_WIN
        m_rightWasDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        m_outsideClickTimer.start();
#endif
    }
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (isVisible() && event->type() == QEvent::MouseButtonPress)
        {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::RightButton &&
                !geometry().contains(mouse->globalPosition().toPoint()))
            {
                // Combo-box popup windows belong to the panel even outside its bounds.
                QWidget *sourceWidget = qobject_cast<QWidget *>(watched);
                // QWindow receives native input first; wait for the QWidget delivery.
                if (!sourceWidget)
                    return QFrame::eventFilter(watched, event);
                QWidget *source = sourceWidget;
                while (source && source != this)
                    source = source->parentWidget();
                if (!source)
                {
                    const bool onOwner = parentWidget() && sourceWidget &&
                                         sourceWidget->window() == parentWidget()->window();
                    // The owner's contextMenuEvent handles its own close/open toggle.
                    if (!onOwner)
                        dismissForOutsideRightClick();
                }
            }
        }
        if (watched == parentWidget() && event->type() == QEvent::Hide)
            close();
        return QFrame::eventFilter(watched, event);
    }
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape)
            event->accept();
        else
            QFrame::keyPressEvent(event);
    }

  private:
    std::function<void()> m_onDismiss;
#ifdef Q_OS_WIN
    QTimer m_outsideClickTimer;
    bool m_rightWasDown = false;
#endif
};
