#ifndef LIVE2DOFFSCREENRENDERER_H
#define LIVE2DOFFSCREENRENDERER_H

#include <QImage>
#include <QSize>
#include <QString>

#include <memory>

/*把 Live2D 模型离屏渲染成一张 RGBA 图。

  三个设计要点（依据方案「实施前核实」与 SDK 实测）：
  1. **全程内存加载**：moc3 / model3.json / 贴图全部自己读，路径从不交给 SDK ——
     Cubism 内部走 csmString→fopen，Windows 下打不开 `樱花miku.moc3` 这类中文路径；
  2. **离屏 FBO**：用 SDK 自带的 CubismRenderTarget_OpenGLES2，不嵌 QOpenGLWidget，
     从而绕开「半透明窗口 + GL 子控件 alpha 合成异常」这个已知坑；
  3. 产物是一张直通 alpha 的 QImage，可直接交给
     `CharacterWindowBase::updateRenderedImage()` 复用现成的穿透/命中判定。*/
class Live2DOffscreenRenderer
{
  public:
    Live2DOffscreenRenderer();
    ~Live2DOffscreenRenderer();

    Live2DOffscreenRenderer(const Live2DOffscreenRenderer &) = delete;
    Live2DOffscreenRenderer &operator=(const Live2DOffscreenRenderer &) = delete;

    /*加载模型。modelDir 为模型目录（允许含中文），modelJsonName 为 model3.json 文件名。*/
    bool load(const QString &modelDir, const QString &modelJsonName, QString *error = nullptr);

    /*渲染一帧并返回 RGBA 图（不预乘）；未加载或失败返回空图。*/
    QImage renderFrame(const QSize &size);

    /*设置参数覆盖值：每帧在 LoadParameters() 之后、Update() 之前施加，因此不会被还原覆盖。
      传中文/自定义参数名均可（内部经 CubismIdManager 注册成 id）。*/
    void setParameter(const QString &parameterId, float value);

    /*读取参数当前值。用于验证覆盖是否真的生效（比对比两帧像素可靠：
      两帧之间物理动画本来就会变，像素差异无法证明是参数造成的）。*/
    float parameterValue(const QString &parameterId) const;

    /*人物可见范围（全部在**绘制输出空间**，画布映射到 [-0.5, 0.5]）。

      用输出空间而不是模型画布像素：本模型的模型画布是 1x1（缩放 2/H_m = 2），
      人物在画布坐标里只占零点几个像素，取整会把它毁成 1x1、缩放因子算错。

      ⚠️ 探针是 1:1 方形帧，而真实画布一般不是方形：renderFrame 会把模型等比缩放到
      画布的一条边上，横长（W/H）就是这个额外缩放。所以 probeFigureMetrics 量的值
      必须再乘 W/H 才是"目标画布输出空间"里的范围 —— figureMetrics() 已经替调用方
      乘好了，直接用它的结果即可。

      - boundsAspect：可见范围宽/高（已在目标画布比例下）
      - spanX / spanY：可见范围在目标画布输出空间的宽/高。目标占比 target 对应
        displayHeightRatio = target / spanY（见 setDisplayHeightRatio）。
      - valid：是否量到内容。*/
    struct FigureMetrics
    {
        float minX = 0.0f;
        float minY = 0.0f;
        float maxX = 0.0f;
        float maxY = 0.0f;
        float spanX = 1.0f;
        float spanY = 1.0f;
        float boundsAspect = 1.0f;
        bool valid = false;
    };

    /*量出 probeFrame（**必须是 1:1 正方形探针帧**）里人物可见部分的输出空间范围，
      并**并进**已累计的范围。返回的是"探针比例下"的原始测量值。*/
    FigureMetrics probeFigureMetrics(const QImage &probeFrame);

    /*已累计的人物可见范围，**已换算到目标画布宽高比**（乘了 canvasWidth/canvasHeight）。
      canvasWidth/canvasHeight 传当前（或即将使用的）目标画布尺寸（逻辑像素）。
      用 int 是为了让这个头文件不依赖 Cubism 的 csmUint32 类型。*/
    FigureMetrics figureMetrics(int canvasWidth, int canvasHeight) const;

    /*设定人物高度占画布高度的比例（0~1]，并把人物**居中**。

      为什么需要它：模型画布里人物的包围盒本身是偏的（本模型明显偏右），
      只把画布放大是没用的 —— 人物像素位置与画布同比例放大，右边缘照样贴着画布。
      这里用"统一缩小 + 平移回画布中心"来同时解决尺寸与偏置。

      默认 1.0 = 保持原样（人物铺满画布高度、不做平移），不影响既有行为。*/
    void setDisplayHeightRatio(float ratio);

    /*把"人物的完整活动范围"告诉渲染器：传**输出空间**里的浮点范围
      （画布映射到 [-0.5, 0.5]，即 probeFigureMetrics 返回的 span/位置）。

      为什么不能只用一帧的范围：待机动作/呼吸/物理会让姿势移动，单帧范围会被超出。
      窗口侧应在整段动作上取样求**并集**，再把并集交给这里。
      为什么是输出空间而不是模型画布像素：本模型的模型画布是 1x1，画布像素坐标下的
      人物范围会被 QRect 取整毁掉（实测退化成 1x1，缩放因子算错、人物被缩小 8 倍）。*/
    void setFigureSpanInOutputSpace(float minX, float minY, float maxX, float maxY);

    /*清空已记录的人物范围（探针失败时用，避免沿用上一次的旧值）*/
    void clearFigureSpan();

    /*水印开关。**注意该模型的水印参数是反相的**（实测，且靠目视确认）：
       0 = 显示水印，1 = 隐藏水印。
       因为 moc 里 Param137 默认为 0（水印可见），而 `水印.exp3.json` 是 `{Param137, +1.0}`，
       触发该表情反而把水印**关掉**；模型说明里的"水印按键默认打开"指的正是"水印可见"。
       → 默认状态设为隐藏水印，不要照字面把"关闭水印"实现成置 0。*/
    void setWatermarkVisible(bool visible);

    /*水印参数 ID（由模型目录里的 *水印*.exp3.json 推出，如 Param137）。置 0 才能关掉水印。*/
    QString watermarkParamId() const;

    bool isLoaded() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif // LIVE2DOFFSCREENRENDERER_H
