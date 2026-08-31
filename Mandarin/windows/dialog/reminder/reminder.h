#ifndef REMINDER_H
#define REMINDER_H

#include <QWidget>

namespace Ui
{
class reminder;
}

class reminder : public QWidget
{
    Q_OBJECT

  public:
    explicit reminder(QWidget *parent = nullptr);
    void clearReminders();
    void addReminder(const QString &id, const QString &whenText,
                     const QString &text, bool repeating);
    ~reminder();

  signals:
    void deleteReminder(const QString &id);
    void clearAllReminders();

  private:
    Ui::reminder *ui;
    void paintEvent(QPaintEvent *event) override;
};

#endif //REMINDER_H
