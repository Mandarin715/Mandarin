#include "settingchild_screenCapture.h"
#include "ui_settingchild_screenCapture.h"

#include "../../../GlobalConstants.h"

#include "ZcJsonLib.h"

#include <QSignalBlocker>

SettingChild_ScreenCapture::SettingChild_ScreenCapture(QWidget *parent)
    : QWidget(parent), ui(new Ui::SettingChild_ScreenCapture)
{
    ui->setupUi(this);

    ui->BreadcrumbBar->setTextPixelSize(25);
    ui->BreadcrumbBar->appendBreadcrumb("屏幕捕获设置");

    // 初始化服务商列表
    ui->comboBox_ServerSelect->addItem("Kimi");
    ui->comboBox_ServerSelect->addItem("OpenAI");
    ui->comboBox_ServerSelect->addItem("Custom");

    // 读取配置
    ZcJsonLib config(JsonSettingPath);
    const bool enabled = config.value("screenCapture/Enable", false).toBool();
    ui->ToggleSwitch_ScreenCaptureEnable->setIsToggled(enabled);

    const QString server =
        config.value("screenCapture/Server", "Kimi").toString();
    {
        const QSignalBlocker blocker(ui->comboBox_ServerSelect);
        ui->comboBox_ServerSelect->setCurrentText(server);
    }

    const QString apiKey =
        config.value("screenCapture/ApiKey").toString();
    {
        const QSignalBlocker blocker(ui->lineEdit_ApiKey);
        ui->lineEdit_ApiKey->setText(apiKey);
    }

    updateModelPresets(server);
    const QString model =
        config.value("screenCapture/Model").toString();
    {
        const QSignalBlocker blocker(ui->comboBox_ModelSelect);
        ui->comboBox_ModelSelect->setCurrentText(model);
    }

    const QString baseUrl =
        config.value("screenCapture/BaseUrl").toString();
    {
        const QSignalBlocker blocker(ui->lineEdit_BaseUrl);
        ui->lineEdit_BaseUrl->setText(baseUrl);
    }

    updateBaseUrlVisibility(server);
}

SettingChild_ScreenCapture::~SettingChild_ScreenCapture()
{
    delete ui;
}

void SettingChild_ScreenCapture::on_BreadcrumbBar_breadcrumbClicked(
    QString breadcrumb, QStringList lastBreadcrumbList)
{
    Q_UNUSED(breadcrumb);
    Q_UNUSED(lastBreadcrumbList);
    ui->stackedWidget->setCurrentIndex(0);
}

void SettingChild_ScreenCapture::on_ToggleSwitch_ScreenCaptureEnable_toggled(
    bool checked)
{
    ZcJsonLib config(JsonSettingPath);
    config.setValue("screenCapture/Enable", checked);
    emit screenCaptureConfigChanged();
}

void SettingChild_ScreenCapture::on_comboBox_ServerSelect_currentTextChanged(
    const QString &text)
{
    if (text.isEmpty())
        return;
    ZcJsonLib config(JsonSettingPath);
    config.setValue("screenCapture/Server", text);
    updateModelPresets(text);
    updateBaseUrlVisibility(text);
    emit screenCaptureConfigChanged();
}

void SettingChild_ScreenCapture::on_lineEdit_ApiKey_textChanged(
    const QString &text)
{
    ZcJsonLib config(JsonSettingPath);
    config.setValue("screenCapture/ApiKey", text.trimmed());
    emit screenCaptureConfigChanged();
}

void SettingChild_ScreenCapture::on_comboBox_ModelSelect_currentTextChanged(
    const QString &text)
{
    if (text.isEmpty())
        return;
    ZcJsonLib config(JsonSettingPath);
    config.setValue("screenCapture/Model", text.trimmed());
    emit screenCaptureConfigChanged();
}

void SettingChild_ScreenCapture::on_lineEdit_BaseUrl_textChanged(
    const QString &text)
{
    ZcJsonLib config(JsonSettingPath);
    config.setValue("screenCapture/BaseUrl", text.trimmed());
    emit screenCaptureConfigChanged();
}

void SettingChild_ScreenCapture::updateModelPresets(const QString &server)
{
    ui->comboBox_ModelSelect->clear();
    if (server == "Kimi")
    {
        ui->comboBox_ModelSelect->addItem("moonshot-v1-8k-vision-preview");
        ui->comboBox_ModelSelect->addItem("moonshot-v1-32k-vision-preview");
        ui->comboBox_ModelSelect->addItem("moonshot-v1-128k-vision-preview");
    }
    else if (server == "OpenAI")
    {
        ui->comboBox_ModelSelect->addItem("gpt-4o-mini");
        ui->comboBox_ModelSelect->addItem("gpt-4o");
        ui->comboBox_ModelSelect->addItem("gpt-4-turbo");
    }
    // Custom: 不添加预设，用户自行输入
}

void SettingChild_ScreenCapture::updateBaseUrlVisibility(const QString &server)
{
    const bool isCustom = (server == "Custom");
    ui->widget_BaseUrl->setVisible(isCustom);
    if (!isCustom && server == "Kimi")
        ui->lineEdit_BaseUrl->setPlaceholderText("https://api.moonshot.cn/v1");
    else if (!isCustom && server == "OpenAI")
        ui->lineEdit_BaseUrl->setPlaceholderText("https://api.openai.com/v1");
}
