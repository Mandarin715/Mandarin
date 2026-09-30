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
#include <QRandomGenerator>
#include <QStringList>
#include <QSurfaceFormat>
#include <QVector>

#include <GL/glew.h>

#include <cmath>

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

/*心情过渡的默认时长（毫秒）。与窗口层 `character/live2dMoodBlendMs` 的默认值一致；
   正常路径由窗口从 config.ini 读出来喂进来，这个常数只保证"没人设过时"的行为是确定的。*/
constexpr int kDefaultMoodBlendMs = 200;

/*心情过渡的收敛 epsilon（参数单位）。

  为什么必须有它：过渡是"起点 → 目标"的**线性**插值，最后一步的浮点乘加会留下
  最后一丝残差（float 在 0.9 附近的表示本身就不精确）。没有这一步，读回值会是
  "0.899999976" 这种"接近但不等于"的数 —— 测试与校准都读不到精确值。
  剩余差值 ≤ epsilon 时直接落到目标值。

  取值 1e-4 的依据：本模型参数的量程最小是 0~1 的开合量，1e-4 是它的万分之一；
  量程最大的是头部角度 ±30，1e-4 是它的三百万分之一。两者都远小于 8bit 输出
  （每级 1/255 ≈ 3.9e-3）能表现的一步，所以"落到目标"与"差一丝"在屏幕上是同一张图；
  而任何有意义的过渡位移都是它的成百上千倍，不会被它提前截断。
  （更小不行：float 在 1.0 附近的 epsilon 是 1.2e-7，但插值的乘加误差会让
   1e-7 这种阈值永远不满足，过渡结束时反而留一个可见量级之外的残差。）*/
constexpr float kBlendSnapEpsilon = 1e-4f;

/*单帧时间步长上限（秒）。与 renderFrame 喂给物理/驱动器的夹取一致：
   超过这个值的间隔不是"一帧"，当一帧处理（首帧/窗口还没映射时可能隔几百毫秒）。
   过渡也必须用同一把尺子，否则一次卡顿会把过渡整段推完，看起来还是"跳"。*/
constexpr float kMaxFrameDeltaSeconds = 0.1f;

/*说话嘴巴（纸片人口型）的参数名与节奏。

  为什么只驱动 ParamMouthOpenY：模型 model3.json 的 Groups[LipSync] 同时声明了
  ParamMouthOpenY 与 ParamMouthForm，但两者语义不同 —— OpenY 是**开口量**（0~1 的上下开合），
  MouthForm 是**嘴形**（本模型 moc 声明 [-1,0]，由心情决定"笑/撇嘴"）。
  两个都写就是心情与口型互相打架（用户看到的是"说话时表情被抹掉"）。

  节奏取 3~5 次/秒：与正常语速同量级（每秒约 4~6 个音节，每个音节一开一合）。需求明确要求"不规则"，所以每次过零都重新摇频率与振幅 ——
  干净正弦读起来像缝纫机。*/
const QString kSpeakingMouthParameter = QStringLiteral("ParamMouthOpenY");
/*嘴**形**参数：它属于**心情**（本模型 moc 声明 [-1,0]），不是身体的任何动作。
   待机摆动与说话扑动都必须跳过它 —— 写了它就会与情绪预设互相打架
   （用户看到的是"说话/呼吸时表情被抹掉"）。它是这里的第二条禁用通道。*/
const QString kSpeakingMouthFormParameter = QStringLiteral("ParamMouthForm");
/*开合频率：**3~5 次/秒**。最初写的是 8~10，实机一看像在快速嘀嗒 —— 正常语速约每秒 4~6 个
  音节，9Hz 的开合远快于任何人说话时的嘴。3~5 与语速同量级，看着像"在说话"而不是"在抖"。*/
constexpr float kSpeakingFlapMinHz = 3.0f;
constexpr float kSpeakingFlapMaxHz = 5.0f;
/*振幅（相对声明量程的比例）：下限保证"看得见"，上限留出余量给心情的开口量叠加
   （扑动是**加**在心情值上的，所以自己不能顶满量程，否则 clamp 会把波峰削平）。*/
constexpr float kSpeakingFlapMinAmplitude = 0.28f;
constexpr float kSpeakingFlapMaxAmplitude = 0.55f;
/*过零时"最张开"仍要保留一点开度：完全合到 0 会让开合在低帧率下闪烁。*/
constexpr float kSpeakingFlapFloor = 0.05f;

/*===== 包络驱动嘴巴的参数（见 setSpeechLevel 与 AudioEnvelope 的说明）=====

  量程比例 0.75：满电平（正常语音里最强的音节）给出约 3/4 量程的开度。
    为什么不是 1.0：扑动量是**加在心情值上**的，而心情本身可能已经张着嘴
    （surprised 预设把开口量放到 0.8）—— 满量程只会让它更早顶到上限。
    0.75 留出的余量正是给"心情本来就张着嘴"的情形（clamp 只挡越界、不做平均）。
    为什么不是 0.4：包络的参考值是 95 分位，正常句子里"响亮的常态"才接近 1，
    0.4 会让中等音量的句子看起来"嘴张不开"。

  抖动 ±25%（慢速随机游走，约 150ms 换一次目标）：这是"纸片人的不规则感"在
    包络模式里的**唯一**来源。真实语音的电平在音节内部几乎恒定，
    只按电平开合会像"跟着音量条张嘴"；慢速游走让同一段响度的口型也有细微变化。
    抖动**乘**在电平上（而不是相加）：电平为 0 时它乘不出任何东西 ——
    静音必须精确等于"嘴停在心情值"，一个字节的残余开合都不许有。

  电平平滑：攻击 60ms、释放 140ms（Dialog 每 50ms 才给一个新电平，直接施加是一格一格跳）。
    两个方向不对称是有理由的：起音要跟得上（不然每个字都晚 50ms 张嘴，口型发拖），
    收音拖一点（不然字与字、音节之间的低电平会让嘴一闪一闪）。
    与心情过渡同一套"时间制线性逼近 + 到达即 snap"的写法：静态电平下必须**精确**
    落到目标，否则静音时电平永远差一丝、嘴闭不严（那是本功能唯一要解决的事）。*/
