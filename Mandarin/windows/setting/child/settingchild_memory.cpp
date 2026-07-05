#include "settingchild_memory.h"
#include "ui_settingchild_memory.h"

#include "../../../GlobalConstants.h"

#include "ElaMessageBar.h"
#include "ElaScrollPageArea.h"
#include "ElaText.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QVBoxLayout>

SettingChild_Memory::SettingChild_Memory(QWidget *parent)
    : QWidget(parent), ui(new Ui::SettingChild_Memory)
{
    ui->setupUi(this);
    RefreshMemoryList();
}

SettingChild_Memory::~SettingChild_Memory()
{
    delete ui;
}

/*清空动态记忆行*/
void SettingChild_Memory::ClearMemoryRows()
{
    for (QWidget *row : m_memoryRows)
    {
        if (row)
            row->deleteLater();
    }
    m_memoryRows.clear();
}

/*刷新对话记忆列表*/
void SettingChild_Memory::RefreshMemoryList()
{
    ClearMemoryRows();

    const QString memoryPath = ReadCharacterMemoryPath();
    if (memoryPath.isEmpty())
        return;

    QFile file(memoryPath);
    if (!file.open(QIODevice::ReadOnly))
        return;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    const QJsonArray summaries = doc.object().value("help_summaries").toArray();
    const QJsonObject personalInfo = doc.object().value("personal_info").toObject();

    // ── 用户信息区 ──
    if (!personalInfo.isEmpty())
    {
        auto *infoRow = new ElaScrollPageArea(ui->widget_MemoryContainer);
        auto *infoLayout = new QHBoxLayout(infoRow);
        infoLayout->setContentsMargins(12, 6, 12, 6);
        infoLayout->setSpacing(8);

        auto *infoLabel = new ElaText(infoRow);
        infoLabel->setFont(QFont(infoLabel->font().family(), 11));
        QStringList infoParts;
        for (auto it = personalInfo.begin(); it != personalInfo.end(); ++it)
            infoParts.append(QString("%1: %2").arg(it.key(), it.value().toString()));
        infoLabel->setText("用户信息 — " + infoParts.join(" | "));
        infoLabel->setStyleSheet("color: #555;");
        infoLayout->addWidget(infoLabel, 1);
        ui->verticalLayout_Memory->addWidget(infoRow);
        m_memoryRows.append(infoRow);
    }

    if (summaries.isEmpty())
    {
        auto *emptyLabel = new ElaText(ui->widget_MemoryContainer);
        emptyLabel->setText("暂无记忆");
        emptyLabel->setFont(QFont(emptyLabel->font().family(), 11));
        emptyLabel->setStyleSheet("color: #999;");
        auto *emptyRow = new QWidget(ui->widget_MemoryContainer);
        auto *emptyLayout = new QHBoxLayout(emptyRow);
        emptyLayout->setContentsMargins(0, 4, 0, 4);
        emptyLayout->addWidget(emptyLabel);
        emptyLayout->addStretch();
        ui->verticalLayout_Memory->addWidget(emptyRow);
        m_memoryRows.append(emptyRow);
        ui->pushButton_ClearAll->setEnabled(false);
        return;
    }

    ui->pushButton_ClearAll->setEnabled(true);
    QVBoxLayout *layout = ui->verticalLayout_Memory;

    for (int i = 0; i < summaries.size(); ++i)
    {
        const QJsonObject entry = summaries[i].toObject();
        const QString topic = entry.value("topic").toString();
        const QString date = entry.value("date").toString();
        const QString summary = entry.value("summary").toString();

        ElaScrollPageArea *row = new ElaScrollPageArea(ui->widget_MemoryContainer);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(12, 4, 8, 4);
        rowLayout->setSpacing(8);

        auto *dateLabel = new ElaText(row);
        dateLabel->setText(date);
        dateLabel->setFont(QFont(dateLabel->font().family(), 10));
        dateLabel->setStyleSheet("color: #999;");
        dateLabel->setFixedWidth(75);
        rowLayout->addWidget(dateLabel);

        QString displayText = topic.isEmpty() ? summary : topic;
        if (displayText.length() > 50)
            displayText = displayText.left(50) + "...";
        auto *topicLabel = new ElaText(row);
        topicLabel->setText(displayText);
        topicLabel->setFont(QFont(topicLabel->font().family(), 11));
        topicLabel->setToolTip(summary);
        rowLayout->addWidget(topicLabel, 1);

        auto *btnDelete = new QPushButton("✕", row);
        btnDelete->setFixedSize(24, 24);
        btnDelete->setStyleSheet(
            "QPushButton{background:transparent;border:none;font-size:13px;color:#bbb;}"
            "QPushButton:hover{background:#e74c3c;color:#fff;border-radius:12px;}");
        btnDelete->setToolTip("删除此条记忆");
        rowLayout->addWidget(btnDelete);

        connect(btnDelete, &QPushButton::clicked, this, [this, i]() {
            const QString path = ReadCharacterMemoryPath();
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly))
                return;
            QJsonDocument d = QJsonDocument::fromJson(f.readAll());
            f.close();
            QJsonObject obj = d.object();
            QJsonArray arr = obj.value("help_summaries").toArray();
            if (i >= 0 && i < arr.size())
                arr.removeAt(i);
            obj["help_summaries"] = arr;
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            {
                f.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
                f.close();
            }
            RefreshMemoryList();
        });

        layout->addWidget(row);
        m_memoryRows.append(row);
    }
}

/*全部清空*/
void SettingChild_Memory::on_pushButton_ClearAll_clicked()
{
    const QString path = ReadCharacterMemoryPath();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    QJsonObject obj = doc.object();
    obj["help_summaries"] = QJsonArray();
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
        file.close();
    }
    RefreshMemoryList();
    ElaMessageBar::success(ElaMessageBarType::BottomRight, "已清空",
                           "对话记忆已全部清除", 3000, this);
}

/*手动刷新*/
void SettingChild_Memory::on_pushButton_Refresh_clicked()
{
    RefreshMemoryList();
}
