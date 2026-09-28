#include "Live2DModelInfo.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

bool Live2DModelInfo::parse(const QByteArray &model3Json, const QByteArray &cdi3Json,
                            QString *error)
{
    m_mocPath.clear();
    m_physicsPath.clear();
    m_displayInfoPath.clear();
    m_texturePaths.clear();
    m_groups.clear();
    m_paramNames.clear();

    QJsonParseError parseError{};
    const QJsonDocument modelDoc = QJsonDocument::fromJson(model3Json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !modelDoc.isObject())
    {
        if (error)
            *error = QStringLiteral("model3.json 解析失败：%1").arg(parseError.errorString());
        return false;
    }

    const QJsonObject root = modelDoc.object();
    const QJsonObject refs = root.value(QStringLiteral("FileReferences")).toObject();

    m_mocPath = refs.value(QStringLiteral("Moc")).toString();
    if (m_mocPath.isEmpty())
    {
        if (error)
            *error = QStringLiteral("model3.json 缺少 FileReferences.Moc");
        return false;
    }
    m_physicsPath = refs.value(QStringLiteral("Physics")).toString();
    m_displayInfoPath = refs.value(QStringLiteral("DisplayInfo")).toString();

    const QJsonArray textures = refs.value(QStringLiteral("Textures")).toArray();
    for (const QJsonValue &texture : textures)
        m_texturePaths.append(texture.toString());

    const QJsonArray groups = root.value(QStringLiteral("Groups")).toArray();
    for (const QJsonValue &groupValue : groups)
    {
        const QJsonObject group = groupValue.toObject();
        const QString name = group.value(QStringLiteral("Name")).toString();
        if (name.isEmpty())
            continue;
        QStringList ids;
        const QJsonArray idArray = group.value(QStringLiteral("Ids")).toArray();
        for (const QJsonValue &id : idArray)
            ids.append(id.toString());
        m_groups.insert(name, ids);
    }

    if (!cdi3Json.isEmpty())
    {
        const QJsonDocument cdiDoc = QJsonDocument::fromJson(cdi3Json);
        const QJsonArray params = cdiDoc.object().value(QStringLiteral("Parameters")).toArray();
        for (const QJsonValue &paramValue : params)
        {
            const QJsonObject param = paramValue.toObject();
            const QString id = param.value(QStringLiteral("Id")).toString();
            if (!id.isEmpty())
                m_paramNames.insert(id, param.value(QStringLiteral("Name")).toString());
        }
    }
    return true;
}

QStringList Live2DModelInfo::groupIds(const QString &groupName) const
{
    return m_groups.value(groupName);
}

QString Live2DModelInfo::expressionParamId(const QByteArray &exp3Json)
{
    const QJsonDocument doc = QJsonDocument::fromJson(exp3Json);
    if (!doc.isObject())
        return QString();
    const QJsonArray params = doc.object().value(QStringLiteral("Parameters")).toArray();
    if (params.isEmpty())
        return QString();
    return params.first().toObject().value(QStringLiteral("Id")).toString();
}

QString Live2DModelInfo::fingerprint(const QByteArray &moc3Bytes)
{
    return QStringLiteral("sha256:")
           + QString::fromLatin1(
               QCryptographicHash::hash(moc3Bytes, QCryptographicHash::Sha256).toHex());
}
