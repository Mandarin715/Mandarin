#ifndef TACHIEGEOMETRY_H
#define TACHIEGEOMETRY_H

#include <QPoint>
#include <QRect>
#include <QSize>

/*立绘窗口的纯几何计算。
  从 Tachie 里抽出来：一是便于单元测试，二是 PNG 与 Live2D 两个渲染器共用同一套画布规则。*/
namespace TachieGeometry
{
/*PNG 立绘的 200% 画布：窗口尺寸 = 缩放后图片 × canvasScale，图片在画布内居中*/
struct CanvasLayout
{
    QSize canvasSize;
    QPoint imageTopLeft;
};

CanvasLayout canvasForScaledSize(const QSize &scaledSize, double canvasScale = 2.0);

/*缩放动画倍率钳制（避免异常值把图片瞬间放大或缩到看不见）*/
double clampScaleFactor(double factor);

/*把 imageSize 以 canvasSize 为画布居中摆放（四舍五入到整像素）*/
QRect centeredRect(const QSize &canvasSize, const QSize &imageSize);
} // namespace TachieGeometry

#endif // TACHIEGEOMETRY_H
