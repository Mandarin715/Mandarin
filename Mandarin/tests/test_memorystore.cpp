#include <QtTest>
#include <QTemporaryDir>
#include <QScopeGuard>
#include "../utils/MemoryStore.h"
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

class TestMemoryStore : public QObject {
    Q_OBJECT
private slots:
    void corruptDataIsNotOverwritten();
    void failedCommitPreservesOriginal();
    void memoryIdsPersistAndDeletionSurvivesReordering();
    void staleCharacterCannotDeleteMemory();
    void extractionMergesLatestFileWithoutResurrectingDeletedEntries();
};

void TestMemoryStore::corruptDataIsNotOverwritten()
{
    QTemporaryDir dir;
    const auto path = dir.filePath("memory.json");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("{interrupted"); file.close();
    QJsonObject result{{"cached", true}};
    QString error;
    QVERIFY(!MemoryStore::load(path, result, &error));
    QCOMPARE(result.value("cached").toBool(), true);
    QVERIFY(!MemoryStore::remove(path, {}, true, &error));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("{interrupted"));
    file.close();
    const QJsonObject malformed{{"help_summaries", "broken"}};
    QVERIFY(AtomicJsonStore::write(path, malformed));
    QVERIFY(!MemoryStore::remove(path, {}, true, &error));
    QJsonObject preserved;
    QVERIFY(AtomicJsonStore::read(path, preserved));
    QCOMPARE(preserved, malformed);

}
void TestMemoryStore::failedCommitPreservesOriginal()
{
#ifdef Q_OS_WIN
    QTemporaryDir dir;
    const auto path = dir.filePath("memory.json");
    QVERIFY(AtomicJsonStore::write(path, {{"saved", 1}}));
    const auto native = QDir::toNativeSeparators(path);
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), GENERIC_READ,
                               FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    QVERIFY(handle != INVALID_HANDLE_VALUE);
    const auto release = qScopeGuard([handle] { CloseHandle(handle); });
    QString error;
    QVERIFY(!AtomicJsonStore::write(path, {{"saved", 2}}, &error));
    QVERIFY(!error.isEmpty());
    QJsonObject cached{{"cached", 42}};
    QVERIFY(!MemoryStore::mergeExtraction(path, {{"is_help", true}, {"help_summary", "blocked"}},
                                         "2026-10-01", cached, &error));
    QCOMPARE(cached.value("cached").toInt(), 42);
    QJsonObject stored;
    QVERIFY(AtomicJsonStore::read(path, stored));
    QCOMPARE(stored.value("saved").toInt(), 1);
#else
    QSKIP("Windows file-sharing regression");
#endif
}
void TestMemoryStore::memoryIdsPersistAndDeletionSurvivesReordering()
{
    QTemporaryDir dir;
    const auto path = dir.filePath("memory.json");
    QJsonArray entries{QJsonObject{{"summary", "first"}}, QJsonObject{{"summary", "second"}}};
    QVERIFY(AtomicJsonStore::write(path, {{"help_summaries", entries}, {"personal_info", QJsonObject{{"name", "user"}}}}));
    QJsonObject data;
    QVERIFY(MemoryStore::load(path, data));
    const auto id = data["help_summaries"].toArray().first().toObject()["id"].toString();
    QVERIFY(!QUuid(id).isNull());
    QJsonObject reloaded;
    QVERIFY(MemoryStore::load(path, reloaded));
    QCOMPARE(data, reloaded);
    auto shuffled = data["help_summaries"].toArray();
    shuffled.prepend(shuffled.takeAt(1));
    data["help_summaries"] = shuffled;
    QVERIFY(AtomicJsonStore::write(path, data));
    QVERIFY(MemoryStore::removeForCharacter(path, path, id, false));
    QVERIFY(MemoryStore::load(path, data));
    QCOMPARE(data["help_summaries"].toArray().size(), 1);
    QCOMPARE(data["help_summaries"].toArray().first().toObject()["summary"].toString(), QString("second"));
    QCOMPARE(data["personal_info"].toObject()["name"].toString(), QString("user"));
    QVERIFY(!MemoryStore::remove(path, id, false));
}
void TestMemoryStore::staleCharacterCannotDeleteMemory()
{
    QTemporaryDir dir;
    const auto oldPath = dir.filePath("atri/memory.json"), newPath = dir.filePath("miku/memory.json");
    const QJsonObject data{{"help_summaries", QJsonArray{QJsonObject{{"id", "same-id"}, {"summary", "keep"}}}}};
    QVERIFY(AtomicJsonStore::write(oldPath, data));
    QVERIFY(AtomicJsonStore::write(newPath, data));
    QVERIFY(!MemoryStore::removeForCharacter(oldPath, newPath, "same-id", false));
    QVERIFY(!MemoryStore::removeForCharacter(oldPath, newPath, {}, true));
    QJsonObject a,b;
    QVERIFY(AtomicJsonStore::read(oldPath, a)); QVERIFY(AtomicJsonStore::read(newPath, b));
    QCOMPARE(a,data); QCOMPARE(b,data);
}
void TestMemoryStore::extractionMergesLatestFileWithoutResurrectingDeletedEntries()
{
    QTemporaryDir dir;
    const auto path = dir.filePath("memory.json");
    QVERIFY(AtomicJsonStore::write(path, {{"help_summaries", QJsonArray{QJsonObject{{"id", "deleted"}, {"summary", "old"}}}}}));
    QVERIFY(MemoryStore::remove(path, "deleted", false));
    QJsonObject committed;
    QVERIFY(MemoryStore::mergeExtraction(path, {{"is_help", true}, {"help_summary", "new"}}, "2026-10-01", committed));
    QCOMPARE(committed["help_summaries"].toArray().size(),1);
    QCOMPARE(committed["help_summaries"].toArray().first().toObject()["summary"].toString(), QString("new"));
    QVERIFY(MemoryStore::mergeExtraction(path, {{"is_help", true}, {"help_summary", "new"}}, "2026-10-01", committed));
    QCOMPARE(committed["help_summaries"].toArray().size(),1);
}

QTEST_GUILESS_MAIN(TestMemoryStore)
#include "test_memorystore.moc"
