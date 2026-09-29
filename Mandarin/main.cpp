#include "windows/dialog/dialog.h"
#include "windows/setting/setting.h"
#include "windows/tachie/tachie.h"
#include "windows/character/characterwindowbase.h"
#include "windows/character/live2dcharacterwindow.h"

#include "GlobalConstants.h"

#include "ElaApplication.h"
#include "ElaMenu.h"

#include "Version.h"

#include <QApplication>
#include <QColor>
#include <QDebug>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QNetworkProxy>
#include <QPalette>
#include <QStandardPaths>
#include <QSettings>
#include <QSystemTrayIcon>

#include <QDir>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace
{
struct DefaultConfigEntry
{
    const char *resourcePath;
    const char *relativePath;
};

void CopyDefaultConfigIfMissing()
{
    const QString documentsRoot =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (documentsRoot.isEmpty())
        return;

    // 从旧版 ZcChat2 迁移配置到 Mandarin
    const QString oldRoot = QDir(documentsRoot).filePath("ZcChat2");
    const QString targetRoot = QDir(documentsRoot).filePath("Mandarin");
    if (QDir(oldRoot).exists() && !QDir(targetRoot).exists())
    {
        // 递归复制整个旧目录到新位置
        QDir().mkpath(targetRoot);
        QDirIterator it(oldRoot, QDir::NoDotAndDotDot | QDir::AllEntries,
                        QDirIterator::Subdirectories);
        while (it.hasNext())
        {
            it.next();
            const QString relPath =
                QDir(oldRoot).relativeFilePath(it.filePath());
            const QString dstPath = QDir(targetRoot).filePath(relPath);
            if (it.fileInfo().isDir())
                QDir().mkpath(dstPath);
            else
                QFile::copy(it.filePath(), dstPath);
        }
    }

    const QString targetIni = QDir(targetRoot).filePath("config.ini");
    if (QFile::exists(targetIni))
        return;

    const DefaultConfigEntry entries[] = {
        {":/default_config/Mandarin/config.ini", "config.ini"},
        {":/default_config/Mandarin/Plugin/Anime/Basic Animation Package.json",
         "Plugin/Anime/Basic Animation Package.json"},
        {":/default_config/Mandarin/Character/Assets/test/config.json",
         "Character/Assets/test/config.json"},
        {":/default_config/Mandarin/Character/Assets/test/Tachie/default.png",
         "Character/Assets/test/Tachie/default.png"},
        {":/default_config/Mandarin/Character/UserConfig/test/config.json",
         "Character/UserConfig/test/config.json"},
    };

    for (const DefaultConfigEntry &entry : entries)
    {
        const QString outPath = QDir(targetRoot).filePath(entry.relativePath);
        const QFileInfo outInfo(outPath);
        if (!outInfo.dir().exists())
            QDir().mkpath(outInfo.dir().absolutePath());
        if (QFile::exists(outPath))
            continue;

        QFile inFile(QString::fromUtf8(entry.resourcePath));
        if (!inFile.open(QIODevice::ReadOnly))
            continue;

        QFile outFile(outPath);
        if (!outFile.open(QIODevice::WriteOnly))
            continue;

        outFile.write(inFile.readAll());
    }
}
} // namespace

