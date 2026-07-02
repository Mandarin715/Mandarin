#ifndef SEARCHPROVIDER_H
#define SEARCHPROVIDER_H

#include <QDateTime>
#include <QJsonArray>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

/// 搜索结果结构
struct SearchResult
{
    QString title;
    QString url;
    QString snippet;
};

/// 联网搜索提供者，封装搜索 API 调用。
/// 支持百度千帆、SearXNG、Bing、SerpAPI 等多种后端。
class SearchProvider : public QObject
{
    Q_OBJECT

public:
    explicit SearchProvider(QObject *parent = nullptr);
    ~SearchProvider();

    /// 配置搜索 API
    void setApiKey(const QString &key);
    void setSecretKey(const QString &key);
    void setBaseUrl(const QString &url);
    void setEnabled(bool enabled);
    bool isEnabled() const;

    /// 发起搜索，完成后通过 searchCompleted 信号返回结果
    void search(const QString &query);

    /// 从多个搜索结果构建摘要文本
    static QString buildSummary(const QList<SearchResult> &results,
                                int maxResults = 5, int maxSnippetLen = 100);

signals:
    void searchCompleted(const QList<SearchResult> &results,
                         const QString &summary);
    void searchFailed(const QString &error);

private slots:
    void onTokenReplyFinished();
    void onSearchReplyFinished();

private:
    /// 百度千帆 OAuth 2.0：用 API Key + Secret Key 换取 access_token
    void requestAccessToken();
    /// 直接执行搜索（已有有效 token 或不需要 token）
    void doSearch(const QString &query);

    QNetworkAccessManager *m_network = nullptr;
    QString m_apiKey;
    QString m_secretKey;
    QString m_baseUrl;
    bool m_enabled = false;
    QNetworkReply *m_activeReply = nullptr;

    // OAuth token 缓存
    QString m_accessToken;
    QDateTime m_tokenExpiry;
    QString m_pendingQuery; // token 获取期间暂存的搜索 query
};

#endif // SEARCHPROVIDER_H
