# Live2D 集成方案（待实施）

> 2026-07-04 设计 · **2026-08-21 优化**：架构收敛为「单窗口 + 可插拔渲染器」，渲染改为离屏管线（规避透明窗口 + GL 冲突），补齐交互区复用、SDK 许可合规、口型方案修正与验收标准。
> **2026-08-22 优化（参考 AAAAGENT 表现层设计）**：新增 ①表现意图语义层（业务只表达意图，表现层负责映射）②参数映射集中化 `parameter-map.json` ③表现预设 + 平滑过渡 + 打断回中 ④口型三要素 ⑤模型指纹校验 ⑥验收清单细化为 7 步。**仅借鉴设计思路，未引入其代码**（其渲染走 Cubism Web SDK，与本项目 Native 路线不同栈）。
> **2026-08-22 二次优化（基于两个实际 Miku 模型实测）**：新增「实测模型」章节；确认口型参数为 `ParamMouthOpenY`、`EyeBlink` 已声明、**`Expressions`/`Motions` 未接线须外部加载**；确立 **禁二传 / 禁二改** 两条硬约束（模型不得进安装包、不得编辑模型文件）；补充 4096 纹理显存风险与内存降采样对策、水印默认关闭。
> **2026-09-28 实施前核实（逐条对照代码与模型文件）**：见「实施前核实」章节——修正 **4 处**（参数映射不全、水印光靠"不加载表情"关不掉、中文文件名的路径风险、阶段 1 影响面含 `MainWindow`），并确认 **Cubism Core 须由使用者自行接受 EULA 后下载**。

## 目标

新增 Live2D 渲染能力，支持导入 Live2D 模型（.moc3），替代或并行现有的 PNG 立绘（Tachie），实现更丰富的动态效果。

## 现状分析

当前 `Tachie` 的职责可以拆成两层：

| 层 | 内容 | Live2D 是否需要 |
|----|------|----------------|
| **窗口层** | 无边框、透明、鼠标穿透、alpha 交互区（`ApplyInteractiveRegionFromImage`）、拖拽（`DragHelper`）、按角色位置持久化（`SaveTachieLoc`/`RestoreTachieLoc`）、内心气泡、右键菜单 → `requestToggleVisible`、文件拖放 | **完全复用** |
| **渲染层** | 加载 QPixmap（PNG）、缩放缓存、`AnimePluginManager` 动画（位移/透明度/缩放步骤） | 换成 Live2D 渲染，机制不同 |

窗口层关注点与"渲染什么"是**可分离**的，这正是本次优化的切入点。

## 实测模型（2026-08-22）

手上有两个待用模型（`miku` / `樱花miku`），实测结论直接决定了几处设计：

| 项 | `miku` | `樱花miku` |
|----|--------|-----------|
| model3.json | `Version: 3` | `Version: 3` |
| moc3 ／ 纹理 | 9.07MB ／ 6×4096px（约 26MB） | 9.33MB ／ 6×4096px |
| 物理 | ✅ `physics3.json` | ✅ |
| 表情 | 8 个（圈圈/脸红/前倾/葱/唱歌/比心/QQ人/**水印**） | 6 个（QQ人/前倾/哭/圈圈/**水印**/脸红） |
| 动作 | `Scene1.motion3.json`（待机） | 同 |

**五个实测发现（已并入下方设计）**：

1. ✅ `Version: 3` → Cubism Native SDK 4/5 可直接加载；
2. ✅ **`EyeBlink` 组已声明**（`ParamEyeLOpen`/`ParamEyeROpen`）+ 有物理 → 自动眨眼、头发物理开箱可用；
3. ⚠️ **`LipSync` 组为空**（`"Ids": []`）→ 口型参数须自行指定。经 `vtube.json` 确认：口型 = **`ParamMouthOpenY`**（0~1），笑容 = `ParamMouthForm`；
4. ⚠️ **model3.json 无 `Expressions` / `Motions` 数组** → 8 个表情与待机动作**文件在但未接线**，标准加载器会得到 **0 表情 0 动作**，必须由应用层**按路径外部加载**（恰好也满足"禁二改"）；
5. ⚠️ 模型使用**中文命名自定义参数**（`Paramwaizui` 歪嘴、`Paramgulian` 鼓脸、`Paramguzui` 鼓嘴、`Paramtushe` 吐舌、`Param7`、`mouthRollLower`）→ **实证了参数映射集中化的必要性**（标准名与自定义名混用）。

**显存估算**：6 张 4096×4096 RGBA ≈ **约 400MB 显存**。且"禁二改"意味着不能换 model3.json 引用的低分辨率贴图 → 只能在**加载时内存降采样**（见风险表）。

## 实施前核实（2026-09-28）

本节是实施前的逐条复核结果，**推翻了原方案 4 处细节**，动手前请以本节为准。

### 资源就位情况

