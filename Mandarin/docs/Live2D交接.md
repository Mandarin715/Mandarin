# Live2D 桌宠 · 交接文档

> 写作时间：2026-09-30。作者：上一个 AI 会话（DeepSeek Harness）。
> 本文档是**接手起点**；`docs/Live2D方案.md`（设计）与 `docs/Live2D实施计划-阶段0-1.md`（阶段 0-1 计划）仍然有效，
> 但 **`docs/Live2D进度与待办.md` 已过时**（落后约 8 轮），其「待办」段落请以本文档为准。

---

## 0. 一分钟上手

| 项 | 值 |
|---|---|
| 仓库 | `C:\Users\asus\Desktop\Mandarin`（Qt 6.6.3 + C++20 + MSVC 2022，VS2022 generator） |
| 构建 | `cmake --build build2 --config Release --target Mandarin` |
| 测试 | `ctest --test-dir build2 -C Release`（10 个目标，全绿约 150s） |
| 单跑用例 | `build2\tests\Release\test_live2dwindow.exe <用例名>` |
| **启动桌宠** | **用户自己跑 `build2\Release\启动.bat`。AI 绝不代启动**（AGENTS.md 明令） |
| 当前配置 | `Documents\Mandarin\config.ini` → `renderMode=live2d`、`live2dModel=miku`、`live2dFps=120`、`live2dScale=1.5` |
| 构建前提 | 链接 `Mandarin` 前必须确认 `Mandarin.exe` 没在跑（否则 `LNK1104`） |
| 控制台编码 | 测试输出是 UTF-8，控制台按 GBK 解 → 中文乱码。**过滤时用 ASCII 关键字**（`Live2D`/`QINFO`/`PASS`），不要用中文 |

### 数据在哪

```
Documents/Mandarin/Live2D/<模型>/                     ← 模型本体（禁二传/禁二改，gitignore，永不入库）
Documents/Mandarin/Character/Assets/亚托莉/Live2D/<模型>/   ← 我们的映射与预设（见下）
Mandarin/assets/live2d-presets/                       ← 上面那三份的**仓库内备份**（只读，应用不读它）
```

每个模型目录下：
- `parameter-map.json`：**语义名 → {真实参数 ID, min, max, neutral}**
- `presets/moods.json`：14 个**情绪原型**（相对 neutral 的增量）+ 26 条「立绘心情名 → 原型」别名
- `presets/idle.json`：待机摆动，每条 = `{语义参数名, 振幅, 周期秒, 相位(占周期比例)}`

**关键分工**：**词表属于角色**（26 个心情名 = 该角色 `Tachie/` 下 PNG 的文件名），**参数属于模型**。
所以同一份 `moods.json` 的别名表对所有模型都一样，只有原型数值按各自模型标定。

---

## 1. 已完成（提交史，全部已推送）

| 提交 | 内容 |
|---|---|
| `70f3cc4` | 阶段 1：窗口层下沉到 `CharacterWindowBase`（穿透/拖拽/位置持久化/气泡/文件拖放），零重复 |
| `cd741cc` | 阶段 2：离屏渲染打通；水印极性实测为**反的**（0 = 显示） |
| `b9894eb` | 桌宠窗口接通；`character/renderMode=png\|live2d`；失败回退 PNG |
| `c72f008` | 修复：模型**完全冻结**（没注册任何参数驱动器）+ 人物**右侧被裁** + `model3.json` 名不匹配 + 测试污染用户配置 |
| `f36d300` | 修复：人物被塞在过宽画布里、且**被横向压扁 17~19%** → 画布宽高比改为等于人物真实宽高比 |
| `46b349c` | 修复：**画面不清晰** → 画布改为**设备像素对齐**（详见 §3） |
| `24427b8` | **心情 → 表情**打通（整组替换覆盖 + `Live2DMoodPreset` + 加载模型/应用心情分离） |
| `e633cc5` | **心情平滑过渡**（约 200ms，可配）+ 去掉切换时那次同步渲染（卡顿的一半）+ **说话张嘴**（3~5Hz 盲开合） |
| `4b6e546` | 修复：**改立绘大小冻结 3 秒**（探针每次重跑，且每敲一个字符触发一次） |
| `6026ea9` | **口型包络**：解析 WAV 算响度包络，**停顿处闭嘴**；非 WAV 退回盲开合 |
| `0270139` | **数据驱动全身摆动**（待机时重心移动，不再是只有呼吸） |
| `5d394d8` | 参数范围对账**泛化到所有有映射表的模型**；腿部实测诊断；校准素材按模型分开 |
| `4fdfe15` | 预设数据入库备份 + README |
| `723c535` | 修复：**同进程第二个渲染器渲染空白**（根因：Cubism 的 GL 对象缓存在进程级单例）+ 连带 5 处测试/数据修正 |

