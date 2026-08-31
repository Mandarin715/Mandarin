#include "reminder.h"
#include "reminderchild.h"
#include "ui_reminder.h"

#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollBar>
#include <QVBoxLayout>

reminder::reminder(QWidget *parent)
    : QWidget(parent), ui(new Ui::reminder)
{
    ui->setupUi(this);
    //无边框置顶透明窗口
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
    setWindowOpacity(0.9);
    setAttribute(Qt::WA_TranslucentBackground);
    //自动滚动到底部，便于查看最新提醒
    ui->scrollArea->setWidgetResizable(true);
    // 列表从顶部开始排列，内容不足时避免居中，删除后自动往上补位
    ui->scrollArea->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    QVBoxLayout *listLayout =
        qobject_cast<QVBoxLayout *>(ui->scrollAreaWidgetContents->layout());
    if (listLayout)
        listLayout->setAlignment(Qt::AlignTop);
    connect(ui->scrollArea->verticalScrollBar(), &QScrollBar::rangeChanged, this,
            [=]()
            {
                ui->scrollArea->verticalScrollBar()->setValue(
                    ui->scrollArea->verticalScrollBar()->maximum());
            });
    connect(ui->pushButton_clearAll, &QPushButton::clicked, this,
            &reminder::clearAllReminders);
}

reminder::~reminder()
{
    delete ui;
}

/*清空列表*/
void reminder::clearReminders()
{
    QVBoxLayout *layout =
        qobject_cast<QVBoxLayout *>(ui->scrollAreaWidgetContents->layout());
    if (!layout)
        return;

    while (QLayoutItem *item = layout->takeAt(0))
    {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
}

/*添加一条提醒卡片*/
void reminder::addReminder(const QString &id, const QString &whenText,
                           const QString &text, bool repeating)
{
    QVBoxLayout *layout =
        qobject_cast<QVBoxLayout *>(ui->scrollAreaWidgetContents->layout());
    if (!layout)
        return;

    reminderchild *newChild =
        new reminderchild(id, whenText, text, repeating, this);
    connect(newChild, &reminderchild::deleteRequested, this,
            &reminder::deleteReminder);
    layout->addWidget(newChild);
}

/*圆角边框（与 history 窗口一致）*/
void reminder::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    QRectF rect(5, 5, this->width() - 10, this->height() - 10);
    path.addRoundedRect(rect, 15, 15);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillPath(path, QBrush(Qt::white));
    QColor color(0, 0, 0, 50);
    for (int i = 0; i < 5; i++)
    {
        QPainterPath shadowPath;
        shadowPath.setFillRule(Qt::WindingFill);
        QRectF shadowRect((5 - i), (5 - i), this->width() - (5 - i) * 2,
                          this->height() - (5 - i) * 2);
        shadowPath.addRoundedRect(shadowRect, 15, 15);
        color.setAlpha(50 - qSqrt(i) * 22);
        painter.setPen(color);
        painter.drawPath(shadowPath);
    }
}
