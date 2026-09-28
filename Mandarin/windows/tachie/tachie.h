#ifndef TACHIE_H
#define TACHIE_H

#include "../character/characterwindowbase.h"

#include "../../utils/AnimePluginManager.h"

#include <QHash>
#include <QPixmap>

class QSequentialAnimationGroup;

namespace Ui
{
class Tachie;
}

/*PNG 立绘渲染器：窗口层（穿透/拖拽/位置/气泡/拖放）全部继承自 CharacterWindowBase*/
class Tachie : public CharacterWindowBase
{
    Q_OBJECT

  public:
    explicit Tachie(QWidget *parent = nullptr);
    ~Tachie() override;

  public slots:
    void SetTachieImg(QString TachieName = "default"); //兼容旧调用名，转发到 reloadContent
    void reloadContent(const QString &contentName) override; //原 SetTachieImg

  protected:
    void relayoutContent() override;    //原 SetTachieSize 的布局部分（用基类的 m_tachieSizePercent）
    QSize contentSize() const override; //当前贴图尺寸

  private:
    Ui::Tachie *ui;
    QPixmap NowTachie;
    AnimePluginManager m_animePluginManager;
    QHash<QString, QPixmap> m_pixmapCache;    // 立绘解码缓存：路径 -> pixmap（避免反复解码大图）
    QHash<QString, qint64> m_pixmapCacheStamp; // 路径 -> mtime_ms
    QPixmap loadTachiePixmapCached(const QString &filePath);
    QSequentialAnimationGroup *m_activeAnimationGroup = nullptr;
    void TryPlayAnimationForAction(const QString &actionName);
};

#endif //TACHIE_H
