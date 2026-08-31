#ifndef REMINDERCHILD_H
#define REMINDERCHILD_H

#include <QWidget>

namespace Ui
{
class reminderchild;
}

class reminderchild : public QWidget
{
    Q_OBJECT

  public:
    explicit reminderchild(const QString &id, const QString &whenText,
                           const QString &text, bool repeating,
                           QWidget *parent = nullptr);
    ~reminderchild();

  signals:
    void deleteRequested(const QString &id);

  private slots:
    void on_pushButton_delete_clicked();

  private:
    Ui::reminderchild *ui;
    QString m_id;
};

#endif //REMINDERCHILD_H
