#include "reminderchild.h"
#include "ui_reminderchild.h"

reminderchild::reminderchild(const QString &id, const QString &whenText,
                             const QString &text, bool repeating, QWidget *parent)
    : QWidget(parent), ui(new Ui::reminderchild), m_id(id)
{
    ui->setupUi(this);
    //显示不抢占焦点
    setAttribute(Qt::WA_ShowWithoutActivating);
    ui->label_when->setText(whenText);
    ui->label_text->setText(text);
    ui->label_repeat->setVisible(repeating);
}

reminderchild::~reminderchild()
{
    delete ui;
}

/*请求删除该条提醒*/
void reminderchild::on_pushButton_delete_clicked()
{
    emit deleteRequested(m_id);
}