| 资源 | 状态 |
|---|---|
| Live2D 模型 | ✅ 已就位 —— `Documents/Mandarin/Live2D/miku/`、`Documents/Mandarin/Live2D/樱花miku/`；另有第三套 `D:\星月水母 试用版\`（单张 8192 贴图，**`LipSync` 组已正确声明**，技术上最好接，但目录内无授权说明，仅作备选） |
| Cubism Native SDK | ❌ **未就位**（`Mandarin/3rdparty/` 只有 ElaWidgetTools / sherpa-onnx / ZcAILib / ZcJsonLib / ZcWidgetTools） |

**SDK 获取需人工介入，不能自动化**：
- 官方下载页 https://www.live2d.com/sdk/download/native/ 的下载按钮**必须勾选同意 EULA 才可点**，不能让工具代接受许可；
- `Live2D/CubismNativeFramework`（GitHub，最新 tag `beta12`）**只有 `src`，不含 `Core`** —— 闭源的 `CubismNativeCore`（`Live2DCubismCore.h` + 预编译库）**只在官方 SDK 压缩包里**；
- 落地：使用者下载 `CubismSdkForNative-5-r.x.zip` → 解压到 `Mandarin/3rdparty/Live2DCubismSDK/`；该目录进 `.gitignore`（**SDK 二进制不入库**）。分发前按 EULA 确认 Core 能否随包重分发，若不可则安装包只带框架源码、Core 由使用者自备。

### 参数映射：名字可以查 `vtube.json`，但**取值范围只能问 moc**（2026-09-29 更正）

`miku.cdi3.json` 显示该模型共 **141 个参数**，且大量是数字名自定义参数（`Param125` 圈圈、`Param130` 脸红、`Param137` 水印…）。而 `miku.vtube.json` 的 `ParameterSettings` 段落**本身就是一张现成的「面捕输入 → Live2D 参数 + 输入/输出范围 + 平滑」映射表** —— 用它**找参数名**很好用。

⚠️ **但它的 `OutputRange` 不是模型的取值边界。** 那是「某个 VTube Studio 配置允许把参数推到多远」；真正生效的边界是 moc 自己声明的 min/max/default，由 Core 暴露：

```cpp
CubismModel::GetParameterMinimumValue(i) / GetParameterDefaultValue(i) / GetParameterMaximumValue(i)
// 渲染器已封装：Live2DOffscreenRenderer::declaredParameterRanges()
```

**实测代价**（atri，2026-09-29）：`parameter-map.json` 的范围当初就是照 `atri_8.vtube.json` 抄的 ——

| 参数 | vtube OutputRange（抄来的） | moc 声明（真值） | 后果 |
|---|---|---|---|
| `ParamMouthForm` | -2.5 ~ 2.5 → 写进 map `[-1, 1]`、neutral 0 | **[-1, 0]，默认 -0.5** | "笑"（+0.6~+0.9）被 Core 夹成 0，永远笑不出来；中立还被钉在 0（不是静息值） |
| `ParamEyeLOpen/ROpen` | 0 ~ 2 → 写进 map `[0, 1.9]` | **[0, 1]，默认 1** | surprised 的 1.7 被夹成 1.0 |
| `ParamMouthOpenY` | 0 ~ 2.1 → 写进 map `[0, 2.1]` | **[0, 1]，默认 0** | 幅度上限本来就到不了 2.1 |

**结论（纪律）**：
1. 参数**名/用途**可以查 `vtube.json` / `cdi3.json`；
2. `parameter-map.json` 的 **min / max / neutral 必须逐条等于 moc 声明的 最小 / 最大 / 默认值**，
   `neutral` 就是模型的**静息值**（≠ 0）；
3. 由测试 `parameterMapRangesMatchModelDeclarations` 逐条核对并打印模型真值，
   换模型/改数据时它会把差异全部报出来。

核实后 `atri` 本版会用到的参数（名字仍来自 vtube/cdi3，范围见上）：

| 用途 | 参数 | 出处 |
|---|---|---|
| 口型开合 | `ParamMouthOpenY` | vtube `MouthOpen`（moc 范围 0~1） |
| 笑容/嘴形 | `ParamMouthForm` | vtube `MouthSmile`（⚠️ moc 只允许 **-1~0**，正数写不进去） |
| 眨眼 | `ParamEyeLOpen` / `ParamEyeROpen` | model3 `EyeBlink` 组 + vtube |
| 眼球 | `ParamEyeBallX` / `ParamEyeBallY` | vtube |
| 呼吸 | `ParamBreath` | vtube（`UseBreathing=True`） |
| 头部 | `ParamAngleX` / `ParamAngleY` / `ParamAngleZ` | vtube，±30 |
| 身体 | `ParamBodyAngleX` / **`ParamBodyAngleY`** / **`ParamBodyAngleZ`** | vtube，±10 ← 原方案只列了 X |
| 眉毛 | `ParamBrowLY`/`RY`/`LAngle`/`RAngle`/`LForm`/`RForm`/`LX`/`RX` | vtube ← **原方案遗漏** |
| 头发摇动 | `ParamHairFront` / `ParamHairSide` / `ParamHairBack` | cdi3「摇动」← **原方案遗漏** |
| 眯眼 | `ParamEyeLSmile` / `ParamEyeRSmile` | cdi3 ← **原方案遗漏**（本模型的"笑"主要靠它） |
| 嘴部杂项 | `ParamMouthShrug` | vtube ← **原方案遗漏** |

> `absent` 的语义应修正为「本模型存在但**本版不使用**的参数**示例**」，**不是穷举** —— 141 个参数里绝大多数是物理/绑定内部参数，不需要逐个声明。

### 水印：不是"置 0"，是**置 1**（2026-09-28 实测更正）

`水印.exp3.json` 内容是 `{"Id": "Param137", "Value": 1.0, "Blend": "Add"}`，说明文件写「**水印按键默认打开，需在设置表情中关闭**」。

**原始推断（已证伪）**：以为"水印参数默认 1，置 0 即关闭"。**实测反了。**

离屏渲染出来后逐版目视比对，结论是：

| `Param137` | 实际画面 |
|---|---|
| `0`（moc 默认，即不施加任何表情） | **水印文字可见**（"禁止直播 / 禁止商用"、"Non-commercial use only"…） |
| `1`（施加 `水印.exp3.json` 的 +1.0） | **干净、无水印** |

原因：moc 里 `Param137` 默认就是 `0`（水印可见），而名为「水印」的表情是 `Add +1.0` ——
**触发该"水印"表情反而是把水印关掉**。模型说明里的"水印按键默认打开"指的是"水印处于可见状态"。

**落地要求（更正）**：
1. 默认隐藏水印 → 启动时**显式 `Param137 = 1`**，不是 0；
2. `parameter-map.json` 里水印项要写成显式的两个值（`hiddenValue` / `visibleValue`），
   **不要用通用的"closedValue: 0"语义**，否则一定写反；
3. 验收判据：启动后 `Param137 == 1` 且画面**无水印文字**（需目视确认，不能只看参数值）；
4. ⚠️ 水印文字本身是**烤进贴图**的观感来源之一，但实测它确实受该参数控制 —— 所以能关，只是极性相反。

> **教训**：这类"参数名听起来像什么、就以为它是什么"的假设，必须靠**渲染出来亲眼看**验证。
> 参数值回读（`Param137=1/0` 都读对了）完全无法发现极性问题。

### 情绪的睁闭眼：与眨眼**合成**，不能互相抢写（2026-09-29 实测）

`CubismEyeBlink` 每帧在 `OnLateUpdate` 里对 `Groups[EyeBlink]` 声明的参数
（`ParamEyeLOpen`/`ParamEyeROpen`）**绝对赋值**为本帧的眨眼进度（0~1）。

如果情绪预设像其他参数一样在 `LoadParameters` 之后"写绝对值覆盖"，结果必然是：
**眨眼后写、情绪被整个丢掉**。实测症状正是用户看到的 `mood-sleepy.png` 里**眼睛睁着** ——
sleepy 帧与 neutral 帧逐像素相同，参数回读恒为 1.00。受影响的还有
surprised / sad / cry / angry / excited 的睁闭眼。

**正确做法**：情绪值当**乘数**，在 `OnLateUpdate` **之后**施加：`final = 情绪值 × 眨眼值`。
- 情绪压到近乎闭合（0.05）→ 最终值也近乎 0，闭眼看得见；
- 眨眼进度仍在变 → 最终值随时间变（**眨眼活着**，这是"她还活着"的唯一线索，
  本模型的三个模型都没有身体待机动作，只有呼吸/眨眼/物理）；
- 情绪不碰眼睛（值 = 1）→ 乘数恒等，眨眼行为一模一样；
- 归眨眼管的参数由 **model3.json 的 `Groups[EyeBlink]`** 决定，不硬编码参数名。

**其余覆盖（眉毛/嘴/腮红/头身角度）位置不变**：物理与呼吸必须看到情绪值
（呼吸是**加性**的，预设值就是它围绕摆动的基准）——把它们也挪到眨眼之后会让它们看不到。

**验收**（`moodEyeOpennessComposesWithBlink` / `moodIgnoringEyesLeavesBlinkUnchanged`，两条都在旧实现上失败）：

| 观察量 | 旧实现（眨眼盖掉情绪） | 现在 |
|---|---|---|
| 相邻帧、只把眼睛乘数 1.0 → 0.05 的像素差 | **1 像素**（等于没变） | **1217~1219 像素**（0.21% 画布） |
| 同一时刻 `ParamEyeLOpen` 读回 | 1.0（= 眨眼值） | 0.05 = 乘数 × 眨眼（差 0.0000） |
| 闭眼心情下 13s 采样窗口里眨眼原始值 | — | 0.00~1.00（仍在眨） |
| 不碰眼睛的心情（neutral/happy） | — | 最终值逐帧 == 眨眼值（零影响） |

> ⚠️ 像素对照必须用**相邻帧**、且等眨眼处于**全睁**时再量：两次独立长采样之间
> 呼吸/物理相位不同，末帧差 5 万像素里绝大多数与眼睛无关；而落在眨眼过程中量，
> 读数会从 1219 掉到 353（实测到的偶发失败）。两条都是开发时踩过的坑。

### 中文文件名的路径风险（原方案遗漏）

`樱花miku` 与 `星月水母 试用版` 的 `model3.json` / `.moc3` / 贴图目录**全是中文名**（`樱花miku.moc3`、`樱花miku.4096/texture_00.png`）。Cubism Native 加载接口走 `csmString` → `fopen`，Windows 下按 ANSI 代码页打开 **UTF-8 中文路径会失败**。

**对策：全程内存加载，不用 SDK 的文件路径接口** ——
- 用 `QFile` 读 `model3.json` / `.moc3` / `physics3.json` / 贴图字节，再交给 `CubismMoc::Create(bytes, size)`、`CubismModelSettingJson(bytes, size)`、`CubismPhysics::Create(bytes, size)`；
- 贴图由我们解码为 RGBA 后交给渲染器；`model3.json` 里的贴图路径只当**相对路径清单**用，不交由 SDK 打开。

这条同时解决「模型目录只读」与「路径含中文/空格」，是**阶段 2 的第一条实现约束**。

### 阶段 1 影响面：不止 `Tachie`

抽 `CharacterWindowBase` 的实际改动面比原方案写的大：

| 文件 | 现状 | 需要改 |
|---|---|---|
| `windows/tachie/tachie.{h,cpp}` | 窗口层 + 渲染层揉在一个类（cpp 697 行） | 窗口层下沉到基类 |
| `main.cpp` | `Tachie tachieWin;`（**栈对象**，L132）+ 5 条信号连接（L138-149） | 改为按配置创建基类指针；渲染模式决定实例类型 |
| `windows/setting/setting.{h,cpp}` | `MainWindow(Dialog*, Tachie*)`（h L22 / cpp L16），连了 `Tachie::SetTachieImg`/`SetTachieSize`/`ResetTachieLoc`（cpp L66-70） | 形参与连接目标改为基类类型 |
| `.gitignore` | 无 Live2D SDK 条目 | 加 `Mandarin/3rdparty/Live2DCubismSDK/` |

> 已核实窗口层能力齐全且**可原样复用**：`WA_TranslucentBackground`（tachie.cpp L46）、`DragHelper`（L56）、`ApplyInteractiveRegion(const QRegion&)`（L85）、`ApplyInteractiveRegionFromImage()` 用 `_scaledImg.createAlphaMask()` + `region.translate(_scaledImgTopLeft)`（L123-130）、`mousePressEvent` 的 alpha 命中判定（L472-480）、`SaveTachieLoc`/`RestoreTachieLoc`（L541/L552）、内心气泡（L578/L636/L673）。
> **离屏渲染出的 alpha QImage 可直接喂给这套逻辑** —— 方案的核心判断成立。

## 核心架构（优化点 1：单窗口 + 可插拔渲染器）

**反对"独立 Live2DWindow 双窗口并行"**：窗口层（穿透/交互区/拖拽/位置/气泡/信号）会整体复制一份，且"互斥切换"UI 与状态管理成本高。改为两层渐进：

```
CharacterWindowBase（窗口层，全部复用）
├── PngRenderer      ← 现有 Tachie 行为（字节等价，零回归）
└── Live2DRenderer   ← 新增
```

**阶段 B（低风险，先做）**：抽出 `CharacterWindowBase`，把窗口关注点（无边框/透明/穿透/交互区/拖拽/位置/气泡/文件拖放/信号）全部下沉；`Tachie`（PNG）与新增 `Live2DWindow` 都继承它。PNG 路径行为不变，回归风险最小。启动时按配置 `character/renderMode: png|live2d` 只创建其中一个窗口（v1 切换渲染器 = 重建窗口，不追求热切换）。

**阶段 A（后续收敛）**：合并为单一 `CharacterWindow` + `CharacterRenderer` 接口，运行时按配置换渲染器。阶段 B 的接口设计直接为此铺路：

```cpp
class CharacterRenderer : public QObject
{
    Q_OBJECT
  public:
    virtual QImage renderFrame(double dt) = 0;              // 渲染一帧 RGBA
    virtual QSize contentSize() const = 0;
    virtual void setParameter(const QString &name, double value) = 0; // 底层参数入口
    virtual void playMotion(const QString &group, int index,
                            float fadeInSec, float fadeOutSec) = 0;   // 动作（带淡入淡出）
  signals:
    void frameReady(const QImage &frame);                   // 渲染线程→主线程
};
```

> `CharacterRenderer` 是**底层**（只管参数/动作/出帧）；业务语义在其上层（见"表现意图语义层"）。`Dialog` 的信号契约不变（`requestSetCharTachie(mood)` / `ShowInnerThought` / `HideInnerThought`），不感知底层渲染器。

## 表现意图语义层（优化点 4：业务与表现解耦）

**问题**：如果让 `Dialog` 直接调 `setParameter("ParamMouthOpenY", 0.8)`，业务代码就绑死了某个具体模型的参数名——换模型要改业务代码。

**方案**：业务只表达**意图**，`PresentationController` 负责把意图解析成参数/动作。

```
Dialog（业务层）
   │  只表达意图："开心" / "开始说话" / "思考中" / "待机" / "被打断"
   ▼
