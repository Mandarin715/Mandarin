#ifndef DEVICEPIXELALIGN_H
#define DEVICEPIXELALIGN_H

#include <QSize>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

/*「画布必须设备像素对齐」这条纯几何规则。

  为什么要单独拎出来：它是"上屏不糊"的根因修复，判据只有算术，跟 Live2D、窗口、GL 全都无关，
  所以放在 utils 里可以直接测（同 utils/TachieGeometry 的做法），不必为了验证它去启渲染器。

  ## 问题

  窗口这样铺满自己：drawImage(rect(), img, QRectF(0, 0, img.width(), img.height()))。
  **源矩形给的是图像像素，目标矩形是逻辑坐标**，于是 Qt 把 img 的像素网格铺到窗口的
  **设备**像素网格（rect() × dpr）上 —— 只有 img 的像素尺寸恰好等于 rect() × dpr 才是 1:1 拷贝。
  登记帧尺寸是 lround(canvas × dpr)，窗口设备矩形是 canvas × dpr，所以 canvas × dpr
  不是整数时，两者必然差着零点几个像素：
  实测（atri、dpr 1.25、400x938 画布）登记帧 500x1173、窗口设备矩形 500x1172.5，
  1173 行被压进 1172.5 个设备行（纵向比例 0.99957），逐行相位从 0 漂到 0.5，
  结果 17.31% 的像素不同、4 邻域 Laplacian 方差 −11.9%（人眼可辨）。

  （登记帧的 devicePixelRatio 也被设成窗口 dpr，所以这件事也可以说成
   "登记帧的逻辑尺寸 ≠ 画布逻辑尺寸"，两种说法在真实管线上等价。纯 Qt 对照用例
   test_devicepixelblit 验证的是更本质的那条：源矩形按像素给，所以决定成败的是
   像素尺寸对不对得上窗口设备矩形。）

  ## 规则

  canvas × dpr 为整数 ⇔ canvas 是 dpr 既约分母的整数倍。常见 dpr 的步长：
  1.0→1、1.25→4、1.5→2、1.75→4、2.0→1（250% 屏 2.5→2、225% 屏 2.25→4）。
  **步长由运行时 dpr 推出，不写死 4**：200%/100% 屏上写死 4 会白白牺牲宽高比精度。

  ## 代价

  画布宽高比就是人物的像素宽高比（渲染器横竖分轴摆放），所以对齐引入的宽高比偏差
  **直接等于人物被拉伸的比例**。两条边各自就近取整最多偏 0.7%（高 ±2/938 + 宽 ±2/400），
  所以 devicePixelAlignedCanvasSize 改在"理想高度附近 ±1 个步长"的几个候选里挑宽高比
  最接近的那一个：高度最多偏一个步长（近千像素画布上 ±4px，人物屏幕高度变化 <0.5%），
  宽高比偏差通常 <0.1%。实测 atri / dpr 1.25：400x938 → 400x936，偏差 0.11%。*/

/*让 canvas × dpr 成为整数所需的最小对齐步长。

  dpr 的既约分母不会大过 kMaxStep（Windows 的 100%~250% 档位分母 ≤4），直接乘整数试即可。
  试不出来（理论上不会发生）返回 1：退化成"不对齐"的旧行为，而不是拿猜出来的步长改画布。*/
inline int devicePixelAlignStep(qreal dpr)
{
    if (!(dpr > 0.0))
        return 1;

    constexpr int kMaxStep = 8;
    for (int step = 1; step <= kMaxStep; ++step)
    {
        const qreal scaled = dpr * step;
        if (std::abs(scaled - std::round(scaled)) < 1e-6)
            return step;
    }
    return 1;
}

/*把理想画布吸附到「canvas × dpr 为整数」的最近尺寸，并在候选里挑宽高比最接近 figureAspect 的。

  ideal        理想画布（逻辑像素，通常来自人物宽高比与目标占比）
  figureAspect 人物真实宽高比（宽/高）—— 画布宽高比要贴住它，否则人物会被拉长/压扁
  step         devicePixelAlignStep(dpr)
  upperBound   尺寸上界（屏幕夹取的结果）：候选一律不超过它，保证 85% 屏高那类夹取是硬保证
  minSide/maxSide 画布边长下限/上限（对齐后仍然夹在里面）*/
inline QSize devicePixelAlignedCanvasSize(const QSize &ideal, double figureAspect, int step,
                                         const QSize &upperBound, int minSide, int maxSide)
{
    const int s = std::max(1, step);
    const int limitW = std::max(s, std::min(maxSide, upperBound.width()));
    const int limitH = std::max(s, std::min(maxSide, upperBound.height()));
    const int lowSide = std::max(s, minSide);

    const auto clampInt = [](int value, int low, int high) {
        return std::min(std::max(value, low), high);
    };

    // 理想高度向下取整到步长，再向两侧各取一个候选（高度最多偏一个步长）
    const int baseH = std::max(s, (clampInt(ideal.height(), minSide, maxSide) / s) * s);

    QSize best;
    double bestScore = -1.0;
    for (int offset = -s; offset <= s; offset += s)
    {
        const int h = baseH + offset;
        if (h < lowSide || h > limitH)
            continue;

        // 宽度按人物宽高比推出，再就近吸附到步长整数倍，并夹进上界（向下取整到步长）
        int w = static_cast<int>(std::lround(h * figureAspect));
        w = ((w + s / 2) / s) * s;
        w = std::max(s, std::min(w, (limitW / s) * s));

        const double aspectError =
            std::abs(static_cast<double>(w) / h - figureAspect) / std::max(1e-6, figureAspect);
        const double heightPenalty =
            std::abs(static_cast<double>(h - ideal.height())) / std::max(1, ideal.height());
        // 主判据是宽高比；高度只用来打破宽高比打平的平局（权重 1e-3）
        const double score = aspectError + heightPenalty * 1e-3;
        if (bestScore < 0.0 || score < bestScore)
        {
            bestScore = score;
            best = QSize(w, h);
        }
    }

    // 候选全被上界挡掉（理论上不会）：原样返回理想尺寸，宁可不对齐也不越界
    return best.isEmpty() ? ideal : best;
}

#endif // DEVICEPIXELALIGN_H
