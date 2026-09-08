#include "settingchild_general.h"
#include "ui_settingchild_general.h"

#include "../../../GlobalConstants.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QSignalBlocker>
#include <QMediaDevices>
#include <QCameraDevice>
#include "ZcJsonLib.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace
{
//与Dialog默认尺寸保持一致，用于首次打开设置页时展示。
constexpr int kDefaultDialogWidth = 650;
constexpr int kDefaultDialogHeight = 200;
}

SettingChild_General::SettingChild_General(QWidget *parent)
    : QWidget(parent), ui(new Ui::SettingChild_General)
{
    ui->setupUi(this);

    ui->BreadcrumbBar->setTextPixelSize(25);
    ui->BreadcrumbBar->appendBreadcrumb("通用设置");

    //初始化控件时屏蔽信号，避免读取配置时反向写入并触发刷新。
    QSettings settings(IniSettingPath, QSettings::IniFormat);
    const QSignalBlocker widthBlocker(ui->spinBox_DialogWidth);
    const QSignalBlocker heightBlocker(ui->spinBox_DialogHeight);
    ui->spinBox_DialogWidth->setValue(
        settings.value("general/DialogWidth", kDefaultDialogWidth).toInt());
    ui->spinBox_DialogHeight->setValue(
        settings.value("general/DialogHeight", kDefaultDialogHeight).toInt());
    ui->ToggleSwitch_AutoStart->setIsToggled(
        settings.value("general/AutoStart", false).toBool());
    ui->lineEdit_Location->setText(
        settings.value("general/Location").toString());
    {
        const QSignalBlocker proactiveBlocker(ui->ToggleSwitch_ProactiveEnable);
        ui->ToggleSwitch_ProactiveEnable->setIsToggled(
            settings.value("general/ProactiveEnable", false).toBool());
    }
    {
        const QSignalBlocker cooldownBlocker(ui->spinBox_ProactiveCooldown);
        ui->spinBox_ProactiveCooldown->setValue(
            settings.value("general/ProactiveCooldownMinutes", 10).toInt());
    }

    // 摄像头感知（config.json，独立于 INI 的 general/*）
    {
        ZcJsonLib camConfig(JsonSettingPath);
        const bool camEnable =
            camConfig.value("cameraPerception/Enable", false).toBool();
        const QSignalBlocker camBlocker(ui->ToggleSwitch_CameraPerceptionEnable);
        ui->ToggleSwitch_CameraPerceptionEnable->setIsToggled(camEnable);

        ui->comboBox_CameraDevice->clear();
        const QList<QCameraDevice> cams = QMediaDevices::videoInputs();
        for (const QCameraDevice &cam : cams)
            ui->comboBox_CameraDevice->addItem(cam.description());
        const QString savedDev =
            camConfig.value("cameraPerception/Device").toString();
        if (!savedDev.isEmpty())
        {
            const int idx = ui->comboBox_CameraDevice->findText(savedDev);
            if (idx >= 0)
            {
                const QSignalBlocker deviceBlocker(ui->comboBox_CameraDevice);
                ui->comboBox_CameraDevice->setCurrentIndex(idx);
            }
        }
        // 无摄像头时隐藏设备选择
        ui->widget_CameraDevice->setVisible(!cams.isEmpty());
    }
}

SettingChild_General::~SettingChild_General()
{
    delete ui;
}

/*设置对话框宽度*/
void SettingChild_General::on_spinBox_DialogWidth_valueChanged(int arg1)
{
    QSettings settings(IniSettingPath, QSettings::IniFormat);
    settings.setValue("general/DialogWidth", arg1);
    emit generalConfigChanged();
}

/*设置对话框高度*/
void SettingChild_General::on_spinBox_DialogHeight_valueChanged(int arg1)
{
    QSettings settings(IniSettingPath, QSettings::IniFormat);
    settings.setValue("general/DialogHeight", arg1);
    emit generalConfigChanged();
}

