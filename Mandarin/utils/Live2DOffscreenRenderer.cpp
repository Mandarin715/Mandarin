#include "Live2DOffscreenRenderer.h"

#include "Live2DCubismRuntime.h"
#include "Live2DModelInfo.h"

#include <CubismFramework.hpp>
#include <CubismModelSettingJson.hpp>
// 直接量 drawable 顶点要用到 Core::csmVector2，由这个 C++ 包装头提供命名空间
// （文档明确要求用它，而不是直接 include Live2DCubismCore.h）
#include <Live2DCubismCore.hpp>
#include <Effect/CubismBreath.hpp>
#include <Effect/CubismEyeBlink.hpp>
#include <Id/CubismIdManager.hpp>
#include <Math/CubismMatrix44.hpp>
#include <Math/CubismModelMatrix.hpp>
#include <Model/CubismModel.hpp>
#include <Model/CubismUserModel.hpp>
#include <Motion/ACubismMotion.hpp>
#include <Motion/CubismBreathUpdater.hpp>
#include <Motion/CubismEyeBlinkUpdater.hpp>
#include <Motion/CubismMotion.hpp>
#include <Motion/CubismMotionJson.hpp>
#include <Motion/CubismPhysicsUpdater.hpp>
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
  没有表情管理器参与 —— model3.json 里本来就没有 Expressions 数组
  （方案实测结论），这也正是"表情动作须外部加载、模型目录只读"的原因。

  **参数驱动器全部走 CubismUpdateScheduler**（5-r.x 的标准做法）：
  呼吸 / 眨眼 / 物理各注册一个 Updater，每帧由 OnLateUpdate 统一驱动。
  不在这里另外直接调 _physics->Evaluate() —— 那会和 PhysicsUpdater 重复求值一次。*/
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
        /*呼吸/眨眼/物理的**所有权在基类**：CubismUserModel::~CubismUserModel() 会
          CubismEyeBlink::Delete(_eyeBlink) / CubismBreath::Delete(_breath) /
          CubismPhysics::Delete(_physics)（都是判空安全）。子类再删一次就是 double free，
          所以这里只把指针置空，让基类去释放。*/
        _physics = nullptr;
        _breath = nullptr;
        _eyeBlink = nullptr;
        m_idleMotion = nullptr; // 由 _motionManager 持有，基类析构时随管理器释放
    }

    /*按 model3.json 装载：moc3（内存）→ 渲染器 → 贴图（自己上传并绑定）→ 物理（内存）
      → 参数驱动器（呼吸/眨眼/待机动作）*/
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

        // 5) 参数驱动器：没有它们模型就是一张静止的画（呼吸/眨眼/物理摆动都不会发生）
        setupBreath();
        setupEyeBlink();
        setupPhysicsUpdater();
        setupIdleMotion();
        // 注册完必须排一次序（按 executionOrder），5-r.x 只保证"首次 OnLateUpdate 时自动排"，
        // 显式排一次让顺序在调试时确定可见。
        _updateScheduler.SortUpdatableList();
        // 参数初值先存一次，之后每帧 LoadParameters() 才有东西可还原
        _model->SaveParameters();
        return true;
    }

    /*一帧的参数推进。顺序对齐官方样例（LAppModel::Update）：

      LoadParameters() → 动作曲线 → 参数覆盖 → SaveParameters()
      → OnLateUpdate()（眨眼/呼吸/物理）→ Update()

    **参数覆盖夹在中间**：LoadParameters() 会把上一次 SaveParameters() 的值还原回来，
    所以覆盖值只能在它之后施加，否则第一帧就被冲掉。
    **OnLateUpdate() 必须在 SaveParameters() 之后、Update() 之前**：驱动器的写入属于
    "本帧最终值"，不能被下一帧的 LoadParameters() 当成基础状态保存下来（否则会自我叠加）。*/
    void tick(float deltaSeconds)
    {
        if (_model == nullptr)
            return;
        _model->LoadParameters();

        // 动作：待机动作每帧推进，写入的参数作为本帧基线
        _motionUpdated = false;
        if (_motionManager != nullptr && m_idleMotion != nullptr)
        {
            _motionUpdated = _motionManager->UpdateMotion(_model, deltaSeconds);
            /*动作已结束后重新起播（Loop=true 时它自己会一直循环，这里只是兜底：
              非循环动作播完就停住，重播一次比僵在原姿势好）。*/
            if (!_motionUpdated && _motionManager->IsFinished())
                _motionManager->StartMotion(m_idleMotion, false);
        }

        // 参数覆盖：盖在动作结果之上，优先级最高
        for (auto it = m_parameterOverrides.constBegin();
             it != m_parameterOverrides.constEnd(); ++it)
        {
            const Csm::csmString id(it.key().toUtf8().constData());
            _model->SetParameterValue(Csm::CubismFramework::GetIdManager()->RegisterId(id),
                                      it.value());
        }

        _model->SaveParameters();

        // 眨眼 / 呼吸 / 物理统一由调度器驱动（这里**不能**再单独调 _physics->Evaluate）
        _updateScheduler.OnLateUpdate(_model, deltaSeconds);

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

    /*模型矩阵（"模型画布像素 → 输出坐标"）的**无状态**算法。

      ⚠️ 必须自己算，不能直接读 GetModelMatrix()：
      那是跨帧复用的对象，renderFrame 在窄画布分支里会反复对它调 SetWidth(2.0f)，
      而 CubismModelMatrix::SetWidth 内部是 Scale(w / _width) —— 累积乘法。
      于是读到的矩阵一帧比一帧大（实测 atri：基准 0.487 → 真实帧里 3.87，差 8 倍），
      任何"按它反推尺寸"的算法都会算错，人物被画得极小。

      这里按 SDK 初始化的方式重建：
        CubismModelMatrix(W_m, H_m) → SetHeight(2.0f)，即缩放 2/H_m、无平移；
      再按 renderFrame 的投影分支（窄画布时把模型铺满宽度）补一次等比缩放。
      W_m/H_m 是模型的固有画布尺寸，只取决于 moc3，与目标画布无关。*/
    void buildModelTransform(Csm::CubismMatrix44 *out, Csm::csmUint32 frameWidth,
                             Csm::csmUint32 frameHeight)
    {
        out->LoadIdentity();
        if (_model == nullptr)
            return;

        const Csm::csmFloat32 canvasWidth = _model->GetCanvasWidth();
        const Csm::csmFloat32 canvasHeight = _model->GetCanvasHeight();
        if (canvasWidth <= 0.0f || canvasHeight <= 0.0f)
            return;

        Csm::csmFloat32 scale = 2.0f / canvasHeight;
        // 与 renderFrame 的分支保持一致：横长模型放进竖长画布时，模型铺满画布宽度
        if (frameWidth < frameHeight && canvasWidth > 1.0f)
            scale *= 2.0f / canvasWidth;
        out->Scale(scale, scale);
    }

    /*按样例的投影规则把模型画到当前绑定 FBO 上。

      除了样例那套投影，这里还要把人物**摆正**：模型画布里人物的活动范围本身是偏的
      （本模型明显偏右），只把画布放大是没用的 —— 人物像素位置与画布同比例放大，
      右边缘照样贴着画布。所以按无状态模型变换把人物缩到 displayHeightRatio 并居中。

      数学：p_N = 模型变换(模型画布像素)。先把活动范围中心搬到原点、再把尺寸压到
      ratio（宽高各算一次取较大者，保证两条边都装得下），于是人物落在画布中心、
      占画布高度的 ratio。displayHeightRatio=1.0 时只做居中，不缩小。*/
    void drawWithProjection(Csm::CubismMatrix44 &projection, Csm::csmUint32 frameWidth,
                            Csm::csmUint32 frameHeight)
    {
        if (_model == nullptr)
            return;
        // _renderer 在 CubismUserModel 里是 private，只能经 GetRenderer<>() 取
        Csm::Rendering::CubismRenderer_OpenGLES2 *renderer =
            GetRenderer<Csm::Rendering::CubismRenderer_OpenGLES2>();
        if (renderer == nullptr)
            return;

        Csm::CubismModelMatrix modelTransform;
        buildModelTransform(&modelTransform, frameWidth, frameHeight);

        if (m_figureSpan.valid)
        {
            const Csm::csmFloat32 centerNx = (m_figureSpan.minX + m_figureSpan.maxX) / 2.0f;
            const Csm::csmFloat32 centerNy = (m_figureSpan.minY + m_figureSpan.maxY) / 2.0f;
            const Csm::csmFloat32 spanNx = m_figureSpan.spanX();
            const Csm::csmFloat32 spanNy = m_figureSpan.spanY();

            const Csm::csmFloat32 ratio = m_displayHeightRatio;
            if (spanNx > 1e-6f && spanNy > 1e-6f)
            {
                /*注意顺序：Translate 直接写平移列、Scale 直接写缩放因子，
                  所以 Translate 之后再 Scale 得到的是"先平移再缩放"。
                  乘进投影后整体作用在模型变换的输出上。*/
                const Csm::csmFloat32 fit = std::max(ratio / spanNx, ratio / spanNy);
                projection.Translate(-centerNx, -centerNy);
                projection.Scale(fit, fit);
            }
        }

        // MultiplyByMatrix 是左乘：先"摆正"再乘模型变换 → 投影 × 摆正 × 模型
        projection.MultiplyByMatrix(&modelTransform);
        renderer->SetMvpMatrix(&projection);
        renderer->DrawModel();
    }

    /*把探针量到的人物可见范围（**输出空间**的浮点矩形）交给绘制阶段使用。

      用输出空间而不是模型画布像素，见 FigureSpan 的说明：本模型画布是 1x1，
      画布像素坐标下的人物范围会被取整毁掉。*/
    void setFigureSpanInOutputSpace(float minX, float minY, float maxX, float maxY)
    {
        m_figureSpan.unite(minX, minY, maxX, maxY);
    }

    void clearFigureSpan() { m_figureSpan = FigureSpan(); }

    /*人物高度占画布高度的比例（见 Live2DOffscreenRenderer::setDisplayHeightRatio）*/
    void setDisplayHeightRatio(float ratio) { m_displayHeightRatio = ratio; }

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

    /*呼吸：参数名沿用官方样例的那一套（ParamAngleX/Y/Z + ParamBodyAngleX + ParamBreath），
      这几条在 miku.cdi3.json 里都真实存在，模型自带呼吸曲线会跟着动。

      只注册**确实存在**的参数：GetParameterIndex 返回 -1 就跳过。
      为什么必须过滤：BreathParameterData 拿到不存在的 id 不会报错，只是安静地什么都不做 ——
      万一换模型，宁可少驱动几条，也不要留下一个"注册了却没效果"的隐形死角。*/
    void setupBreath()
    {
        _breath = Csm::CubismBreath::Create();

        Csm::csmVector<Csm::CubismBreath::BreathParameterData> breathParameters;
        addBreathParameter(breathParameters, "ParamAngleX", 0.0f, 15.0f, 6.5345f);
        addBreathParameter(breathParameters, "ParamAngleY", 0.0f, 8.0f, 3.5345f);
        addBreathParameter(breathParameters, "ParamAngleZ", 0.0f, 10.0f, 5.5345f);
        addBreathParameter(breathParameters, "ParamBodyAngleX", 0.0f, 4.0f, 15.5345f);
        addBreathParameter(breathParameters, "ParamBreath", 0.5f, 0.5f, 3.2345f);
        _breath->SetParameters(breathParameters);

        _updateScheduler.AddUpdatableList(CSM_NEW Csm::CubismBreathUpdater(*_breath));
    }

    void addBreathParameter(Csm::csmVector<Csm::CubismBreath::BreathParameterData> &out,
                            const char *parameterId, Csm::csmFloat32 offset,
                            Csm::csmFloat32 peak, Csm::csmFloat32 cycle)
    {
        Csm::CubismIdHandle id = Csm::CubismFramework::GetIdManager()->GetId(parameterId);
        if (id == nullptr || _model->GetParameterIndex(id) < 0)
            return;
        out.PushBack(Csm::CubismBreath::BreathParameterData(id, offset, peak, cycle, 0.5f));
    }

    /*眨眼：参数组直接取 model3.json 的 `Groups[Name=EyeBlink].Ids`
      （本模型是 ParamEyeROpen / ParamEyeLOpen），模型没声明就不注册。
      传 _motionUpdated 的引用是 5-r.x 的约定：动作**没有**更新参数的帧才让眨眼覆盖上去。*/
    void setupEyeBlink()
    {
        if (_model->GetParameterCount() <= 0
            || m_setting->GetEyeBlinkParameterCount() <= 0)
            return;
        _eyeBlink = Csm::CubismEyeBlink::Create(m_setting.get());
        if (_eyeBlink == nullptr)
            return;
        _updateScheduler.AddUpdatableList(
            CSM_NEW Csm::CubismEyeBlinkUpdater(_motionUpdated, *_eyeBlink));
    }

    /*物理也必须走调度器（PhysicsUpdater）；tick() 里不再直接 Evaluate —— 两边都做就是每帧算两次。*/
    void setupPhysicsUpdater()
    {
        if (_physics == nullptr)
            return;
        _updateScheduler.AddUpdatableList(CSM_NEW Csm::CubismPhysicsUpdater(*_physics));
    }

    /*待机动作：在模型目录里挑一个**真正能驱动身体**的 motion3.json。

      本模型的 model3.json **没有 Motions / Expressions 数组**，所以动作文件只能自己按
      路径从目录里找、自己读成字节、再用 LoadMotion 从内存建动作对象
      （与 moc3/物理一致：路径不进 SDK，中文路径安全）。

      为什么不能"随便挑一个 motion3.json 就播"（实测三台模型都踩过）：
        - `atri/model.motion3.json` 的 Meta 写着 Loop=true 但 **CurveCount=0** —— 播了等于没播；
        - `miku/Scene1.motion3.json` 的 5 条曲线驱动的是 Param16/45/63/126/70，
          也就是**大葱/比心/唱歌这类道具表情参数**，播起来是道具忽隐忽现，不是待机动作；
        - `樱花miku/Scene1.motion3.json` 同样只驱动 7 条道具曲线；
        - `atri/dec-l|dec-r.motion3.json` 只动 Param4..Param7 的装饰件。
      播这些比不播更糟。所以这里逐个解析曲线，只接受"动身体"的动作：
      曲线里必须有头部/身体角度或呼吸参数；只有道具/表情曲线的候选一律跳过。

      循环与否**从 motion3.json 自己的 Meta.Loop 读**，不从文件名猜
      （CubismMotion::Create 并不会把 Meta.Loop 应用到对象上 —— SDK 里那行是注释掉的 ——
      所以必须显式 SetLoop）。

      **优雅降级是一等公民**：一个合格的动作都没有就只靠呼吸/眨眼（+物理），
      打一条 qInfo 说明，不算错误。*/
    void setupIdleMotion()
    {
        QDir dir(m_modelDir);
        QStringList candidates = dir.entryList({QStringLiteral("*.motion3.json")}, QDir::Files);
        candidates.sort();
        if (candidates.isEmpty())
        {
            qInfo("[Live2D] 模型目录里没有 motion3.json，仅靠呼吸/眨眼驱动");
            return;
        }

        QString chosenPath;
        for (const QString &fileName : candidates)
        {
            const QString path = joinPath(m_modelDir, fileName);
            QByteArray bytes;
            if (!readAllBytes(path, &bytes))
                continue;

            Csm::CubismMotionJson json(
                reinterpret_cast<const Csm::csmByte *>(bytes.constData()),
                static_cast<Csm::csmSizeInt>(bytes.size()));
            if (!json.IsValid())
            {
                qInfo("[Live2D] 跳过（不是合法 motion3）：%s", qPrintable(fileName));
                continue;
            }
            const int curveCount = json.GetMotionCurveCount();
            if (curveCount <= 0)
            {
                // atri/model.motion3.json 就是这种空壳：Loop=true 但 0 条曲线
                qInfo("[Live2D] 跳过（0 条曲线，播了等于没播）：%s", qPrintable(fileName));
                continue;
            }
            if (!motionDrivesBody(json))
            {
                qInfo("[Live2D] 跳过（只驱动道具/表情参数，不是待机动作）：%s",
                      qPrintable(fileName));
                continue;
            }

            chosenPath = path;
            qInfo("[Live2D] 选中待机动作：%s（%d 条曲线，动身体）", qPrintable(fileName),
                  curveCount);
            break;
        }

        if (chosenPath.isEmpty())
        {
            qInfo("[Live2D] 没有可用的待机动作，仅靠呼吸/眨眼/物理驱动");
            return;
        }

        QByteArray motionBytes;
        if (!readAllBytes(chosenPath, &motionBytes))
            return;

        const bool shouldLoop = motionLoops(motionBytes);
        Csm::ACubismMotion *motion = LoadMotion(
            reinterpret_cast<const Csm::csmByte *>(motionBytes.constData()),
            static_cast<Csm::csmSizeInt>(motionBytes.size()), "idle");
        if (motion == nullptr)
        {
            qWarning("[Live2D] 待机动作解析失败：%s", qPrintable(chosenPath));
            return;
        }
        motion->SetLoop(shouldLoop);
        m_idleMotion = motion;

        if (_motionManager == nullptr)
            _motionManager = CSM_NEW Csm::CubismMotionManager();
        // autoDelete=false：这个动作实例要一直复用（循环播放），不能播完就被管理器删掉
        _motionManager->StartMotion(m_idleMotion, false);
        qInfo("[Live2D] 待机动作已装载：%s（Loop=%s，时长 %.3fs）", qPrintable(chosenPath),
              shouldLoop ? "true" : "false", motion->GetLoopDuration());
    }

    /*这个动作是否"动身体"：曲线里至少有一条打到头部/身体角度或呼吸参数。

      只认这些参数名（官方样例的待机曲线就是这一套，本模型 cdi3 里也都声明了）：
      ParamAngleX/Y/Z、ParamBodyAngleX/Z、ParamBreath。
      只驱动 Param**数字**（道具/表情）的动作会被判为不合格。*/
    static bool motionDrivesBody(const Csm::CubismMotionJson &json)
    {
        static const char *const kBodyParameters[] = {
            "ParamAngleX", "ParamAngleY", "ParamAngleZ",
            "ParamBodyAngleX", "ParamBodyAngleZ", "ParamBreath",
        };

        const int curveCount = json.GetMotionCurveCount();
        for (int index = 0; index < curveCount; ++index)
        {
            // 只关心 Parameter 曲线；Part/Model 曲线（不透明度之类）不算"动身体"
            const Csm::csmChar *target = json.GetMotionCurveTarget(index);
            if (target == nullptr || strcmp(target, "Parameter") != 0)
                continue;
            const Csm::CubismIdHandle curveId = json.GetMotionCurveId(index);
            if (curveId == nullptr)
                continue;
            const Csm::csmString curveName = curveId->GetString();
            for (const char *bodyId : kBodyParameters)
            {
                if (curveName == bodyId)
                    return true;
            }
        }
        return false;
    }

    /*读 motion3.json 的 Meta.Loop。用 SDK 自己的解析器而不是自己拼 JSON，
      顺便也让 SDK 先校验一遍这份文件是不是合法的 motion3。*/
    static bool motionLoops(const QByteArray &motionBytes)
    {
        Csm::CubismMotionJson json(reinterpret_cast<const Csm::csmByte *>(motionBytes.constData()),
                                   static_cast<Csm::csmSizeInt>(motionBytes.size()));
        if (!json.IsValid())
            return true; // 解析不了就按循环处理（待机动作循环是更安全的默认）
        return json.IsMotionLoop();
    }

    QString m_modelDir;
    std::unique_ptr<Csm::CubismModelSettingJson> m_setting;
    QVector<GLuint> m_textureIds;
    QHash<QString, float> m_parameterOverrides;

    Csm::ACubismMotion *m_idleMotion = nullptr; // 待机动作；所有权在 _motionManager
    /*眨眼 Updater 拿的是这个标志的**引用**：动作没更新参数的那些帧才让眨眼生效。
      成员不能挪位置/不能是临时量，否则引用悬空。*/
    Csm::csmBool _motionUpdated = false;

    /*人物可见范围（**输出空间**浮点，画布映射到 [-0.5, 0.5]）。

      ⚠️ 不能存成模型画布像素的 QRect：本模型的模型画布是 1x1（缩放 2/H_m = 2），
      人物在画布坐标里只占零点几个像素，取整后必然退化成 1x1，缩放因子随之算错
      （实测人物被缩成画布的 13%）。输出空间的浮点值不受画布尺寸取整影响。*/
    struct FigureSpan
    {
        float minX = 0.0f;
        float minY = 0.0f;
        float maxX = 0.0f;
        float maxY = 0.0f;
        bool valid = false;

        float spanX() const { return maxX - minX; }
        float spanY() const { return maxY - minY; }
        void unite(float x0, float y0, float x1, float y1)
        {
            if (!valid)
            {
                minX = x0;
                minY = y0;
                maxX = x1;
                maxY = y1;
                valid = true;
                return;
            }
            minX = std::min(minX, x0);
            minY = std::min(minY, y0);
            maxX = std::max(maxX, x1);
            maxY = std::max(maxY, y1);
        }
    };
    FigureSpan m_figureSpan;
    float m_displayHeightRatio = 1.0f; // 默认 1.0 = 保持原有"铺满画布高度"的行为
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

    /*人物可见范围（**输出空间**浮点，画布映射到 [-0.5, 0.5]）。

      ⚠️ 不能存成模型画布像素的 QRect：本模型的模型画布是 1x1（缩放 2/H_m = 2），
      人物在画布坐标里只占零点几个像素，取整后必然退化成 1x1，缩放因子随之算错
      （实测人物被缩成画布的 13%）。输出空间的浮点值不受画布尺寸取整影响。*/
    struct FigureSpan
    {
        float minX = 0.0f;
        float minY = 0.0f;
        float maxX = 0.0f;
        float maxY = 0.0f;
        bool valid = false;

        float spanX() const { return maxX - minX; }
        float spanY() const { return maxY - minY; }
        void unite(float x0, float y0, float x1, float y1)
        {
            if (!valid)
            {
                minX = x0;
                minY = y0;
                maxX = x1;
                maxY = y1;
                valid = true;
                return;
            }
            minX = std::min(minX, x0);
            minY = std::min(minY, y0);
            maxX = std::max(maxX, x1);
            maxY = std::max(maxY, y1);
        }
    };
    FigureSpan figureSpan;
    float displayHeightRatio = 1.0f; // 人物高度占画布高度的比例

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
    /*⚠️ 这里**不能再动 GetModelMatrix()**：它是跨帧复用的对象，SetWidth(2.0f) 内部是
      Scale(w/_width)（累积乘法），会导致模型矩阵一帧比一帧大（实测 atri 差 8 倍），
      而 drawable 顶点位置是用它换算的。
      现在模型变换由 drawWithProjection 按帧无状态重建（buildModelTransform），
      这里只负责投影的等比缩放。*/
    if (model != nullptr && model->GetCanvasWidth() > 1.0f
        && width < height) // 横长模型放进竖长画布
    {
        projection.Scale(1.0f, static_cast<Csm::csmFloat32>(width)
                                  / static_cast<Csm::csmFloat32>(height));
    }
    else
    {
        projection.Scale(static_cast<Csm::csmFloat32>(height)
                             / static_cast<Csm::csmFloat32>(width),
                         1.0f);
    }

    /*单帧时间步长夹取。

      为什么必须夹：这个 delta 直接喂给呼吸/眨眼/物理。load() 之后到第一帧渲染之间
      可能隔着几百毫秒（窗口还没映射、dpr 未定），甚至测试里会隔几秒 —— 那不是"一帧"，
      而是把物理积分一次推进几百毫秒，头发/衣服会被"踹"到极端位置（实测首帧包裹盒
      比之后大 40%，还会留下持续的摆动）。
      不夹的话：首帧必抖一下，人物包围盒测量也随之失真（探针量到的活动范围与真实帧对不上，
      画布余量算不准）。所以按一个正常的帧间隔上限夹住，超大间隔当"一帧"处理。*/
    constexpr float kMaxFrameDeltaSeconds = 0.1f; // 10fps 以下就当一帧
    const float deltaSeconds = std::min(
        static_cast<float>(m_impl->clock.restart()) / 1000.0f, kMaxFrameDeltaSeconds); // 首帧即建立时间基准
    m_impl->model->tick(deltaSeconds);
    m_impl->model->drawWithProjection(projection, width, height);

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

Live2DOffscreenRenderer::FigureMetrics
Live2DOffscreenRenderer::probeFigureMetrics(const QImage &probeFrame)
{
    FigureMetrics metrics;
    if (m_impl->model == nullptr || probeFrame.isNull())
        return metrics;

    // 1) 探针帧里的 alpha 包围盒（与 CharacterWindow 同一口径：alpha > 32）
    const QImage rgba = probeFrame.convertToFormat(QImage::Format_RGBA8888);
    int minX = rgba.width();
    int minY = rgba.height();
    int maxX = -1;
    int maxY = -1;
    for (int y = 0; y < rgba.height(); ++y)
    {
        const uchar *line = rgba.constScanLine(y);
        for (int x = 0; x < rgba.width(); ++x)
        {
            if (line[x * 4 + 3] <= 32)
                continue;
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
        }
    }
    if (maxX < minX || maxY < minY)
        return metrics;

    /*2) 帧像素 → 模型变换的**输出坐标**。

      renderFrame 的投影是 Scale(W/2, H/2) + Translate(W/2, H/2)（再乘分支的等比缩放，
      探针固定 1:1 时分支缩放为 1）。所以帧像素 p 对应的输出坐标是 (2p - W)/W。
      **这一步不能省**：模型变换只负责"模型画布 → 输出"，从输出到帧像素是投影的缩放；
      少了它量出来的尺寸会差一个分辨率倍数，缩放因子随之算成天文数字。
      这两个 span 就是"人物占输出空间的多少"，是换算目标占比的依据。*/
    const auto toOutput = [](double pixel, int extent) {
        return (pixel - extent / 2.0) / (extent / 2.0);
    };
    const float oMinX = static_cast<float>(toOutput(minX, rgba.width()));
    const float oMaxX = static_cast<float>(toOutput(maxX + 1, rgba.width()));
    const float oMinY = static_cast<float>(toOutput(minY, rgba.height()));
    const float oMaxY = static_cast<float>(toOutput(maxY + 1, rgba.height()));

    metrics.spanX = oMaxX - oMinX;
    metrics.spanY = oMaxY - oMinY;
    metrics.minX = oMinX;
    metrics.minY = oMinY;
    metrics.maxX = oMaxX;
    metrics.maxY = oMaxY;
    metrics.valid = true;
    if (metrics.spanY > 1e-6f)
        metrics.boundsAspect = metrics.spanX / metrics.spanY;

    /*3) 把范围并进调度器的 FigureSpan（输出空间，浮点）。

      **换算到模型画布像素再存是不行的**：本模型的模型画布是 1x1，人物在画布坐标里
      只占零点几个像素，QRect 一取整就退化成 1x1，缩放因子随之算错（实测人物被缩成
      画布的 13%）。绘制时直接在输出空间用这份浮点范围，画布尺寸怎么取整都不受影响。*/
    m_impl->figureSpan.unite(oMinX, oMinY, oMaxX, oMaxY);
    if (m_impl->model != nullptr)
        m_impl->model->setFigureSpanInOutputSpace(oMinX, oMinY, oMaxX, oMaxY);
    return metrics;
}

Live2DOffscreenRenderer::FigureMetrics
Live2DOffscreenRenderer::figureMetrics(int canvasWidth, int canvasHeight) const
{
    FigureMetrics metrics;
    if (!m_impl->figureSpan.valid || canvasHeight == 0)
        return metrics;

    /*把"1:1 探针比例下"的范围换算到目标画布比例。

      探针是正方形，renderFrame 在竖长画布分支（W < H）里会把模型变换再乘 2/W_m，
      横长画布分支只乘 2/H_m；两分支相差 H/W。所以同一个人物在真实画布输出空间里
      的跨度 = 探针量到的值 × (H/W)（不是 × W/H —— 方向搞反会让 calculate 出来的
      显示比例越算越大，人物反而被放大到贴边）。*/
    const float stretch = static_cast<float>(canvasHeight) /
                          static_cast<float>(canvasWidth);
    metrics.minX = m_impl->figureSpan.minX * stretch;
    metrics.minY = m_impl->figureSpan.minY;
    metrics.maxX = m_impl->figureSpan.maxX * stretch;
    metrics.maxY = m_impl->figureSpan.maxY;
    metrics.spanX = m_impl->figureSpan.spanX() * stretch;
    metrics.spanY = m_impl->figureSpan.spanY();
    metrics.valid = true;
    if (metrics.spanY > 1e-6f)
        metrics.boundsAspect = metrics.spanX / metrics.spanY;
    return metrics;
}

void Live2DOffscreenRenderer::setFigureSpanInOutputSpace(float minX, float minY, float maxX,
                                                         float maxY)
{
    m_impl->figureSpan.unite(minX, minY, maxX, maxY);
    if (m_impl->model != nullptr)
        m_impl->model->setFigureSpanInOutputSpace(minX, minY, maxX, maxY);
}

void Live2DOffscreenRenderer::clearFigureSpan()
{
    m_impl->figureSpan = Impl::FigureSpan();
    if (m_impl->model != nullptr)
        m_impl->model->clearFigureSpan();
}

void Live2DOffscreenRenderer::setDisplayHeightRatio(float ratio)
{
    m_impl->displayHeightRatio = std::min(1.0f, std::max(0.05f, ratio));
    if (m_impl->model != nullptr)
        m_impl->model->setDisplayHeightRatio(m_impl->displayHeightRatio);
}