PresentationController（表现语义层）
   │  查 parameter-map.json + 表现预设 → 得到具体参数与动作
   ▼
CharacterRenderer（底层：setParameter / playMotion / renderFrame）
```

```cpp
enum class PresentationState {   // 生命周期性状态（互斥）
    Idle,        // 待机：呼吸 + 自动眨眼 + 视线游移
    Speaking,    // 说话中：口型联动
    Thinking,    // 思考
    Working      // 工作
};

class PresentationController : public QObject
{
    Q_OBJECT
  public:
    void setState(PresentationState state);      // 切状态（带过渡）
    void playMood(const QString &mood);          // 一次性情绪表现（如"开心"）
    void notifyInterrupt();                      // 被打断：立即停口型 + 平滑回中
  signals:
    void applyParameter(const QString &name, double value);
    void playMotion(const QString &group, int index, float fadeIn, float fadeOut);
};
```

**收益**：
- `Dialog` 只发 `心情` 语义，不碰参数名 → **换模型零改业务代码**；
- 打断、回中、状态互斥等规则集中在表现层，不会散落各处；
- 表现预设可独立预览/调试（对应 AAAAGENT 的 `presentation-preview`）。

## 渲染管线（优化点 2：离屏渲染，规避透明窗口 + GL 冲突）

原方案风险"OpenGL 与透明穿透窗口冲突"是真实的：**Qt 半透明窗口（`WA_TranslucentBackground`）里直接嵌 `QOpenGLWidget` 子控件，在多数平台上 alpha 合成异常**（GL 表面盖住窗口透明度）。对策：

```
Live2D 渲染线程（独立 QOpenGLContext）
  → Cubism 按帧更新（物理/动作/参数）
  → 离屏 FBO → 读回 RGBA QImage
  → 投递主线程：只做 blit（QImage 画到现有半透明窗口上）
