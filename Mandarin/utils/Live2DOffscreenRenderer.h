#ifndef LIVE2DOFFSCREENRENDERER_H
#define LIVE2DOFFSCREENRENDERER_H

#include "Live2DMoodPreset.h"

#include <QHash>
#include <QImage>
#include <QSize>
#include <QString>
#include <QVector>

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

    /***整组替换**参数覆盖（情绪预设走这条）。

      为什么必须有它、而且必须是"整组"：`setParameter` 只能往里加/改单条，永远删不掉 ——
      上一种情绪的条目会一直留在表里，每帧继续施加，于是"换成 neutral"看起来毫无变化
      （这就是「一个人只有一副表情」的第二个成因）。整组替换让"这次心情的参数集"
      成为表里唯一的内容，切回 neutral 时旧条目自然消失。

      为什么不是"先清空再逐条加"：那中间会有一瞬间的空表，而这里换表是单条语句、
      下一帧才被读取（tick 在主线程同一调用栈里跑，不存在半途被读的可能）。
      key 为参数 ID（如 ParamEyeLSmile）。*/
    void setParameterOverrides(const QHash<QString, float> &overrides);

    /*清空整组覆盖：回到"每个参数完全由动作/驱动器决定"的状态。
      情绪功能自关（预设文件缺失/非法）时用它，避免留下上一帧的残留情绪。*/
    void clearParameterOverrides();

    /***心情过渡时长**（毫秒；0 = 不插值，立刻跳到目标）。

      为什么需要它：`setParameterOverrides` 是**整组原子替换**，于是换心情时参数在一帧里
      从 0.9 直接落到 0（用户看到的就是"顿一下"）。现在覆盖值按**时间**逐帧逼近目标，
      一帧只走"帧时间 / 过渡时长"那么大的一步。

      时长由 `character/live2dMoodBlendMs` 提供（窗口层从 config.ini 读，默认 200ms）：
      fps 与渲染倍数都是可调的，过渡手感当然也要可调 —— 硬编码一个常数等于把用户
      已经能调的两件事之一锁死。

      实现约定（三条都是可观察行为，见 test_live2doffscreen 的 BLEND 系列）：
        - 时间制：每帧位移 = 总差值 × (帧时间 / 时长)，所以 60fps 与 120fps 手感一致；
        - **必须收敛**：剩余差值小于 kBlendSnapEpsilon 时直接落到目标值。
          纯指数逼近（value += (target-value) × k × dt）永远到不了目标，
          读回值会一直是"接近但不等于" —— 那样参数永远停在过渡态，测试也读不到精确值；
        - 同一个目标重复施加是**幂等**的（不重启过渡）：值不会被打回起点。*/
    void setMoodBlendDurationMs(int milliseconds);

    /***说话状态**：打开后渲染器给嘴加一层"纸片人开合"（不是音素口型、也不是音量驱动）。

      为什么要有它：TTS 在播的时候脸上什么都不会动，看着像"她在放录音"。

      实现约定（都是可观察行为，见 test_live2doffscreen 的 SPEAK 系列）：
        - 只驱动 **`ParamMouthOpenY`**（嘴的开口量），**绝不动 `ParamMouthForm`**：
          模型声明的 LipSync 组里两者都有，但 MouthForm 是嘴的**形状**，属于心情 ——
          两个东西都去写就是互相打架；
        - 振荡不规则（每次过零换一个目标频率与振幅），干净正弦读起来像机器；
        - 叠加方式是 `clamp(心情值 + 扑动量, min, max)`，上限/下限取模型**声明**的范围
          （`declaredParameterRanges()`），绝不硬编码：所以"说话时刚好很惊讶"仍然是张大嘴；
        - 停止说话后扑动幅度按与心情过渡**同一套机制**衰减回 0（不是啪一下闭嘴）；
        - 不在说话时扑动量恒为 0（对渲染结果零影响）。*/
    void setSpeaking(bool speaking);

    /***这一帧的 TTS 响度电平**（0~1，由 Dialog 从 TTS 字节算出的包络提供）。

      与 `setSpeaking` 成对：那个说"在不在播"，这个说"这一拍有多响"。

      语义与约定（都是可观察行为，见 test_live2doffscreen 的 ENVELOPE 系列）：
        - 电平**只在真的有包络时**才被喂进来。一旦喂过，渲染器进入"包络模式"：
          `开口量 = clamp(心情值 + 电平 × 量程比例 × (1+不规则抖动), 声明min, 声明max)`；
        - **电平为 0 = 嘴停在心情自己的值**（一丝残余开合都不许有）——
          这就是用户要的"句子之间闭嘴"；
        - `setSpeaking(false)` 会把包络模式复位。于是**下一句没有包络时**
          （例如用户把 vits 的 format 配成 mp3）行为回到与今天逐位相同的盲扑动：
          忘了复位 = 嘴整句冻在心情值上，而画面症状与"接线断了"一模一样，极难反推；
        - 离散电平（Dialog 每 50ms 一拍）由攻击/释放平滑，否则嘴是一格一格地跳；
        - 与扑动一样**只驱动 `ParamMouthOpenY`**，绝不碰 `ParamMouthForm`（嘴形属于心情）。

      窗口层只转手这一个数 —— 采样率/位深/声道那些事到 Dialog 为止，
      渲染器不该知道世上存在 WAV。*/
    void setSpeechLevel(float level);

    /***情绪睁闭眼乘数**（默认 1.0 = 眨眼自己说了算）。

      语义：对模型声明的**眨眼参数**（model3.json 的 Groups[EyeBlink]），
      最终值 = 乘数 × 眨眼本帧写下的值（见 .cpp 里 applyEyeOpennessMultiplier 的说明）。
      不在眨眼组里的参数不受影响；乘数 ≤0 会被抬到一个小正数（不彻底闭合）。

      `setParameterOverrides` 会自动把覆盖表里涉及眨眼参数的那几项挑出来喂给它 ——
      所以正常路径（情绪预设）不需要手动调这个。它单独存在是为了：
        1) 把"情绪对眨眼的干预"做成一个**可独立开关**的量：关掉（=1.0）就是
           "这个心情不碰眼睛"的旧行为，用来量"闭眼到底改变了多少像素"；
        2) 校准/实验时可以单独试某一个开度，不必伪造一整个心情。*/
    void setEyeOpennessMultiplier(const QHash<QString, float> &moodValues);

    /*解除上面那份**显式**乘数，让眼睛重新跟随当前覆盖表（心情）。

       为什么要这个"解除"：显式乘数在过渡逻辑里优先于覆盖表（否则校准用的乘数会被
       每帧重推回心情值，实测画面差 0 像素）。所以校准/实验用完必须显式放手 ——
       否则那份乘数会把之后所有心情的眼睛都钉住。换心情（setParameterOverrides）
       会自动解除。*/
    void clearEyeOpennessMultiplierOverride();

    /*读取参数当前值。用于验证覆盖是否真的生效（比对比两帧像素可靠：
      两帧之间物理动画本来就会变，像素差异无法证明是参数造成的）。*/
    float parameterValue(const QString &parameterId) const;

    /*眨眼驱动器本帧写下的**原始**开眼度（0 = 闭紧、1 = 全睁；模型没声明眨眼组时恒为 1）。

      与 parameterValue 的区别：那是"情绪乘完之后的最终值"。判断"眨眼还活着，只是被情绪
      压小了"还是"眨眼被情绪钉死了"，必须看这个原始值 —— 只看最终值会把两者混为一谈。*/
    float blinkValue() const;

    /*模型**自己声明**的参数取值域（moc 的真值）。

      为什么必须有它：数据表（`parameter-map.json`）里的 [min,max]/中立值只能来自这里。
      曾经那几个范围是从模型自带的 `*.vtube.json` 的 VTS OutputRange 手抄的 ——
      那是"某个 VTube Studio 配置允许推多远"，与 moc 声明的取值域是两回事。
      实测代价：`ParamMouthForm` 被抄成 [-1,+1]、中立 0.0，而 moc 是 [-1,0]、默认 -0.5，
      于是"想笑"的 +0.9 被 Core 夹成 0.0（表情里写死一个到不了的值），
      中立位还被钉在 0.0（不是作者的静息姿势）。范围核对测试拿这份数据去钉住这个错误类别。*/
    struct DeclaredRange
    {
        float min = 0.0f;     // moc 声明的最小值
        float neutral = 0.0f; // moc 声明的**默认值**（静息值，不是 0）
        float max = 0.0f;     // moc 声明的最大值
    };
    /*参数 ID → 声明范围。未加载/该参数不存在时该项不出现（调用方据 contains 判定）。*/
    QHash<QString, DeclaredRange> declaredParameterRanges() const;

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

    /*水印参数 ID（由模型目录里的 *水印.exp3.json 推出，如 Param137）。置 0 才能关掉水印。*/
    QString watermarkParamId() const;

    /***指定**下一帧**的时间步长（秒），只生效一帧；不指定就走墙钟。

      两种正当用途，本质是同一件事 —— 把"时间"从墙钟手里拿回来：

        1. **生产（探针的虚拟时间）**：立绘大小变更的探针要在主线程上连渲 20 帧，
           而待机动作/呼吸/物理全都由帧步长驱动。以前靠 `QThread::msleep(150)` 让时间
           真的过去，代价是 **3.0s 主线程冻结**（实测；输入框每敲一个字符就触发一次，
           用户看到的就是"改完大小之后持续卡顿"）。改成每帧喂一个固定步长之后，
           动作照走、"这一段时间"照覆盖，但主线程一秒都不睡。

        2. **测试（把相邻两帧钉在虚拟时间上）**：`renderFrame` 的帧步长来自墙钟，
           并且被夹在 kMaxFrameDeltaSeconds = 0.1s 以内。于是"连续渲两帧"在没有抢占时是
           几毫秒的虚拟时间，在被抢占时却会**每一步都吃满 100ms** —— 这个夹取让"墙钟间隔"
           与"虚拟时间"彻底脱钩：100ms 的墙钟间隔与 150ms 的墙钟间隔给出的是**同一个**
           100ms 步长。
           实测后果（2026-09-29，26 个 CPU 燃烧线程把 24 逻辑核压到 100%）：
           test_live2dwindow 的 moodEyeOpennessComposesWithBlink 里"相邻两帧"的背景漂移
           从空闲时的 0~5 像素涨到 1276 / 4121 像素，而闭眼的信号只有 1223 像素
           （两帧眨眼原始值都是 1.0000）—— "漂移"与"信号"同量级，比值断言必然会红。
           注意这条**不是**偶发时序抖动：负载下每一帧都吃满夹取上限，所以它每次都发生在
           同一处。置 0 之后呼吸/待机动作/物理/眨眼都停在原地，只有要观察的那个量在变。

      步长**仍然过 kMaxFrameDeltaSeconds 的夹取**，所以两条用途走的是与真实路径同一条
      代码路径，只是输入不同（探针传 150ms 会被夹成 100ms，与它以前真正看到的步长逐位相同）。
      用法：渲染一帧前调一次，它只影响紧接着的那一帧；
      clearNextFrameDelta() 回到墙钟路径（并重置时间基准，免得"冻结期间走过的墙钟"被算进
      解冻后的第一帧）。*/
    void setNextFrameDeltaSeconds(float seconds);
    void clearNextFrameDelta();

    /***待机摆动（idle sway）**：让角色"站着、重心在动"，而不是只有呼吸。

      数据来自 `<模型目录>/presets/idle.json`（由 Live2DMoodPreset::loadIdle 装载，并已完成
      "语义名 → 参数 ID"的解析与"驱动器占用的参数要剔除"的体检）；这里只负责**按累计虚拟
      时间**把它施加出去：`参数 += amplitude × sin(2π(t/period + phase))`。

      五条约定（都是可观察行为，见 test_live2doffscreen 的 IDLE 系列）：
        - **加在心情值之上**：心情覆盖（applyMoodBlend）先写绝对基准，摆动是加法。
          于是"生气地站着"仍然是生气的，只是身体在重心上来回移 —— 这是需求的原话；
        - 施加位置在 `SaveParameters()` **之后**（与呼吸/物理同一段）：驱动器这一帧写下的值
          不进"本帧保存值"，下一帧 LoadParameters 就不会把上一次的摆动当成新基准再叠一遍；
        - 结果**夹取到模型声明的范围**（declaredParameterRanges），绝不硬编码；
        - 呼吸/头发（ParamBreath / ParamHair*）与 ParamMouthForm **永远不写**：前两个由呼吸
          驱动器与物理每帧写入，后者是嘴形、属于心情。**即使条目里写了也跳过** ——
          数据写错时驱动器自己也要守纪律，不能只靠数据表自觉；
        - **空表 = 关掉**（参数不再被这个驱动器碰）：idle.json 缺失/非法时就是这条路径。

      时间基准是渲染器自己累计的帧步长（renderFrame 的 deltaSeconds，含 0.1s 夹取），
      所以注入虚拟时间（setNextFrameDeltaSeconds）能让相位完全可复现。*/
    void setIdleSway(const QVector<Live2DMoodPreset::IdleSwayEntry> &entries);

    /*待机摆动**本帧真正施加的偏移量**（参数单位；该参数不在条目里、或被跳过、或摆动已关时
      是**精确的 0**）。

      为什么需要它（而不只是读 parameterValue）：ParamAngleZ / ParamBodyAngleX 每帧还被
      呼吸驱动器**加**一个摆动，读回值是"心情 + 呼吸 + 摆动"的合成值，从中量不出摆动自己的
      幅度/周期/相位。这个通道就是"这个驱动器这一帧贡献了多少"，校准与测试按它判读。*/
    float idleSwayOffset(const QString &parameterId) const;

    /*待机摆动的**累计虚拟时间**（秒）—— 相位与周期的唯一时基。
      调用方（出对照图/测试）按它对齐采样：注入固定步长后 t 只由帧序号决定，与机器负载无关。*/
    float idleSwayElapsedSeconds() const;

    /*摆动是否真的在施加 = 至少有一条条目**能落地**（数值合法、不是呼吸/头发/嘴形这类
       "别人的"参数，而且模型确实声明了那个参数）。空表、或整张表都是非法条目时都是 false。*/
    bool isIdleSwayActive() const;

    bool isLoaded() const;

    /***诊断快照：把"GL 侧到底发生了什么"从渲染器里读出来。

      为什么需要它（而不是只看渲染结果）：多个渲染器共处一个进程时，画面症状只有
      一个"空白"，但它可能是上下文没切过去、FBO 不完整、贴图落在了别的上下文里，
      或者着色器/离屏单例那类**进程级**状态被另一个实例改写。这些原因在像素上长得
      一模一样，只有把每一层各自的标志读出来才能区分。所以这里把四件事一次性交出去：
        - 上下文是否真的是**本实例的**（contextCurrent 与 contextId）；
        - 本帧画到哪张 FBO 上、读回又从哪张读（frameBufferId / readBackFboId）；
        - 贴图 ID 列表（上传到了哪个上下文，由 ID 与当前上下文对照判定）；
        - 由框架着色器单例交出去的着色器程序（shaderProgramIds，前 4 个）。

      **只用于诊断**，不参与渲染路径，运行时开销为零。*/
    struct DebugState
    {
        unsigned long long contextId = 0;   // 本实例的 QOpenGLContext 指针（身份）
        unsigned long long currentContextId = 0; // 调用时真正 current 的上下文
        bool contextCurrent = false;        // 上两者是否一致
        unsigned int frameBufferId = 0;     // 本帧真正绑定的那张 FBO（渲染目标）
        unsigned int readBackFboId = 0;     // 读回时绑定着的 FBO
        unsigned int colorBufferId = 0;     // 渲染目标的颜色附件贴图
        QVector<unsigned int> textureIds;   // 模型贴图（上传时所在的上下文即创建它的上下文）
        /*本帧真正用过的着色器程序名（去重，最多几条）与"它们在当前上下文里存不存在"。
           为什么必须查 glIsProgram 而不是只看非零：GL 的程序名是**按上下文**命名的，
           同一个数字在另一个上下文里可能完全不存在 —— 那正是"画不出来但不报错"的形态。*/
        QVector<unsigned int> shaderProgramIds;
        QVector<unsigned int> shaderProgramMissing; // 在当前上下文里**不存在**的程序名
        QVector<unsigned int> textureMissing;       // 在当前上下文里**不存在**的贴图名
        unsigned int frameBufferStatus = 0;         // glCheckFramebufferStatus 的原样返回值
        bool targetValid = false;
        /*"这一帧有没有真的发出去绘制"的逐 drawable 计数（见 .cpp 里 DrawDiagnostics 的说明）：
           CPU 侧数据正常但 GPU 侧一条三角形都没画时，这两个数会在两个实例之间分道扬镳。*/
        struct DrawDiagnostics
        {
            int drawableCount = 0;
            int visibleCount = 0;
            int visibleWithTexture = 0;
            int visibleWithIndices = 0;
            int unbindableTextureCount = 0;
            int maskCount = 0;
        };
        DrawDiagnostics drawDiagnostics;
    };
    DebugState debugState() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif // LIVE2DOFFSCREENRENDERER_H
