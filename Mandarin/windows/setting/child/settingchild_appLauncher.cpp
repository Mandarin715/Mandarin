#include "settingchild_appLauncher.h"
#include "./ui_settingchild_appLauncher.h"

#include "../../../GlobalConstants.h"
#include "ZcJsonLib.h"
#include <ElaScrollPageArea.h>
#include <ElaText.h>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

SettingChild_AppLauncher::SettingChild_AppLauncher(QWidget *parent)
    : QWidget(parent), ui(new Ui::SettingChild_AppLauncher)
{
    ui->setupUi(this);
    loadFromConfig();
}

SettingChild_AppLauncher::~SettingChild_AppLauncher()
{
    delete ui;
}

void SettingChild_AppLauncher::on_BreadcrumbBar_breadcrumbClicked(
    QString breadcrumb, QStringList lastBreadcrumbList)
{
    Q_UNUSED(breadcrumb);
    Q_UNUSED(lastBreadcrumbList);
}

void SettingChild_AppLauncher::loadFromConfig()
{
    // 清除现有卡片
    while (!m_cards.isEmpty())
        removeCard(0);

    ZcJsonLib config(JsonSettingPath);
    const QJsonArray commands =
        config.value("appLauncher/commands", QJsonArray()).toArray();

    for (int i = 0; i < commands.size(); ++i)
    {
        const QJsonObject obj = commands[i].toObject();
        addCard(obj.value("keyword").toString(),
                obj.value("path").toString());
    }
}

void SettingChild_AppLauncher::addCard(const QString &keyword,
                                        const QString &path)
{
    AppCommandCard card;
    const int index = m_cards.size();

    // 外层卡片
    card.area = new ElaScrollPageArea(ui->scrollContent);
    card.area->setToolTip("输入含关键词的文本时自动启动对应应用");

    auto *outerLayout = new QVBoxLayout(card.area);
    outerLayout->setContentsMargins(12, 8, 12, 8);
    outerLayout->setSpacing(6);

    // ── 标题行：编号 + 删除按钮 ──
    auto *headerRow = new QHBoxLayout();
    auto *labelIndex = new QLabel(QString("应用 #%1").arg(index + 1), card.area);
    labelIndex->setStyleSheet("font-weight:bold;font-size:13px;color:#666;");
    headerRow->addWidget(labelIndex);
    headerRow->addStretch();
    auto *btnDelete = new QPushButton("✕", card.area);
    btnDelete->setFixedSize(28, 28);
    btnDelete->setStyleSheet(
        "QPushButton{background:#e0e0e0;border:none;border-radius:14px;"
        "font-size:14px;color:#888;}QPushButton:hover{background:#e74c3c;color:#fff;}");
    headerRow->addWidget(btnDelete);
    outerLayout->addLayout(headerRow);

    // ── 关键词行 ──
    auto *keywordRow = new QHBoxLayout();
    auto *kwLabel = new ElaText(card.area);
    kwLabel->setText("关键词");
    kwLabel->setFont(QFont(kwLabel->font().family(), 12));
    kwLabel->setMinimumWidth(50);
    keywordRow->addWidget(kwLabel);
    card.keywordEdit = new QLineEdit(card.area);
    card.keywordEdit->setText(keyword);
    card.keywordEdit->setPlaceholderText("例如：打开记事本");
    keywordRow->addWidget(card.keywordEdit, 1);
    outerLayout->addLayout(keywordRow);

    // ── 路径行 ──
    auto *pathRow = new QHBoxLayout();
    auto *pLabel = new ElaText(card.area);
    pLabel->setText("路  径");
    pLabel->setFont(QFont(pLabel->font().family(), 12));
    pLabel->setMinimumWidth(50);
    pathRow->addWidget(pLabel);
    card.pathEdit = new QLineEdit(card.area);
    card.pathEdit->setText(path);
    card.pathEdit->setPlaceholderText("C:\\...\\app.exe");
    pathRow->addWidget(card.pathEdit, 1);
    auto *btnBrowse = new QPushButton("浏览", card.area);
    btnBrowse->setMinimumWidth(60);
    pathRow->addWidget(btnBrowse);
    outerLayout->addLayout(pathRow);

    m_cards.append(card);
    ui->verticalLayout_Cards->addWidget(card.area);

    // ── 连接信号 ──
    auto saveLambda = [this]() { saveAll(); };
    connect(card.keywordEdit, &QLineEdit::textChanged, this, saveLambda);
    connect(card.pathEdit, &QLineEdit::textChanged, this, saveLambda);

    connect(btnBrowse, &QPushButton::clicked, this, [this, index]() {
        if (index >= m_cards.size())
            return;
        const QString p =
            QFileDialog::getOpenFileName(this, "选择应用或文件", QString(),
                                         "所有文件 (*.*);;可执行文件 (*.exe);;快捷方式 (*.lnk)");
        if (!p.isEmpty() && index < m_cards.size())
        {
            m_cards[index].pathEdit->setText(p);
            saveAll();
        }
    });

    connect(btnDelete, &QPushButton::clicked, this, [this, index]() {
        if (index < m_cards.size())
        {
            removeCard(index);
            saveAll();
        }
    });
}

void SettingChild_AppLauncher::removeCard(int index)
{
    if (index < 0 || index >= m_cards.size())
        return;

    AppCommandCard &card = m_cards[index];
    ui->verticalLayout_Cards->removeWidget(card.area);
    card.area->deleteLater();
    m_cards.removeAt(index);

    // 重新编号：遍历每个卡片，找到 header 里的 QLabel 更新
    for (int i = 0; i < m_cards.size(); ++i)
    {
        auto *areaLayout = m_cards[i].area->layout();
        if (!areaLayout || areaLayout->count() < 1)
            continue;
        // 第一项是 headerRow (QHBoxLayout)
        auto *item = areaLayout->itemAt(0);
        if (!item)
            continue;
        auto *headerLayout = item->layout();
        if (!headerLayout || headerLayout->count() < 1)
            continue;
        auto *label = qobject_cast<QLabel *>(headerLayout->itemAt(0)->widget());
        if (label)
            label->setText(QString("应用 #%1").arg(i + 1));
    }
}

void SettingChild_AppLauncher::saveAll()
{
    QJsonArray commands;
    for (const auto &card : m_cards)
    {
        const QString kw = card.keywordEdit->text().trimmed();
        const QString pt = card.pathEdit->text().trimmed();
        if (kw.isEmpty() && pt.isEmpty())
            continue;

        QJsonObject obj;
        obj["keyword"] = kw;
        obj["path"] = pt;
        commands.append(obj);
    }

    ZcJsonLib config(JsonSettingPath);
    config.setValue("appLauncher/commands", commands);

    emit appLauncherConfigChanged();
}

void SettingChild_AppLauncher::on_pushButton_Add_clicked()
{
    addCard();
    saveAll();
}