---

## 2. 架构与每帧数据流

```
Dialog（业务枢纽）
 ├─ AI 回复 "心情|中文|日语|内心独白" → requestSetCharTachie(心情名) → CharacterWindowBase::reloadContent(心情) → 应用情绪预设
 ├─ TTS 播放 → requestSpeakState(bool) → SetSpeaking(bool)
 └─ TTS 音频字节（QBuffer 里）→ AudioEnvelope 解析 WAV → 50ms 定时器按 QMediaPlayer::position() 采样 → requestSpeakLevel(float) → SetSpeechLevel(float)

CharacterWindowBase（基类承接信号；PNG 路径为空实现）
 └─ Live2DCharacterWindow
      ├─ loadModel(名) / reloadContent(心情名)  ← 两者必须分开（曾经混用导致换心情无效）
      ├─ probeFigureMetrics()（**按模型缓存**，见 §3）
      ├─ relayoutContent()（画布尺寸 + 设备像素对齐 + 实测校正，最多 2 轮）
      └─ paintEvent（把帧绘进 rect()；自检不可重入）
            └─ Live2DOffscreenRenderer
                 ├─ 每帧：LoadParameters → motion → 覆盖（情绪插值）→ SaveParameters
                 │        → OnLateUpdate（眨眼/呼吸/物理）→ **眼睛乘数** → **口型包络** → **待机摆动** → Update
                 └─ 离屏 FBO → glReadPixels → QImage(RGBA8888) → 交给窗口
```

**持有者**：`Dialog` 只懂音频字节与播放位置；窗口只收到「一个数」；渲染器只懂参数与帧。
**不要让音频格式知识越过 Dialog**。

### 情绪预设的语义

- `neutral` = 模型**声明的默认值**（`mouthForm` 在 atri 是 -0.5，不是 0！）
- 原型只写**与 neutral 的增量**；未列出的参数保持 neutral
- 词表外的词 → **回退 neutral**（绝不让上一个情绪挂在脸上），只记一次日志
- 数据缺失/损坏 → 整个功能**优雅关闭**（记一条日志，什么都不应用，不崩）

---

## 3. 硬约束与陷阱（**最值钱的一节**，都是实测代价换来的）

### 3.1 参数范围只能从模型自己读

`Live2DOffscreenRenderer::declaredParameterRanges()`（底层 `CubismModel::GetParameter{Minimum,Default,Maximum}Value`）。
**不要**从 `*.vtube.json` / `*.cdi3.json` 手抄——那是"作者/VTS 怎么配的"，不是 moc 的能力边界。
上一个会话这么抄 atri，**27 条里错 4 条**（`mouthForm` 的 max 与 neutral、`eyeLOpen/ROpen` 的 max、`mouthOpen` 的 max），
后果是"笑"被夹成 0、且 neutral 把嘴钉在偏离静息的位置。
**已有测试逐参数对账**（`parameterMapRangesMatchModelDeclarations`，遍历所有带映射表的模型）——不要绕过它。

### 3.2 画布必须与设备像素对齐

`paintEvent` 用 `drawImage(rect(), img)`，而**源矩形以图像像素计** →
**图像像素尺寸必须精确等于 `rect() × dpr`**，否则 Qt 每帧做亚像素重采样（实测：清晰度 −11.9%，17% 像素被改）。
关键：`938 × 1.25 = 1172.5` **不是整数** → 就是这个半像素。
修法：`utils/DevicePixelAlign.h`，运行期从 dpr 推出对齐步长（1.25→4、1.5→2、**不许写死 4**）。
**不要去关 `SmoothPixmapTransform`**——那会连带毁掉超采样应有的平滑降采样。

### 3.3 画布宽高比必须等于人物真实宽高比

渲染器**分轴独立缩放**，所以「像素宽高比 == 画布宽高比」。一旦画布比例偏离人物比例，**人物就被拉伸**。
（原缺陷：画布 0.830 vs 人物 0.427 → 横向压扁 17~19%，即"像被挤扁了"。）
所有屏幕/画布夹取**必须保持比例**。

### 3.4 眼睛开合必须"乘"在眨眼之上

`CubismEyeBlink` 在 `OnLateUpdate` 里对睁闭眼参数**绝对赋值**。情绪值若写在它之前会被**完全丢弃**
（实测：困倦帧与中立**逐像素相同**）。
现方案：`最终 = 情绪值 × 眨眼值`，在 `OnLateUpdate` **之后**施加，且**只对 model3.json `EyeBlink` 组声明的参数**生效。
**不要**关掉眨眼——本模型没有身体待机动作，眨眼是"她还活着"的底线。

