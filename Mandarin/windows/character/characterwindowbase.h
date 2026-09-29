#ifndef CHARACTERWINDOWBASE_H
#define CHARACTERWINDOWBASE_H

#include <QImage>
#include <QPoint>
#include <QRegion>
#include <QStringList>
#include <QWidget>

class QDragEnterEvent;
class QDropEvent;
class QTimer;

/*立绘窗口的「窗口层」：无边框透明、鼠标穿透与交互区、拖拽、位置持久化、内心气泡、文件拖放。
  渲染层交由子类实现（Tachie = PNG，后续 Live2D = 离屏帧），
  目的是 Live2D 接入时窗口行为零复制、PNG 路径零回归。*/
class CharacterWindowBase : public QWidget
{
    Q_OBJECT

  public:
    explicit CharacterWindowBase(QWidget *parent = nullptr);
    ~CharacterWindowBase() override;

  signals:
    void requestToggleVisible();                 //切换对话框显示状态
    void requestFileDrop(QStringList filePaths); //文件拖放到立绘

  public slots:
    void ShowInnerThought(QString text);
    void HideInnerThought();
    void ResetTachieLoc();
    /*立绘大小（百分比）：基类只记住值并触发 relayoutContent()，具体缩放交由子类*/
    void SetTachieSize(int size);
    /*按角色/心情名重载内容（由 Dialog::requestSetCharTachie 驱动，必须是 public 才能 connect）*/
    virtual void reloadContent(const QString &contentName) = 0;
    /*TTS 是否在播（由 Dialog::requestSpeakState 驱动）。
       放在基类是因为"说话"是**角色的行为**，与立绘用 PNG 还是 Live2D 无关：
       PNG 路径没有可动的地方，用默认空实现（什么都不做，也绝不报错）；
       Live2D 路径覆写它把状态转给渲染器，让嘴巴开合。
       与 ShowInnerThought/HideInnerThought 同一套理由 —— 基类承接信号，子类各自实现。*/
    virtual void SetSpeaking(bool speaking);

  protected:
    /*子类渲染完成后调用：登记 alpha 图及其窗口内位置，并重算交互区*/
    void updateRenderedImage(const QImage &image, const QPoint &topLeft);

    void ApplyInteractiveRegion(const QRegion &region);
    void ApplyInteractiveRegionFromImage();
    void ApplyInteractiveRegionFullWindow();

    void SaveTachieLoc();
    void RestoreTachieLoc();
    void RepositionInnerThoughtBubble();

    /*子类实现：把当前内容按窗口尺寸重新布局（PNG 缩放贴图 / Live2D 重设画布）*/
    virtual void relayoutContent() = 0;
    /*子类实现：当前内容尺寸（用于气泡定位等）*/
    virtual QSize contentSize() const = 0;

    void contextMenuEvent(QContextMenuEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

    QImage m_scaledImg;                   //当前内容的 alpha 图（原 Tachie::_scaledImg）
    QPoint m_scaledImgTopLeft{0, 0};      //内容在窗口内的左上角（原 Tachie::_scaledImgTopLeft）
    bool m_tachiePosRestoreDone = false;  //位置恢复完成后才允许自动保存
    int m_tachieSizePercent = 100;        //立绘大小百分比（原 Tachie::SetTachieSize 的入参）

    QWidget *m_innerThoughtBubble = nullptr;
    QTimer *m_innerThoughtTimer = nullptr;
};

#endif // CHARACTERWINDOWBASE_H
