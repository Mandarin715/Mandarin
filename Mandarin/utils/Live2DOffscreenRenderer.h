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

    /*人物可见范围（全部在**绘制输出空间**，也叫 NDC：画布横竖都映射到 [-1, 1]，
      即"满画布"= 2）。

      用输出空间而不是模型画布像素：本模型的模型画布是 1x1（缩放 2/H_m = 2），
      人物在画布坐标里只占零点几个像素，取整会把它毁成 1x1、缩放因子算错。

      ⚠️ 关于"要不要再乘画布宽高比"（踩过的坑，写在这里免得再踩）：
      这里量到的是**探针帧**（必须是 1:1 正方形）里的范围。正方形帧下输出空间到帧像素的
      映射是各向同性的，所以这份范围就是人物**没有被任何东西拉伸**时的真实形状。
      曾经的做法是"再乘画布高/画布宽"再交给调用方当目标宽高比 —— 那是错的：那个换算想
      补偿的"投影分支各向异性"其实早被后面的等比 Scale 覆盖掉了（CubismMatrix44::Scale
      是直接赋值 _tr[0]/_tr[5]），于是这个换算凭空把宽高比抬了一截，画布随之被定得过宽。
      现在的约定是：**这里给什么就是什么，调用方不再做任何比例换算。**

      - boundsAspect：可见范围宽/高 = 人物真实宽高比（画布就该取这个宽高比）。
      - spanX / spanY：可见范围在输出空间的宽/高（满画布 = 2）。目标占比 ratio 对应的
        显示缩放是 fit = 2 * ratio / span（两个方向各算一次，见 setDisplayRatios）。
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

    /*测量模式开关：打开后用**固定的等比变换**渲染（人物缩到画布中央的一半），
      取样之间不会因为"上一次量到多少"而改变变换。

      为什么必须这样：探针的意义是量出"人物在一段时间里占过的最大范围"，这要求所有取样
      在**同一个坐标系**里。以前探针帧的变换是由上一次测量结果推出来的，而并集又喂给下一次
      测量 —— 测量基准被自己的结果改写，并集成了"不同缩放下的框的并集"。实测 atri：
      并集宽高比被抬到 0.498（真实 0.436，虚高 14%），纵向更是顶到画布边缘饱和成 2.0。
      画布宽度正是按这个虚高的宽高比定的，于是画布比人物宽了 2.3 倍。*/
    void setMeasureMode(bool on);

    /*量出 probeFrame（在测量模式下渲染的帧）里人物可见部分的输出空间范围，
      并**并进**已累计的范围。返回本次的测量值（已换算回"自然缩放"下的输出空间）。*/
    FigureMetrics probeFigureMetrics(const QImage &probeFrame);

    /*已累计的人物可见范围（输出空间，**不做任何画布比例换算**）。
      未做测量时 valid=false。*/
    FigureMetrics figureMetrics() const;

    /*设定人物在画布 x/y 两个方向各占的比例（0~1]。

      ⚠️ 为什么两个方向要**分别**缩放，而不是只给一个等比因子：
      输出空间到帧像素的映射本身是各向异性的（x 乘 W/2、y 乘 H/2）。只给一个等比因子时，
      人物的像素宽高比 = 模型宽高比 × 画布宽高比 —— 画布越宽人物越胖、越窄越瘦，
      实测 atri 在 779x938 的画布上被横向压扁 17%（0.436 → 0.355）。
      两个方向分别缩放正好抵消这个各向异性：人物的像素宽高比变成画布宽高比、与画布尺寸
      无关；只要画布宽高比取人物真实宽高比（figureMetrics().boundsAspect），
      人物就既不被拉伸、又在四个方向留出一样宽的余量。*/
    void setDisplayRatios(float widthRatio, float heightRatio);

    /*把"人物的完整活动范围"告诉渲染器：传**输出空间**里的浮点范围
      （画布横竖都映射到 [-1, 1]，即 figureMetrics() 返回的 span/位置）。

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