```

- **交互区复用**：blit 用的就是 alpha QImage，`ApplyInteractiveRegionFromImage` 的命中判定直接复用（透明区域穿透、模型区域可点击），"鼠标穿透 vs 可交互"风险基本消解。
- **渲染线程与"全主线程"约定的关系**：业务逻辑仍全在主线程；渲染线程属于**媒体渲染管线**（与 QMediaPlayer 解码线程同性质），不破坏单线程业务约定。Cubism 物理/动作 60fps 在主线程跑大概率卡顿，建议 v1 就直接上渲染线程。
- **v1 保守档**：主线程离屏渲染（`makeCurrent`/`swapBuffers` 都在主线程）作为降级选项，文档标注取舍。

## 资源布局（优化点 5：参数映射集中化）

```
Character/Assets/<角色>/Live2D/<model>/
├── <model>.model3.json      # 入口（引用 moc3/纹理/动作/物理）
├── <model>.moc3
├── textures/  motions/  physics/
├── parameter-map.json       # ✨ 参数名/范围/闭口值的唯一来源（本次新增）
└── presets/                 # ✨ 表现预设（intent/mood → expression/motion）
    ├── idle.json
    ├── speaking.json
    └── moods.json
```

**`parameter-map.json`（换模型只改这里，不改代码）**：

```json
{
  "modelId": "miku",
  "fingerprint": "sha256:…",          // 见"模型指纹校验"
  "source": "miku.vtube.json",        // 生成来源（有则优先，避免手抄参数名出错）
  "params": {
    "mouthOpen":  { "id": "ParamMouthOpenY", "min": 0.0, "max": 1.0, "closedValue": 0.0 },
    "mouthForm":  { "id": "ParamMouthForm",  "min": -2.5, "max": 2.5 },
    "eyeL":       { "id": "ParamEyeLOpen",   "min": 0.0, "max": 1.0 },
    "eyeR":       { "id": "ParamEyeROpen",   "min": 0.0, "max": 1.0 },
    "eyeSquintL": { "id": "EyeL_Squint",     "min": 0.0, "max": 1.0 },
    "eyeSquintR": { "id": "EyeR_Squint",     "min": 0.0, "max": 1.0 },
    "headX":      { "id": "ParamAngleX",     "min": -30, "max": 30 },
    "headY":      { "id": "ParamAngleY",     "min": -30, "max": 30 },
    "headZ":      { "id": "ParamAngleZ",     "min": -30, "max": 30 },
    "bodyX":      { "id": "ParamBodyAngleX", "min": -10, "max": 10 },
    "bodyY":      { "id": "ParamBodyAngleY", "min": -10, "max": 10 },
    "bodyZ":      { "id": "ParamBodyAngleZ", "min": -10, "max": 10 },
    "gazeX":      { "id": "ParamEyeBallX",   "min": -1,  "max": 1 },
    "gazeY":      { "id": "ParamEyeBallY",   "min": -1,  "max": 1 },
    "breath":     { "id": "ParamBreath" },
    "hairFront":  { "id": "ParamHairFront" },
    "hairSide":   { "id": "ParamHairSide" },
    "hairBack":   { "id": "ParamHairBack" },
    "watermark":  { "id": "Param137", "min": 0.0, "max": 1.0, "closedValue": 0.0 },
    "browLY":     { "id": "ParamBrowLY", "min": -1, "max": 1 },
    "browRY":     { "id": "ParamBrowRY", "min": -1, "max": 1 },
    "browLAngle": { "id": "ParamBrowLAngle" },
    "browRAngle": { "id": "ParamBrowRAngle" },
    "browLForm":  { "id": "ParamBrowLForm" },
    "browRForm":  { "id": "ParamBrowRForm" }
  },
  "absent": ["Paramwaizui", "Paramgulian", "Paramguzui", "Paramguzui2",
             "Paramtushe", "Param7", "mouthRollLower", "ParamMouthShrug"]
}
```

> `absent` 是**本版不用、但模型确实存在**的参数**示例**，不是穷举（该模型共 141 个参数，其余为物理/绑定内部参数）。
> 表情触发参数也是数字名（`Param125` 圈圈、`Param130` 脸红、`Param133` 大葱、`Param134` 唱歌、`Param135` 比心、`Param137` 水印），由 `presets/moods.json` 按需置 1，不写进 `params`。

> 上表已按 `miku` 实测参数填写（见"实测模型"）：口型用 `ParamMouthOpenY`（模型 `LipSync` 组为空，必须显式指定）、闭口值 0；`absent` 是本模型存在但本版**不使用**的中文自定义参数（歪嘴/鼓脸/鼓嘴/吐舌等），显式声明以便后续按需启用。

- **不做隐式假设**：不同模型的参数 ID 不同（有的用 `ParamAngleX`，有的自定义）。缺参数时**显式声明在 `absent` 并降级跳过**，不静默失败、不硬写同名值；
- 表情/动作映射不再散落角色 `config.json`，统一进 `presets/`。

## 模型文件只读：表情/动作外部加载（硬约束：禁二改）

实测发现两个 Miku 的 `model3.json` **都没有 `Expressions` / `Motions` 数组**（表情与动作文件在磁盘上但未接线）。常规做法是往 `model3.json` 里补这两个数组——但模型授权明确 **禁止二改**，**不能动模型文件**。

**对策：应用层按路径外部加载，模型目录保持只读。**

```
presets/moods.json            # 我们自己的文件（不动模型）
  "开心" → { "expression": "脸红.exp3.json", "fade": 0.5 }
  "待机" → { "motion": "Scene1.motion3.json", "loop": true }

