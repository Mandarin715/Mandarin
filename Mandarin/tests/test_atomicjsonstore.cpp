#include <QtTest>
#include <QTemporaryDir>
#include <QScopeGuard>
#include "../utils/AtomicJsonStore.h"
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

class TestAtomicJsonStore : public QObject {
    Q_OBJECT
private slots:
    void malformedReadPreservesOutput();
    void failedCommitPreservesOriginal();
};

void TestAtomicJsonStore::malformedReadPreservesOutput()
{
    QTemporaryDir dir;
    const auto path = dir.filePath("data.json");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("{interrupted");
    file.close();
    QJsonObject result{{"cached", 42}};
    QString error;
    QVERIFY(!AtomicJsonStore::read(path, result, &error));
    QCOMPARE(result, QJsonObject({{"cached", 42}}));
    QVERIFY(!error.isEmpty());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("{interrupted"));
}

void TestAtomicJsonStore::failedCommitPreservesOriginal()
{
#ifdef Q_OS_WIN
    QTemporaryDir dir;
    const auto path = dir.filePath("data.json");
    QVERIFY(AtomicJsonStore::write(path, {{"saved", 1}}));
    const auto native = QDir::toNativeSeparators(path);
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), GENERIC_READ,
                               FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    QVERIFY(handle != INVALID_HANDLE_VALUE);
    const auto release = qScopeGuard([handle] { CloseHandle(handle); });
    QString error;
    QVERIFY(!AtomicJsonStore::write(path, {{"saved", 2}}, &error));
    QVERIFY(!error.isEmpty());
    QJsonObject stored;
    QVERIFY(AtomicJsonStore::read(path, stored));
    QCOMPARE(stored, QJsonObject({{"saved", 1}}));
#else
    QSKIP("Windows file-sharing regression");
#endif
}

QTEST_GUILESS_MAIN(TestAtomicJsonStore)
#include "test_atomicjsonstore.moc"
