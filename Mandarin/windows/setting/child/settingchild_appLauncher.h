#ifndef SETTINGCHILD_APPLAUNCHER_H
#define SETTINGCHILD_APPLAUNCHER_H

#include <QWidget>

class ElaScrollPageArea;
class ElaText;
class QLineEdit;
class QPushButton;

namespace Ui
{
class SettingChild_AppLauncher;
}

struct AppCommandCard
{
    ElaScrollPageArea *area = nullptr;
    QLineEdit *keywordEdit = nullptr;
    QLineEdit *pathEdit = nullptr;
};

class SettingChild_AppLauncher : public QWidget
{
    Q_OBJECT

  public:
    explicit SettingChild_AppLauncher(QWidget *parent = nullptr);
    ~SettingChild_AppLauncher();

  signals:
    void appLauncherConfigChanged();

  private slots:
    void on_BreadcrumbBar_breadcrumbClicked(QString breadcrumb,
                                            QStringList lastBreadcrumbList);
    void on_pushButton_Add_clicked();

  private:
    Ui::SettingChild_AppLauncher *ui;
    QList<AppCommandCard> m_cards;

    void addCard(const QString &keyword = QString(),
                 const QString &path = QString());
    void removeCard(int index);
    void saveAll();
    void loadFromConfig();
};

#endif //SETTINGCHILD_APPLAUNCHER_H