constexpr float kSpeechEnvelopeScale = 0.75f;
constexpr float kSpeechWobbleRange = 0.25f;
constexpr float kSpeechWobbleIntervalSeconds = 0.15f;
constexpr float kSpeechLevelAttackSeconds = 0.06f;
constexpr float kSpeechLevelReleaseSeconds = 0.14f;

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

        // 参数覆盖：盖在动作结果之上，优先级最高。
        // **按帧时间朝目标逼近**（不是原子换表）：换心情才不会一帧跳过去（见 applyMoodBlend）。
        applyMoodBlend(deltaSeconds);

        _model->SaveParameters();

        // 眨眼 / 呼吸 / 物理统一由调度器驱动（这里**不能**再单独调 _physics->Evaluate）
        _updateScheduler.OnLateUpdate(_model, deltaSeconds);

        /*待机摆动（"站着、重心在动"）与呼吸同一段：**加法**，基准是本帧参数当前的值
           （即心情覆盖刚写下的绝对基准）。放在 SaveParameters() 之后是必须的 ——
           否则这一次摆动的结果会被存成下一帧的基准、再叠一遍（见 applyIdleSway）。*/
        applyIdleSway(deltaSeconds);

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

    /*按帧时间把一个参数从"过渡起点"**线性**推进到目标。

      ⚠️ 必须是**线性**（current = start + (target-start) × progress），不能写成
      "current += (target-current) × progress"：后者是指数逼近 —— 每帧只吃掉剩余差值的
      一个比例，永远到不了目标（实测：设了 200ms 的过渡，1.4s 后还差 0.001 没走完），
      而且**过渡长度与配置的时长不成比例**（200ms 与 600ms 观察到的帧数只差 1.9 倍而不是
      3 倍：绝大部分时间花在"追最后那一点点"上）。线性推进则保证 time>=duration 时
      恰好落在目标上。这也是 kBlendSnapEpsilon 存在的前提：最后一步直接落到目标。*/
    static float blendToward(float start, float target, float progress)
    {
        const float mixed = start + (target - start) * progress;
        return (std::fabs(target - mixed) <= kBlendSnapEpsilon) ? target : mixed;
    }

    /*整组替换**目标**覆盖值。

      为什么要整组：情绪预设换了之后，上一种情绪的条目**必须消失**，
      否则它每帧继续施加、新预设看起来没生效（"切了中立她还在笑"）。

      ⚠️ 换的是**目标**，不是本帧真正施加的值：真正施加的值由 applyMoodBlend 逐帧推进。
      目标**没变**时这里什么都不做（幂等）—— 重复施加同一个心情不会把过渡重启、
      更不会把参数打回起点。目标变了才重新取起点并重置计时。*/
    void setParameterOverrides(const QHash<QString, float> &overrides)
    {
        if (m_parameterOverrides == overrides)
            return; // 同一个目标：过渡不重启（"同一个心情再说一次"必须是无操作）
        /* 取起点必须**无条件**做：即使上一次过渡还没跑完也要从"此刻的值"重新出发，
           否则起点表里没有这个参数，就会从 0 或上一次的旧起点开始 —— 屏幕上是一次跳变。
           （"过渡途中又换心情"是真实时序：AI 的两句话之间常常不足 200ms。）*/
        captureBlendStart();
        m_parameterOverrides = overrides;
        m_blendActive = true;
        m_blendElapsedSeconds = 0.0f;
        /*换心情即结束"显式乘数"状态：之后眼睛乘数重新由覆盖表推导。
           否则一次校准实验的乘数会把之后所有心情的眼睛都钉住。*/
        m_eyeMultiplierExplicit = false;
    }

    /*用于测试/重建正常路径：解除显式乘数，让眼睛重新跟随覆盖表。*/
    void clearExplicitEyeMultiplier() { m_eyeMultiplierExplicit = false; }

    /*设置/清除"显式乘数"标记（由 Live2DOffscreenRenderer::setEyeOpennessMultiplier 驱动）。*/
    void setExplicitEyeMultiplier(bool on) { m_eyeMultiplierExplicit = on; }

    /*说话开关。语义只是"目标状态"：真正的幅度爬升/衰减在 advanceSpeakingFlap 里按帧时间做。*/
    void setSpeaking(bool speaking)
    {
        m_speaking = speaking;
        if (!speaking)
        {
            /*停播 = 这一句结束了：**包络模式必须在这里复位**。

                为什么非复位不可：用户可能把 vits 的 format 配成 mp3（那一句没有包络）。
                如果渲染器还留在"包络模式"、电平恒 0，嘴会**整句冻在心情值上** ——
                那比今天的盲扑动更糟，而且画面症状（"她说话时嘴不动"）与"接线断了"
                一模一样，从像素上几乎无法反推。复位后下一句自动回到盲扑动。

                顺序无关：即使 Dialog 紧接着补一个"电平清零"，setSpeechLevel 也不会
                因此重新进入包络模式（见那里的条件）。*/
            m_speechLevelValid = false;
            setSpeechLevelTarget(0.0f);
        }
    }

    /*说话电平（0~1，来自 Dialog 对 TTS 字节算出的包络；语义见公开头文件）。*/
    void setSpeechLevel(float level)
    {
        const float clamped = std::clamp(level, 0.0f, 1.0f);
        setSpeechLevelTarget(clamped);
        /*只有"这一句真的带来了包络"才进入包络模式。
            停播后 Dialog 会把电平清零（那是**上**一句的收尾，不是新一句的包络），
            把它当包络就会让嘴从盲扑动切进包络模式、冻在心情值上。*/
        if (m_speaking || clamped > 0.0f)
            m_speechLevelValid = true;
        /*⚠️ 不在这里把平滑值归零：连着两句之间电平是有连续性的，
            从残留值出发比"打回 0 再来"更平滑（相位那种"随机起点"的问题这里不存在）。
            真正需要起点快照的是**目标变化**的那一刻，见 setSpeechLevelTarget。*/
    }

    /*把电平目标改成新值：**起点取当前平滑值**（不跳变），计时清零。

       与心情过渡同一套"快照起点 + 时间制线性推进"的写法，理由也一样：
       指数逼近（value += (target-value)×k×dt）永远到不了目标 ——
       那意味着释放到 0 之后嘴还留着一条缝，"停顿闭嘴"就不成立。*/
    void setSpeechLevelTarget(float level)
    {
        if (level == m_speechLevelTarget)
            return;
        m_speechLevelFrom = m_speechLevelSmoothed;
        m_speechLevelElapsedSeconds = 0.0f;
        m_speechLevelTarget = level;
    }

    /*心情过渡时长（毫秒）。0 = 关掉插值，立刻跳到目标。

       已有过渡在跑时改时长要**重新取起点**：否则"新进度 × 旧起点"会在下一帧跳一下
       （换配置本来就是一次用户可见的变化，重取起点让它从头平滑地走完新时长）。*/
    void setMoodBlendDurationMs(int milliseconds)
    {
        const int clamped = std::max(0, milliseconds);
        if (clamped == m_moodBlendMs)
            return;
        if (m_blendActive)
            captureBlendStart();
        m_moodBlendMs = clamped;
        m_blendActive = true;
        m_blendElapsedSeconds = 0.0f;
    }

    /*把当前值记成过渡起点、计时清零。*/
    void captureBlendStart()
    {
        m_blendStartValues = m_currentOverrides;
        m_blendElapsedSeconds = 0.0f;
    }

    /*按帧时间把当前覆盖值朝目标推进，再写进模型。

      为什么需要它（用户症状）：覆盖原本是整组原子替换，参数会在一帧里从 0.9 落到 0 ——
      屏幕上就是"换心情时顿一下"。

      三条约定（都是可观察行为，见 test_live2doffscreen 的 BLEND 系列）：
        1. **时间制**：进度 = 累计帧时间 / 过渡时长。用帧时间而不是"每帧固定走百分之几"，
           是为了高帧率下手感不变（本项目 fps 可配：固定比例的实现会让 120fps 快一倍）；
        2. **线性且收敛**：见 blendToward。过渡结束时值**精确等于**目标；
        3. **不重启**：目标没变就不动起点（见 setParameterOverrides）。

      被移除的参数（目标表里没有、当前表里还有）朝 **0** 回落，落到位后删掉条目 ——
      绝大多数参数的默认值就是 0；留着条目才是"残留情绪"那个 bug。

      最后一步把**说话扑动**加到嘴巴的开口量上（见 advanceSpeakingFlap）。*/
    void applyMoodBlend(float deltaSeconds)
    {
        if (_model == nullptr)
            return;

        // 时长 ≤0 = 不插值：直接采用目标表（保留"立刻生效"这条老路径，调试与性能实验都用得上）
        if (m_moodBlendMs <= 0)
        {
            m_currentOverrides = m_parameterOverrides;
            m_blendActive = false;
        }
        else if (m_blendActive)
        {
            m_blendElapsedSeconds += deltaSeconds;
        }

        const float durationSeconds = static_cast<float>(m_moodBlendMs) / 1000.0f;
        const float progress =
            (durationSeconds > 0.0f)
                ? std::min(m_blendElapsedSeconds / durationSeconds, 1.0f)
                : 1.0f;

        // 1) 目标表里仍然存在的参数：从起点线性推进到目标
        for (auto it = m_parameterOverrides.constBegin(); it != m_parameterOverrides.constEnd();
             ++it)
        {
            const float start = m_blendStartValues.value(it.key(), it.value());
            m_currentOverrides.insert(it.key(), blendToward(start, it.value(), progress));
        }

        // 2) 目标表里已经没有的参数：朝 0 回落，落到就删掉让驱动器/物理重新完全接管
        for (auto it = m_currentOverrides.begin(); it != m_currentOverrides.end();)
        {
            if (m_parameterOverrides.contains(it.key()))
            {
                ++it;
                continue;
            }
            const float start = m_blendStartValues.value(it.key(), 0.0f);
            const float released = blendToward(start, 0.0f, progress);
            if (released == 0.0f)
            {
                m_blendStartValues.remove(it.key());
                it = m_currentOverrides.erase(it);
            }
            else
            {
                it.value() = released;
                ++it;
            }
        }

        /*3) 说话扑动：先推进相位/幅度，再取这一帧的扑动量（未说话时恒为 0）。
            必须在写参数**之前**算：扑动量要"加在心情值上"一起写进去。*/
        const float flap = advanceSpeakingFlap(deltaSeconds);

        // 4) 本帧真正施加的值 = 插值后的当前表（+ 嘴巴的扑动量）
        for (auto it = m_currentOverrides.constBegin(); it != m_currentOverrides.constEnd(); ++it)
        {
            const Csm::csmString id(it.key().toUtf8().constData());
            const Csm::CubismIdHandle handle =
                Csm::CubismFramework::GetIdManager()->RegisterId(id);
            float value = it.value();
            if (flap != 0.0f && it.key() == kSpeakingMouthParameter)
            {
                /*组合方式：clamp(心情值 + 扑动量, min, max)。
                   所以"说话时刚好很惊讶"（心情把嘴张到 0.8）不会被压回去 ——
                   clamp 只挡越界，不做平均。范围取**模型声明**的 min/max，不硬编码。*/
                const Live2DOffscreenRenderer::DeclaredRange range = declaredRangeOf(kSpeakingMouthParameter);
                value = std::min(range.max, std::max(range.min, value + flap));
            }
            _model->SetParameterValue(handle, value);
        }

        /*眼睛乘数用**本帧真正施加的覆盖值**推导（而不是目标表）：否则闭眼参数还在路上、
           乘数已经到目标，两者错帧，过渡期间眼睛会比应有的样子更闭/更睁。
           这里顺手替掉了窗口层那次 setEyeOpennessMultiplier（见 .cpp 的 setParameterOverrides）。

           ⚠️ 但**显式**给的乘数（setEyeOpennessMultiplierForTest，校准/实验用）优先：
           那种调用就是要"这一帧眼睛按我说的开"，不能被心情值重推回去。*/
        if (!m_eyeMultiplierExplicit)
            setEyeOpennessMultiplier(m_currentOverrides);
    }

    /*说话扑动：一帧的推进与取值。

      为什么是"相位累加"而不是"t × 频率"：频率每次过零都在变，
      用绝对时间乘频率会在换频的那一帧产生相位跳变（嘴会抽一下）。
      累加相位则天然连续 —— 换频只改变接下来走多快。

      不规则从哪来：每个周期（相位转过 2π）重新摇一次目标频率（3~5Hz）与振幅，
      再朝它平滑过渡。干净正弦读起来像缝纫机，这是"像在说话"与"像机器人"的区别。

      返回这一帧的扑动量（参数单位；未说话且幅度已衰减完时为**精确的 0**）。*/
    float advanceSpeakingFlap(float deltaSeconds)
    {
        /*幅度按与心情过渡**同一套机制**走：线性趋近目标（说话=1，停止=0），到达即 snap。
           注意两个方向都要走时长，尤其**起播那一下不能一帧抬满** ——
           TTS 每次开口都会触发一次 setSpeaking(true)，若一帧就抬满，
           嘴在开口瞬间"啪"地跳到满幅，用户看到的就是又一次跳变。*/
        const float amplitudeTarget = m_speaking ? 1.0f : 0.0f;
        const float durationSeconds = static_cast<float>(m_moodBlendMs) / 1000.0f;
        const float step =
            (durationSeconds > 0.0f) ? std::min(deltaSeconds / durationSeconds, 1.0f) : 1.0f;

        if (m_speakingFlapAmplitude != amplitudeTarget)
        {
            if (!m_speakingFlapDecaying)
            {
                /*开始说话：把幅度与相位都归零，从"闭着的嘴"平滑张开。
                   不归零的话，上一次说话残留的相位会让这一句以一个随机的开口量起步
                   （幅度从 0 长起来，但相位决定它先从哪一侧开始）—— 那是一次可见的抽动。*/
                m_speakingFlapAmplitude = 0.0f;
                m_speakingFlapPhase = 0.0f;
                m_speakingFlapFrequencyHz = kSpeakingFlapMinHz;
                m_speakingFlapDecaying = true;
            }
            const float mixed = m_speakingFlapAmplitude
                                + (amplitudeTarget - m_speakingFlapAmplitude) * step;
            m_speakingFlapAmplitude =
                (std::fabs(amplitudeTarget - mixed) <= kBlendSnapEpsilon) ? amplitudeTarget
                                                                         : mixed;
        }
        else if (!m_speaking)
        {
            // 幅度已经到 0 并停住：扑动彻底退出，返回精确 0（"不贡献任何东西"）
            m_speakingFlapAmplitude = 0.0f;
            m_speakingFlapDecaying = false;
            m_speakingFlapPhase = 0.0f;
            return 0.0f;
        }

        if (m_speakingFlapAmplitude <= 0.0f)
            return 0.0f;

        /*包络模式：Dialog 这一句真的带来了电平（见 setSpeechLevel）。
            放在这里而不是函数开头是有意的 —— "说话幅度"的爬升/衰减上面已经算完，
            两种模式共用同一个开关，所以"停播后回落"天然只有一条路径。
            没有包络时（用户配成 mp3）走下面逐位不变的盲扑动。*/
        if (m_speechLevelValid)
            return advanceSpeechEnvelope(deltaSeconds);

        // 推进相位；转过一个周期就重新摇频率（这就是"不规则"的来源）
        m_speakingFlapPhase += 2.0f * static_cast<float>(M_PI) * m_speakingFlapFrequencyHz
                               * deltaSeconds;
        if (m_speakingFlapPhase >= 2.0f * static_cast<float>(M_PI))
        {
            m_speakingFlapPhase = std::fmod(m_speakingFlapPhase, 2.0f * static_cast<float>(M_PI));
            m_speakingFlapFrequencyHz =
                kSpeakingFlapMinHz
                + (kSpeakingFlapMaxHz - kSpeakingFlapMinHz) * uniformUnitRandom();
            m_speakingFlapTargetAmplitude =
                kSpeakingFlapMinAmplitude
                + (kSpeakingFlapMaxAmplitude - kSpeakingFlapMinAmplitude) * uniformUnitRandom();
        }
        // 振幅本身也平滑过渡（周期内不去突变），看起来是"越说越有劲/越说越轻"
        m_speakingFlapCurrentAmplitude +=
            (m_speakingFlapTargetAmplitude - m_speakingFlapCurrentAmplitude) * 0.25f;

        const Live2DOffscreenRenderer::DeclaredRange mouthRange = declaredRangeOf(kSpeakingMouthParameter);
        const float span = mouthRange.max - mouthRange.min;
        if (span <= 0.0f)
            return 0.0f;

        /*波形：sin 在 [-1,1]，映射到 [kSpeakingFlapFloor, 1] 的**单侧**开合 ——
           嘴不会"负开"，只会从"几乎闭合"到"张开"。
           乘上"说话幅度"（0→1 的包络）与量程，就是参数单位的扑动量。*/
        const float wave = 0.5f + 0.5f * std::sin(m_speakingFlapPhase); // 0~1
        const float shaped = kSpeakingFlapFloor + (1.0f - kSpeakingFlapFloor) * wave;
        return m_speakingFlapAmplitude * m_speakingFlapCurrentAmplitude * span * shaped;
    }

    /*包络模式的一帧：把这一句的响度电平变成一个开口量。

      `开口量 = 电平 × 量程比例 × (1 + 不规则抖动)`，由调用方加在心情值上、再 clamp。
      返回**本身**的扑动量（参数单位）；电平为 0 时是**精确的 0** ——
      嘴停在心情值上，这正是用户要的"句子之间闭嘴"。

      为什么必须精确：用户诉求的另一半是"停顿闭嘴"，"接近 0 的电平"会在屏幕上留下
      一条一直在微微抖的缝。所以下面的平滑一旦落到目标就 snap（与心情过渡同一套机制），
      而抖动是**乘**在电平上的（0 乘任何数还是 0）。*/
    float advanceSpeechEnvelope(float deltaSeconds)
    {
        // 1) 电平平滑：攻击快、释放慢（见 kSpeechLevelAttackSeconds 的说明）
        const float tau = (m_speechLevelTarget >= m_speechLevelFrom) ? kSpeechLevelAttackSeconds
                                                                     : kSpeechLevelReleaseSeconds;
        m_speechLevelElapsedSeconds += deltaSeconds;
        const float progress =
            (tau > 0.0f) ? std::min(m_speechLevelElapsedSeconds / tau, 1.0f) : 1.0f;
        /*进度满 ⇒ **直接落到目标**（不是"接近"）。这一条是"静音 = 心情值"的前提：
            线性逼近走到头就是目标本身，而指数逼近会永远差一丝。*/
        m_speechLevelSmoothed =
            (progress >= 1.0f)
                ? m_speechLevelTarget
                : (m_speechLevelFrom + (m_speechLevelTarget - m_speechLevelFrom) * progress);

        const float span = speakingMouthSpan();
        if (span <= 0.0f)
            return 0.0f;

        /*2) 不规则抖动：慢速随机游走（每 ~150ms 换一个目标，期间平滑过渡）。
            为什么不能省：真实语音在音节内部的电平几乎恒定，只按电平开合看起来是
            "跟着音量条张嘴"；这一层细微摆动才是"像在说话"的来源。*/
        m_speechWobbleElapsedSeconds += deltaSeconds;
        if (m_speechWobbleElapsedSeconds >= kSpeechWobbleIntervalSeconds)
        {
            m_speechWobbleElapsedSeconds = 0.0f;
            m_speechWobbleTarget = (uniformUnitRandom() * 2.0f - 1.0f) * kSpeechWobbleRange;
        }
        const float wobbleStep =
            std::min(deltaSeconds / kSpeechWobbleIntervalSeconds, 1.0f);
        m_speechWobble += (m_speechWobbleTarget - m_speechWobble) * wobbleStep;

        /*3) 静音（电平已落到 0）**精确**返回 0：一个字节的残余都不许有。
            顺序上先判它再乘抖动，是为了让"静音 = 心情值"这件事不依赖抖动的正负。*/
        if (m_speechLevelSmoothed <= 0.0f)
            return 0.0f;

        return m_speakingFlapAmplitude * m_speechLevelSmoothed * kSpeechEnvelopeScale * span
               * (1.0f + m_speechWobble);
    }

    /*嘴巴参数**模型声明**的量程（span）。取不到（模型没声明这个参数）时返回 0，
       于是两条扑动路径都自动失效，而不是拿硬编码量程去写越界值。*/
    float speakingMouthSpan() const
    {
        const Live2DOffscreenRenderer::DeclaredRange mouthRange =
            declaredRangeOf(kSpeakingMouthParameter);
        return mouthRange.max - mouthRange.min;
    }

    /*[0,1) 均匀随机。用 QRandomGenerator 而不是 rand()：后者在多线程/库混用下
       种子与序列都不可控，而这条曲线的"不规则"是**用户看得见**的行为，值得一个像样的源。*/
    static float uniformUnitRandom()
    {
        return static_cast<float>(QRandomGenerator::global()->generateDouble());
    }

    /*取某个参数**模型声明**的范围；模型没声明时给一个"不干涉"的兜底
       （min=0、max=0 → span=0 → 扑动自动失效，而不是拿一个硬编码的量程去写越界值）。*/
    Live2DOffscreenRenderer::DeclaredRange declaredRangeOf(const QString &parameterId) const
    {
        const QHash<QString, Live2DOffscreenRenderer::DeclaredRange> ranges = declaredParameterRanges();
        return ranges.value(parameterId, Live2DOffscreenRenderer::DeclaredRange());
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
        // 当前表也必须清：留着它的话，"关掉情绪功能"之后那一份插值中的值还会被施加
        m_currentOverrides.clear();
        m_eyeOpennessMultiplier.clear();
    }

    /*待机摆动的一帧：把 `amplitude × sin(2π(t/period + phase))` **加到**参数上。
       （数据 schema 与"语义名 → 参数 ID"的解析都在 Live2DMoodPreset::loadIdle 里，
        这里只认参数 ID 与模型自己声明的范围。）

      位置在 tick() 里 `SaveParameters()` **之后**、与呼吸/物理同一段，理由见头文件：
      驱动器这一帧写下的值不进"本帧保存值"，下一帧 LoadParameters 就不会把上一次的摆动
      当成新基准再叠一遍（否则它会发散）。与呼吸的"加法"是同一种语义。

      t 是**累计虚拟时间**（帧步长累加、含 0.1s 夹取），不是墙钟：相位因此可以按注入的
      时间完全复现，机器被抢占也不会让"这一帧落在周期里的哪一点"漂移。*/
    void applyIdleSway(float deltaSeconds)
    {
        m_idleSwayOffsets.clear();
        if (_model == nullptr || m_idleSwayEntries.isEmpty())
            return;

        m_idleSwayElapsedSeconds += deltaSeconds;

        /*声明范围**一帧取一次**（而不是每条轴取一次）：declaredParameterRanges() 会遍历模型
           全部 88 个参数建表，放在逐条循环里就是每帧 3 倍的浪费。*/
        const QHash<QString, Live2DOffscreenRenderer::DeclaredRange> ranges =
            declaredParameterRanges();

        for (const Live2DMoodPreset::IdleSwayEntry &entry : m_idleSwayEntries)
        {
            /*三道防御，每一道都对应"数据写错时屏幕上会出现什么"：
                1) 参数 ID 为空 / 周期非正 / 幅度或相位不是有限数 → 跳过
                   （除零与 NaN 会顺着参数表污染整个模型）；
                2) 驱动器/物理占用的参数（breath / hair*）→ **即使数据里写了也跳过**。
                   Live2DMoodPreset 装载时已经剔除并告警过，这里再挡一层：这个入口是公开的，
                   谁都能喂条目进来，"永不写坏别人的动作"必须是驱动器自己的纪律；
                3) 嘴形（ParamMouthForm）同理 —— 它是心情的，不是身体的。
               （第 2、3 条与 isIdleSwayActive() 共用同一个判定，
                 免得"报激活"与"真的写了什么"各说各话）*/
            if (!isApplicableIdleSwayEntry(entry))
                continue;

            const Live2DOffscreenRenderer::DeclaredRange range =
                ranges.value(entry.parameterId);
            /*span ≤ 0 = 模型没有这个参数（或声明退化）：夹取会把值钉死在 0，
               不如整条跳过 —— 那正是"数据表与模型对不上"的形态。*/
            if (!(range.max > range.min))
                continue;

            const float angle = 2.0f * static_cast<float>(M_PI) *
                                (m_idleSwayElapsedSeconds / entry.periodSeconds + entry.phase);
            const float wanted = entry.amplitude * std::sin(angle);

            const Csm::csmString id(entry.parameterId.toUtf8().constData());
            const Csm::CubismIdHandle handle =
                Csm::CubismFramework::GetIdManager()->RegisterId(id);
            /*基准取参数**当前**值（= 动作/心情刚写下的绝对基准，见 tick 的顺序），
               所以摆动是加在它上面，不是盖掉它。*/
            const float base = _model->GetParameterValue(handle);
            const float applied = std::min(range.max, std::max(range.min, base + wanted));
            _model->SetParameterValue(handle, applied);
            /*记下**真正施加**的偏移（夹取之后）而不是"想要的偏移"：
               校准与测试要看的必须是模型实际发生了什么。*/
            m_idleSwayOffsets.insert(entry.parameterId, applied - base);
        }
    }

    float parameterValue(const QString &parameterId) const
    {
        if (_model == nullptr || parameterId.isEmpty())
            return 0.0f;
        const Csm::csmString id(parameterId.toUtf8().constData());
        return _model->GetParameterValue(Csm::CubismFramework::GetIdManager()->RegisterId(id));
    }

    /*待机摆动的条目表（**整组替换**，与情绪覆盖表同一个理由：换了数据之后上一条轴必须
       真的消失，否则它会一直往参数上加东西）。累计时间**不重置**：换表不该让相位跳一下
       （相位跳变在屏幕上是可见的抽动）。*/
    void setIdleSwayEntries(const QVector<Live2DMoodPreset::IdleSwayEntry> &entries)
    {
        m_idleSwayEntries = entries;
        m_idleSwayOffsets.clear(); // 上一帧的偏移属于上一张表，不能留
    }

    /*这条条目能不能落到参数上：数值/周期合法，且不是"别人的"参数（呼吸/头发/嘴形）。
       与 applyIdleSway 共用同一个判定 —— 否则 isIdleSwayActive() 会与"真的写了什么"各说各话。*/
    static bool isApplicableIdleSwayEntry(const Live2DMoodPreset::IdleSwayEntry &entry)
    {
        if (entry.parameterId.isEmpty() || !(entry.periodSeconds > 0.0f) ||
            !std::isfinite(entry.amplitude) || !std::isfinite(entry.phase))
            return false;
        return !Live2DMoodPreset::isUpdaterOwnedParameter(entry.parameterId) &&
               entry.parameterId != kSpeakingMouthFormParameter;
    }

    /*"摆动真的在施加" = 至少有一条**能落地**的轴（而且模型确实声明了这个参数）。
       只数"条目表非空"是不够的：一份全是非法条目的表（错字/换了模型）会让调用方以为它在动。*/
    bool isIdleSwayActive() const
    {
        if (_model == nullptr)
            return false;
        for (const Live2DMoodPreset::IdleSwayEntry &entry : m_idleSwayEntries)
        {
            if (!isApplicableIdleSwayEntry(entry))
                continue;
            const Live2DOffscreenRenderer::DeclaredRange range =
                declaredRangeOf(entry.parameterId);
            if (range.max > range.min)
                return true;
        }
        return false;
    }

    float idleSwayElapsedSeconds() const { return m_idleSwayElapsedSeconds; }

    float idleSwayOffset(const QString &parameterId) const
    {
        return m_idleSwayOffsets.value(parameterId, 0.0f);
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
        /*诊断（见 Live2DOffscreenRenderer::debugState）：Cubism 每画完一个 drawable 就
           glUseProgram(0)，所以在 DrawModel() 返回**之后**读"当前程序"永远是 0。
           要拿到它这一帧真正会用的程序名，只能在这之前记下**进入时**的那个 ——
           那正是着色器单例上一帧留给这一帧的、也是它接下来会拿去 glUseProgram 的值。
           这一段只读状态、不改任何 GL 状态，对单一渲染器路径零影响。*/
        m_drawPrograms.clear();
        {
            GLint entryProgram = 0;
            glGetIntegerv(GL_CURRENT_PROGRAM, &entryProgram);
            if (entryProgram != 0)
                m_drawPrograms.append(static_cast<unsigned int>(entryProgram));
        }
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

    /*上传时所在的上下文创建出来的贴图名（诊断，见 Live2DOffscreenRenderer::debugState）*/
    QVector<unsigned int> textureIds() const
    {
        QVector<unsigned int> ids;
        for (GLuint textureId : m_textureIds)
            ids.append(textureId);
        return ids;
    }

    /*本帧 drawModel() 见到的着色器程序名（诊断，见 drawPrograms）*/
    QVector<unsigned int> drawPrograms() const
    {
        QVector<unsigned int> programs;
        for (unsigned int program : m_drawPrograms)
            programs.append(program);
        return programs;
    }

    /*"这一帧到底有没有真的发出去绘制"的逐 drawable 计数。

       为什么需要它：模型数据（可见数/顶点/贴图）在 CPU 侧完全正常，但 GPU 侧可能一条
       三角形都没画 —— 这两种"空白"在像素上一模一样。Cubism 在循环里**静默跳过**
       贴图名为 0 的 drawable（见 DrawMeshOpenGL 的 `_textures[...] == 0` 分支），
       所以"贴图在**当前上下文**里名字为 0"就是"什么都画不出来"的直接原因。*/
    struct DrawDiagnostics
    {
        int drawableCount = 0;
        int visibleCount = 0;       // 动态可见标志为真的 drawable
        int visibleWithTexture = 0; // 可见且贴图名非 0（= 真的会进绘制）
        int visibleWithIndices = 0; // 可见、有贴图、且有顶点索引（三者齐备才会 glDrawElements）
        int unbindableTextureCount = 0; // 可见但贴图名为 0 的 drawable 数
        int maskCount = 0;          // 引用遮罩的 drawable 数（>0 说明遮罩路径真的被走到）
    };
    DrawDiagnostics drawDiagnostics()
    {
        DrawDiagnostics diagnostics;
        if (_model == nullptr)
            return diagnostics;
        diagnostics.drawableCount = _model->GetDrawableCount();
        Csm::Rendering::CubismRenderer_OpenGLES2 *renderer =
            GetRenderer<Csm::Rendering::CubismRenderer_OpenGLES2>();
        /*把"贴图索引 → 贴图名"抄出来：GetBindedTextures() 给的是**const** 引用，
           而 csmMap 的 const operator[] 在键不存在时会去 new 一个 dummy（编译期直接
           拒绝：const 对象里改成员）。抄一份之后查起来就只是一次线性查找 ——
           这里只有 6 张贴图，代价可以忽略。*/
        QHash<Csm::csmInt32, GLuint> textureByIndex;
        if (renderer != nullptr)
        {
            const Csm::csmMap<Csm::csmInt32, GLuint> &textures = renderer->GetBindedTextures();
            for (Csm::csmMap<Csm::csmInt32, GLuint>::const_iterator it = textures.Begin();
                 it != textures.End(); ++it)
            {
                const Csm::csmPair<Csm::csmInt32, GLuint> &pair = *it;
                textureByIndex.insert(pair.First, pair.Second);
            }
        }
        const Csm::csmInt32 *maskCounts = _model->GetDrawableMaskCounts();
        for (Csm::csmInt32 index = 0; index < _model->GetDrawableCount(); ++index)
        {
            if (!_model->GetDrawableDynamicFlagIsVisible(index))
                continue;
            ++diagnostics.visibleCount;
            if (maskCounts != nullptr && maskCounts[index] > 0)
                ++diagnostics.maskCount;
            const Csm::csmInt32 textureIndex = _model->GetDrawableTextureIndex(index);
            const GLuint textureId = textureByIndex.value(textureIndex, 0);
            if (textureId == 0)
            {
                ++diagnostics.unbindableTextureCount;
                continue;
            }
            ++diagnostics.visibleWithTexture;
            if (_model->GetDrawableVertexIndexCount(index) > 0)
                ++diagnostics.visibleWithIndices;
        }
        return diagnostics;
    }

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
    /*覆盖值的**目标**表（setParameterOverrides 写这里）*/
    QHash<QString, float> m_parameterOverrides;
    /*覆盖值的**当前**表：本帧真正施加的就是它，逐帧朝目标推进。
       初始为空：第一次施加目标表时，起点直接取目标值（不做"从 0 淡入"——
       装载模型后的第一份心情必须立刻是它该有的样子，不能慢半拍）。*/
    QHash<QString, float> m_currentOverrides;
    /*本次过渡的**起点**快照（每个参数从哪个值开始走）与已累计的帧时间。
       为什么要快照而不是"拿当前值按比例走"：后者是指数逼近，永远到不了目标
       （见 blendToward 的说明）。快照 + 线性推进保证 time>=时长 时恰好落在目标。*/
    QHash<QString, float> m_blendStartValues;
    float m_blendElapsedSeconds = 0.0f;
    bool m_blendActive = false;
    /*心情过渡时长（毫秒）。窗口层从 character/live2dMoodBlendMs 读出来喂进来。*/
    int m_moodBlendMs = kDefaultMoodBlendMs;

    /*说话扑动（见 advanceSpeakingFlap）：
       相位是**累加**的（换频时不跳变），频率/振幅每个周期重摇一次以获得不规则感。
       m_speakingFlapDecaying 是"已经开始过一句话"的记号 —— 它把"刚开口"（幅度与相位归零）
       与"说到一半"区分开，不然每帧都会把相位重置，嘴就永远不动了。*/
    bool m_speaking = false;
    bool m_speakingFlapDecaying = false;
    float m_speakingFlapPhase = 0.0f;
    float m_speakingFlapFrequencyHz = kSpeakingFlapMinHz;
    float m_speakingFlapAmplitude = 0.0f;        // 0~1 的包络（说话=1，停止=0）
    float m_speakingFlapTargetAmplitude = kSpeakingFlapMinAmplitude;
    float m_speakingFlapCurrentAmplitude = kSpeakingFlapMinAmplitude;

    /*说话电平（见 setSpeechLevel 的说明）。
       m_speechLevelValid = "这一句有包络"，由 setSpeechLevel 置真、setSpeaking(false) 复位；
       为假时走今天逐位相同的盲扑动（这就是 mp3 配置下的回退契约）。*/
    bool m_speechLevelValid = false;
    float m_speechLevelTarget = 0.0f;
    float m_speechLevelSmoothed = 0.0f;
    /*电平平滑是一次"时间制线性推进"：目标一变就快照起点并重新计时（见 setSpeechLevelTarget）。
       为什么不是"每帧按比例逼近"：那样释放到 0 永远差一丝，嘴在停顿里会留一条缝。*/
    float m_speechLevelFrom = 0.0f;
    float m_speechLevelElapsedSeconds = 0.0f;
    /*不规则抖动（±kSpeechWobbleRange）与其慢速游走状态，见 advanceSpeechEnvelope。*/
    float m_speechWobble = 0.0f;
    float m_speechWobbleTarget = 0.0f;
    float m_speechWobbleElapsedSeconds = 0.0f;

    /*睁闭眼的乘数（**只包含眨眼声明的参数**），由 setEyeOpennessMultiplier 从覆盖表里挑出来。
      为什么单独存一份而不是每次从 m_parameterOverrides 里筛：眨眼参数要在 OnLateUpdate
      之后重写一次，而"哪些参数归眨眼管"是模型声明（Groups[EyeBlink]）决定的，
      每次筛都要问一遍 m_setting；存下来也让"这一帧到底乘了什么"可以一眼看清。*/
    QHash<QString, float> m_eyeOpennessMultiplier;
    /*true = 这份乘数是**显式**给的（setEyeOpennessMultiplier 的调用方，
       校准/实验用），applyMoodBlend 不许用覆盖表把它重推回去。
       见 OffscreenUserModel::applyMoodBlend 的说明。*/
    bool m_eyeMultiplierExplicit = false;

    Csm::ACubismMotion *m_idleMotion = nullptr; // 待机动作；所有权在 _motionManager
    /*待机摆动（见 applyIdleSway）：条目表由 setIdleSway 喂进来（语义名已在数据层解析成
       参数 ID），每个参数 ID 在**本帧**真正施加的偏移存一份给"读回"用，累计时间则是相位的
       唯一时基。三者都是"这一帧发生了什么"的可观察面，校准与测试全靠它们。*/
    QVector<Live2DMoodPreset::IdleSwayEntry> m_idleSwayEntries;
    QHash<QString, float> m_idleSwayOffsets;
    float m_idleSwayElapsedSeconds = 0.0f;
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
    /*诊断用：本帧 drawModel() 见到的着色器程序名（见 drawPrograms）*/
    QVector<unsigned int> m_drawPrograms;
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
    /*诊断用：**实际**画上去的那张 FBO 与读回时绑定的那张（见 debugState）。
       不从 renderTarget 直接取是因为"画到哪张"是这一帧的运行时事实：
       CubismRenderer 内部会经 CubismOffscreenManager 换 FBO（离屏/遮罩），
       一旦换错，renderTarget.GetRenderTexture() 仍然会报一个漂亮的值。*/
    unsigned int lastDrawFbo = 0;
    unsigned int lastReadBackFbo = 0;
    /*下一帧的时间步长覆盖（见 Live2DOffscreenRenderer::setNextFrameDeltaSeconds）。
       hasNextFrameDelta=false 时走正常墙钟路径；true 时该值**原样**当帧步长
       （仍然过 kMaxFrameDeltaSeconds 的夹取，保证与真实路径同一把尺子）。*/
    bool hasNextFrameDelta = false;
    float nextFrameDelta = 0.0f;

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
        /*⚠️ 必须**在 create() 之前**设共享上下文，否则本上下文自成一个名字空间，
           框架那些按进程级单例缓存的 GL 对象（着色器程序/uniform 位置、离屏渲染目标）
           在这里全都不存在 —— glUseProgram 静默失败、一个三角形都发不出去。
           详见 Live2DCubismRuntime::shareContext() 的长说明（同进程第二个渲染器空白
           那个既存缺陷的根因）。单一渲染器路径同样走这条：多一个共享根上下文不影响
           自己的 FBO/贴图/绘制结果（实测逐位相同）。*/
        if (QOpenGLContext *shared = Live2DCubismRuntime::shareContext())
            context->setShareContext(shared);
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
    /*诊断：记下**这一帧真的被绑定**的那张 FBO（见 Impl::lastDrawFbo 的说明）*/
    {
        GLint boundFbo = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &boundFbo);
        m_impl->lastDrawFbo = static_cast<unsigned int>(boundFbo);
    }
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
    /*单帧时间步长夹取。上限常量 kMaxFrameDeltaSeconds 在文件顶部：心情过渡的插值也用
       **同一把尺子**，否则一次卡顿会把过渡整段推完，看起来仍然是"跳"。

      为什么必须夹：这个 delta 直接喂给呼吸/眨眼/物理。load() 之后到第一帧渲染之间
      可能隔着几百毫秒（窗口还没映射、dpr 未定），甚至测试里会隔几秒 —— 那不是"一帧"，
      而是把物理积分一次推进几百毫秒，头发/衣服会被"踹"到极端位置（实测首帧包裹盒
      比之后大 40%，还会留下持续的摆动）。
      不夹的话：首帧必抖一下，人物包围盒测量也随之失真（探针量到的活动范围与真实帧对不上，
      画布余量算不准）。所以按一个正常的帧间隔上限夹住，超大间隔当"一帧"处理。*/
    const float wallDeltaSeconds = static_cast<float>(m_impl->clock.restart()) / 1000.0f;
    const float deltaSeconds =
        std::min(m_impl->hasNextFrameDelta ? m_impl->nextFrameDelta : wallDeltaSeconds,
                 kMaxFrameDeltaSeconds); // 首帧即建立时间基准
    m_impl->hasNextFrameDelta = false;      //覆盖只生效一帧（见 setNextFrameDeltaSeconds）
    m_impl->model->tick(deltaSeconds);
    m_impl->model->drawModel();

    m_impl->renderTarget.EndDraw();
    Csm::Rendering::CubismOffscreenManager_OpenGLES2::GetInstance()->EndFrameProcess();

    // 读回：重新绑定该 FBO 后用 glReadPixels。
    // 不用 glGetTexImage —— 它在部分 NVIDIA 驱动上会走到有问题的路径（实测崩在 DrvPresentBuffers）。
    QVector<uchar> pixels(static_cast<int>(width) * static_cast<int>(height) * 4);
    m_impl->renderTarget.BeginDraw();
    {
        GLint boundFbo = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &boundFbo);
        m_impl->lastReadBackFbo = static_cast<unsigned int>(boundFbo);
    }
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
    }
}

