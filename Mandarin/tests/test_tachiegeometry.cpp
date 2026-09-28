#include <QtTest>

#include "../utils/TachieGeometry.h"

class TestTachieGeometry : public QObject
{
    Q_OBJECT

  private slots:
    void canvasForScaledSizeMatchesCurrentBehavior();
    void clampScaleFactorBounds();
    void centeredRectRounds();
};

/*200% 画布与居中（值取自改造前 SetTachieSize 的算法）*/
void TestTachieGeometry::canvasForScaledSizeMatchesCurrentBehavior()
{
    const TachieGeometry::CanvasLayout even =
        TachieGeometry::canvasForScaledSize(QSize(300, 500));
    QCOMPARE(even.canvasSize, QSize(600, 1000));
    QCOMPARE(even.imageTopLeft, QPoint(150, 250));

    // 奇数差：整数除法向下取整，与改造前一致
    const TachieGeometry::CanvasLayout odd =
        TachieGeometry::canvasForScaledSize(QSize(301, 501));
    QCOMPARE(odd.canvasSize, QSize(602, 1002));
    QCOMPARE(odd.imageTopLeft, QPoint(150, 250));

    // 退化尺寸不允许出现 0 画布
    const TachieGeometry::CanvasLayout tiny =
        TachieGeometry::canvasForScaledSize(QSize(0, 0));
    QCOMPARE(tiny.canvasSize, QSize(1, 1));

    // 自定义画布倍率
    const TachieGeometry::CanvasLayout custom =
        TachieGeometry::canvasForScaledSize(QSize(100, 100), 1.5);
    QCOMPARE(custom.canvasSize, QSize(150, 150));
    QCOMPARE(custom.imageTopLeft, QPoint(25, 25));
}

/*倍率钳制（改造前是 qBound(0.05, factor, 2.0)）*/
void TestTachieGeometry::clampScaleFactorBounds()
{
    QCOMPARE(TachieGeometry::clampScaleFactor(0.001), 0.05);
    QCOMPARE(TachieGeometry::clampScaleFactor(1.5), 1.5);
    QCOMPARE(TachieGeometry::clampScaleFactor(99.0), 2.0);
}

/*居中矩形的 round 行为（改造前是 qRound(center - size / 2.0)）*/
void TestTachieGeometry::centeredRectRounds()
{
    QCOMPARE(TachieGeometry::centeredRect(QSize(600, 1000), QSize(301, 501)),
             QRect(QPoint(150, 250), QSize(301, 501)));
    QCOMPARE(TachieGeometry::centeredRect(QSize(601, 1001), QSize(300, 500)),
             QRect(QPoint(151, 251), QSize(300, 500)));
}

QTEST_MAIN(TestTachieGeometry)
#include "test_tachiegeometry.moc"
