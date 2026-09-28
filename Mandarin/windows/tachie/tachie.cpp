#include "tachie.h"
#include "ui_tachie.h"

#include "../../GlobalConstants.h"

#include "../../utils/DragHelper.h"
#include "../../utils/TachieGeometry.h"
#include "ZcJsonLib.h"
#include <QAbstractAnimation>
#include <QBitmap>
#include <QColor>
#include <QDebug>
#include <QDir>
#include <QEasingCurve>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGraphicsOpacityEffect>
#include <QImage>
#include <QLabel>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPropertyAnimation>
#include <QSequentialAnimationGroup>
#include <QTimer>
#include <QVariantAnimation>
#include <memory>

#ifdef Q_OS_LINUX
#include <X11/Xlib.h>
#include <X11/Xutil.h> //必须包含这个处理图像转换
#include <X11/extensions/shape.h>
#endif

Tachie::Tachie(QWidget *parent)
    : CharacterWindowBase(parent), ui(new Ui::Tachie)
{
    /*窗口设置*/
    ui->setupUi(this);
    // 立绘 label 改为绝对定位，避免布局系统与动画 setGeometry 冲突。
    if (ui->gridLayout)
        ui->gridLayout->removeWidget(ui->label_tachie1);
    ui->label_tachie1->setParent(this);

    //延迟加载立绘
    QTimer::singleShot(0, this, [this]()
                       {
                           m_animePluginManager.Reload();
                           SetTachieImg("default");
                       });
}

Tachie::~Tachie()
{
    if (m_activeAnimationGroup)
    {
        m_activeAnimationGroup->stop();
        delete m_activeAnimationGroup;
        m_activeAnimationGroup = nullptr;
    }
    delete ui;
}

/*立绘解码缓存：路径 + mtime 未变则复用，避免反复解码大图*/
QPixmap Tachie::loadTachiePixmapCached(const QString &filePath)
{
    const qint64 mtimeMs =
        QFileInfo(filePath).lastModified().toMSecsSinceEpoch();
    if (m_pixmapCache.contains(filePath) &&
        m_pixmapCacheStamp.value(filePath) == mtimeMs)
    {
        return m_pixmapCache.value(filePath);
    }
    QPixmap pm;
    if (!pm.load(filePath))
        return QPixmap();
    if (m_pixmapCache.size() >= 8) // 有界缓存
    {
        m_pixmapCache.clear();
        m_pixmapCacheStamp.clear();
    }
    m_pixmapCache.insert(filePath, pm);
    m_pixmapCacheStamp.insert(filePath, mtimeMs);
    return pm;
}

//设置立绘（渲染层入口，由窗口层按角色/心情名调用）
void Tachie::reloadContent(const QString &contentName)
{
    const QString tachieDirPath = ReadCharacterTachiePath();
    if (tachieDirPath.isEmpty())
    {
        return;
    }

    const QString normalizedName = contentName.trimmed();
    QPixmap loadedPixmap;
    bool loaded = false;

    //兼容 AI 返回无扩展名和带扩展名两种情况，同时支持 png/jpg/jpeg。
    QFileInfo inputInfo(normalizedName);
    QStringList candidates;
    if (inputInfo.suffix().isEmpty())
    {
        candidates << (normalizedName + ".png") << (normalizedName + ".jpg")
                   << (normalizedName + ".jpeg");
    }
    else
    {
        candidates << normalizedName;
    }

    QDir tachieDir(tachieDirPath);
    for (const QString &candidate : candidates)
    {
        const QString filePath = tachieDir.filePath(candidate);
        if (!QFileInfo::exists(filePath))
            continue;
        loadedPixmap = loadTachiePixmapCached(filePath);
        if (!loadedPixmap.isNull())
        {
            loaded = true;
            break;
        }
    }

    //再做一次按文件名（不区分大小写）的兜底匹配，避免 AI 返回大小写不一致。
    if (!loaded)
    {
        const QStringList files = tachieDir.entryList(
            QStringList() << "*.png" << "*.jpg" << "*.jpeg", QDir::Files);
        for (const QString &fileName : files)
        {
            if (QFileInfo(fileName).completeBaseName().compare(
                    normalizedName, Qt::CaseInsensitive) != 0)
            {
                continue;
            }
            loadedPixmap = loadTachiePixmapCached(tachieDir.filePath(fileName));
            if (!loadedPixmap.isNull())
            {
                loaded = true;
                break;
            }
        }
    }

    if (loaded)
    {
        NowTachie = loadedPixmap;
    }
    else if (NowTachie.isNull())
    {
        qWarning() << "立绘加载失败:" << normalizedName;
        return;
    }

    //读取立绘大小
    ZcJsonLib charUserConfig(ReadCharacterUserConfigPath());
    SetTachieSize(charUserConfig.value("tachieSize").toString().toInt());
    RestoreTachieLoc();

    //按当前动作尝试播放绑定动画
    QString actionName = QFileInfo(normalizedName).completeBaseName();
    if (!actionName.isEmpty())
        TryPlayAnimationForAction(actionName);
}

