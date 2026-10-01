#include "settingchild_memory.h"
#include "ui_settingchild_memory.h"

#include "../../../GlobalConstants.h"
#include "../../../utils/MemoryStore.h"

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
    ui->pushButton_ClearAll->setEnabled(false);

    const QString memoryPath = ReadCharacterMemoryPath();
    m_displayedMemoryPath = memoryPath;
    if (memoryPath.isEmpty())
        return;

    QJsonObject data;
    QString error;
    if (!MemoryStore::load(memoryPath, data, &error)) {
        ElaMessageBar::error(ElaMessageBarType::BottomRight, "读取失败", error, 5000, this);
        return;
    }

    const QJsonArray summaries = data.value("help_summaries").toArray();
    const QJsonObject personalInfo = data.value("personal_info").toObject();

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

        const QString entryId = entry.value("id").toString();
        connect(btnDelete, &QPushButton::clicked, this, [this, memoryPath, entryId]() {
            if (ReadCharacterMemoryPath() != memoryPath) {
                RefreshMemoryList();
                return;
            }
            QString error;
            if (!MemoryStore::removeForCharacter(memoryPath, ReadCharacterMemoryPath(), entryId, false, &error)) {
                ElaMessageBar::error(ElaMessageBarType::BottomRight, "删除失败", error, 5000, this);
                RefreshMemoryList();
                return;
            }
            RefreshMemoryList();
            emit requestReloadMemory();
        });

        layout->addWidget(row);
        m_memoryRows.append(row);
    }
}

/*全部清空*/
void SettingChild_Memory::on_pushButton_ClearAll_clicked()
{
    const QString path = m_displayedMemoryPath;
    if (path.isEmpty() || ReadCharacterMemoryPath() != path) {
        RefreshMemoryList();
        return;
    }
    QString error;
    if (!MemoryStore::removeForCharacter(path, ReadCharacterMemoryPath(), {}, true, &error)) {
        ElaMessageBar::error(ElaMessageBarType::BottomRight, "清空失败", error, 5000, this);
        return;
    }
    RefreshMemoryList();
    emit requestReloadMemory();
    ElaMessageBar::success(ElaMessageBarType::BottomRight, "已清空",
                           "对话记忆已全部清除", 3000, this);
}

/*手动刷新*/
void SettingChild_Memory::on_pushButton_Refresh_clicked()
{
    RefreshMemoryList();
}
