#include "TachieGeometry.h"

#include <QtGlobal>

#include <algorithm>

namespace TachieGeometry
{
CanvasLayout canvasForScaledSize(const QSize &scaledSize, double canvasScale)
{
    CanvasLayout layout;
    const int canvasWidth = std::max(1, qRound(scaledSize.width() * canvasScale));
    const int canvasHeight = std::max(1, qRound(scaledSize.height() * canvasScale));
    layout.canvasSize = QSize(canvasWidth, canvasHeight);
    // 整数除法：与 Tachie::SetTachieSize 改造前的行为逐位一致
    layout.imageTopLeft = QPoint((canvasWidth - scaledSize.width()) / 2,
                                 (canvasHeight - scaledSize.height()) / 2);
    return layout;
}

double clampScaleFactor(double factor)
{
    return qBound(0.05, factor, 2.0);
}

QRect centeredRect(const QSize &canvasSize, const QSize &imageSize)
{
    const int x = qRound(canvasSize.width() / 2.0 - imageSize.width() / 2.0);
    const int y = qRound(canvasSize.height() / 2.0 - imageSize.height() / 2.0);
    return QRect(QPoint(x, y), imageSize);
}
} // namespace TachieGeometry