/*开机自启开关*/
void SettingChild_General::on_ToggleSwitch_AutoStart_toggled(bool checked)
{
    QSettings settings(IniSettingPath, QSettings::IniFormat);
    settings.setValue("general/AutoStart", checked);

    const QString batPath = QDir::toNativeSeparators(
        QDir(QCoreApplication::applicationDirPath()).filePath("启动.bat"));
#ifdef Q_OS_WIN
    if (checked) {
        HKEY hKey;
        LONG openResult = RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, KEY_SET_VALUE | KEY_QUERY_VALUE, &hKey);
        if (openResult == ERROR_SUCCESS) {
            const auto utf16 = batPath.utf16();
            RegSetValueExW(hKey, L"Mandarin", 0, REG_SZ,
                reinterpret_cast<const BYTE *>(utf16),
                static_cast<DWORD>((batPath.size() + 1) * sizeof(QChar)));
            RegCloseKey(hKey);
        }
    } else {
        HKEY hKey;
        if (RegOpenKeyExW(HKEY_CURRENT_USER,
                          L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                          0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
            RegDeleteValueW(hKey, L"Mandarin");
            RegCloseKey(hKey);
        }
    }
#elif defined(Q_OS_MACOS)
    const QString plistPath = QDir::homePath() +
        "/Library/LaunchAgents/com.mandarin715.mandarin.plist";
    if (checked) {
        QFile file(plistPath);
        QDir().mkpath(QFileInfo(plistPath).absolutePath());
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QStringLiteral(
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                "<plist version=\"1.0\">\n<dict>\n"
                "<key>Label</key><string>com.mandarin715.mandarin</string>\n"
                "<key>ProgramArguments</key><array><string>%1</string></array>\n"
                "<key>RunAtLoad</key><true/>\n"
                "</dict>\n</plist>\n").arg(batPath).toUtf8());
        }
    } else {
        QFile::remove(plistPath);
    }
#elif defined(Q_OS_LINUX)
    const QString desktopPath = QDir::homePath() +
        "/.config/autostart/mandarin.desktop";
    if (checked) {
        QFile file(desktopPath);
        QDir().mkpath(QFileInfo(desktopPath).absolutePath());
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QStringLiteral(
                "[Desktop Entry]\nType=Application\nName=Mandarin\nExec=%1\n"
                "X-GNOME-Autostart-enabled=true\n").arg(batPath).toUtf8());
        }
    } else {
        QFile::remove(desktopPath);
    }
#endif
}

/*手动设置所在城市*/
void SettingChild_General::on_lineEdit_Location_textChanged(const QString &arg1)
{
    QSettings settings(IniSettingPath, QSettings::IniFormat);
    settings.setValue("general/Location", arg1.trimmed());
    emit generalConfigChanged();
}

/*主动对话开关*/
void SettingChild_General::on_ToggleSwitch_ProactiveEnable_toggled(bool checked)
{
    QSettings settings(IniSettingPath, QSettings::IniFormat);
    settings.setValue("general/ProactiveEnable", checked);
    emit generalConfigChanged();
}

/*主动对话冷却间隔*/
void SettingChild_General::on_spinBox_ProactiveCooldown_valueChanged(int value)
{
    QSettings settings(IniSettingPath, QSettings::IniFormat);
    settings.setValue("general/ProactiveCooldownMinutes", value);
    emit generalConfigChanged();
}

/*摄像头感知开关*/
void SettingChild_General::on_ToggleSwitch_CameraPerceptionEnable_toggled(bool checked)
{
    ZcJsonLib config(JsonSettingPath);
    config.setValue("cameraPerception/Enable", checked);
    emit cameraPerceptionConfigChanged();
}

/*摄像头设备选择*/
void SettingChild_General::on_comboBox_CameraDevice_currentTextChanged(const QString &text)
{
    ZcJsonLib config(JsonSettingPath);
    config.setValue("cameraPerception/Device", text);
    emit cameraPerceptionConfigChanged();
}
