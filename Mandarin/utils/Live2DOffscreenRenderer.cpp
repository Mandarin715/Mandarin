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
#include <QStringList>
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

/*睁闭眼乘数的下限。

  为什么要一个下限：预设里写着 `sleepy: eyeLOpen 0.05` 这种"近乎闭眼"的值，
  乘出来是 0.05×眨眼进度；但**表情数据必须落在模型声明的 [min,max] 内**（0~1），
  而 0 会让模型进入"闭合到底"的姿态（对 0.05 这种"半梦半醒"的意图是过头的）。
  取 0.01 的意思是"保留一丝开度"，同时把夹取空间留给数据侧 ——
  数据超出 [0,1] 时 Core 本来也会夹，这里只是提前一步把负值/零值挡在乘数之外。
  注意这不是"修数据"：预设的 0.05 原样生效，只有 ≤0 的非法值才被抬到这个下限。*/
constexpr float kEyeOpennessMultiplierFloor = 0.01f;

/*把文件读成字节。**唯一**的读盘入口：QFile 走 Windows 宽字符 API，中文路径没问题。*/
bool readAllBytes(const QString &path, QByteArray *out)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    *out = file.readAll();
    return !out->isEmpty();
}

/*测量模式下人物被缩到画布的这个比例（等比、居中）。

  为什么测量模式要缩小：人物的自然大小几乎占满输出空间（实测 atri 纵向 1.98 / 2.0），
  动作一摆就会顶到画布边缘，量到的范围被裁成"整幅画布"，并集随之失真。
  先缩到一半，动作范围就稳稳落在画布内了。*/