### 3.5 摆动/呼吸是**加法**，情绪值是基准

所以"生气地站着"仍然是生气的。摆动写 `clamp(值 + A·sin(2π(t/T+φ)), 声明min, 声明max)`。
⚠️ **miku 的摆动轴全是物理输出**（`physics3.json` 的 `Output.Destination.Id`），物理每帧重写基准 →
"基准 == 情绪值"在 miku 上**不成立**（测试已按"偏移叠加在所观察基准上"重写）。

### 3.6 Cubism 的 GL 对象在**进程级单例**里

`CubismShader_OpenGLES2::GetInstance()`（着色器程序名/属性位置）与 `CubismOffscreenManager_OpenGLES2::GetInstance()`（离屏目标）。
GL 对象名**按上下文解析** → 第二个未共享上下文里这些名字不存在 → `glUseProgram` **静默失败** → 空白帧。
**一个进程多个渲染器必须共享根上下文**（`Live2DCubismRuntime::shareContext()` + `setShareContext()` 在 `create()` 之前）。
这是硬约束，不是优化。

### 3.7 时基：帧步长来自墙钟且被夹在 0.1s

后果：**机器一被抢占，"墙钟间隔"与"虚拟时间"就脱钩**，所有"相邻帧"类测量都会失去可比性
（实测负载下漂移从 0~5 像素涨到 1276/4121 像素，而信号只有 1223 像素 → 断言必红）。
需要确定性的测量用注入 API：`setNextFrameDeltaSeconds()` / `clearNextFrameDelta()`（**会走同一条夹取路径**）。
注意：连着调 `renderFrameNow()` 时间几乎不推进；需要真实时间时必须 `QTest::qWait`。

### 3.8 测试**绝不许写用户的 `config.ini`**

窗口侧读配置走 `settingsPath()`（`MANDARIN_CONFIG_INI` 环境变量可重定向），测试指向 `QTemporaryDir`。
**验证方式**：跑 `ctest` 前后对真实 `config.ini` 做 SHA256 比对。
（历史上被污染过一次：用户设的 120/1.5 被测试清成 60/1。）

### 3.9 探针（probe）按模型缓存

探针量的是**模型的属性**（人物占多大、宽高比），**与画布尺寸无关** → `loadModel()` 才失效。
以前每次改大小都重探（20 次取样 × 150ms = **3 秒主线程冻结**），而且一个字符一次 → 敲 "150" 冻结 **9369ms**。
现在探针用**虚拟时间推进**（同一条夹取路径、覆盖的时间段与修复前逐位相同）**一秒都不睡**。

### 3.10 判断"观感"必须**看图**，而且常常要**放大**看

- 清晰度：1:1 原尺寸看不出，**2 倍最近邻放大**后才看清（做法见 `_compare-zoom.png`）
- 表情：出对照表逐张判读（`_mood-sheet*.png`）
- 全身动作：**不能用脸部裁切**（会把摆动藏起来），要出全身帧
- 单渲染器是否退化：**比帧哈希**，不要靠断言

---

## 4. 待办（按建议优先级）

### 4.1 `樱花miku` 的预设数据（**已证实不能复用 miku 的**）

实测：miku 有 `Param134/135`、樱花miku 有 `Param89/90`，两者**声明参数集不同**（各 141 条，
范围一致、最坏轮廓差 1.618%）。所以需要**单独做** `parameter-map.json` / `moods.json` / `idle.json`
**并重新目视校准**。做法完全照 miku 那次的流程（见 §5）。注意它的表情命名也不同
（`Param133` 在它是「哭」，在 miku 是「大葱」）。

### 4.2 B 阶段：换装 / 道具（用户说"稳定后再逐步改进"）

作者的 16 个 `exp3.json` 其实是**互斥开关组**（都是 `=±30` 的二元量），`vtube.json` 的热键就是作者的原命名：
- atri：`dress1/dress2/dress1.5`、`shoe1/shoe2`、`white eye`、`no highlight`、`Blood`、`Bird`、`Kani`蟹、`Screen`、`血衣组合`、`YES/NO`（`Param39` 点头/摇头）
- miku：`圈圈`(Param125)、`脸红`(Param130)、`前倾`(Param132)、`葱`(Param133)、`唱歌`(Param134)、`比心`(Param135)、`QQ人`(Param131+136)、`水印`(Param137)

**触发方式尚未决定**（用户在"右键菜单 / AI 剧情联动 / 台词联动点头摇头"里没选）。
注意 `Param131 QQ人` 实测会改动 **80.4%** 的像素（是个形态切换，不是小道具），慎用。

