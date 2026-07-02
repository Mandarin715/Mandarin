#ifndef SETTINGCHILD_SEARCH_H
#define SETTINGCHILD_SEARCH_H

#include <QWidget>

namespace Ui
{
class SettingChild_Search;
}

class SettingChild_Search : public QWidget
{
    Q_OBJECT

public:
    explicit SettingChild_Search(QWidget *parent = nullptr);
    ~SettingChild_Search();

signals:
    void searchConfigChanged();

private slots:
    void on_BreadcrumbBar_breadcrumbClicked(QString breadcrumb,
                                            QStringList lastBreadcrumbList);
    void on_ToggleSwitch_SearchEnable_toggled(bool checked);
    void on_ToggleSwitch_AutoSearch_toggled(bool checked);
    void on_lineEdit_ApiKey_textChanged(const QString &text);
    void on_lineEdit_SecretKey_textChanged(const QString &text);
    void on_lineEdit_BaseUrl_textChanged(const QString &text);

private:
    Ui::SettingChild_Search *ui;
};

#endif // SETTINGCHILD_SEARCH_H
