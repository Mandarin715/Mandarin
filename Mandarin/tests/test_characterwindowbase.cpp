#include <QtTest>

#include "../windows/character/characterwindowbase.h"

#include <QImage>
#include <QPainter>
#include <QScreen>
#include "../utils/InnerThoughtGeometry.h"
#include "../utils/ReplyAppearanceIntent.h"

/*CharacterWindowBase 的**窗口层契约**验证。

  为什么这个用例值得单独存在：`Dialog::requestSpeakState → CharacterWindowBase::SetSpeaking`
  与 `Dialog::requestSpeakLevel → CharacterWindowBase::SetSpeechLevel` 这两条连接在
  main.cpp 里指向的都是**基类**（这样 PNG 与 Live2D 两条路径共用同一条连接）。
  一旦基类那个槽缺了实现或语义错了，PNG 路径（默认立绘渲染器）就会在每次
  TTS 播放开始/结束时受影响 —— 而它本来什么都不该做。
  这里用最小的具体子类把基类**单独**装起来，不需要模型、不需要 GL、不需要真窗口。*/

namespace
{
/*最小具体子类：只为把基类的纯虚接口补齐，自己不做任何事。
   它存在的意义是"证明基类的默认 SetSpeaking 对子类零影响"。*/
class ProbeCharacterWindow : public CharacterWindowBase
{
  public:
    using CharacterWindowBase::CharacterWindowBase;

    void reloadContent(const QString &contentName) override
    {
        lastContentName = contentName;
        ++reloadCount;
    }

    int reloadCount = 0;
    bool frameFillsCanvas = false;
    QString lastContentName;

  protected:
    void relayoutContent() override {}
    QSize contentSize() const override { return QSize(); }
    QRect renderedImageRect() const override
    {
        return frameFillsCanvas ? rect() : CharacterWindowBase::renderedImageRect();
    }

  public:
    /*把基类 protected 的内容尺寸暴露出来，供"零副作用"断言使用
       （基类里它是 protected，测试类不是它的子类，读不到）。*/
    QSize probeContentSize() const { return contentSize(); }
    void setFrame(const QImage &image, QPoint offset = {}) { updateRenderedImage(image, offset); }
    QWidget *thought() const { return m_innerThoughtBubble; }
};

/*假信号源：复刻 Dialog::requestSpeakState 的形状（bool → 槽），
   用来证明基类的槽**真的能被信号驱动**（而不只是"能直接调用"）。*/
class SpeakStateEmitter : public QObject
{
    Q_OBJECT
  public:
    void fire(bool speaking) { emit requestSpeakState(speaking); }
  signals:
    void requestSpeakState(bool speaking);
};

/*第二个假信号源：复刻 Dialog::requestSpeakLevel 的形状（float → 槽）。
   为什么要单独一个而不是给上面那个加个信号：两条连接的**参数类型不同**，
   混在一个类里会让"哪一个信号连到哪一个槽"变得看不出来。*/
class SpeakLevelEmitter : public QObject
{
    Q_OBJECT
  public:
    void fire(float level) { emit requestSpeakLevel(level); }
  signals:
    void requestSpeakLevel(float level);
};
} // namespace

class TestCharacterWindowBase : public QObject
{
    Q_OBJECT

  private slots:
    void defaultSetSpeakingIsAcceptedAndInert();
    void setSpeakingIsAConnectableSlot();
    /*响度电平同理：PNG 路径没有可以跟着电平动的嘴，默认实现必须零副作用*/
    void defaultSetSpeechLevelIsAcceptedAndInert();
    void setSpeechLevelIsAConnectableSlot();
    void thoughtAvoidsFaceAtScreenEdges();
    void thoughtUsesVisiblePixelsAndFollowsWindow();
    void validatesReplyAppearanceIntent();
};

/*默认实现必须"接受任何值、什么都不做" —— 别的行为都属于越权。*/
void TestCharacterWindowBase::defaultSetSpeakingIsAcceptedAndInert()
{
    ProbeCharacterWindow window;

    window.SetSpeaking(true);
    window.SetSpeaking(false);
    window.SetSpeaking(true);

    //没有任何可以观察到的副作用：内容没被换、窗口没被显示/隐藏
    QCOMPARE(window.reloadCount, 0);
    QVERIFY(window.lastContentName.isEmpty());
    QVERIFY(window.probeContentSize().isEmpty());
    QVERIFY2(!window.isVisible(), "默认 SetSpeaking 不该把窗口显出来");
    qInfo("BASE SetSpeaking: 三次调用（true/false/true）无任何副作用，reloadCount=0");
}

