#ifndef SETTINGCHILD_SPEECH_H
#define SETTINGCHILD_SPEECH_H

#include <QWidget>

namespace Ui
{
class SettingChild_Speech;
}

class SettingChild_Speech : public QWidget
{
    Q_OBJECT

  public:
    explicit SettingChild_Speech(QWidget *parent = nullptr);
    ~SettingChild_Speech();

  signals:
    void speechConfigChanged();

  private slots:
    void on_BreadcrumbBar_breadcrumbClicked(QString breadcrumb,
                                            QStringList lastBreadcrumbList);
    void on_pushButton_Baidu_Set_clicked();
    void on_ToggleSwitch_SpeechInputEnable_toggled(bool checked);
    void on_ToggleSwitch_GlobalHotkeyEnable_toggled(bool checked);
    void on_ToggleSwitch_ContinuousHotkeyEnable_toggled(bool checked);
    void on_spinBox_ContinuousAudioDelay_valueChanged(int value);
    void on_ToggleSwitch_WakeWordEnable_toggled(bool checked);
    void on_spinBox_WakeWordSensitivity_valueChanged(double value);
    void on_lineEdit_BaiduApiKey_textChanged(const QString &arg1);
    void on_lineEdit_BaiduSecretKey_textChanged(const QString &arg1);
    void on_keyBinder_GlobalHotkey_binderKeyTextChanged(QString binderKeyText);
    void on_keyBinder_GlobalHotkey_nativeVirtualBinderKeyChanged(
        quint32 binderKey);
    void on_keyBinder_ContinuousHotkey_binderKeyTextChanged(QString binderKeyText);
    void on_keyBinder_ContinuousHotkey_nativeVirtualBinderKeyChanged(
        quint32 binderKey);

  private:
    Ui::SettingChild_Speech *ui;
    void initializeNativeGlobalHotkeyBinder(); //延迟重建Ela原生绑定框
    void initializeNativeContinuousHotkeyBinder();
    void refreshBaiduStatus();
    void refreshGlobalHotkeyBinderState();
    void refreshContinuousHotkeyBinderState();
    void saveGlobalHotkeyConfig();
    void saveContinuousHotkeyConfig();
};

#endif //SETTINGCHILD_SPEECH_H