int main(int argc, char *argv[])
{
#ifdef Q_OS_LINUX
    qputenv("QT_QPA_PLATFORM", "xcb");
#endif
    QApplication a(argc, argv);

    // 桌宠调用的均为国内 API（DeepSeek/Kimi/百度）+ 本地 VITS，绕过系统代理：
    // 否则系统代理（如 Clash）一开一关/挂掉就会让聊天、视觉、主动对话全部请求失败。
    QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::NoProxy));

    // Keep text readable on all platforms when system uses dark mode.
    QPalette labelPalette = a.palette();
    labelPalette.setColor(QPalette::WindowText, QColor(20, 20, 20));
    QApplication::setPalette(labelPalette, "QLabel");

    QPalette textEditPalette = a.palette();
    textEditPalette.setColor(QPalette::Text, QColor(20, 20, 20));
    textEditPalette.setColor(QPalette::PlaceholderText, QColor(120, 120, 120));
    QApplication::setPalette(textEditPalette, "QTextEdit");

    CopyDefaultConfigIfMissing();
    a.setQuitOnLastWindowClosed(false);

    QCoreApplication::setApplicationName("Mandarin");
    QCoreApplication::setApplicationVersion(APP_VERSION);
    QCoreApplication::setOrganizationName("MyOrganization");

    /*窗口创建*/
    Dialog dialogWin;
    dialogWin.show();

    //立绘渲染器由配置决定：默认 png，保持既有行为不变；live2d 为显式选择加入。
    QSettings mainSettings(IniSettingPath, QSettings::IniFormat);
    const QString renderMode =
        mainSettings.value("character/renderMode", "png").toString().trimmed().toLower();
    const QString live2dModel =
        mainSettings.value("character/live2dModel", "miku").toString().trimmed();
    CharacterWindowBase *characterWin = nullptr;
    if (renderMode == "live2d")
    {
#ifdef MANDARIN_HAS_LIVE2D
        auto *live2dWin = new Live2DCharacterWindow();
        //装载模型与「应用心情」是两个入口：模型只在启动期装载一次（失败必须能感知，
        //否则用户会得到一个看不见的桌宠）；之后每次 AI 回复只经 requestSetCharTachie
        //→ reloadContent(心情名) 换情绪预设。
        live2dWin->loadModel(live2dModel);
        if (live2dWin->isModelLoaded())
        {
            characterWin = live2dWin;
        }
        else
        {
            qWarning() << "Live2D 模型装载失败，回退到 PNG 立绘:" << live2dModel;
            delete live2dWin;
            characterWin = new Tachie();
        }
#else
        qWarning() << "本构建未包含 Live2D SDK（live2d 渲染器不可用），回退到 PNG 立绘";
        characterWin = new Tachie();
#endif
    }
    else
    {
        if (renderMode != "png")
            qWarning() << "未知的 character/renderMode，按 png 处理:" << renderMode;
        characterWin = new Tachie();
    }
    characterWin->show();
    MainWindow *settings = nullptr;

    /*一些绑定*/
    //对话框的开启和关闭
    QObject::connect(characterWin, &CharacterWindowBase::requestToggleVisible,
                     &dialogWin, &Dialog::ToggleVisible);
    //文件拖放到立绘
    QObject::connect(characterWin, &CharacterWindowBase::requestFileDrop,
                     &dialogWin, &Dialog::handleFileDrop);
    //修改立绘图片
    QObject::connect(&dialogWin, &Dialog::requestSetCharTachie, characterWin,
                     &CharacterWindowBase::reloadContent);
    QObject::connect(&dialogWin, &Dialog::requestShowInnerThought, characterWin,
                     &CharacterWindowBase::ShowInnerThought);
    QObject::connect(&dialogWin, &Dialog::requestHideInnerThought, characterWin,
                     &CharacterWindowBase::HideInnerThought);
    /*TTS 播放状态 → 立绘说话（Live2D 让嘴巴开合；PNG 路径是空实现）。
       走基类的槽是为了让两种立绘共用同一条连接：连到派生类就要在这里写 if 分支。*/
    QObject::connect(&dialogWin, &Dialog::requestSpeakState, characterWin,
                     &CharacterWindowBase::SetSpeaking);
    /*TTS 响度电平 → 立绘的开口量（Live2D 按真实响度驱动嘴巴，句间停顿闭嘴）。
       同样走基类：PNG 路径的空实现什么都不做。
       没有包络的句子（用户把 vits 的 format 配成 mp3）**一个电平都不发**，
       渲染器据此保持今天的盲扑动 —— 这条回退在 Dialog 侧就决定了。*/
    QObject::connect(&dialogWin, &Dialog::requestSpeakLevel, characterWin,
                     &CharacterWindowBase::SetSpeechLevel);

    /*托盘*/
    QSystemTrayIcon tray;
    tray.setIcon(QIcon(":/res/img/logo/logo.png"));
    tray.setToolTip("Mandarin");
    ElaMenu trayMenu;
    QAction *actionSettings = trayMenu.addAction("设置");
    QAction *actionQuit = trayMenu.addAction("退出");
    tray.setContextMenu(&trayMenu);
    tray.show();
    // 将窗口强制拉到前台（托盘图标点击响应是用户主动行为，不会被系统拦截）
    auto bringToFront = [](QWidget *w) {
        w->show();
        // 如果最小化了就先还原
        if (w->isMinimized())
            w->setWindowState(w->windowState() & ~Qt::WindowMinimized);
        w->raise();
        w->activateWindow();
#ifdef Q_OS_WIN
        // Qt 的 activateWindow 在部分 Windows 版本不够强力，补一刀原生 API
        SetForegroundWindow(reinterpret_cast<HWND>(w->winId()));
#endif
    };

    //左键点击托盘打开设置
    QObject::connect(&tray, QOverload<QSystemTrayIcon::ActivationReason>::of(&QSystemTrayIcon::activated),
                     [&](QSystemTrayIcon::ActivationReason reason)
                     {
                         if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
                         {
                             if (!settings)
                             {
                                 eApp->init();
                                 settings = new MainWindow(&dialogWin, characterWin);
                             }
                             bringToFront(settings);
                         }
                     });
    //设置界面懒加载
    QObject::connect(actionSettings, &QAction::triggered, [&]()
                     {
                         if (!settings)
                         {
                             eApp->init();
                             settings = new MainWindow(&dialogWin, characterWin);
                         }
                         bringToFront(settings); });
    //退出程序
    QObject::connect(actionQuit, &QAction::triggered, &a, &QApplication::quit);

    const int exitCode = a.exec();
    delete characterWin; //立绘窗口是堆对象（原为栈对象），退出前释放
    return exitCode;
}
