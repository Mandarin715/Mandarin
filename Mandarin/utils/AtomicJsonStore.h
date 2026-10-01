#pragma once
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace AtomicJsonStore {
inline bool read(const QString &path, QJsonObject &result, QString *error = nullptr)
{
    QFile file(path);
    if (!file.exists()) { result = {}; return true; }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    QJsonParseError parseError;
    const auto bytes = file.readAll();
    if (file.error() != QFile::NoError) {
        if (error) *error = file.errorString();
        return false;
    }
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (!document.isObject() || parseError.error != QJsonParseError::NoError) {
        if (error) *error = QStringLiteral("数据文件格式错误：%1").arg(path);
        return false;
    }
    result = document.object();
    return true;
}
inline bool write(const QString &path, const QJsonObject &value, QString *error = nullptr)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error) *error = QStringLiteral("无法创建数据目录");
        return false;
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size()) {
        if (error) *error = file.errorString();
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}
}
