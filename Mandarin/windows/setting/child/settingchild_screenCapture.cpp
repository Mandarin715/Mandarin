#include "settingchild_screenCapture.h"
#include "ui_settingchild_screenCapture.h"

#include "../../../GlobalConstants.h"

#include "ZcJsonLib.h"

#include <QSignalBlocker>

namespace {
// 旧版屏幕捕获把所有服务商共用一个 ApiKey；现改为每服务商独立 key（与 LLM 页一致）。
// 迁移：把旧共享 key 写入“当前服务商”路径后清空旧字段（ZcJsonLib 无 remove，置空即视为失效，
// 避免 dialog 回退时拿到陈旧 key 打到错误端点）。幂等：已迁移则不再写。
void migrateLegacyApiKey(ZcJsonLib &config, const QString &server)
{
    const QString legacyKey =
        config.value("screenCapture/ApiKey").toString().trimmed();
    if (legacyKey.isEmpty())
        return;
    if (!config.value("screenCapture/" + server + "/ApiKey")
             .toString()
             .trimmed()
             .isEmpty())
        return; // 已迁移过
    config.setValue("screenCapture/" + server + "/ApiKey", legacyKey);
    config.setValue("screenCapture/ApiKey", QString());
}
} // namespace

SettingChild_ScreenCapture::SettingChild_ScreenCapture(QWidget *parent)
    : QWidget(parent), ui(new Ui::SettingChild_ScreenCapture)
{
    ui->setupUi(this);

    ui->BreadcrumbBar->setTextPixelSize(25);
    ui->BreadcrumbBar->appendBreadcrumb("屏幕捕获设置");

    // 初始化服务商列表
    ui->comboBox_ServerSelect->addItem("Kimi");
    ui->comboBox_ServerSelect->addItem("DeepSeek");
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

    // 一次性迁移：旧版单一共享 ApiKey → 当前服务商独立 key（幂等，之后清空旧字段防陈旧回退）
    migrateLegacyApiKey(config, server);
    const QString apiKey =
        config.value("screenCapture/" + server + "/ApiKey").toString();
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
    // 切换服务商时带出该服务商独立的 ApiKey（未配置则为空）
    {
        const QSignalBlocker blocker(ui->lineEdit_ApiKey);
        ui->lineEdit_ApiKey->setText(
            config.value("screenCapture/" + text + "/ApiKey").toString());
    }
    emit screenCaptureConfigChanged();
}

void SettingChild_ScreenCapture::on_lineEdit_ApiKey_textChanged(
    const QString &text)
{
    const QString server = ui->comboBox_ServerSelect->currentText();
    if (server.isEmpty())
        return;
    ZcJsonLib config(JsonSettingPath);
    config.setValue("screenCapture/" + server + "/ApiKey", text.trimmed());
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
        // 以 /v1/models 返回为准：旧 moonshot-v1-*-vision-preview / kimi-latest 已废弃
        ui->comboBox_ModelSelect->addItem("kimi-k2.6");
        ui->comboBox_ModelSelect->addItem("kimi-k3");
        ui->comboBox_ModelSelect->addItem("kimi-k2.7-code-highspeed");
    }
    else if (server == "DeepSeek")
    {
        ui->comboBox_ModelSelect->addItem("deepseek-v4-flash-vision-exp");
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
    else if (!isCustom && server == "DeepSeek")
        ui->lineEdit_BaseUrl->setPlaceholderText("https://api.deepseek.com/v1");
    else if (!isCustom && server == "OpenAI")
        ui->lineEdit_BaseUrl->setPlaceholderText("https://api.openai.com/v1");
}