### 4.3 已知的小缺陷与缺口

| 项 | 说明 |
|---|---|
| 4 处 `QString::arg` 格式 bug | `.arg()` 链里混了 printf 的 `%.4f` → 失败信息显示字面 `%.4f`，且每次运行 Qt 打 `Argument missing`。`test_live2dwindow.cpp:3584`、`test_live2doffscreen.cpp:658/768/816`。**已在 HEAD 里**（非新引入），只影响失败信息 |
| Dialog 胶水层无单测 | 仓库没有 Dialog 测试目标（链接它会拖进整个应用）。两半都测了（`AudioEnvelope`、渲染器的"电平→嘴"），中间 Qt 接线只靠审阅。要补需先抽一个可测的 `VitsEnvelopeSampler` 值类型 |
| `rendersMoodArchetypeCalibrationSheet` 的偏差日志 | 会列出**驱动器持有的轴**（呼吸/摆动写的那些），做情绪校准时看这份日志要注意区分 |
| 回归证明用例非历史无关 | `singleRendererFrameHashesForRegressionProof` 在不同调用间帧哈希会变（进程级状态），但**基线构建表现相同** → 不是缺陷；其"固定输入"注释言过其实 |
| 老文档过时 | `docs/Live2D进度与待办.md` 落后约 8 轮 |

### 4.4 性能相关的可选优化

- **miku 在 120fps/1.5× 下 6.28ms**（预算 8.33ms 的 **78%**，atri 只要 4.18ms）。嫌紧就 `live2dScale=1`（4.35ms）
- **渲染在主线程**：想让高倍率不挤占主线程（流式输出/TTS/UI），正解是**渲染线程**（方案里的主方案）
- 布线条数最少可写：摆动**只对清单里的轴**做正弦，每帧 3 个 `sin` 量级，不需优化

---

## 5. 验证方法（**怎么证明"做完了"**）

1. **`ctest --test-dir build2 -C Release` 必须 10/10 全绿**——红着不许提交（会叫狼来了的测试比没有更糟）
2. **新断言必须在旧代码上失败**，并把 RED 证据（失败信息 + 数字）留下来；做不到就明说
3. **"防御型"断言要用变异测试证明它有牙**：把生产代码改坏 → 看它是否变红 → 改回来
4. **观感问题看图**（§3.10），别只看断言
5. **数据改动后同步仓库备份**并核对两份 SHA256 一致
6. **测试前后核对 `config.ini` 的 SHA256**（先取基准再动手）
7. 报数：每帧成本（`reportsFullPipelineFrameCost`，用户档位 `120fps/1.5×`）、`ctest` 总时长

---

## 6. 工作方式与纪律（本会话踩过的坑）

- **AI 不启动桌宠**。验证编译结果用产物存在性/报错判断，不要跑 GUI
- **禁止用烧 CPU 的方式加压**。上一个会话起过 16 个燃烧作业占满用户 CPU **且没先问**，用户直接发现了。
  需要负载**先问用户**，并且加压器必须**自终止 + 结束时验证进程真的消失**（别用 `-ErrorAction SilentlyContinue` 吞错误）
- **子代理会静默退出**（本会话出现 3 次，都在"追加第二轮指示后"）。所以：**收到交付必须自己独立复跑验证**，
  不要相信"9/9 通过"这种转述（真实发生过：转述 9/9，我自己跑出 1 个失败）
- **修 bug 先用实测定位根因**：本会话两次靠这个推翻了错误假设——
  ①"paintEvent 每帧重排"被 60 次重绘 0 次重排否证，真因是"每次输入事件一次 3 秒冻结"；
  ②"人物真实宽高比 0.355"是**被压扁后**的渲染值，真值是 0.427
- **测试可以"假绿"**：有一处曾经用 `continue` 跳过全部比较而"通过"。所以新测试要加**防空断言**（如 `comparedPieces > 0`）
- **行尾**：仓库索引是 CRLF，改动文件别引入整文件 EOL 重写（提交前看 `git diff --stat` 是否只有增量）
- PowerShell 5.1：`&&` 不支持；提交信息含引号/中文时**写到文件再 `git commit -F`**（避免被 shell 拆成 pathspec）

---

## 7. 一句话现状

**atri 与 miku 两个模型的功能都是完整的**（表情 / 平滑过渡 / 说话口型含停顿闭嘴 / 全身待机摆动 / 呼吸眨眼物理 /
设备像素对齐 / 构图正确），测试 10/10 全绿，工作树干净，全部已推送。
**剩下的主要工作是把 `樱花miku` 单独做完，以及 B 阶段的换装/道具。**
