#ifndef SETTING_H
#define SETTING_H

#include "ElaWindow.h"
#include <QMainWindow>

#include "../dialog/dialog.h"
#include "../character/characterwindowbase.h"

QT_BEGIN_NAMESPACE
namespace Ui
{
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public ElaWindow
{
    Q_OBJECT

  public:
    MainWindow(Dialog *dialog, CharacterWindowBase *characterWin, QWidget *parent = nullptr);
    ~MainWindow();

  private:
    Ui::MainWindow *ui;
};
#endif //SETTING_H
