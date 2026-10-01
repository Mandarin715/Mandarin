#ifndef SETTINGCHILD_MEMORY_H
#define SETTINGCHILD_MEMORY_H

#include <QWidget>

namespace Ui
{
class SettingChild_Memory;
}

class SettingChild_Memory : public QWidget
{
    Q_OBJECT

  public:
    explicit SettingChild_Memory(QWidget *parent = nullptr);
    ~SettingChild_Memory();
    void RefreshMemoryList();

  signals:
    void requestReloadMemory();

  private slots:
    void on_pushButton_ClearAll_clicked();
    void on_pushButton_Refresh_clicked();

  private:
    Ui::SettingChild_Memory *ui;
    QList<QWidget *> m_memoryRows;
    QString m_displayedMemoryPath;
    void ClearMemoryRows();
};

#endif //SETTINGCHILD_MEMORY_H
