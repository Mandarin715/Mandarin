#include "windows/dialog/dialog.h"
#include "windows/setting/setting.h"
#include "windows/tachie/tachie.h"

#include "ElaApplication.h"
#include "ElaMenu.h"

#include "Version.h"

#include <QApplication>
#include <QColor>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QPalette>
#include <QStandardPaths>
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
    Tachie tachieWin;
    tachieWin.show();
    MainWindow *settings = nullptr;

    /*一些绑定*/
    //对话框的开启和关闭
    QObject::connect(&tachieWin, &Tachie::requestToggleVisible, &dialogWin,
                     &Dialog::ToggleVisible);
    //修改立绘图片
    QObject::connect(&dialogWin, &Dialog::requestSetCharTachie, &tachieWin,
                     &Tachie::SetTachieImg);

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
                                 settings = new MainWindow(&dialogWin, &tachieWin);
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
                             settings = new MainWindow(&dialogWin, &tachieWin);
                         }
                         bringToFront(settings); });
    //退出程序
    QObject::connect(actionQuit, &QAction::triggered, &a, &QApplication::quit);

    return a.exec();
}