//兼容旧调用名：转发到 reloadContent
void Tachie::SetTachieImg(QString TachieName)
{
    reloadContent(TachieName);
}

/*播放动画*/
void Tachie::TryPlayAnimationForAction(const QString &actionName)
{
    const QString charName = ReadNowSelectChar();
    if (charName.isEmpty() || charName == "未选择")
        return;

    //动作绑定存放于角色资源配置
    ZcJsonLib charAssetConfig(CharacterAssestPath + "/" + charName +
                              "/config.json");
    const QJsonObject animationMap =
        charAssetConfig.value("tachieAnimations", QJsonObject()).toObject();
    const QString uniqueKey = animationMap.value(actionName).toString().trimmed();
    if (uniqueKey.isEmpty())
        return;

    //每次播放前刷新索引，确保刚导入/删除插件也生效
    m_animePluginManager.Reload();

    AnimePluginDefinition plugin;
    AnimePluginAnimation animation;
    if (!m_animePluginManager.TryGetAnimationByUniqueKey(uniqueKey, plugin,
                                                         animation))
    {
        qWarning() << "动画绑定无效:" << actionName << uniqueKey;
        return;
    }

    //停止上一个动画，避免并发导致位置/透明度冲突
    if (m_activeAnimationGroup)
    {
        m_activeAnimationGroup->stop();
        m_activeAnimationGroup->deleteLater();
        m_activeAnimationGroup = nullptr;
    }

    QSequentialAnimationGroup *seq = new QSequentialAnimationGroup(this);

    struct ScaleSequenceState
    {
        QRect baseImageRect;
        QPixmap intermediate; // 预缩放到最大倍率的中间图，动画每帧只缩它（避免对原始大图逐帧平滑缩放）
        bool initialized = false;
    };
    auto scaleSequenceState = std::make_shared<ScaleSequenceState>();

    auto ensureScaleBaseInitialized = [this, scaleSequenceState]()
    {
        if (scaleSequenceState->initialized)
            return;
        // 锁定整段缩放动画的基准矩形，避免多 step 累积漂移。
        scaleSequenceState->baseImageRect = ui->label_tachie1->geometry();
        // 一次性预生成"最大倍率 2.0"的中间图
        if (!NowTachie.isNull() && !scaleSequenceState->baseImageRect.isEmpty())
        {
            const int mw = qMax(
                1, qRound(scaleSequenceState->baseImageRect.width() * 2.0));
            const int mh = qMax(
                1, qRound(scaleSequenceState->baseImageRect.height() * 2.0));
            scaleSequenceState->intermediate = NowTachie.scaled(
                mw, mh, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
        scaleSequenceState->initialized = true;
    };

    for (const AnimePluginStep &step : animation.steps)
    {
        const int durationMs = qMax(1, static_cast<int>(step.durationSec * 1000.0));

        if (step.type == AnimePluginStep::Type::Move)
        {
            struct MoveState
            {
                QPoint basePos;
                bool initialized = false;
            };
            auto moveState = std::make_shared<MoveState>();

            QVariantAnimation *moveAnim = new QVariantAnimation(seq);
            moveAnim->setDuration(durationMs);
            moveAnim->setEasingCurve(QEasingCurve::InOutQuad);
            moveAnim->setStartValue(0.0);
            moveAnim->setEndValue(1.0);
            connect(moveAnim, &QVariantAnimation::valueChanged, this,
                    [this, moveState, step](const QVariant &v)
                    {
                        if (!moveState->initialized)
                        {
                            moveState->basePos = this->pos();
                            moveState->initialized = true;
                        }

                        const double progress = v.toDouble();
                        const int dx = qRound(step.x * progress);
                        const int dy = qRound(step.y * progress);
                        this->move(moveState->basePos + QPoint(dx, dy));
                    });
            seq->addAnimation(moveAnim);
            continue;
        }

        if (step.type == AnimePluginStep::Type::Opacity)
        {
            QPropertyAnimation *opacityAnim =
                new QPropertyAnimation(this, "windowOpacity", seq);
            opacityAnim->setDuration(durationMs);
            opacityAnim->setEasingCurve(QEasingCurve::InOutQuad);
            opacityAnim->setStartValue(step.from);
            opacityAnim->setEndValue(step.to);
            seq->addAnimation(opacityAnim);
            continue;
        }

        if (step.type == AnimePluginStep::Type::Scale)
        {
            QVariantAnimation *scaleAnim = new QVariantAnimation(seq);
            scaleAnim->setDuration(durationMs);
            scaleAnim->setEasingCurve(QEasingCurve::InOutQuad);
            scaleAnim->setStartValue(step.scaleFrom);
            scaleAnim->setEndValue(step.scaleTo);

            auto applyScaleFrame = [this, scaleSequenceState](double factor)
            {
                // 保护倍率，避免异常值导致图片瞬间过大。
                const double safeFactor = TachieGeometry::clampScaleFactor(factor);
                const int w = qMax(
                    1, qRound(scaleSequenceState->baseImageRect.width() * safeFactor));
                const int h = qMax(
                    1, qRound(scaleSequenceState->baseImageRect.height() * safeFactor));

                // 固定窗口，仅在画布中心缩放，保证始终从中心放大/缩小。
                const QRect frame = TachieGeometry::centeredRect(size(), QSize(w, h));
                const int x = frame.x();
                const int y = frame.y();

                if (!NowTachie.isNull())
                {
                    // 优先缩预生成的中间图（远小于原始立绘），显著降低每帧 CPU/拷贝
                    const QPixmap &src = scaleSequenceState->intermediate.isNull()
                                             ? NowTachie
                                             : scaleSequenceState->intermediate;
                    const QPixmap scaledPixmap =
                        src.scaled(w, h, Qt::IgnoreAspectRatio,
                                   Qt::SmoothTransformation);
                    ui->label_tachie1->setPixmap(scaledPixmap);
                    ui->label_tachie1->setGeometry(x, y, w, h);
                    m_scaledImg = scaledPixmap.toImage();
                    m_scaledImgTopLeft = QPoint(x, y);
                }
            };

            connect(scaleAnim, &QVariantAnimation::valueChanged, this,
                    [ensureScaleBaseInitialized, applyScaleFrame](const QVariant &v)
                    {
                        ensureScaleBaseInitialized();

                        applyScaleFrame(v.toDouble());
                    });

            connect(scaleAnim, &QVariantAnimation::finished, this,
                    [ensureScaleBaseInitialized, applyScaleFrame, step]()
                    {
                        ensureScaleBaseInitialized();
                        // 每一步结束时吸附到目标值，消除帧步进带来的残余误差。
                        applyScaleFrame(step.scaleTo);
                    });
            seq->addAnimation(scaleAnim);
        }
    }

    if (seq->animationCount() <= 0)
    {
        seq->deleteLater();
        return;
    }

    m_activeAnimationGroup = seq;
    connect(seq, &QSequentialAnimationGroup::finished, this,
            [this]()
            { m_activeAnimationGroup = nullptr; });
    seq->start(QAbstractAnimation::DeleteWhenStopped);
}

//按基类记录的立绘大小百分比重新布局并重载立绘
void Tachie::relayoutContent()
{
    if (NowTachie.isNull())
    {
        return;
    }

    //缩放新图片并设置到 label
    QPixmap scaledPixmap =
        NowTachie.scaled(NowTachie.size() * (m_tachieSizePercent / 100.0),
                         Qt::KeepAspectRatio, Qt::SmoothTransformation);

    // 预留 200% 画布，缩放动画只动图片层，不改窗口几何，避免抖动。
    const TachieGeometry::CanvasLayout layout =
        TachieGeometry::canvasForScaledSize(scaledPixmap.size());

    this->resize(layout.canvasSize);
    ui->label_tachie1->setPixmap(scaledPixmap);
    ui->label_tachie1->setGeometry(layout.imageTopLeft.x(), layout.imageTopLeft.y(),
                                   scaledPixmap.width(), scaledPixmap.height());

    updateRenderedImage(scaledPixmap.toImage(), layout.imageTopLeft);

#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS) //这里保留透明输入区域逻辑，macOS 也沿用同一套 region 处理
    ApplyInteractiveRegionFromImage();
#else
    //Windows 下不裁剪窗口形状，避免半透明边缘被硬裁切后出现“略微缩小/边缘异常”。
    this->clearMask();
#endif
}

/*当前内容尺寸：当前贴图尺寸*/
QSize Tachie::contentSize() const
{
    return NowTachie.size();
}
