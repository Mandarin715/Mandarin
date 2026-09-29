#include <QtTest>

#include "../windows/character/characterwindowbase.h"

#include <QImage>

/*CharacterWindowBase 的**窗口层契约**验证。

  为什么这个用例值得单独存在：`Dialog::requestSpeakState → CharacterWindowBase::SetSpeaking`
  这条连接在 main.cpp 里指向的是**基类**（这样 PNG 与 Live2D 两条路径共用同一条连接）。
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
    QString lastContentName;

  protected:
    void relayoutContent() override {}
    QSize contentSize() const override { return QSize(); }

  public:
    /*把基类 protected 的内容尺寸暴露出来，供"零副作用"断言使用
       （基类里它是 protected，测试类不是它的子类，读不到）。*/
    QSize probeContentSize() const { return contentSize(); }
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
} // namespace

class TestCharacterWindowBase : public QObject
{
    Q_OBJECT

  private slots:
    void defaultSetSpeakingIsAcceptedAndInert();
    void setSpeakingIsAConnectableSlot();
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

QTEST_MAIN(TestCharacterWindowBase)
#include "test_characterwindowbase.moc"
