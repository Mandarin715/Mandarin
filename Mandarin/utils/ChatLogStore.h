#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>

/// 单角色聊天日志存储（JSONL 追加式持久化）
///
/// 每条消息一行 JSON，提交即落盘（不依赖防抖），崩溃最多丢半行。
/// 负责：追加消息、读取（坏行跳过）、旧版 context.json 一次性迁移。
class ChatLogStore : public QObject
{
    Q_OBJECT

  public:
    /// @param logPath chat.jsonl 的完整路径
    explicit ChatLogStore(const QString &logPath, QObject *parent = nullptr);

    /// 追加一条消息（role: "user" / "assistant"），即时写一行 JSON
    void appendMessage(const QString &role, const QString &content,
                       const QJsonObject &meta = QJsonObject());

    /// 读取全部消息（按行序；坏行跳过并 qWarning），返回 QJsonArray
    QJsonArray loadMessages() const;

    /// 整文件重写为给定消息（用于回退/删除后同步持久化；原子写：临时文件 + rename）
    void rewrite(const QJsonArray &messages);

    /// chat.jsonl 是否已存在
    bool exists() const;

    QString logPath() const;

    /// 一次性迁移：解析旧 context.json 的 history 行，写入 logPath。
    /// - "用户：xxx" / "角色：xxx" / "角色：[对话摘要] xxx" / "[M月d日]" 日期行
    /// - 日期行驱动消息 time 的日期（年份取文件修改时间），消息间递增 1 秒保序
    /// - 若 logPath 已存在则拒绝（返回空数组）
    static QJsonArray migrateFromLegacy(const QString &legacyContextPath,
                                        const QString &logPath);

  private:
    QString m_logPath;
};