/*过渡时长：窗口层从 character/live2dMoodBlendMs 读出来喂进来（见头文件说明）。
   刻意**不**在 setParameterOverrides 里顺手重设 —— 那样"换一次心情"会把用户/测试
   刚设好的时长覆盖掉。*/
void Live2DOffscreenRenderer::setMoodBlendDurationMs(int milliseconds)
{
    if (m_impl->model != nullptr)
        m_impl->model->setMoodBlendDurationMs(milliseconds);
}

void Live2DOffscreenRenderer::setSpeaking(bool speaking)
{
    if (m_impl->model != nullptr)
        m_impl->model->setSpeaking(speaking);
}

void Live2DOffscreenRenderer::setSpeechLevel(float level)
{
    if (m_impl->model != nullptr)
        m_impl->model->setSpeechLevel(level);
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
    {
        /*显式乘数置位：正常路径（换心情）走 setParameterOverrides，那里会清掉这个标记；
           只有"直接调这个接口"的校准/实验才留下它（见 OffscreenUserModel::applyMoodBlend）。*/
        m_impl->model->setExplicitEyeMultiplier(!moodValues.isEmpty());
        m_impl->model->setEyeOpennessMultiplier(moodValues);
    }
}

void Live2DOffscreenRenderer::clearEyeOpennessMultiplierOverride()
{
    if (m_impl->model != nullptr)
        m_impl->model->clearExplicitEyeMultiplier();
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

/*见头文件说明：把下一帧的时间步长钉住。生产代码用它做探针的**虚拟时间**推进
  （不再 msleep 3s），测试用它把"相邻两帧"钉在虚拟时间上。*/
void Live2DOffscreenRenderer::setNextFrameDeltaSeconds(float seconds)
{
    m_impl->hasNextFrameDelta = true;
    m_impl->nextFrameDelta = std::max(0.0f, seconds);
}

void Live2DOffscreenRenderer::clearNextFrameDelta()
{
    m_impl->hasNextFrameDelta = false;
    m_impl->nextFrameDelta = 0.0f;
    /*时间基准也要重置：冻结期间墙钟一直在走，回到真实路径时第一帧的间隔里会含着
       整段冻结时长（会被夹取上限吃掉）。重置基准让"解冻后的第一帧"就是一个正常帧间隔，
       免得它莫名其妙地吃满 100ms 再继续。*/
    m_impl->clock.restart();
}

bool Live2DOffscreenRenderer::isLoaded() const
{
    return m_impl->model != nullptr;
}

/*诊断快照（见头文件说明）。**不切上下文、不改状态**：它读的就是"此刻"的 GL 状态，
   所以调用方必须自己在合适的时机调（每个实例渲染完之后、下一个实例动手之前）。*/
Live2DOffscreenRenderer::DebugState Live2DOffscreenRenderer::debugState() const
{
    DebugState state;
    state.contextId = reinterpret_cast<unsigned long long>(m_impl->context);
    state.currentContextId =
        reinterpret_cast<unsigned long long>(QOpenGLContext::currentContext());
    state.contextCurrent =
        m_impl->context != nullptr && QOpenGLContext::currentContext() == m_impl->context;
    state.frameBufferId = m_impl->lastDrawFbo;
    state.readBackFboId = m_impl->lastReadBackFbo;
    state.colorBufferId = m_impl->renderTarget.GetColorBuffer();
    state.targetValid = m_impl->renderTarget.IsValid();
    if (m_impl->model != nullptr)
    {
        state.textureIds = m_impl->model->textureIds();
        state.shaderProgramIds = m_impl->model->drawPrograms();
        const OffscreenUserModel::DrawDiagnostics diagnostics =
            m_impl->model->drawDiagnostics();
        state.drawDiagnostics.drawableCount = diagnostics.drawableCount;
        state.drawDiagnostics.visibleCount = diagnostics.visibleCount;
        state.drawDiagnostics.visibleWithTexture = diagnostics.visibleWithTexture;
        state.drawDiagnostics.visibleWithIndices = diagnostics.visibleWithIndices;
        state.drawDiagnostics.unbindableTextureCount = diagnostics.unbindableTextureCount;
        state.drawDiagnostics.maskCount = diagnostics.maskCount;
    }
    /*着色器程序名与贴图名都是**按上下文**命名的：同一个数字在另一个上下文里可能根本
       不存在。glIsProgram / glIsTexture 是唯一能在"当前上下文"里验证它们的手段 ——
       GL 不会为不存在的名字报错，只是 glUseProgram 静默失败、一个三角形都不发，
       画面是一片空白而数据看着全对。*/
    for (unsigned int program : state.shaderProgramIds)
    {
        if (glIsProgram(static_cast<GLuint>(program)) != GL_TRUE)
            state.shaderProgramMissing.append(program);
    }
    for (unsigned int texture : state.textureIds)
    {
        if (glIsTexture(static_cast<GLuint>(texture)) != GL_TRUE)
            state.textureMissing.append(texture);
    }
    /*帧缓冲完整性由 glCheckFramebufferStatus 说，而不是"我们以为它建好了"。*/
    if (m_impl->renderTarget.IsValid() && state.contextCurrent)
    {
        GLint previous = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
        glBindFramebuffer(GL_FRAMEBUFFER,
                          static_cast<GLuint>(m_impl->renderTarget.GetRenderTexture()));
        state.frameBufferStatus =
            static_cast<unsigned int>(glCheckFramebufferStatus(GL_FRAMEBUFFER));
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous));
    }
    return state;
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

void Live2DOffscreenRenderer::setIdleSway(const QVector<Live2DMoodPreset::IdleSwayEntry> &entries)
{
    if (m_impl->model == nullptr)
        return;
    m_impl->model->setIdleSwayEntries(entries);
}

float Live2DOffscreenRenderer::idleSwayOffset(const QString &parameterId) const
{
    if (m_impl->model == nullptr || parameterId.isEmpty())
        return 0.0f;
    return m_impl->model->idleSwayOffset(parameterId);
}

float Live2DOffscreenRenderer::idleSwayElapsedSeconds() const
{
    if (m_impl->model == nullptr)
        return 0.0f;
    return m_impl->model->idleSwayElapsedSeconds();
}

bool Live2DOffscreenRenderer::isIdleSwayActive() const
{
    if (m_impl->model == nullptr)
        return false;
    return m_impl->model->isIdleSwayActive();
}

void Live2DOffscreenRenderer::setMeasureMode(bool on)
{
    if (m_impl->model != nullptr)
        m_impl->model->setMeasureMode(on);
}
