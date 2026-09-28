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
