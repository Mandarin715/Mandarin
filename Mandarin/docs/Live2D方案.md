# Live2D 集成方案（待实施）

> 2026-07-04 设计 · **2026-08-21 优化**：架构收敛为「单窗口 + 可插拔渲染器」，渲染改为离屏管线（规避透明窗口 + GL 冲突），补齐交互区复用、SDK 许可合规、口型方案修正与验收标准。

## 目标

新增 Live2D 渲染能力，支持导入 Live2D 模型（.moc3），替代或并行现有的 PNG 立绘（Tachie），实现更丰富的动态效果。

## 现状分析

当前 `Tachie` 的职责可以拆成两层：

| 层 | 内容 | Live2D 是否需要 |
|----|------|----------------|
| **窗口层** | 无边框、透明、鼠标穿透、alpha 交互区（`ApplyInteractiveRegionFromImage`）、拖拽（`DragHelper`）、按角色位置持久化（`SaveTachieLoc`/`RestoreTachieLoc`）、内心气泡、右键菜单 → `requestToggleVisible`、文件拖放 | **完全复用** |
| **渲染层** | 加载 QPixmap（PNG）、缩放缓存、`AnimePluginManager` 动画（位移/透明度/缩放步骤） | 换成 Live2D 渲染，机制不同 |

窗口层关注点与"渲染什么"是**可分离**的，这正是本次优化的切入点。

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
    virtual void setParameter(const QString &name, double value) = 0; // 桥接入口
    virtual void triggerAction(const QString &action) = 0;  // mood/点击等
  signals:
    void frameReady(const QImage &frame);                   // 渲染线程→主线程
};
```

`Dialog` 的信号契约不变（`requestSetCharTachie(mood)` / `ShowInnerThought` / `HideInnerThought`），不感知底层渲染器。

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

## 资源布局

```
Character/Assets/<角色>/Live2D/<model>/
├── <model>.model3.json      # 入口（引用 moc3/纹理/动作/物理）
├── <model>.moc3
├── textures/  motions/  physics/
└── motions.json             # 新增：mood/action → .motion3.json 映射
```

- 表情映射（mood → 表情参数名）放角色 `config.json` 的 live2d 段，与现有角色配置风格一致。
- 动作映射机制**复用 AnimePlugin 的触发词思路**（action 名 → 动画文件），但 Live2D 播的是 .motion3.json，不走 PNG 的 move/opacity/scale 步骤。

## 参数桥接层（优化点 3：输入源明确 + 口型方案修正）

| 输入 | 参数 | 说明 |
|------|------|------|
| AI 心情（`requestSetCharTachie(mood)`） | 表情参数 | 经映射表转表情/动作 |
| VITS 播放（`QMediaPlayer::playbackState`） | 嘴型二元 | v1：播放中=张嘴循环，停止=闭嘴 |
| 待机 | 呼吸 + 自动眨眼 | SDK 内建参数（breath/blink） |
| 点击（model3.json hit areas） | 交互动作 | hit-test 命中 → 动作/表情 |
| （v2）音量口型 | 嘴型连续 | 见下 |

**口型方案修正**：原方案"QAudioOutput + PCM 分析"不可行——Qt6 的 `QAudioOutput` 是 sink（只放音，不可读）。要拿音量得走 `QAudioDecoder` 把 MP3 解成 PCM → RMS → 嘴参数，或改 VITS 管线直接送 PCM。作为 v2 优化项，桥接接口（`setParameter("mouth", value)`）预先留好，v1 二元、v2 连续不破坏接口。

## SDK 集成

- **Live2D Cubism SDK for Native**（4.x），OpenGL ES 2 渲染器（`CSM_Renderer_OpenGLES2`）+ LAppModel 模式。
- 加载链：model3.json → .moc3 → textures → physics → motions。
- CMake：SDK 以静态库集成（`target_link_libraries` + include 目录），放进 `3rdparty/Live2DCubismSDK/`。
- **许可/合规（新增风险项）**：
  - 下载需同意 [Live2D Proprietary Software License Agreement](https://www.live2d.com/download/cubism-sdk/)。
  - 分发前核对 EULA：SDK 二进制能否随应用重分发、是否需要展示 Live2D 版权标识。
  - 模型文件版权归作者所有，导入 UI 提示用户自行确认授权。

## 与现有功能关系

| 功能 | 关系 |
|------|------|
| Dialog | 信号不变，不感知底层渲染器 |
| AnimePlugin | PNG 动画步骤对 Live2D 不适用，但**触发映射机制**复用 |
| VITS | 完全复用，`playbackState` 驱动嘴型 |
| 位置/拖拽/穿透/气泡 | 由 `CharacterWindowBase` 全量复用 |

## 分阶段实施（优化：补充验收标准）

| 阶段 | 内容 | 验收 | 预估 |
|------|------|------|------|
| 1 | 抽 `CharacterWindowBase`，Tachie 行为收敛到基类 | PNG 路径行为与现在一致；拖拽/穿透/位置/气泡回归通过 | 1 天 |
| 2 | CMake 集成 Cubism SDK + 离屏渲染管线（渲染线程） | 静态模型渲染到半透明窗口，alpha 正确，穿透/交互区正确 | 2-3 天 |
| 3 | 参数控制（眨眼/呼吸/表情映射） | 待机有呼吸眨眼；mood → 表情正确 | 1-2 天 |
| 4 | 动作播放 + 物理 | motions.json 触发正确；物理开启无崩 | 1 天 |
| 5 | AI/VITS 桥接 | 心情换表情；VITS 播放中张嘴、停止闭嘴 | 1 天 |
| 6 | 点击交互（hit areas） | 点击模型区域触发对应动作 | 1-2 天 |
| 7 | 模型导入 UI + 渲染器切换 | 按角色导入/切换模型；config 持久化；重启恢复 | 1 天 |
| 8 | 全量回归 + 低端机性能验证 | 双渲染路径可用；帧率达标（可降帧档位） | 0.5-1 天 |

总计约 **9-12 天**。

## 风险与对策（更新）

| 风险 | 对策 |
|------|------|
| SDK 许可 / 分发合规 | 实施前核对 EULA；安装包附版权说明 |
| 透明窗口 + GL 合成异常 | **离屏渲染规避**（不嵌 QOpenGLWidget） |
| 渲染线程 GL 上下文管理 | 独立 QOpenGLContext + 帧队列投递主线程；参考 Open-LLM-VTuber |
| 主线程卡顿（若走保守档） | 渲染线程为主方案，保守档仅降级 |
| 编译产物增大 | SDK 库 +5MB 左右；模型文件按角色按需放置，不入安装包 |
| 与 AnimePlugin 的语义冲突 | PNG 动画仅 PNG 渲染器使用，映射机制共用但数据源分离 |
| 低端机 60fps 压力 | 可降帧（30fps）或缩小渲染分辨率 |

## 参考

- [Live2D Cubism SDK for Native](https://www.live2d.com/download/cubism-sdk/)
- [Open-LLM-VTuber](https://github.com/Open-LLM-VTuber/Open-LLM-VTuber) — 渲染线程 + 参数桥接参考实现
- Qt 透明窗口 + OpenGL 合成问题（`WA_TranslucentBackground` + GL 子控件）
