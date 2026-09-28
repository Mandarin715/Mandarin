#include "Live2DOffscreenRenderer.h"

#include "Live2DCubismRuntime.h"
#include "Live2DModelInfo.h"

#include <CubismFramework.hpp>
#include <CubismModelSettingJson.hpp>
#include <Id/CubismIdManager.hpp>
#include <Math/CubismMatrix44.hpp>
#include <Math/CubismModelMatrix.hpp>
#include <Model/CubismModel.hpp>
#include <Model/CubismUserModel.hpp>
#include <Physics/CubismPhysics.hpp>
#include <Rendering/OpenGL/CubismOffscreenManager_OpenGLES2.hpp>
#include <Rendering/OpenGL/CubismRenderTarget_OpenGLES2.hpp>
#include <Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QSurfaceFormat>
#include <QVector>

#include <GL/glew.h>

namespace Csm = Live2D::Cubism::Framework;

namespace
{
/*该模型水印参数的**反相**取值（实测 + 目视确认，见头文件说明）：
  0 = 水印可见，1 = 水印隐藏。别把它当成普通的"0 就是关"。*/
constexpr float kWatermarkVisibleValue = 0.0f;
constexpr float kWatermarkHiddenValue = 1.0f;

/*把文件读成字节。**唯一**的读盘入口：QFile 走 Windows 宽字符 API，中文路径没问题。*/
bool readAllBytes(const QString &path, QByteArray *out)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    *out = file.readAll();
    return !out->isEmpty();
}

/*把 QImage 上传成 GL 贴图。Cubism 的贴图坐标已按 GL 约定（原点左下）生成，
  所以这里保持原始行序，不做垂直翻转。

  ⚠️ 必须生成 mipmap：框架在每次绘制时会把贴图的 MIN_FILTER 设成
  GL_LINEAR_MIPMAP_LINEAR（CubismShader_OpenGLES2.cpp 内），如果贴图没有 mipmap
  就是"不完整纹理"，GL 规定采样返回 (0,0,0,1) —— 表现为**形状完全正确但纯黑的剪影**，
  极难从现象反推。官方 LAppTextureManager 同样在 glTexImage2D 后调了 glGenerateMipmap。*/
