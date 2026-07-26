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

    // 外层卡片
    card.area = new ElaScrollPageArea(ui->scrollContent);
    card.area->setToolTip("输入含关键词的文本时自动启动对应应用");

    auto *outerLayout = new QVBoxLayout(card.area);
    outerLayout->setContentsMargins(14, 6, 14, 6);
    outerLayout->setSpacing(4);

    // 标签固定宽度，保证多卡片对齐
    constexpr int kLabelWidth = 55;

    // ── 关键词行：标签 + 输入框 + 删除按钮 ──
    auto *keywordRow = new QHBoxLayout();
    keywordRow->setSpacing(8);
    auto *kwLabel = new ElaText(card.area);
    kwLabel->setText("关键词");
    kwLabel->setFont(QFont(kwLabel->font().family(), 12));
    kwLabel->setMinimumWidth(kLabelWidth);
    keywordRow->addWidget(kwLabel);
    card.keywordEdit = new QLineEdit(card.area);
    card.keywordEdit->setText(keyword);
    card.keywordEdit->setPlaceholderText("例如：打开记事本");
    keywordRow->addWidget(card.keywordEdit, 1);
    auto *btnDelete = new QPushButton("✕", card.area);
    btnDelete->setFixedSize(28, 28);
    btnDelete->setStyleSheet(
        "QPushButton{background:#e0e0e0;border:none;border-radius:14px;"
        "font-size:14px;color:#888;}QPushButton:hover{background:#e74c3c;color:#fff;}");
    keywordRow->addWidget(btnDelete);
    outerLayout->addLayout(keywordRow);

    // ── 路径行：标签 + 输入框 + 浏览按钮 ──
    auto *pathRow = new QHBoxLayout();
    pathRow->setSpacing(8);
    auto *pLabel = new ElaText(card.area);
    pLabel->setText("路  径");
    pLabel->setFont(QFont(pLabel->font().family(), 12));
    pLabel->setMinimumWidth(kLabelWidth);
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

    connect(btnBrowse, &QPushButton::clicked, this, [this, area = card.area]() {
        // 按控件指针查找当前索引，避免删除前面卡片后索引错位
        for (int i = 0; i < m_cards.size(); ++i) {
            if (m_cards[i].area != area) continue;
            const QString p =
                QFileDialog::getOpenFileName(this, "选择应用或文件", QString(),
                                             "所有文件 (*.*);;可执行文件 (*.exe);;快捷方式 (*.lnk)");
            if (!p.isEmpty())
                m_cards[i].pathEdit->setText(p);
            saveAll();
            return;
        }
    });

    connect(btnDelete, &QPushButton::clicked, this, [this, area = card.area]() {
        for (int i = 0; i < m_cards.size(); ++i) {
            if (m_cards[i].area == area) {
                removeCard(i);
                saveAll();
                return;
            }
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
