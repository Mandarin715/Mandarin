#include "settingchild_search.h"
#include "ui_settingchild_search.h"

#include "../../../GlobalConstants.h"

#include "ZcJsonLib.h"

#include <QSignalBlocker>

SettingChild_Search::SettingChild_Search(QWidget *parent)
    : QWidget(parent), ui(new Ui::SettingChild_Search)
{
    ui->setupUi(this);

    ui->BreadcrumbBar->setTextPixelSize(25);
    ui->BreadcrumbBar->appendBreadcrumb(QStringLiteral("联网搜索设置"));

    // 读取配置
    ZcJsonLib config(JsonSettingPath);
    const bool enabled = config.value("search/Enable", false).toBool();
    ui->ToggleSwitch_SearchEnable->setIsToggled(enabled);

    const bool autoSearch =
        config.value("search/AutoSearch", true).toBool();
    ui->ToggleSwitch_AutoSearch->setIsToggled(autoSearch);

    const QString apiKey = config.value("search/ApiKey").toString();
    {
        const QSignalBlocker blocker(ui->lineEdit_ApiKey);
        ui->lineEdit_ApiKey->setText(apiKey);
    }

    const QString secretKey = config.value("search/SecretKey").toString();
    {
        const QSignalBlocker blocker(ui->lineEdit_SecretKey);
        ui->lineEdit_SecretKey->setText(secretKey);
    }

    QString baseUrl = config.value("search/BaseUrl").toString();
    if (baseUrl.isEmpty())
        baseUrl = QStringLiteral(
            "https://qianfan.baidubce.com/v2/ai_search/web_search");
    {
        const QSignalBlocker blocker(ui->lineEdit_BaseUrl);
        ui->lineEdit_BaseUrl->setText(baseUrl);
    }
}

SettingChild_Search::~SettingChild_Search()
{
    delete ui;
}

void SettingChild_Search::on_BreadcrumbBar_breadcrumbClicked(
    QString breadcrumb, QStringList lastBreadcrumbList)
{
    Q_UNUSED(breadcrumb);
    Q_UNUSED(lastBreadcrumbList);
}

void SettingChild_Search::on_ToggleSwitch_SearchEnable_toggled(bool checked)
{
    ZcJsonLib config(JsonSettingPath);
    config.setValue("search/Enable", checked);
    emit searchConfigChanged();
}

void SettingChild_Search::on_ToggleSwitch_AutoSearch_toggled(bool checked)
{
    ZcJsonLib config(JsonSettingPath);
    config.setValue("search/AutoSearch", checked);
    emit searchConfigChanged();
}

void SettingChild_Search::on_lineEdit_ApiKey_textChanged(const QString &text)
{
    ZcJsonLib config(JsonSettingPath);
    config.setValue("search/ApiKey", text.trimmed());
    emit searchConfigChanged();
}

void SettingChild_Search::on_lineEdit_SecretKey_textChanged(const QString &text)
{
    ZcJsonLib config(JsonSettingPath);
    config.setValue("search/SecretKey", text.trimmed());
    emit searchConfigChanged();
}

void SettingChild_Search::on_lineEdit_BaseUrl_textChanged(const QString &text)
{
    ZcJsonLib config(JsonSettingPath);
    config.setValue("search/BaseUrl", text.trimmed());
    emit searchConfigChanged();
}
