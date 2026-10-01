#include <QtTest>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include "../utils/SearchProvider.h"

class ControlledReply : public QNetworkReply {
public:
    QByteArray body;
    qint64 cursor = 0;
    bool aborted = false;
    ControlledReply(const QNetworkRequest &request, QObject *parent) : QNetworkReply(parent) {
        setRequest(request); setUrl(request.url()); open(QIODevice::ReadOnly);
    }
    void abort() override { aborted = true; } // A server may still produce a late response.
    void finish(const QByteArray &data, bool fail = false) {
        body=data;
        if (fail) setError(QNetworkReply::AuthenticationRequiredError, "old credential error");
        setFinished(true);
        emit readyRead(); emit finished();
    }
    qint64 bytesAvailable() const override { return body.size()-cursor + QNetworkReply::bytesAvailable(); }
protected:
    qint64 readData(char *target, qint64 max) override {
        const auto count = qMin(max, body.size()-cursor);
        if (count <= 0) return -1;
        memcpy(target, body.constData()+cursor, size_t(count)); cursor+=count; return count;
    }
};
class ControlledNetwork : public QNetworkAccessManager {
public:
    QList<ControlledReply *> replies;
    QList<QByteArray> bodies;
protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *data) override {
        auto *reply=new ControlledReply(request,this);
        replies.append(reply); bodies.append(data ? data->readAll() : QByteArray{});
        return reply;
    }
};

class TestSearchProvider : public QObject
{
    Q_OBJECT
private slots:
    void replacementIgnoresCancelledRequest();
    void destructionCancelsWithoutSignals();
    void oauthCoalescesAndCachesShortLivedToken();
    void configurationRejectsOldTokenResponse();
    void disablingRejectsLateSearchResponse();
};

void TestSearchProvider::replacementIgnoresCancelledRequest()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    SearchProvider provider;
    provider.setEnabled(true);
    provider.setBaseUrl(QString("http://127.0.0.1:%1/search").arg(server.serverPort()));
    QSignalSpy failed(&provider, &SearchProvider::searchFailed);
    QSignalSpy completed(&provider, &SearchProvider::searchCompleted);
    provider.search("old");
    QTRY_VERIFY(server.hasPendingConnections());
    auto *oldSocket = server.nextPendingConnection();
    QTRY_VERIFY(oldSocket->bytesAvailable() > 0);
    provider.search("new");
    QCOMPARE(failed.count(), 0);
    QTRY_VERIFY(server.hasPendingConnections());
    auto *socket = server.nextPendingConnection();
    QTRY_VERIFY(socket->bytesAvailable() > 0);
    QVERIFY(socket->readAll().contains("q=new"));
    const QByteArray body = R"({"results":[{"title":"new result","url":"https://example.com","content":"summary"}]})";
    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                  + QByteArray::number(body.size()) + "\r\n\r\n" + body);
    socket->disconnectFromHost();
    QTRY_COMPARE(completed.count(), 1);
    QCOMPARE(failed.count(), 0);
    QVERIFY(completed.at(0).at(1).toString().contains("new result"));
}

void TestSearchProvider::destructionCancelsWithoutSignals()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    auto *provider = new SearchProvider;
    provider->setEnabled(true);
    provider->setBaseUrl(QString("http://127.0.0.1:%1/search").arg(server.serverPort()));
    QSignalSpy failed(provider, &SearchProvider::searchFailed);
    provider->search("pending");
    QTRY_VERIFY(server.hasPendingConnections());
    auto *socket = server.nextPendingConnection();
    QTRY_VERIFY(socket->bytesAvailable() > 0);
    delete provider;
    QCOMPARE(failed.count(), 0);
}

void TestSearchProvider::oauthCoalescesAndCachesShortLivedToken()
{
    ControlledNetwork network;
    SearchProvider provider(nullptr,&network);
    provider.setEnabled(true); provider.setApiKey("key"); provider.setSecretKey("secret");
    provider.setBaseUrl("https://qianfan.baidubce.com/v2/ai_search/web_search");
    QSignalSpy failed(&provider,&SearchProvider::searchFailed);
    provider.search("first"); provider.search("latest");
    QCOMPARE(network.replies.size(),1);
    QVERIFY(network.bodies[0].contains("client_id=key"));
    QVERIFY(!network.replies[0]->url().hasQuery());
    network.replies[0]->finish(R"({"access_token":"current","expires_in":3600})");
    QCOMPARE(network.replies.size(),2);
    QCOMPARE(network.replies[1]->request().rawHeader("X-Appbuilder-Authorization"), QByteArray("Bearer current"));
    QVERIFY(network.bodies[1].contains("latest"));
    QVERIFY(!network.bodies[1].contains("first"));
    provider.setApiKey("key"); provider.setSecretKey("secret"); // Unrelated setting reload.
    provider.search("next");
    QCOMPARE(network.replies.size(),3); // A short-lived token is still cached.
    QVERIFY(network.replies[1]->aborted);
    QCOMPARE(network.replies[2]->request().rawHeader("X-Appbuilder-Authorization"), QByteArray("Bearer current"));
    QCOMPARE(failed.count(),0);
}
void TestSearchProvider::configurationRejectsOldTokenResponse()
{
    ControlledNetwork network;
    SearchProvider provider(nullptr,&network);
    provider.setEnabled(true); provider.setApiKey("old-key"); provider.setSecretKey("secret");
    provider.setBaseUrl("https://qianfan.baidubce.com/v2/ai_search/web_search");
    QSignalSpy failed(&provider,&SearchProvider::searchFailed);
    provider.search("old-query");
    auto *old=network.replies.first();
    provider.setApiKey("new-key");
    QVERIFY(old->aborted);
    QCOMPARE(failed.count(),1); // Releases the caller's busy state.
    provider.search("new-query");
    old->finish(R"({"access_token":"stale","expires_in":3600})");
    QCOMPARE(network.replies.size(),2);
    network.replies[1]->finish(R"({"access_token":"fresh","expires_in":3600})");
    QCOMPARE(network.replies.size(),3);
    QCOMPARE(network.replies[2]->request().rawHeader("X-Appbuilder-Authorization"),QByteArray("Bearer fresh"));
    QVERIFY(network.bodies[2].contains("new-query"));
    old->finish("{}",true);
    QCOMPARE(failed.count(),1);
    provider.search("more");
    QCOMPARE(network.replies.size(),4);
    QCOMPARE(network.replies[3]->request().rawHeader("X-Appbuilder-Authorization"),QByteArray("Bearer fresh"));
}
void TestSearchProvider::disablingRejectsLateSearchResponse()
{
    ControlledNetwork network;
    SearchProvider provider(nullptr,&network);
    provider.setEnabled(true); provider.setBaseUrl("http://example.test/search");
    QSignalSpy completed(&provider,&SearchProvider::searchCompleted);
    QSignalSpy failed(&provider,&SearchProvider::searchFailed);
    provider.search("pending");
    auto *old=network.replies.first();
    provider.setEnabled(false);
    QVERIFY(old->aborted);
    old->finish(R"({"results":[{"title":"late"}]})");
    QCOMPARE(completed.count(),0);
    QCOMPARE(failed.count(),1);
}

QTEST_GUILESS_MAIN(TestSearchProvider)
#include "test_searchprovider.moc"
