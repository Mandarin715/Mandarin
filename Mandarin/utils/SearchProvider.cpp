#include "SearchProvider.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>

SearchProvider::SearchProvider(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
}

SearchProvider::~SearchProvider()
{
    if (m_activeReply)
    {
        m_activeReply->abort();
        m_activeReply->deleteLater();
    }
}

void SearchProvider::setApiKey(const QString &key)
{
    m_apiKey = key.trimmed();
    // API Key 变更时使 token 失效
    m_accessToken.clear();
    m_tokenExpiry = QDateTime();
}

void SearchProvider::setSecretKey(const QString &key)
{
    m_secretKey = key.trimmed();
    m_accessToken.clear();
    m_tokenExpiry = QDateTime();
}

void SearchProvider::setBaseUrl(const QString &url)
{
    m_baseUrl = url.trimmed();
}

void SearchProvider::setEnabled(bool enabled)
{
    m_enabled = enabled;
}

bool SearchProvider::isEnabled() const
{
    return m_enabled;
}

// ─── OAuth 2.0 换取 access_token ───

void SearchProvider::requestAccessToken()
{
    if (m_apiKey.isEmpty() || m_secretKey.isEmpty())
    {
        emit searchFailed(QStringLiteral("百度搜索API需要同时配置API Key和Secret Key"));
        return;
    }

    QUrl url("https://aip.baidubce.com/oauth/2.0/token");
    QUrlQuery params;
    params.addQueryItem("grant_type", "client_credentials");
    params.addQueryItem("client_id", m_apiKey);
    params.addQueryItem("client_secret", m_secretKey);
    url.setQuery(params);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      "application/x-www-form-urlencoded");

    QNetworkReply *reply = m_network->post(request, url.query().toUtf8());

    connect(reply, &QNetworkReply::finished, this,
            &SearchProvider::onTokenReplyFinished);
}

void SearchProvider::onTokenReplyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply)
        return;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError)
    {
        m_accessToken.clear();
        emit searchFailed(
            QStringLiteral("获取百度access_token失败：%1").arg(reply->errorString()));
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    const QJsonObject obj = doc.object();

    m_accessToken = obj.value("access_token").toString();

    if (!m_accessToken.isEmpty())
    {
        // 缓存 token，提前 1 天过期以确保安全
        const int expiresIn = obj.value("expires_in").toInt(2592000);
        m_tokenExpiry = QDateTime::currentDateTime()
                            .addSecs(expiresIn - 86400);

        // token 获取成功，继续执行之前暂存的搜索
        if (!m_pendingQuery.isEmpty())
        {
            const QString query = m_pendingQuery;
            m_pendingQuery.clear();
            doSearch(query);
        }
    }
    else
    {
        const QString errorDesc =
            obj.value("error_description").toString(
                obj.value("error").toString("unknown"));
        emit searchFailed(
            QStringLiteral("百度access_token获取失败：%1").arg(errorDesc));
    }
}

// ─── 搜索请求 ───

static bool isBaiduQianfan(const QString &url)
{
    return url.contains("qianfan.baidubce.com");
}

void SearchProvider::search(const QString &query)
{
    if (!m_enabled)
    {
        emit searchFailed(QStringLiteral("联网搜索未启用"));
        return;
    }

    if (m_baseUrl.isEmpty())
    {
        emit searchFailed(QStringLiteral("搜索API地址未配置"));
        return;
    }

    // 百度千帆认证：
    // - 有 Secret Key → OAuth 2.0 获取 access_token（传统模式）
    // - 仅 API Key → 直接作为 AppBuilder API Key 使用（推荐）
    if (isBaiduQianfan(m_baseUrl))
    {
        if (!m_secretKey.isEmpty())
        {
            // OAuth 模式
            const bool tokenExpired =
                m_accessToken.isEmpty() ||
                m_tokenExpiry.isNull() ||
                QDateTime::currentDateTime() > m_tokenExpiry;

            if (tokenExpired)
            {
                m_pendingQuery = query;
                requestAccessToken();
                return;
            }
        }
        // else: 直接使用 API Key 作为 AppBuilder Key（无需 OAuth）
    }

    doSearch(query);
}