constexpr float kMeasureScale = 0.5f;

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

        /*情绪的睁闭眼乘数必须在 OnLateUpdate **之后**：眨眼刚把 ParamEyeLOpen/ROpen
          绝对赋值成本帧的眨眼进度，这里再乘上情绪给的系数 —— 顺序反了就什么都看不见
          （那正是 sleepy 帧与 neutral 逐像素相同的成因）。其余覆盖不动，仍在老位置，
          因为物理/呼吸要看到它们（呼吸是加性的，预设值就是摆动基准）。*/
        applyEyeOpennessMultiplier();

        _model->Update();
    }

    void setParameter(const QString &parameterId, float value)
    {
        if (parameterId.isEmpty())
            return;
        m_parameterOverrides.insert(parameterId, value);
    }

    /*整组替换：一次赋值换掉整张表（不是"清空 + 逐条插入"）。
      为什么要这样：情绪预设换了之后，上一种情绪的条目**必须消失**，
      否则它每帧继续施加、新预设看起来没生效（见头文件说明）。*/
    void setParameterOverrides(const QHash<QString, float> &overrides)
    {
        m_parameterOverrides = overrides;
    }

    /*睁闭眼的**乘数**（情绪预设专用），在 OnLateUpdate 之后施加。

      为什么不能在 Override 阶段"写绝对值"：`CubismEyeBlink` 在 OnLateUpdate 里对
      眨眼参数是**绝对赋值**（0~1 的眨眼进度，见 Effect/CubismEyeBlink.cpp），
      谁先写谁被盖 —— 实测 sleepy(0.05) 的帧与 neutral 逐像素相同、读回恒为 1.00。
      为了"让情绪看得见"去关掉/中性化眨眼是错解法：眨眼是"她还活着"的唯一线索。

      所以约定成 final = mood × blink：
        - mood 不碰眼睛（值 == 1）→ 乘数恒等，眨眼行为完全不受影响；
        - mood 把眼睛压到近乎闭合（小正数）→ final 也是小正数，闭眼看得见；
        - 眨眼进度仍在变化 → final 随时间变化，眨眼依然活着（**绝不冻结**）；
        - 眨眼参数为 0（闭到最紧）时 final 也是 0，与"闭眼"的语义一致。

      为什么按"眨眼声明的参数集合"判定而不是按名字硬编码：哪些参数归眨眼管是
      model3.json 的 Groups[EyeBlink] 说的（换模型可能不同）；不在这组里的参数
      （例如 ParamEyeLSmile）继续走普通绝对值覆盖，语义不变。*/
    void setEyeOpennessMultiplier(const QHash<QString, float> &moodValues)
    {
        m_eyeOpennessMultiplier.clear();
        if (_model == nullptr || _eyeBlink == nullptr)
            return; //模型没声明眨眼参数组：睁闭眼就是普通参数，交给覆盖表写绝对值

        const Csm::csmVector<Csm::CubismIdHandle> &ids = _eyeBlink->GetParameterIds();
        for (Csm::csmUint32 index = 0; index < ids.GetSize(); ++index)
        {
            const Csm::CubismIdHandle id = ids[index];
            if (id == nullptr)
                continue;
            const QString idName = QString::fromUtf8(id->GetString().GetRawString());
            if (!moodValues.contains(idName))
                continue; //这个心情不碰眼睛 → 乘数保持 1（眨眼按自己的节奏走）
            const float moodValue = moodValues.value(idName);
            m_eyeOpennessMultiplier.insert(
                idName, moodValue < kEyeOpennessMultiplierFloor ? kEyeOpennessMultiplierFloor
                                                                : moodValue);
        }
    }

    /*OnLateUpdate 之后的收尾：把情绪给的睁闭眼乘数乘到眨眼刚写下的值上。

      位置是这件事的全部要点 —— OnLateUpdate 之前做没用（眨眼会盖掉），
      之后做才能"既让步给眨眼、又让情绪的闭眼看得见"。
      其余覆盖（眉毛/嘴/腮红/头身角度）**保持原位置不动**：物理与呼吸必须看到情绪值
      （呼吸是加性的，预设值就是它围绕摆动的基准），挪到眨眼之后会让它们看不到。

      ⚠️ 只对**本帧眨眼真的写过的参数**动手（`_eyeBlink` 声明的那些）。
      为什么不能对覆盖表里所有参数一律相乘：那样会造出一个"每帧乘一次"的连乘 ——
      乘数 1.0 时结果不变（所以看不出错），但一旦某帧的乘数不等于 1，
      下一帧读到的是上一帧乘过的结果，眨眼参数会指数衰减。判定必须按"谁归眨眼管"，
      而不是按"表里有什么"。*/
    void applyEyeOpennessMultiplier()
    {
        if (_model == nullptr || m_eyeOpennessMultiplier.isEmpty())
            return;
        for (auto it = m_eyeOpennessMultiplier.constBegin();
             it != m_eyeOpennessMultiplier.constEnd(); ++it)
        {
            const Csm::csmString id(it.key().toUtf8().constData());
            const Csm::CubismIdHandle handle =
                Csm::CubismFramework::GetIdManager()->RegisterId(id);
            const float blinked = _model->GetParameterValue(handle);
            _model->SetParameterValue(handle, blinked * it.value());
        }
    }

    void clearParameterOverrides()
    {
        m_parameterOverrides.clear();
        m_eyeOpennessMultiplier.clear();
    }

    float parameterValue(const QString &parameterId) const
    {
        if (_model == nullptr || parameterId.isEmpty())
            return 0.0f;
        const Csm::csmString id(parameterId.toUtf8().constData());
        return _model->GetParameterValue(Csm::CubismFramework::GetIdManager()->RegisterId(id));
    }

    /*眨眼驱动器**本帧写下的原始值**（0 = 闭紧、1 = 全睁）。

      为什么要单独暴露它：`parameterValue()` 给的是"情绪乘完之后的最终值"，
      单看它分不清"眨眼活着但被情绪压小"与"眨眼被情绪钉死了"——
      这两种情况在校准一张闭眼图时给出的结论完全相反。
      读法：眨眼参数**声明**的那几个（`eyeBlinkParameterIds`）在本帧 OnLateUpdate 里
      被绝对写成同一个进度值；这里把乘数除掉就能还原它。别用来判定别的参数。*/
    float blinkValue() const
    {
        if (_model == nullptr || _eyeBlink == nullptr)
            return 1.0f;
        const Csm::csmVector<Csm::CubismIdHandle> &ids = _eyeBlink->GetParameterIds();
        if (ids.GetSize() == 0)
            return 1.0f;
        const Csm::CubismIdHandle id = ids[0];
        if (id == nullptr)
            return 1.0f;
        const QString idName = QString::fromUtf8(id->GetString().GetRawString());
        const float finalValue = _model->GetParameterValue(id);
        const float multiplier = m_eyeOpennessMultiplier.value(idName, 1.0f);
        if (multiplier <= 0.0f)
            return finalValue; //除零保护：乘数不该是 0（下限见 kEyeOpennessMultiplierFloor）
        return finalValue / multiplier;
    }

    /*模型声明的参数取值域。真源就是 moc 本身（CubismModel 的三个 GetParameter*Value），
      不读 vtube.json / cdi3.json —— 那两个是作者/VTS 的配置，不是模型的能力边界。
      换模型时这份数据自然跟着换，数据表（parameter-map.json）照它填即可。*/
    QHash<QString, Live2DOffscreenRenderer::DeclaredRange> declaredParameterRanges() const
    {
        QHash<QString, Live2DOffscreenRenderer::DeclaredRange> ranges;
        if (_model == nullptr)
            return ranges;
        const Csm::csmInt32 count = _model->GetParameterCount();
        for (Csm::csmInt32 index = 0; index < count; ++index)
        {
            const Csm::CubismIdHandle id = _model->GetParameterId(index);
            if (id == nullptr)
                continue;
            const QString idName = QString::fromUtf8(id->GetString().GetRawString());
            if (idName.isEmpty())
                continue;
            Live2DOffscreenRenderer::DeclaredRange range;
            range.min = _model->GetParameterMinimumValue(static_cast<Csm::csmUint32>(index));
            range.neutral = _model->GetParameterDefaultValue(static_cast<Csm::csmUint32>(index));
            range.max = _model->GetParameterMaximumValue(static_cast<Csm::csmUint32>(index));
            ranges.insert(idName, range);
        }
        return ranges;
    }

    /*归眨眼管的参数（model3.json 的 Groups[Name=EyeBlink].Ids）。
      没声明眨眼组时返回空 —— 调用方据此知道"睁闭眼就是普通参数"，
      而不是去猜哪两个参数是眼睛。*/
    QStringList eyeBlinkParameterIds() const
    {
        QStringList ids;
        if (m_setting == nullptr)
            return ids;
        const Csm::csmInt32 count = m_setting->GetEyeBlinkParameterCount();
        for (Csm::csmInt32 index = 0; index < count; ++index)
        {
            const Csm::CubismIdHandle id = m_setting->GetEyeBlinkParameterId(index);
            if (id == nullptr)
                continue;
            const QString idName = QString::fromUtf8(id->GetString().GetRawString());
            if (!idName.isEmpty())
                ids.append(idName);
        }
        return ids;
    }

    /*模型画布 → 输出空间的缩放：把模型画布高度映射到 2（= 满画布），居中、无平移。

      ⚠️ 必须自己算，不能直接读 GetModelMatrix()：
      那是跨帧复用的对象，历史上 renderFrame 在窄画布分支里会反复对它调 SetWidth(2.0f)，
      而 CubismModelMatrix::SetWidth 内部是 Scale(w / _width) —— 累积乘法。
      于是读到的矩阵一帧比一帧大（实测 atri：基准 0.487 → 真实帧里 3.87，差 8 倍），
      任何"按它反推尺寸"的算法都会算错，人物被画得极小。

      注意这里**不再**依赖画布宽高比：以前按 renderFrame 的投影分支给窄画布补过一次
      2/W_m，那是为了配合"横长模型铺满画布宽度"的投影 —— 而那个投影分支在
      drawModel 里会被等比 Scale 覆盖掉（CubismMatrix44::Scale 是直接赋值），
      所以这个补丁只会让"测量模式"与"真实画布"用上不同的模型缩放，量出来的跨度对不上。
      分轴缩放（见 drawModel）已经把尺寸的事全接管了，模型变换只管"居中、高度为 2"。*/
    float modelToOutputScale() const
    {
        if (_model == nullptr)
            return 1.0f;
        const Csm::csmFloat32 canvasHeight = _model->GetCanvasHeight();
        if (canvasHeight <= 0.0f)
            return 1.0f;
        return 2.0f / canvasHeight;
    }

    /*把模型画到当前绑定的 FBO 上：自己拼 MVP（模型变换 × 分轴摆放）再交给渲染器。

      数学（行向量约定，p' = p · M）：
        q  = modelToOutputScale() · p_model     // 模型画布 → 输出空间，高度 2、居中
        ndc = fit ⊙ q - fit ⊙ center            // 目标占比 + 把活动范围中心搬到画布中心
      合成后就是 diag(s·fitX, s·fitY) 加上平移 (-fitX·centerX, -fitY·centerY)。
      CubismMatrix44::Scale / Translate 都是**直接赋值** _tr[0]/_tr[5] 与 _tr[12]/_tr[13]，
      所以按 Scale 再 Translate 的顺序写就得到上面这个矩阵，不需要再 Multiply。

      ⚠️ 四条必须记住的结论（都是实测踩出来的）：
      1. **分轴缩放**（fitX ≠ fitY）是"不拉伸"的**必要条件**：输出空间到帧像素的映射是
         各向异性的（x 乘 W/2、y 乘 H/2）。只给一个等比因子时，人物的像素宽高比
         = 模型宽高比 × 画布宽高比 —— 实测 atri 在 779x938 上被横向压扁 17%
         （0.436 → 0.355），画布越宽人物越胖。
      2. 分轴之后，人物的像素宽高比 = 画布宽高比、**与画布尺寸无关**；调用方只要把画布
         宽高比取成人物真实宽高比（figureMetrics().boundsAspect），就同时拿到
         "两个方向都占目标比例"和"不拉伸"。
      3. 以前这里用 `fit = max(ratio/spanX, ratio/spanY)`（单因子），于是横向占比被钉死在
         `ratio × 人物宽高比 / 画布宽高比` 上 —— 换画布宽度**完全不影响**横向占比
         （实测 400/600/779/938/1200 宽的画布上横向占比恒为 0.357）。所以"只改画布宽度"
         是修不好这个 bug 的。
      4. renderFrame 里那套"横长模型铺满宽度"的投影分支在这里是**死代码**：
         Scale 直接赋值会把它的各向异性覆盖掉。（曾经的探针换算就是为了补偿这个
         根本不存在的各向异性，属于白算一遍还引入误差。）*/
    void drawModel()
    {
        if (_model == nullptr)
            return;
        // _renderer 在 CubismUserModel 里是 private，只能经 GetRenderer<>() 取
        Csm::Rendering::CubismRenderer_OpenGLES2 *renderer =
            GetRenderer<Csm::Rendering::CubismRenderer_OpenGLES2>();
        if (renderer == nullptr)
            return;

        const Csm::csmFloat32 modelToOutput = modelToOutputScale();

        Csm::csmFloat32 fitX = 0.0f;
        Csm::csmFloat32 fitY = 0.0f;
        Csm::csmFloat32 centerX = 0.0f;
        Csm::csmFloat32 centerY = 0.0f;

        if (m_measureMode)
        {
            /*测量模式：固定等比、不居中（模型变换已经把模型画布中心放在输出空间原点），
              缩到 kMeasureScale 保证动作范围不被画布裁掉。所有取样共用这一个变换，
              并集才有意义。*/
            fitX = kMeasureScale;
            fitY = kMeasureScale;
        }
        else
        {
            // 没有测量结果时按"人物铺满画布"兜底：span = 2（满画布）、中心 = 原点
            const Csm::csmFloat32 spanX = m_figureSpan.valid ? m_figureSpan.spanX() : 2.0f;
            const Csm::csmFloat32 spanY = m_figureSpan.valid ? m_figureSpan.spanY() : 2.0f;
            if (m_figureSpan.valid)
            {
                centerX = (m_figureSpan.minX + m_figureSpan.maxX) / 2.0f;
                centerY = (m_figureSpan.minY + m_figureSpan.maxY) / 2.0f;
            }
            // 目标：可见范围在输出空间里占 2*ratio ⇒ fit = 2*ratio/span
            fitX = 2.0f * m_displayWidthRatio / std::max(1e-6f, spanX);
            fitY = 2.0f * m_displayHeightRatio / std::max(1e-6f, spanY);
        }

        Csm::CubismMatrix44 mvp;
        mvp.LoadIdentity();
        mvp.Scale(modelToOutput * fitX, modelToOutput * fitY);
        // 平移按 fit 缩放：要搬的是"缩放之后"的中心
        mvp.Translate(-fitX * centerX, -fitY * centerY);

        // 记下来给探针反解"自然缩放"下的范围用（见 probeFigureMetrics）
        m_lastFitX = fitX;
        m_lastFitY = fitY;

        renderer->SetMvpMatrix(&mvp);
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

    /*人物在画布 x/y 方向各占的比例（见 Live2DOffscreenRenderer::setDisplayRatios）*/
    void setDisplayRatios(float widthRatio, float heightRatio)
    {
        m_displayWidthRatio = widthRatio;
        m_displayHeightRatio = heightRatio;
    }

    /*测量模式：固定等比变换，取样之间不改变变换（见 Live2DOffscreenRenderer::setMeasureMode）*/
    void setMeasureMode(bool on) { m_measureMode = on; }
    bool isMeasureMode() const { return m_measureMode; }

    /*上一帧实际用在输出空间上的 x/y 缩放（见 drawModel）。probeFigureMetrics 靠它把量到的
      输出范围折回"自然缩放"下的范围。*/
    float lastFrameFitX() const { return m_lastFitX; }
    float lastFrameFitY() const { return m_lastFitY; }

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

    /*睁闭眼的乘数（**只包含眨眼声明的参数**），由 setEyeOpennessMultiplier 从覆盖表里挑出来。
      为什么单独存一份而不是每次从 m_parameterOverrides 里筛：眨眼参数要在 OnLateUpdate
      之后重写一次，而"哪些参数归眨眼管"是模型声明（Groups[EyeBlink]）决定的，
      每次筛都要问一遍 m_setting；存下来也让"这一帧到底乘了什么"可以一眼看清。*/
    QHash<QString, float> m_eyeOpennessMultiplier;

    Csm::ACubismMotion *m_idleMotion = nullptr; // 待机动作；所有权在 _motionManager
    /*眨眼 Updater 拿的是这个标志的**引用**：动作没更新参数的那些帧才让眨眼生效。
      成员不能挪位置/不能是临时量，否则引用悬空。*/
    Csm::csmBool _motionUpdated = false;

    /*人物可见范围（**输出空间**浮点，画布横竖都映射到 [-1, 1]）。

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
    /*人物在画布 x/y 方向各占的比例。两个方向**分别**给：见 drawModel 的说明，
      分轴缩放是"既不拉伸又占满目标比例"的必要条件。*/
    float m_displayWidthRatio = 1.0f;
    float m_displayHeightRatio = 1.0f;
    bool m_measureMode = false; // true = 探针取样用的固定等比变换
    float m_lastFitX = 1.0f;    // 上一帧实际用的输出空间 x 缩放（供探针反解自然跨度）
    float m_lastFitY = 1.0f;
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

    /*人物可见范围（**输出空间**浮点，画布横竖都映射到 [-1, 1]）。

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

    /*⚠️ 这里**不能再动 GetModelMatrix()**：它是跨帧复用的对象，SetWidth(2.0f) 内部是
      Scale(w/_width)（累积乘法），会导致模型矩阵一帧比一帧大（实测 atri 差 8 倍），
      而 drawable 顶点位置是用它换算的。
      摆放（模型缩放 + 分轴目标占比 + 居中）全部由 drawModel() 按帧**无状态**重建，
      这里不再自己拼投影。

      顺带说明为什么这里原来那段"横长模型铺满画布宽度"的投影分支被删了：
      CubismMatrix44::Scale 是**直接赋值** _tr[0]/_tr[5]，drawModel 里那次分轴 Scale
      会把它整个覆盖掉 —— 那段分支从来就没生效过（只在"还没有任何测量结果"的首帧上
      短暂生效）。留着它只会让人以为投影里有一层各向异性补偿，从而去写
      "再乘画布宽高比"之类的错误换算。*/
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
    m_impl->model->drawModel();

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

void Live2DOffscreenRenderer::setParameterOverrides(const QHash<QString, float> &overrides)
{
    if (m_impl->model != nullptr)
    {
        m_impl->model->setParameterOverrides(overrides);
        /*同一份覆盖表顺手推给"睁闭眼乘数"：哪些参数算眼睛由模型声明决定，
          这里不重复判断一次（也不会漏掉"换了模型、眨眼参数不同"的情况）。*/
        m_impl->model->setEyeOpennessMultiplier(overrides);
    }
}

void Live2DOffscreenRenderer::clearParameterOverrides()
{
    if (m_impl->model != nullptr)
    {
        m_impl->model->clearParameterOverrides();
        // 乘数也必须一起清：否则"关掉情绪功能"之后眨眼仍被上一次的系数压着
        m_impl->model->setEyeOpennessMultiplier(QHash<QString, float>());
    }
}

void Live2DOffscreenRenderer::setEyeOpennessMultiplier(const QHash<QString, float> &moodValues)
{
    if (m_impl->model != nullptr)
        m_impl->model->setEyeOpennessMultiplier(moodValues);
}

QHash<QString, Live2DOffscreenRenderer::DeclaredRange>
Live2DOffscreenRenderer::declaredParameterRanges() const
{
    QHash<QString, DeclaredRange> ranges;
    if (m_impl->model == nullptr)
        return ranges;
    return m_impl->model->declaredParameterRanges();
}

float Live2DOffscreenRenderer::parameterValue(const QString &parameterId) const
{
    if (m_impl->model == nullptr)
        return 0.0f;
    return m_impl->model->parameterValue(parameterId);
}

float Live2DOffscreenRenderer::blinkValue() const
{
    if (m_impl->model == nullptr)
        return 1.0f;
    return m_impl->model->blinkValue();
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

    /*⚠️ 只允许量**测量模式**下渲染的帧：非测量帧的摆放缩放是"上一次测量结果"推出来的，
      拿它当基准会把测量结果又喂回测量基准（实测让 atri 的宽高比虚高 14%）。
      这里显式告警，免得以后又有人拿普通帧来喂探针。*/
    if (!m_impl->model->isMeasureMode())
        qWarning("[Live2D] probeFigureMetrics 收到了非测量模式的帧：测量基准会被自己的"
                 "结果改写，请先 setMeasureMode(true)");

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

    /*2) 帧像素 → 绘制**输出空间**（画布横竖都映射到 [-1, 1]，满画布 = 2）。

      输出空间到帧像素的映射是 p = (o + 1) * N / 2，所以反解是 (2p - N)/N。
      **这一步不能省**：模型变换只负责"模型画布 → 输出"，从输出到帧像素是投影的缩放；
      少了它量出来的尺寸会差一个分辨率倍数，缩放因子随之算成天文数字。
      这两个 span 就是"人物占输出空间的多少"，是定目标占比的依据。*/
    const auto toOutput = [](double pixel, int extent) {
        return (pixel - extent / 2.0) / (extent / 2.0);
    };
    /*把量到的输出范围折回**自然缩放**（fit = 1）下的范围。

      为什么需要：测量帧里人物被等比缩到 kMeasureScale，真实画布上的 fit 则是按自然跨度
      算的（fit = 2*ratio/span），两边必须同一把尺子。测量模式下中心在原点，所以
      natural = output / fit 就是精确反解。*/
    const float fitX = std::max(1e-6f, m_impl->model->lastFrameFitX());
    const float fitY = std::max(1e-6f, m_impl->model->lastFrameFitY());
    const float oMinX = static_cast<float>(toOutput(minX, rgba.width())) / fitX;
    const float oMaxX = static_cast<float>(toOutput(maxX + 1, rgba.width())) / fitX;
    const float oMinY = static_cast<float>(toOutput(minY, rgba.height())) / fitY;
    const float oMaxY = static_cast<float>(toOutput(maxY + 1, rgba.height())) / fitY;

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
    m_impl->model->setFigureSpanInOutputSpace(oMinX, oMinY, oMaxX, oMaxY);
    return metrics;
}

Live2DOffscreenRenderer::FigureMetrics Live2DOffscreenRenderer::figureMetrics() const
{
    FigureMetrics metrics;
    if (!m_impl->figureSpan.valid)
        return metrics;

    /*⚠️ 这里**不做任何画布比例换算**。

      曾经这里按 canvasHeight/canvasWidth 乘过一个 stretch，想补偿"探针是正方形、真实
      画布不是正方形"带来的投影差异。那个差异并不存在：投影里那段分支会被 drawModel 的
      分轴 Scale 直接覆盖（CubismMatrix44::Scale 是赋值）。于是这个 stretch 成了纯粹的
      误差源 —— 它把 atri 的宽高比从 0.436 抬到 0.524（乘 1.204），而画布宽度正是按它
      定的，画布因此比人物宽 2.3 倍。头注释当时写的是"必须再乘 W/H"、代码写的是乘 H/W，
      两处都不对：正确做法是**不换算**（探针空间本身已经各向同性）。*/
    metrics.minX = m_impl->figureSpan.minX;
    metrics.minY = m_impl->figureSpan.minY;
    metrics.maxX = m_impl->figureSpan.maxX;
    metrics.maxY = m_impl->figureSpan.maxY;
    metrics.spanX = m_impl->figureSpan.spanX();
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

void Live2DOffscreenRenderer::setDisplayRatios(float widthRatio, float heightRatio)
{
    const float width = std::min(1.0f, std::max(0.05f, widthRatio));
    const float height = std::min(1.0f, std::max(0.05f, heightRatio));
    if (m_impl->model != nullptr)
        m_impl->model->setDisplayRatios(width, height);
}

void Live2DOffscreenRenderer::setMeasureMode(bool on)
{
    if (m_impl->model != nullptr)
        m_impl->model->setMeasureMode(on);
}
