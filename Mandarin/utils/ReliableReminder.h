#pragma once
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QUuid>
#include <algorithm>
#include <optional>

struct ReminderDeliveryGate {
    bool audioExpected = false;
    bool audioEnded = false;
    bool failed = false;
    std::optional<bool> completion(bool pipelineIdle) const {
        if (!pipelineIdle) return std::nullopt;
        if (failed) return false;
        if (audioExpected && !audioEnded) return std::nullopt;
        return true;
    }
};

struct ReliableReminder {
    QString id;
    QDateTime time;
    QString text;
    int repeatSec = 0;
    bool triggered = false;
    QString attemptId;
    int failures = 0;
    QDateTime retryAt;
    bool due(const QDateTime &now) const {
        return !triggered && attemptId.isEmpty() && time <= now &&
               (!retryAt.isValid() || retryAt <= now);
    }
    QString claim() {
        attemptId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        return attemptId;
    }
    bool fail(const QString &attempt, const QDateTime &now) {
        if (attempt.isEmpty() || attempt != attemptId) return false;
        attemptId.clear();
        failures = std::min(failures + 1, 10);
        retryAt = now.addSecs(std::min(300, 5 * (1 << std::min(failures - 1, 6))));
        return true;
    }
    bool deliver(const QString &attempt, const QDateTime &now) {
        if (attempt.isEmpty() || attempt != attemptId) return false;
        attemptId.clear(); failures = 0; retryAt = {};
        if (repeatSec <= 0) triggered = true;
        else if (time <= now) time = time.addSecs((time.secsTo(now) / repeatSec + 1) * qint64(repeatSec));
        return true;
    }
    QJsonObject json() const {
        return {{"id", id}, {"time", time.toString(Qt::ISODate)}, {"text", text},
                {"repeat", repeatSec}, {"triggered", triggered}, {"attempt", attemptId},
                {"failures", failures}, {"retry_at", retryAt.toString(Qt::ISODate)}};
    }
    static ReliableReminder fromJson(const QJsonObject &object) {
        ReliableReminder result;
        result.id = object.value("id").toString();
        result.time = QDateTime::fromString(object.value("time").toString(), Qt::ISODate);
        result.text = object.value("text").toString();
        result.repeatSec = qMax(0, object.value("repeat").toInt());
        result.triggered = object.value("triggered").toBool();
        result.failures = std::clamp(object.value("failures").toInt(), 0, 10);
        result.retryAt = QDateTime::fromString(object.value("retry_at").toString(), Qt::ISODate);
        // A previous process cannot still own a request; retain the reminder for retry.
        return result;
    }
};

inline bool decodeReminders(const QJsonObject &data, QList<ReliableReminder> &result,
                           QString *error = nullptr)
{
    if (data.contains("schedules") && !data.value("schedules").isArray()) {
        if (error) *error = QStringLiteral("日程列表格式错误，原文件已保留");
        return false;
    }
    QList<ReliableReminder> candidate;
    for (const auto &value : data.value("schedules").toArray()) {
        const auto reminder = ReliableReminder::fromJson(value.toObject());
        if (!value.isObject() || !reminder.time.isValid() || reminder.text.trimmed().isEmpty()) {
            if (error) *error = QStringLiteral("日程条目格式错误，原文件已保留");
            return false;
        }
        candidate.append(reminder);
    }
    result = candidate;
    return true;
}
