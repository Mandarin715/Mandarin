#include "ChatLogStore.h"

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUuid>

ChatLogStore::ChatLogStore(const QString &logPath, QObject *parent)
    : QObject(parent), m_logPath(logPath)
{
}

/*追加一条消息：即时写一行 JSON（无防抖，崩溃最多丢半行）*/
void ChatLogStore::appendMessage(const QString &role, const QString &content,
                                 const QJsonObject &meta)
{
    QDir().mkpath(QFileInfo(m_logPath).absolutePath());

    QJsonObject obj;
    obj["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    obj["role"] = role;
    obj["content"] = content;
    // 带时区偏移的 ISO8601（用于排序与日期合成）
    const QDateTime now = QDateTime::currentDateTime();
    obj["time"] = now.toOffsetFromUtc(now.offsetFromUtc()).toString(Qt::ISODate);
    if (!meta.isEmpty())
        obj["meta"] = meta;

    QFile file(m_logPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    file.write("\n");
}

/*读取全部消息（坏行跳过并告警）*/
QJsonArray ChatLogStore::loadMessages() const
{
    QJsonArray messages;
    QFile file(m_logPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return messages;

    while (!file.atEnd())
    {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty())
            continue;
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject())
        {
            qWarning() << "ChatLogStore: skipping bad line:" << err.errorString();
            continue;
        }
        messages.append(doc.object());
    }
    return messages;
}

bool ChatLogStore::exists() const
{
    return QFile::exists(m_logPath);
}

/*整文件重写（回退/删除后同步持久化）：临时文件 + rename 原子替换*/
void ChatLogStore::rewrite(const QJsonArray &messages)
{
    QDir().mkpath(QFileInfo(m_logPath).absolutePath());
    const QString tmpPath = m_logPath + QStringLiteral(".tmp");
    QFile out(tmpPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    for (const QJsonValue &v : messages)
    {
        if (!v.isObject())
            continue;
        out.write(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
        out.write("\n");
    }
    out.close();
    QFile::remove(m_logPath);
    QFile::rename(tmpPath, m_logPath);
}

QString ChatLogStore::logPath() const
{
    return m_logPath;
}

/*一次性迁移旧 context.json（history 行 → chat.jsonl）*/
QJsonArray ChatLogStore::migrateFromLegacy(const QString &legacyContextPath,
                                           const QString &logPath)
{
    if (QFile::exists(logPath))
        return QJsonArray(); // 已迁移过，拒绝覆盖

    QJsonArray messages;
    QFile in(legacyContextPath);
    if (!in.open(QIODevice::ReadOnly | QIODevice::Text))
        return messages;

    const QJsonDocument doc = QJsonDocument::fromJson(in.readAll());
    const QJsonArray history = doc.object().value("history").toArray();

    // 时间基准：文件修改时间；"[M月d日]" 日期行更新日期（年份取基准年份）
    QDateTime current = QFileInfo(legacyContextPath).lastModified();
    const QRegularExpression dateRe(
        QStringLiteral("^\\[(\\d{1,2})月(\\d{1,2})日\\]$"));

    for (const QJsonValue &v : history)
    {
        const QString line = v.toString().trimmed();
        if (line.isEmpty())
            continue;

        // 日期标记行：只更新日期基准，不落为消息
        const QRegularExpressionMatch dm = dateRe.match(line);
        if (dm.hasMatch())
        {
            const QDate d(current.date().year(), dm.captured(1).toInt(),
                          dm.captured(2).toInt());
            if (d.isValid())
                current = QDateTime(d, current.time());
            continue;
        }

        QString role;
        QString content = line;
        QJsonObject meta;
        if (line.startsWith(QStringLiteral("用户：")))
        {
            role = QStringLiteral("user");
            content = line.mid(3);
        }
        else if (line.startsWith(QStringLiteral("角色：[对话摘要] ")))
        {
            role = QStringLiteral("assistant");
            content = line.mid(QStringLiteral("角色：[对话摘要] ").size());
            meta["summary"] = true;
        }
        else if (line.startsWith(QStringLiteral("角色：")))
        {
            role = QStringLiteral("assistant");
            content = line.mid(3);
        }
        else
        {
            continue; // 无法识别的行，跳过
        }
        if (content.trimmed().isEmpty())
            continue;

        QJsonObject msg;
        msg["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
        msg["role"] = role;
        msg["content"] = content;
        msg["time"] =
            current.toOffsetFromUtc(current.offsetFromUtc()).toString(Qt::ISODate);
        if (!meta.isEmpty())
            msg["meta"] = meta;
        messages.append(msg);

        // 消息间递增 1 秒，保留顺序
        current = current.addSecs(1);
    }

    // 写出 chat.jsonl
    if (!messages.isEmpty())
    {
        QDir().mkpath(QFileInfo(logPath).absolutePath());
        QFile out(logPath);
        if (out.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            for (const QJsonValue &v : messages)
            {
                out.write(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
                out.write("\n");
            }
        }
    }
    return messages;
}