GLuint uploadTexture(const QImage &image)
{
    if (image.isNull())
        return 0;

    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    GLuint textureId = 0;
    glGenTextures(1, &textureId);
    glBindTexture(GL_TEXTURE_2D, textureId);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba.width(), rgba.height(), 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, rgba.constBits());
    if (GLEW_VERSION_3_0 || GLEW_ARB_framebuffer_object)
    {
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    else
    {
        qWarning("[Live2D] 无 glGenerateMipmap 可用，贴图将因缺少 mipmap 被采样为不透明黑");
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return textureId;
}

/*离屏渲染只需要 CubismUserModel 提供的基础能力：模型、渲染器、物理。
  没有表情与动作管理器参与 —— model3.json 里本来就没有 Expressions/Motions 数组
  （方案实测结论），这也正是"表情动作须外部加载、模型目录只读"的原因。*/
class OffscreenUserModel : public Csm::CubismUserModel
{
  public:
    ~OffscreenUserModel() override
    {
        for (GLuint textureId : m_textureIds)
        {
            if (textureId != 0)
                glDeleteTextures(1, &textureId);
        }
        m_textureIds.clear();
        // _physics 的所有权归本类：CubismUserModel 不负责删除它
        if (_physics != nullptr)
        {
            Csm::CubismPhysics::Delete(_physics);
            _physics = nullptr;
        }
    }

    /*按 model3.json 装载：moc3（内存）→ 渲染器 → 贴图（自己上传并绑定）→ 物理（内存）*/
    bool setup(const QString &modelDir, const QByteArray &model3Bytes, Csm::csmUint32 width,
               Csm::csmUint32 height, QString *error)
    {
        m_modelDir = modelDir;
        m_setting = std::make_unique<Csm::CubismModelSettingJson>(
            reinterpret_cast<const Csm::csmByte *>(model3Bytes.constData()),
            static_cast<Csm::csmSizeInt>(model3Bytes.size()));

        // 1) moc3：全程内存，路径不进 SDK
        QByteArray mocBytes;
        const QString mocPath = joinPath(modelDir, QString::fromUtf8(m_setting->GetModelFileName()));
        if (!readAllBytes(mocPath, &mocBytes))
        {
            if (error)
                *error = QStringLiteral("读不到 moc3：%1").arg(mocPath);
            return false;
        }
        LoadModel(reinterpret_cast<const Csm::csmByte *>(mocBytes.constData()),
                  static_cast<Csm::csmSizeInt>(mocBytes.size()));
        if (_model == nullptr)
        {
            if (error)
                *error = QStringLiteral("CubismUserModel::LoadModel 失败（moc3 版本或内容异常）");
            return false;
        }

        // 2) 渲染器（OpenGL 后端由 Framework 静态库里的 CubismRenderer::Create 决定）
        CreateRenderer(width, height);

        // 3) 贴图：自己解码 + 上传 + 按索引绑定
        const Csm::csmInt32 textureCount = m_setting->GetTextureCount();
        for (Csm::csmInt32 index = 0; index < textureCount; ++index)
        {
            const QString textureName = QString::fromUtf8(m_setting->GetTextureFileName(index));
            if (textureName.isEmpty())
                continue;
            const QString texturePath = joinPath(modelDir, textureName);
            const QImage textureImage(texturePath);
            if (textureImage.isNull())
            {
                if (error)
                    *error = QStringLiteral("贴图加载失败：%1").arg(texturePath);
                return false;
            }
            const GLuint textureId = uploadTexture(textureImage);
            if (textureId == 0)
            {
                if (error)
                    *error = QStringLiteral("贴图上传失败：%1").arg(texturePath);
                return false;
            }
            m_textureIds.append(textureId);
            GetRenderer<Csm::Rendering::CubismRenderer_OpenGLES2>()->BindTexture(
                static_cast<Csm::csmUint32>(index), textureId);
        }
        // 直通 alpha（非预乘），与 QImage::Format_RGBA8888 的语义一致
        GetRenderer<Csm::Rendering::CubismRenderer_OpenGLES2>()->IsPremultipliedAlpha(false);

        // 4) 物理：同样走内存，位置在模型目录下的 physics3.json
        const QString physicsName = QString::fromUtf8(m_setting->GetPhysicsFileName());
        if (!physicsName.isEmpty())
        {
            QByteArray physicsBytes;
            const QString physicsPath = joinPath(modelDir, physicsName);
            if (readAllBytes(physicsPath, &physicsBytes))
            {
                _physics = Csm::CubismPhysics::Create(
                    reinterpret_cast<const Csm::csmByte *>(physicsBytes.constData()),
                    static_cast<Csm::csmSizeInt>(physicsBytes.size()));
            }
        }
        return true;
    }

    /*一帧的参数推进。顺序与官方样例一致，且**参数覆盖必须夹在中间**：
       LoadParameters() 会把上一次 SaveParameters() 的值还原回来，
       所以覆盖值只能在它之后、SaveParameters() 之前施加，否则第一帧就被冲掉。*/
    void tick(float deltaSeconds)
    {
        if (_model == nullptr)
            return;
        _model->LoadParameters();
        for (auto it = m_parameterOverrides.constBegin();
             it != m_parameterOverrides.constEnd(); ++it)
        {
            const Csm::csmString id(it.key().toUtf8().constData());
            _model->SetParameterValue(Csm::CubismFramework::GetIdManager()->RegisterId(id),
                                      it.value());
        }
        if (_physics != nullptr)
            _physics->Evaluate(_model, deltaSeconds);
        _model->SaveParameters();
        _model->Update();
    }

    void setParameter(const QString &parameterId, float value)
    {
        if (parameterId.isEmpty())
            return;
        m_parameterOverrides.insert(parameterId, value);
    }

    float parameterValue(const QString &parameterId) const
    {
        if (_model == nullptr || parameterId.isEmpty())
            return 0.0f;
        const Csm::csmString id(parameterId.toUtf8().constData());
        return _model->GetParameterValue(Csm::CubismFramework::GetIdManager()->RegisterId(id));
    }

    /*按样例的投影规则把模型画到当前绑定 FBO 上。*/
    void drawWithProjection(Csm::CubismMatrix44 &projection)
    {
        if (_model == nullptr)
            return;
        // _renderer 在 CubismUserModel 里是 private，只能经 GetRenderer<>() 取
        Csm::Rendering::CubismRenderer_OpenGLES2 *renderer =
            GetRenderer<Csm::Rendering::CubismRenderer_OpenGLES2>();
        if (renderer == nullptr)
            return;
        projection.MultiplyByMatrix(GetModelMatrix());
        renderer->SetMvpMatrix(&projection);
        renderer->DrawModel();
    }

    Csm::CubismModel *model() const { return _model; }

    /*从**目录扫描**推出水印参数 ID，不硬编码 Param137。

      为什么不能走 setting->GetExpressionFileName()：实测该模型的 model3.json 里
      **没有 Expressions 数组**（表情文件在磁盘上但未接线），GetExpressionCount() 恒为 0。
      这正是方案里"模型目录只读、表情按路径外部加载"那条硬约束的由来。
      文件路径匹配而不是靠 setting，也顺便满足"禁二改"（不改模型文件）。*/
    QString watermarkParamId() const
    {
        QDir dir(m_modelDir);
        const QStringList candidates =
            dir.entryList({QStringLiteral("*水印*.exp3.json")}, QDir::Files);
        for (const QString &fileName : candidates)
        {
            QByteArray bytes;
            if (!readAllBytes(joinPath(m_modelDir, fileName), &bytes))
                continue;
            const QString paramId = Live2DModelInfo::expressionParamId(bytes);
            if (!paramId.isEmpty())
                return paramId;
        }
        return QString();
    }

  private:
    static QString joinPath(const QString &dir, const QString &name)
    {
        if (dir.endsWith(QLatin1Char('/')))
            return dir + name;
        return dir + QLatin1Char('/') + name;
    }

    QString m_modelDir;
    std::unique_ptr<Csm::CubismModelSettingJson> m_setting;
    QVector<GLuint> m_textureIds;
    QHash<QString, float> m_parameterOverrides;
};
} // namespace