void SearchProvider::doSearch(const QString &query)
{
    // 取消进行中的请求
    if (m_activeReply)
    {
        m_activeReply->abort();
        m_activeReply->deleteLater();
        m_activeReply = nullptr;
    }

    if (isBaiduQianfan(m_baseUrl))
    {
        // ─── 百度千帆 AI 搜索 API ───
        // 优先使用 access_token（OAuth模式），否则直接用 API Key（AppBuilder模式）
        const QString authToken =
            m_accessToken.isEmpty() ? m_apiKey : m_accessToken;

        QUrl url(m_baseUrl);
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        request.setRawHeader("X-Appbuilder-Authorization",
                             ("Bearer " + authToken).toUtf8());

        QJsonObject body;
        QJsonArray messages;
        QJsonObject msg;
        msg["role"] = "user";
        msg["content"] = query;
        messages.append(msg);
        body["messages"] = messages;
        body["search_source"] = "baidu_search_v2";

        QJsonArray resourceFilter;
        QJsonObject webFilter;
        webFilter["type"] = "web";
        webFilter["top_k"] = 5;
        resourceFilter.append(webFilter);
        body["resource_type_filter"] = resourceFilter;

        m_activeReply = m_network->post(
            request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    }
    else
    {
        // ─── 通用 GET API（SearXNG 等） ───
        QUrl url(m_baseUrl);
        QUrlQuery urlQuery(url);
        urlQuery.addQueryItem("q", query);
        if (!urlQuery.hasQueryItem("format"))
            urlQuery.addQueryItem("format", "json");
        urlQuery.addQueryItem("language", "zh-CN");
        url.setQuery(urlQuery);

        QNetworkRequest request(url);
        request.setRawHeader("Accept", "application/json");

        if (!m_apiKey.isEmpty())
            request.setRawHeader("Authorization",
                                 ("Bearer " + m_apiKey).toUtf8());

        m_activeReply = m_network->get(request);
    }

    connect(m_activeReply, &QNetworkReply::finished, this,
            &SearchProvider::onSearchReplyFinished);
}

// ─── 搜索响应解析 ───

void SearchProvider::onSearchReplyFinished()
{
    if (!m_activeReply)
        return;

    QNetworkReply *reply = m_activeReply;
    m_activeReply = nullptr;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError)
    {
        const QString errorMsg = reply->errorString();
        emit searchFailed(QStringLiteral("搜索请求失败：%1").arg(errorMsg));
        return;
    }

    const QByteArray responseData = reply->readAll();
    const QJsonDocument doc = QJsonDocument::fromJson(responseData);

    if (!doc.isObject())
    {
        emit searchFailed(QStringLiteral("搜索响应格式错误"));
        return;
    }

    const QJsonObject root = doc.object();

    // 检查百度千帆错误
    if (root.contains("code") && root.contains("message"))
    {
        const QString errCode = root.value("code").toString();
        const QString errMsg = root.value("message").toString();
        // access_token 过期，重新获取
        if (errCode == "216003" || errCode.contains("Auth"))
        {
            m_accessToken.clear();
            m_tokenExpiry = QDateTime();
            emit searchFailed(
                QStringLiteral("百度搜索授权过期，请重新打开设置面板刷新token：%1")
                    .arg(errMsg));
            return;
        }
        emit searchFailed(QStringLiteral("搜索API错误[%1]：%2").arg(errCode, errMsg));
        return;
    }

    QJsonArray resultsArray;

    // 百度千帆：{ "references": [...] }
    if (root.contains("references"))
        resultsArray = root.value("references").toArray();
    // SearXNG / 自定义：{ "results": [...] }
    else if (root.contains("results"))
        resultsArray = root.value("results").toArray();
    // Bing：{ "webPages": { "value": [...] } }
    else if (root.contains("webPages"))
    {
        const QJsonObject webPages = root.value("webPages").toObject();
        resultsArray = webPages.value("value").toArray();
    }
    // SerpAPI：{ "organic_results": [...] }
    else if (root.contains("organic_results"))
        resultsArray = root.value("organic_results").toArray();
    // Google CSE：{ "items": [...] }
    else if (root.contains("items"))
        resultsArray = root.value("items").toArray();
    else
    {
        emit searchFailed(QStringLiteral("搜索响应格式不兼容"));
        return;
    }

    QList<SearchResult> searchResults;
    for (const QJsonValue &val : resultsArray)
    {
        const QJsonObject item = val.toObject();
        SearchResult r;
        r.title = item.value("title").toString();
        r.url = item.value("url").toString();
        if (r.url.isEmpty())
            r.url = item.value("link").toString();

        r.snippet = item.value("content").toString();       // 百度千帆 / SearXNG
        if (r.snippet.isEmpty())
            r.snippet = item.value("snippet").toString();   // 自定义 / Bing
        if (r.snippet.isEmpty())
            r.snippet = item.value("description").toString(); // SerpAPI

        if (!r.title.isEmpty())
            searchResults.append(r);
    }

    if (searchResults.isEmpty())
    {
        emit searchFailed(QStringLiteral("未找到相关搜索结果"));
        return;
    }

    const QString summary = SearchProvider::buildSummary(searchResults);
    emit searchCompleted(searchResults, summary);
}

QString SearchProvider::buildSummary(const QList<SearchResult> &results,
                                     int maxResults, int maxSnippetLen)
{
    QStringList lines;
    const int count = qMin(results.size(), maxResults);

    for (int i = 0; i < count; ++i)
    {
        const SearchResult &r = results[i];
        QString snippet = r.snippet;
        if (snippet.length() > maxSnippetLen)
            snippet = snippet.left(maxSnippetLen) + QStringLiteral("...");

        lines.append(QStringLiteral("%1. %2\n   %3")
                         .arg(i + 1)
                         .arg(r.title.trimmed(), snippet.trimmed()));
    }

    return lines.join(QStringLiteral("\n"));
}
