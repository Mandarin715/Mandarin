#include <QtTest>

#include "../utils/ChatLogStore.h"

#include <QDate>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

class TestChatLogStore : public QObject
{
    Q_OBJECT

  private slots:
    void appendThenLoadRoundTrips();
    void loadSkipsBadLines();
    void migrateParsesLegacyFormat();
    void migrateDropsDateMarkers();
    void rewriteReplacesContent();
};

/*追加后读回一致（role/content/time/id/meta）*/
void TestChatLogStore::appendThenLoadRoundTrips()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("chat.jsonl");

    ChatLogStore store(path);
    store.appendMessage("user", QStringLiteral("今天天气好热啊"));
    store.appendMessage("assistant", QStringLiteral("开心|是啊|そうだね"),
                        QJsonObject{{QStringLiteral("mood"), QStringLiteral("开心")}});

    const QJsonArray msgs = store.loadMessages();
    QCOMPARE(msgs.size(), 2);
    QCOMPARE(msgs[0].toObject().value("role").toString(), QStringLiteral("user"));
    QCOMPARE(msgs[0].toObject().value("content").toString(),
             QStringLiteral("今天天气好热啊"));
    QVERIFY(!msgs[0].toObject().value("id").toString().isEmpty());
    QVERIFY(!msgs[0].toObject().value("time").toString().isEmpty());
    QCOMPARE(msgs[1].toObject().value("role").toString(), QStringLiteral("assistant"));
    QCOMPARE(msgs[1].toObject().value("meta").toObject().value("mood").toString(),
             QStringLiteral("开心"));
}

/*坏行跳过，不崩，其余行照常读回*/
void TestChatLogStore::loadSkipsBadLines()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("chat.jsonl");

    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write("{\"id\":\"m1\",\"role\":\"user\",\"content\":\"好\",\"time\":\"2026-08-31T19:00:00+08:00\"}\n");
    f.write(QStringLiteral("这不是JSON\n").toUtf8());
    f.write("{\"id\":\"m2\",\"role\":\"assistant\",\"content\":\"hi\",\"time\":\"2026-08-31T19:00:01+08:00\"}\n");
    f.close();

    const QJsonArray msgs = ChatLogStore(path).loadMessages();
    QCOMPARE(msgs.size(), 2);
    QCOMPARE(msgs[1].toObject().value("content").toString(), QStringLiteral("hi"));
}

/*迁移：正确解析 用户：/角色：/[对话摘要]，日期行驱动 time，且写出文件*/
void TestChatLogStore::migrateParsesLegacyFormat()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString legacy = dir.filePath("context.json");
    QFile f(legacy);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write("{\"history\": [\"用户：你好\", \"角色：你好呀\", "
            "\"角色：[对话摘要] 一段摘要\", \"[8月7日]\", \"用户：在干嘛\"]}");
    f.close();

    const QString log = dir.filePath("chat.jsonl");
    const QJsonArray msgs = ChatLogStore::migrateFromLegacy(legacy, log);

    QCOMPARE(msgs.size(), 4); // 日期行不落为消息
    QCOMPARE(msgs[0].toObject().value("role").toString(), QStringLiteral("user"));
    QCOMPARE(msgs[0].toObject().value("content").toString(), QStringLiteral("你好"));
    QCOMPARE(msgs[1].toObject().value("role").toString(), QStringLiteral("assistant"));
    QCOMPARE(msgs[2].toObject().value("meta").toObject().value("summary").toBool(), true);
    QCOMPARE(msgs[3].toObject().value("content").toString(), QStringLiteral("在干嘛"));

    // 日期行后的消息 time 应落在 8月7日（年份取文件修改时间）
    const QString year = QString::number(QDate::currentDate().year());
    QVERIFY(msgs[3].toObject().value("time").toString().startsWith(year + "-08-07"));

    // 迁移产物已写出
    QVERIFY(QFile::exists(log));
    QCOMPARE(ChatLogStore(log).loadMessages().size(), 4);
}

/*迁移：日期标记不影响消息条数，且写入的 jsonl 可被正常读回*/
void TestChatLogStore::migrateDropsDateMarkers()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString legacy = dir.filePath("context.json");
    QFile f(legacy);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write("{\"history\": [\"用户：好\", \"[8月15日]\", \"角色：嗨\"]}");
    f.close();

    const QString log = dir.filePath("chat.jsonl");
    const QJsonArray msgs = ChatLogStore::migrateFromLegacy(legacy, log);
    QCOMPARE(msgs.size(), 2);
    const QString year = QString::number(QDate::currentDate().year());
    QVERIFY(msgs[1].toObject().value("time").toString().startsWith(year + "-08-15"));
}

/*重写：整文件替换为给定消息（回退/删除后同步持久化）*/
void TestChatLogStore::rewriteReplacesContent()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("chat.jsonl");

    ChatLogStore store(path);
    store.appendMessage("user", QStringLiteral("第一条"));
    store.appendMessage("assistant", QStringLiteral("第二条"));
    store.appendMessage("user", QStringLiteral("第三条"));

    // 模拟回退：只保留前两条
    QJsonArray kept;
    const QJsonArray all = store.loadMessages();
    kept.append(all[0]);
    kept.append(all[1]);
    store.rewrite(kept);

    const QJsonArray msgs = store.loadMessages();
    QCOMPARE(msgs.size(), 2);
    QCOMPARE(msgs[0].toObject().value("content").toString(), QStringLiteral("第一条"));
    QCOMPARE(msgs[1].toObject().value("content").toString(), QStringLiteral("第二条"));
}

QTEST_MAIN(TestChatLogStore)
#include "test_chatlogstore.moc"
