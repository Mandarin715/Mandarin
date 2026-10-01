#include <QtTest>
#include <QTemporaryDir>
#include "../utils/AtomicJsonStore.h"
#include "../utils/ReliableReminder.h"

class TestReliability : public QObject {
    Q_OBJECT
private slots:
    void invalidSchedulePreservesCache();
    void remindersRetryAndRejectLateAcknowledgements();
    void restartRecoversUnfinishedReminder();
    void recurringReminderAdvancesOnlyAfterDelivery();
    void playbackStopAloneDoesNotAcknowledgeReminder();
};

void TestReliability::invalidSchedulePreservesCache()
{
    ReliableReminder cached;
    cached.id = "cached";
    QList<ReliableReminder> schedules{cached};
    QString error;
    QVERIFY(!decodeReminders({{"schedules", QJsonArray{QJsonObject{{"time", "invalid"}}}}}, schedules, &error));
    QCOMPARE(schedules.size(), 1);
    QCOMPARE(schedules.first().id, QString("cached"));
}

void TestReliability::remindersRetryAndRejectLateAcknowledgements()
{
    const auto now = QDateTime(QDate(2026,10,1), QTime(9,0), Qt::UTC);
    ReliableReminder reminder;
    reminder.id="reminder"; reminder.time=now.addSecs(-60); reminder.text="drink water";
    QVERIFY(reminder.due(now));
    const auto first = reminder.claim();
    QVERIFY(!reminder.due(now));
    QVERIFY(reminder.fail(first,now));
    QVERIFY(!reminder.due(now.addSecs(4)));
    QVERIFY(reminder.due(now.addSecs(5)));
    const auto second=reminder.claim();
    QVERIFY(first != second);
    QVERIFY(!reminder.deliver(first,now));
    QVERIFY(!reminder.fail(first,now));
    QVERIFY(reminder.deliver(second,now));
    QVERIFY(reminder.triggered);
    QVERIFY(!reminder.due(now.addDays(1)));
    QVERIFY(!reminder.deliver(second,now));
}
void TestReliability::restartRecoversUnfinishedReminder()
{
    QTemporaryDir dir;
    const auto now=QDateTime(QDate(2026,10,1), QTime(9,0), Qt::UTC);
    ReliableReminder reminder;
    reminder.id="persistent"; reminder.time=now.addDays(-2); reminder.text="overdue";
    const auto oldAttempt=reminder.claim();
    const auto path=dir.filePath("schedules.json");
    QVERIFY(AtomicJsonStore::write(path, {{"schedules", QJsonArray{reminder.json()}}}));
    QJsonObject loaded;
    QVERIFY(AtomicJsonStore::read(path,loaded));
    auto restored=ReliableReminder::fromJson(loaded["schedules"].toArray().first().toObject());
    QCOMPARE(restored.id,reminder.id);
    QVERIFY(restored.due(now));
    const auto newAttempt=restored.claim();
    QVERIFY(!restored.deliver(oldAttempt,now));
    QVERIFY(restored.deliver(newAttempt,now));
}
void TestReliability::recurringReminderAdvancesOnlyAfterDelivery()
{
    const auto now=QDateTime(QDate(2026,10,1), QTime(9,0), Qt::UTC);
    ReliableReminder reminder;
    reminder.time=now.addDays(-500); reminder.repeatSec=24*3600;
    const auto original=reminder.time;
    const auto first=reminder.claim();
    QVERIFY(reminder.fail(first,now));
    QCOMPARE(reminder.time,original);
    const auto second=reminder.claim();
    QVERIFY(reminder.deliver(second,now));
    QCOMPARE(reminder.time,now.addDays(1));
    QVERIFY(!reminder.triggered);
    QVERIFY(!reminder.due(now));
}
void TestReliability::playbackStopAloneDoesNotAcknowledgeReminder()
{
    ReminderDeliveryGate delivery;
    delivery.audioExpected = true;
    QVERIFY(!delivery.completion(false).has_value()); // Synthesis is still running.
    QVERIFY(!delivery.completion(true).has_value()); // Stopped without EndOfMedia.
    delivery.audioEnded = true;
    QVERIFY(!delivery.completion(false).has_value()); // Another sentence is pending.
    QCOMPARE(delivery.completion(true).value(), true);
    delivery.failed = true;
    QCOMPARE(delivery.completion(true).value(), false); // A failed sentence prevents ack.
    ReminderDeliveryGate textOnly;
    QCOMPARE(textOnly.completion(true).value(), true);
}

QTEST_GUILESS_MAIN(TestReliability)
#include "test_reliability.moc"