/*它必须是**能连的槽**：main.cpp 那条 connect 用的是基类成员函数指针
   （&CharacterWindowBase::SetSpeaking）。若有人把它挪成普通成员函数，
   那条连接会在编译期失败 —— 这条测试是"基类契约"那一侧的钉子（main.cpp 那侧的
   编译期检查由构建本身提供）。
   顺带证明信号真的能驱动它（Qt 走虚函数表，落到子类覆写上）。*/
void TestCharacterWindowBase::setSpeakingIsAConnectableSlot()
{
    ProbeCharacterWindow window;
    const QMetaObject *meta = window.metaObject();
    const int index = meta->indexOfSlot("SetSpeaking(bool)");
    QVERIFY2(index >= 0, "CharacterWindowBase 里找不到 SetSpeaking(bool) 槽");

    SpeakStateEmitter emitter;
    const bool connected = QObject::connect(&emitter, &SpeakStateEmitter::requestSpeakState,
                                            &window, &CharacterWindowBase::SetSpeaking);
    QVERIFY2(connected, "requestSpeakState(bool) 连不上 CharacterWindowBase::SetSpeaking");
    emitter.fire(true);
    emitter.fire(false);
    qInfo("BASE SetSpeaking: metaObject 槽索引 %d，信号两次触发均无异常", index);
    QVERIFY(!window.isVisible());
}

/*默认实现必须"接受任何电平、什么都不做" —— 与 SetSpeaking 同理，别的行为都属于越权。*/
void TestCharacterWindowBase::defaultSetSpeechLevelIsAcceptedAndInert()
{
    ProbeCharacterWindow window;

    window.SetSpeechLevel(0.0f);
    window.SetSpeechLevel(0.35f);
    window.SetSpeechLevel(1.0f);
    window.SetSpeechLevel(-5.0f); // 越界值也必须安静接受（不许断言/不许崩）

    QCOMPARE(window.reloadCount, 0);
    QVERIFY(window.lastContentName.isEmpty());
    QVERIFY(window.probeContentSize().isEmpty());
    QVERIFY2(!window.isVisible(), "默认 SetSpeechLevel 不该把窗口显出来");
    qInfo("BASE SetSpeechLevel: 四次调用（0/0.35/1/-5）无任何副作用，reloadCount=0");
}

/*它同样必须是**能连的槽**（main.cpp 那条 connect 用的是基类成员函数指针）。*/
void TestCharacterWindowBase::setSpeechLevelIsAConnectableSlot()
{
    ProbeCharacterWindow window;
    const QMetaObject *meta = window.metaObject();
    const int index = meta->indexOfSlot("SetSpeechLevel(float)");
    QVERIFY2(index >= 0, "CharacterWindowBase 里找不到 SetSpeechLevel(float) 槽");

    SpeakLevelEmitter emitter;
    const bool connected = QObject::connect(&emitter, &SpeakLevelEmitter::requestSpeakLevel,
                                            &window, &CharacterWindowBase::SetSpeechLevel);
    QVERIFY2(connected, "requestSpeakLevel(float) 连不上 CharacterWindowBase::SetSpeechLevel");
    emitter.fire(0.0f);
    emitter.fire(0.8f);
    qInfo("BASE SetSpeechLevel: metaObject 槽索引 %d，信号两次触发均无异常", index);
    QVERIFY(!window.isVisible());
}

void TestCharacterWindowBase::thoughtAvoidsFaceAtScreenEdges()
{
    const QRect desktop(-1920, -100, 1920, 1080);
    const QSize bubble(320, 70);
    for (const QRect figure : {QRect(-1400, 300, 400, 600),
                               QRect(-1400, -90, 400, 600),
                               QRect(-440, -90, 400, 600),
                               QRect(-1910, -90, 400, 600)})
    {
        const QRect placed(InnerThoughtGeometry::position(figure, bubble, desktop), bubble);
        QVERIFY(desktop.contains(placed));
        const QRect head(figure.left(), figure.top(), figure.width(), figure.height() / 3);
        QVERIFY2(!placed.intersects(head), "Thought bubble overlaps face near a screen edge");
        if (figure.top() == 300)
            QVERIFY(placed.bottom() < figure.top());
    }
}