struct Live2DOffscreenRenderer::Impl
{
    QOpenGLContext *context = nullptr;
    QOffscreenSurface *surface = nullptr;
    std::unique_ptr<OffscreenUserModel> model;
    Csm::Rendering::CubismRenderTarget_OpenGLES2 renderTarget;
    bool glReady = false;
    QSize targetSize;
    QElapsedTimer clock;

    bool makeCurrent()
    {
        if (context == nullptr || surface == nullptr)
            return false;
        return context->makeCurrent(surface);
    }

    /*建离屏 GL 上下文。这一步不需要任何窗口，是全无头环境也能跑的前提。*/
    bool initializeGl(QString *error)
    {
        QSurfaceFormat format;
        format.setRenderableType(QSurfaceFormat::OpenGL);
        format.setProfile(QSurfaceFormat::NoProfile); // Cubism 走 ES2 风格接口，不要求 core profile
        // 用 2.0：实测请求 3.0 会让 Cubism 的绘制阶段报 GL_INVALID_OPERATION 且什么都画不出来。
        // mipmap 用 glGenerateMipmap，它由 ARB_framebuffer_object 扩展提供（NVIDIA 在 2.0 上下文里也有）。
        format.setVersion(2, 0);
        format.setAlphaBufferSize(8);
        format.setSamples(0);

        surface = new QOffscreenSurface();
        surface->setFormat(format);
        surface->create();
        if (!surface->isValid())
        {
            if (error)
                *error = QStringLiteral("QOffscreenSurface 创建失败");
            return false;
        }

        context = new QOpenGLContext();
        context->setFormat(format);
        if (!context->create())
        {
            if (error)
                *error = QStringLiteral("QOpenGLContext 创建失败");
            return false;
        }
        if (!makeCurrent())
        {
            if (error)
                *error = QStringLiteral("makeCurrent 失败");
            return false;
        }

        // GLEW 必须在上下文 current 之后初始化，否则函数指针全是空
        const GLenum glewStatus = glewInit();
        if (glewStatus != GLEW_OK)
        {
            if (error)
                *error = QStringLiteral("glewInit 失败：%1")
                             .arg(QString::fromUtf8(reinterpret_cast<const char *>(
                                 glewGetErrorString(glewStatus))));
            return false;
        }
        glReady = true;
        const GLubyte *version = glGetString(GL_VERSION);
        const GLubyte *renderer = glGetString(GL_RENDERER);
        qInfo("[Live2D] GL 上下文就绪：%s | %s | mipmap=%d",
              version ? reinterpret_cast<const char *>(version) : "?",
              renderer ? reinterpret_cast<const char *>(renderer) : "?",
              (GLEW_VERSION_3_0 || GLEW_ARB_framebuffer_object) ? 1 : 0);
        return true;
    }
};

