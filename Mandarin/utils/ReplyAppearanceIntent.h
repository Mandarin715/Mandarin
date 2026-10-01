#pragma once

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>

namespace ReplyAppearanceIntent
{
inline QJsonObject requested(const QString &input)
{
    if (QRegularExpression(QStringLiteral("不要|不用|不想|不许|不换|不穿|不必|不需要|别|昨天|上次|以前|如果|喜欢|为什么|我穿|我换|[“”\"`]")).match(input).hasMatch())
        return {};
    QJsonObject result;
    const auto match = [&](const QString &names) {
        const QString verb = QStringLiteral("(?:换(?:上|成|回|一下)?|穿(?:上|回)?|恢复)");
        return QRegularExpression(verb + QStringLiteral("\\s*(?:你的|一套|一双|一件|件)?\\s*(?:") + names +
            QStringLiteral(")|(?:") + names + QStringLiteral(").{0,3}") + verb + QStringLiteral("(?:吧|呀|！|。|$)")).match(input).hasMatch();
    };
    if (match(QStringLiteral("睡衣"))) result["clothes"] = "pajamas";
    else if (match(QStringLiteral("比基尼|泳装|泳衣"))) result["clothes"] = "bikini";
    else if (match(QStringLiteral("南瓜裤"))) result["clothes"] = "shorts";
    else if (match(QStringLiteral("默认(?:服装|衣服|装扮)|默认(?!鞋)|原来(?:的衣服|的服装)?|平时(?:的衣服|的服装)?|日常衣服|校服|水手服"))) result["clothes"] = "default";
    if (match(QStringLiteral("凉鞋"))) result["shoes"] = "sandals";
    else if (match(QStringLiteral("鞋子|普通鞋"))) result["shoes"] = "shoes";
    if (QRegularExpression(QStringLiteral("脱(?:掉)?(?:鞋子|鞋)|恢复默认鞋|换回默认鞋")).match(input).hasMatch()) result["shoes"] = "default";
    return result;
}

inline QJsonObject parse(const QString &reply, const QString &input)
{
    const QJsonObject requestedValues = requested(input);
    if (requestedValues.isEmpty() || reply.count('|') != 4) return {};
    const auto doc = QJsonDocument::fromJson(reply.section('|', 4, 4).trimmed().toUtf8());
    if (!doc.isObject() || doc.object().size() != 1 || !doc.object().value("appearance").isObject()) return {};
    const auto values = doc.object().value("appearance").toObject();
    if (values.isEmpty() || values != requestedValues) return {};
    for (auto it = values.begin(); it != values.end(); ++it)
        if (!it.value().isString() || !requestedValues.contains(it.key()) || requestedValues.value(it.key()) != it.value()) return {};
    return values;
}

inline bool isShock(const QString &mood)
{
    return QStringList{QStringLiteral("震惊"), QStringLiteral("惊讶"), QStringLiteral("吃惊"),
                       QStringLiteral("惊愕"), QStringLiteral("惊呆"), QStringLiteral("惊吓")}.contains(mood.trimmed());
}

inline QString prompt(const QJsonObject &state)
{
    if (state.isEmpty()) return {};
    return QStringLiteral("\n当前实际装扮：") + QString::fromUtf8(QJsonDocument(state).toJson(QJsonDocument::Compact)) +
        QStringLiteral("\n仅用户明确要求你换装时执行；提及、回忆、否定、假设不换。未指定目标时先询问。"
                       "可用 clothes: default(默认服装), pajamas(睡衣), bikini(比基尼/泳衣), shorts(南瓜裤)；"
                       "shoes: default(默认/不穿鞋), sandals(凉鞋), shoes(普通鞋)。"
                       "执行时回复确认，并在原四段后追加第五段 JSON，例如："
                       "快乐|我换上睡衣啦！|パジャマに着替えました！||{\"appearance\":{\"clothes\":\"pajamas\"}}。"
                       "JSON 仅包含本次明确要求的字段，不重复未要求的字段。没有动作时不输出第五段，不输出其他道具动作。"
                       "震惊时心情使用震惊或惊讶，白眼由程序临时处理。\n");
}
}