void TestCharacterWindowBase::thoughtUsesVisiblePixelsAndFollowsWindow()
{
    ProbeCharacterWindow window;
    const QRect desktop = window.screen()->availableGeometry();
    window.resize(300, 500);
    window.move(desktop.center() - QPoint(150, 100));
    QImage frame(600, 800, QImage::Format_ARGB32_Premultiplied);
    frame.fill(Qt::transparent);
    {
        QPainter painter(&frame);
        painter.fillRect(QRect(100, 200, 400, 600), Qt::white);
    }
    frame.setDevicePixelRatio(2.0);
    window.setFrame(frame, QPoint(0, 10));
    window.show();
    window.ShowInnerThought(QStringLiteral("This is a long thought that must stay clear of the face."));
    QWidget *bubble = window.thought();
    QVERIFY(bubble);
    QVERIFY(bubble->isWindow());
    QVERIFY(bubble->windowFlags().testFlag(Qt::WindowTransparentForInput));
    const int figureTop = window.mapToGlobal(QPoint(0, 110)).y();
    QVERIFY(bubble->geometry().bottom() < figureTop);
    QVERIFY(bubble->geometry().bottom() > window.mapToGlobal(QPoint(0, 0)).y());
    const QPoint before = bubble->pos();
    window.move(window.pos() + QPoint(20, 20));
    QCOMPARE(bubble->pos(), before + QPoint(20, 20));
    // Live2D draws supersampled frames into rect(), not image.size()/DPR.
    window.frameFillsCanvas = true;
    QImage supersampled = frame.scaled(1200, 1600);
    supersampled.setDevicePixelRatio(1.25);
    window.setFrame(supersampled);
    QCOMPARE(bubble->geometry().bottom(), window.mapToGlobal(QPoint(0, 125)).y() - 17);
    const QPoint supersampledPosition = bubble->pos();
    frame.setDevicePixelRatio(1.25);
    window.setFrame(frame);
    QCOMPARE(bubble->pos(), supersampledPosition);
    window.hide();
    QVERIFY(!bubble->isVisible());
    QVERIFY(!window.thought());
    window.ShowInnerThought(QStringLiteral("A hidden character must not leave a floating bubble."));
    QVERIFY(!window.thought());
}

void TestCharacterWindowBase::validatesReplyAppearanceIntent()
{
    const QString reply = QStringLiteral("快乐|换好了|着替えました||{\"appearance\":{\"clothes\":\"pajamas\"}}");
    QCOMPARE(ReplyAppearanceIntent::parse(reply, QStringLiteral("换上睡衣")), QJsonObject({{"clothes", "pajamas"}}));
    QCOMPARE(ReplyAppearanceIntent::parse(reply, QStringLiteral("把睡衣换上吧")), QJsonObject({{"clothes", "pajamas"}}));
    QCOMPARE(ReplyAppearanceIntent::requested(QStringLiteral("穿回平时的衣服")), QJsonObject({{"clothes", "default"}}));
    QCOMPARE(ReplyAppearanceIntent::requested(QStringLiteral("换回默认鞋")), QJsonObject({{"shoes", "default"}}));
    for (const QString &request : {QStringLiteral("不要换睡衣"), QStringLiteral("昨天换上睡衣"),
                                   QStringLiteral("喜欢睡衣吗"), QStringLiteral("如果换上睡衣"),
                                   QStringLiteral("换上比基尼"), QStringLiteral("聊聊睡衣"), QStringLiteral("我穿上睡衣了")})
        QVERIFY(ReplyAppearanceIntent::parse(reply, request).isEmpty());
    QVERIFY(ReplyAppearanceIntent::parse(reply, QStringLiteral("我不换睡衣")).isEmpty());
    QVERIFY(ReplyAppearanceIntent::parse(reply, QStringLiteral("换上睡衣和换上凉鞋")).isEmpty());
    QVERIFY(ReplyAppearanceIntent::parse(QStringLiteral("快乐|好|はい||{\"appearance\":{\"white-eye\":\"on\"}}"), QStringLiteral("换上睡衣")).isEmpty());
    QVERIFY(ReplyAppearanceIntent::parse(reply + "|extra", QStringLiteral("换上睡衣")).isEmpty());
    QVERIFY(ReplyAppearanceIntent::parse("快乐|好|はい||broken", QStringLiteral("换上睡衣")).isEmpty());
    const QString shoes = QStringLiteral("快乐|好|はい||{\"appearance\":{\"shoes\":\"sandals\"}}");
    QCOMPARE(ReplyAppearanceIntent::parse(shoes, QStringLiteral("换上凉鞋")), QJsonObject({{"shoes", "sandals"}}));
    QVERIFY(ReplyAppearanceIntent::isShock(QStringLiteral("震惊")));
    QVERIFY(ReplyAppearanceIntent::isShock(QStringLiteral("惊讶")));
    QVERIFY(!ReplyAppearanceIntent::isShock(QStringLiteral("快乐")));
    const QString withThought = QStringLiteral("震惊|什么？|えっ？|内心独白！|{\"appearance\":{}}");
    QCOMPARE(withThought.section('|', 2, 2), QStringLiteral("えっ？"));
}
QTEST_MAIN(TestCharacterWindowBase)
#include "test_characterwindowbase.moc"