Live2DOffscreenRenderer::Live2DOffscreenRenderer() : m_impl(std::make_unique<Impl>()) {}

Live2DOffscreenRenderer::~Live2DOffscreenRenderer()
{
    m_impl->model.reset(); // 先销毁模型（会删 GL 贴图），上下文还在
    if (m_impl->context != nullptr)
    {
        if (m_impl->surface != nullptr)
            m_impl->context->makeCurrent(m_impl->surface);
        m_impl->renderTarget.DestroyRenderTarget();
        m_impl->context->doneCurrent();
    }
    delete m_impl->context;
    delete m_impl->surface;
    Live2DCubismRuntime::release();
}

bool Live2DOffscreenRenderer::load(const QString &modelDir, const QString &modelJsonName,
                                   QString *error)
{
    if (!Live2DCubismRuntime::acquire(error))
        return false;

    if (!m_impl->glReady && !m_impl->initializeGl(error))
        return false;

    if (!m_impl->makeCurrent())
    {
        if (error)
            *error = QStringLiteral("makeCurrent 失败（load）");
        return false;
    }

    QByteArray model3Bytes;
    const QString model3Path = modelDir + QLatin1Char('/') + modelJsonName;
    if (!readAllBytes(model3Path, &model3Bytes))
    {
        if (error)
            *error = QStringLiteral("读不到 model3.json：%1").arg(model3Path);
        return false;
    }

    m_impl->model = std::make_unique<OffscreenUserModel>();
    // 先按 1x1 建渲染器，真正的目标尺寸在 renderFrame 里按需重建
    if (!m_impl->model->setup(modelDir, model3Bytes, 1, 1, error))
    {
        m_impl->model.reset();
        return false;
    }
    m_impl->clock.start();

    // 默认隐藏水印。注意是置 **1**，不是 0（该参数反相，见 kWatermarkHiddenValue 说明）。
    const QString watermarkParamId = m_impl->model->watermarkParamId();
    if (watermarkParamId.isEmpty())
    {
        qWarning("[Live2D] 未能在模型目录里找到水印表情，无法确认水印状态");
    }
    else
    {
        m_impl->model->setParameter(watermarkParamId, kWatermarkHiddenValue);
    }
    return true;
}

