#pragma once

#include <QRect>
#include <QSize>
#include <limits>

namespace InnerThoughtGeometry
{
// All coordinates are screen logical pixels. Keep the upper part of the figure clear.
inline QPoint position(const QRect &figure, const QSize &bubble, const QRect &screen)
{
    constexpr int gap = 16;
    const QRect safe = screen.adjusted(8, 8, -8, -8);
    const QRect head(figure.left(), figure.top(), figure.width(),
                     qMax(1, figure.height() / 3));
    const QPoint candidates[] = {
        {figure.center().x() - bubble.width() / 2,
         figure.top() - gap - bubble.height()},
        {figure.right() + gap, figure.top()},
        {figure.left() - gap - bubble.width(), figure.top()},
        {figure.center().x() - bubble.width() / 2, head.bottom() + gap}
    };
    QPoint best;
    qint64 leastOverlap = std::numeric_limits<qint64>::max();
    for (const QPoint &candidate : candidates)
    {
        const QPoint point(qBound(safe.left(), candidate.x(),
                                  qMax(safe.left(), safe.right() + 1 - bubble.width())),
                           qBound(safe.top(), candidate.y(),
                                  qMax(safe.top(), safe.bottom() + 1 - bubble.height())));
        const QRect overlap = QRect(point, bubble).intersected(head.adjusted(-gap, -gap, gap, gap));
        const qint64 area = qint64(overlap.width()) * overlap.height();
        if (area < leastOverlap)
        {
            leastOverlap = area;
            best = point;
        }
        if (area == 0)
            break;
    }
    return best;
}
} // namespace InnerThoughtGeometry