Live2DRenderer 启动时：
  1. 读 model3.json（只读）→ 载 moc3 / 纹理 / 物理
  2. 从 presets/ 拿到 .exp3.json / .motion3.json 的相对路径
  3. 直接按路径创建 Expression / Motion 并注册（不走 model3.json 的数组）
```

- Cubism Native 支持从文件直接创建 `CubismExpressionMotion` / `CubismMotion`，无需模型声明；
- **收益**：既解决"表情动作读不到"，又天然满足"禁二改"（模型目录全程只读）；
- **约束**：`presets/` 里引用的是模型文件路径，**不复制、不改名、不重打包**模型文件；
- **水印**：`miku`/`樱花miku` 的 `水印.exp3.json` **默认是开启状态**，模型说明也要求"需在设置表情中关闭"——**默认预设必须不启用 `水印`**（启动时不加载该 expression）。

## 口型与动作（优化点 6：口型三要素 + 打断回中）

| 输入 | 表现 | 说明 |
|------|------|------|
| AI 心情（`requestSetCharTachie(mood)`） | 表情/动作 | 经 `presets/moods.json` 映射 |
| VITS 播放（`QMediaPlayer::playbackState`） | 嘴型二元（v1） | 播放中=张嘴循环，停止=闭嘴 |
| 待机 | 呼吸 + 自动眨眼 + 视线 | SDK 内建参数（breath/blink） |
| 点击（model3.json hit areas） | 交互动作 | hit-test 命中 → 动作/表情 |
| （v2）音量口型 | 嘴型连续 | 见下 |

**口型三要素（必须逐项落实，否则"看起来不对"）**：

| 要素 | 要求 |
|------|------|
| **开口参数** | 用 `parameter-map` 里的 `mouthOpen.id`，取值钳制在 `min~max` |
| **默认闭口值** | 停止说话必须回到模型定义的 `closedValue`（通常 0），**不能停在半开** |
| **播放结束归零** | VITS 播放结束 → 口型归零 + 平滑回中性（非硬切） |

**打断语义**：用户新输入打断当前回复时 → `notifyInterrupt()` → **立即停止口型参数**（不等当前句播完）+ 平滑回中。这是"自然感"的关键，也是 AAAAGENT 文档里明确要求的验收项。

**口型方案修正（保留原结论）**：原方案"`QAudioOutput` + PCM 分析"不可行——Qt6 的 `QAudioOutput` 是 sink（只放音、不可读）。要拿音量得走 `QAudioDecoder` 把 MP3 解成 PCM → RMS → 嘴参数，或改 VITS 管线直接送 PCM。作为 v2 优化项，接口（`setParameter(mouthOpen, value)`）预先留好，v1 二元、v2 连续不破坏接口。

## 模型指纹校验（优化点 7）

- 每个模型的 `parameter-map.json` / `presets/` 记录 **`fingerprint`**（对 `.moc3` + 关键资源算 hash）；
- **加载时校验**：指纹不匹配 → **明确报错并拒绝启用该模型的预设**（不静默沿用旧映射）；
- **目的**：防止"模型换了、参数映射还是旧的"这类**静默错误**——它会表现为动作错乱/表情残留，极难排查。

## SDK 集成

- **Live2D Cubism SDK for Native**（4.x），OpenGL ES 2 渲染器（`CSM_Renderer_OpenGLES2`）+ LAppModel 模式。
- 加载链：model3.json → .moc3 → textures → physics → motions。
- CMake：SDK 以静态库集成（`target_link_libraries` + include 目录），放进 `3rdparty/Live2DCubismSDK/`。
- **许可/合规**：
  - 下载需同意 [Live2D Proprietary Software License Agreement](https://www.live2d.com/download/cubism-sdk/)。
  - 分发前核对 EULA：SDK 二进制能否随应用重分发、是否需要展示 Live2D 版权标识。
  - 模型文件版权归作者所有，导入 UI 提示用户自行确认授权。
  - ⚠️ **实测模型的授权（必须遵守）**：`miku` / `樱花miku` 的使用说明写明——**可免费使用作为桌宠**（正是本项目用途），但 **不可二传二改**、严禁商用与直播牟利；水印表情默认开启须关闭；非商用发布视频需注明出处（绘制 **玄宝酱** ／ 建模 **怂怂koe**）。落地要求：**模型不入安装包 / Release**、**模型目录只读**。
  - ⚠️ **美术是效果的大头**：参考项目（AAAAGENT）演示观感好的主要原因是它使用了**量贩购买的第三方模型**，且该模型**不随其仓库分发**。自备免费/低价模型时，表现效果取决于该模型的绑定质量，**不是换渲染器就能达到**。

## 与现有功能关系

| 功能 | 关系 |
|------|------|
| Dialog | 信号不变；只发"心情/状态"语义，经 `PresentationController` 落到参数 |
| AnimePlugin | PNG 动画步骤对 Live2D 不适用，但**触发映射机制**复用 |
| VITS | 完全复用，`playbackState` 驱动嘴型（含三要素与打断回中） |
| 位置/拖拽/穿透/气泡 | 由 `CharacterWindowBase` 全量复用 |

## 分阶段实施

| 阶段 | 内容 | 验收 | 预估 |
|------|------|------|------|
| **0** | **前置**：下载 Cubism Native SDK（需人工同意 EULA）→ `3rdparty/Live2DCubismSDK/`；`.gitignore` 加该目录；确定首测模型 | SDK 头/库可被 CMake 找到；`git status` 不出现 SDK 与模型文件 | 0.5 天（含下载） |
| 1 | 抽 `CharacterWindowBase`，Tachie 行为收敛到基类（**含 `main.cpp`/`setting.*` 改造**，见影响面表） | PNG 路径行为与现在一致；拖拽/穿透/位置/气泡回归通过 | 1–1.5 天 |
| 2 | CMake 集成 Cubism SDK + 离屏渲染管线（渲染线程） | 静态模型渲染到半透明窗口，alpha 正确，穿透/交互区正确 | 2-3 天 |
| 3 | **`parameter-map.json` + 模型指纹校验 + 表情/动作外部加载** | 换模型只改映射文件即可加载；指纹不匹配明确报错；缺参数按 `absent` 降级；**模型目录只读的前提下表情与动作能加载** | 1-1.5 天 |
| 4 | 参数控制（眨眼/呼吸/表情映射 + 纹理降采样 + 水印默认关） | 待机有呼吸眨眼；mood → 表情正确；4096 纹理按配置内存降采样；**默认不显示水印** | 1-2 天 |
| 5 | **表现意图语义层 + 预设 + 平滑过渡** | `Dialog` 不出现任何参数名；状态切换有过渡；预设可独立预览 | 1-2 天 |
| 6 | 动作播放 + 物理 | motions 触发正确；物理开启无崩 | 1 天 |
| 7 | AI/VITS 桥接（含口型三要素 + 打断回中） | 心情换表情；播放中张嘴、结束闭口回零；**打断立即停口型并回中** | 1 天 |
| 8 | 点击交互（hit areas） | 点击模型区域触发对应动作 | 1-2 天 |
| 9 | 模型导入 UI + 渲染器切换 | 按角色导入/切换模型；config 持久化；重启恢复 | 1 天 |
| 10 | 全量回归 + 低端机性能验证 + **打包合规检查** | 见下方 **9 步**验收清单；帧率达标（可降帧档位）；**打包产物 `build2/Release/**` 不含模型文件** | 0.5-1 天 |

总计约 **12-16 天**。

## 验收清单（优化点 8：逐项可测）

1. **中性姿态**：静态加载正确，无错位、无裁切、透明混合正确；
2. **口型**：连续说话后**正确闭口**；播放结束口型归零；
3. **眨眼**：待机自动眨眼，频率自然、不机械；
4. **转头/视线**：头顶/眼球参数在范围内，**不过冲**；
5. **动作叠加**：表情与动作可叠加，**不同表情切换不互相残留**；
6. **打断**：说话中被打断 → 口型**立即停止** + 平滑回中；
7. **回中**：思考/工作状态结束 → 回到中性姿态，无残留；
8. **水印**：默认**不显示水印**（`水印.exp3.json` 未被启用）；
9. **授权合规**：打包产物 `build2/Release/**` 中**不含任何模型文件**；模型目录保持**未被修改**（只读使用）。

## 风险与对策

| 风险 | 对策 |
|------|------|
| SDK 许可 / 分发合规 | 实施前核对 EULA；安装包附版权说明 |
| **SDK 必须人工下载**（官方页需勾选同意 EULA；`Core` 不在开源框架仓库） | 列入阶段 0 前置；解压到 `3rdparty/Live2DCubismSDK/`；该目录进 `.gitignore` |
| **模型文件名含中文**（`樱花miku` / `星月水母`） | **全程内存加载**，不用 SDK 的路径接口（见实施前核实章节） |
| **水印光靠"不加载表情"关不掉**（`Param137` 初值可能为 1） | 启动时**显式置 `Param137 = 0`**；验收判据改为"启动后 `Param137 == 0` 且画面无水印" |
| **阶段 1 影响面被低估**（`main.cpp` 栈对象 + `MainWindow(Dialog*, Tachie*)`） | 影响面表已补 `main.cpp` / `setting.*`；预留 0.5 天 |
| **参数映射手抄易错**（141 个参数、数字名混用） | 优先从模型自带 `*.vtube.json` 的 `ParameterSettings` 生成 |
| **模型授权「禁二传」**（不得再分发） | 模型**绝不进安装包 / Release**：只放 `Documents/Mandarin/Character/Assets/`（安装目录之外），由用户自备；打包脚本排除模型目录；README 说明需自行取得授权模型 |
| **模型授权「禁二改」**（不得改模型文件） | 模型目录**全程只读**：表情/动作**外部加载**，不编辑 `model3.json`、不重打包模型；`presets/` 只引用路径 |
| **表情/动作未接线**（model3.json 无 Expressions/Motions 数组） | 应用层按路径外部创建 Expression/Motion（见"模型文件只读"） |
| **水印默认开启** | 默认预设不加载 `水印.exp3.json`；设置里提供开关（默认关） |
| **4096×6 纹理 ≈ 400MB 显存** | 加载时**内存降采样**（配置可设纹理上限，如 2048）；低端机降档；不修改贴图文件 |
| **美术质量不达预期**（换渲染器≠好看） | 明确预期：效果主要由**模型绑定质量**决定；先选定授权模型再评估 |
| 透明窗口 + GL 合成异常 | **离屏渲染规避**（不嵌 QOpenGLWidget） |
| 渲染线程 GL 上下文管理 | 独立 QOpenGLContext + 帧队列投递主线程 |
| 主线程卡顿（保守档） | 渲染线程为主方案，保守档仅降级 |
| **换模型后参数映射不匹配** | `parameter-map.json` 集中化 + **指纹校验**拒绝静默错误 |
| 业务代码绑死参数名 | **表现意图语义层**：Dialog 只发语义 |
| 口型停在半开 / 打断不归零 | 口型三要素 + `notifyInterrupt()` 逐项进验收 |
| 编译产物增大 | SDK 库 +5MB 左右；模型文件按角色按需放置，不入安装包 |
| 与 AnimePlugin 的语义冲突 | PNG 动画仅 PNG 渲染器使用，映射机制共用但数据源分离 |
| 低端机 60fps 压力 | 可降帧（30fps）或缩小渲染分辨率 |

## 参考

- [Live2D Cubism SDK for Native](https://www.live2d.com/download/cubism-sdk/)
- [Open-LLM-VTuber](https://github.com/Open-LLM-VTuber/Open-LLM-VTuber) — 渲染线程 + 参数桥接参考实现
- [phoiex/AAAAGENT](https://github.com/phoiex/AAAAGENT) —— **表现层设计参考**（表现意图语义层、参数映射集中化、预设与预览、口型/打断验收清单、模型指纹思路）。⚠️ 该项目走 Cubism **Web** SDK + Electron，与本项目 Native 路线**不同栈**，仅借鉴设计、未引入其代码；其演示模型为第三方购买、不随其仓库分发，本项目需自备授权模型。
- Qt 透明窗口 + OpenGL 合成问题（`WA_TranslucentBackground` + GL 子控件）