QImage Live2DOffscreenRenderer::renderFrame(const QSize &size)
{
    if (m_impl->model == nullptr || !m_impl->glReady)
        return QImage();
    if (size.width() <= 0 || size.height() <= 0)
        return QImage();
    if (!m_impl->makeCurrent())
        return QImage();

    const Csm::csmUint32 width = static_cast<Csm::csmUint32>(size.width());
    const Csm::csmUint32 height = static_cast<Csm::csmUint32>(size.height());

    // 目标尺寸变了就重建 FBO
    if (!m_impl->renderTarget.IsValid() || m_impl->targetSize != size)
    {
        m_impl->renderTarget.DestroyRenderTarget();
        if (!m_impl->renderTarget.CreateRenderTarget(width, height))
            return QImage();
        m_impl->targetSize = size;
        m_impl->model->SetRenderTargetSize(width, height);
    }

    // 遮罩用的离屏管理必须包住整帧
    Csm::Rendering::CubismOffscreenManager_OpenGLES2::GetInstance()->BeginFrameProcess();

    m_impl->renderTarget.BeginDraw();
    m_impl->renderTarget.Clear(0.0f, 0.0f, 0.0f, 0.0f);
    glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    // 混合状态必须由调用方准备好：CubismRenderer_OpenGLES2::PreDraw 只保证 BLEND 处于启用，
    // 混合函数是它不管的。配合 IsPremultipliedAlpha(false) 用直通 alpha 公式。
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    Csm::CubismMatrix44 projection;
    projection.LoadIdentity();
    Csm::CubismModel *model = m_impl->model->model();
    if (model != nullptr && model->GetCanvasWidth() > 1.0f
        && width < height) // 横长模型放进竖长画布
    {
        m_impl->model->GetModelMatrix()->SetWidth(2.0f);
        projection.Scale(1.0f, static_cast<Csm::csmFloat32>(width)
                                  / static_cast<Csm::csmFloat32>(height));
    }
    else
    {
        projection.Scale(static_cast<Csm::csmFloat32>(height)
                             / static_cast<Csm::csmFloat32>(width),
                         1.0f);
    }

    const float deltaSeconds =
        static_cast<float>(m_impl->clock.restart()) / 1000.0f; // 首帧即建立时间基准
    m_impl->model->tick(deltaSeconds);
    m_impl->model->drawWithProjection(projection);

    m_impl->renderTarget.EndDraw();
    Csm::Rendering::CubismOffscreenManager_OpenGLES2::GetInstance()->EndFrameProcess();

    // 读回：重新绑定该 FBO 后用 glReadPixels。
    // 不用 glGetTexImage —— 它在部分 NVIDIA 驱动上会走到有问题的路径（实测崩在 DrvPresentBuffers）。
    QVector<uchar> pixels(static_cast<int>(width) * static_cast<int>(height) * 4);
    m_impl->renderTarget.BeginDraw();
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA,
                 GL_UNSIGNED_BYTE, pixels.data());
    glFinish();
    m_impl->renderTarget.EndDraw();
    if (const GLenum glError = glGetError(); glError != GL_NO_ERROR)
        qWarning("[Live2D] 读回后 glGetError=0x%04X", glError);

    // GL 原点在左下，QImage 在左上，垂直镜像
    QImage image(reinterpret_cast<const uchar *>(pixels.constData()), static_cast<int>(width),
                 static_cast<int>(height), QImage::Format_RGBA8888);
    return image.mirrored(false, true);
}

void Live2DOffscreenRenderer::setParameter(const QString &parameterId, float value)
{
    if (m_impl->model != nullptr)
        m_impl->model->setParameter(parameterId, value);
}

float Live2DOffscreenRenderer::parameterValue(const QString &parameterId) const
{
    if (m_impl->model == nullptr)
        return 0.0f;
    return m_impl->model->parameterValue(parameterId);
}

void Live2DOffscreenRenderer::setWatermarkVisible(bool visible)
{
    if (m_impl->model == nullptr)
        return;
    const QString paramId = m_impl->model->watermarkParamId();
    if (!paramId.isEmpty())
    {
        m_impl->model->setParameter(paramId,
                                    visible ? kWatermarkVisibleValue : kWatermarkHiddenValue);
    }
}

QString Live2DOffscreenRenderer::watermarkParamId() const
{
    if (m_impl->model == nullptr)
        return QString();
    return m_impl->model->watermarkParamId();
}

bool Live2DOffscreenRenderer::isLoaded() const
{
    return m_impl->model != nullptr;
}
