# Live2D 集成方案（待实施）

> 2026-07-04 设计，暂不实施

## 目标

新增 `Live2DWindow` 窗口，支持导入 Live2D 模型（.moc3），替代或并行现有的 PNG 立绘（Tachie），实现更丰富的动态效果。

## 技术方案

```
Live2DWindow（QOpenGLWidget）
├── Cubism SDK for Native (C++)
│   ├── .moc3 模型加载
│   ├── 物理引擎（头发/衣服摇摆）
│   ├── 动作文件播放 (.motion3.json)
│   └── 参数控制（表情/嘴型/呼吸/眨眼）
├── 参数桥接层
│   ├── AI 心情 → 表情参数
│   ├── TTS 播放中 → 嘴型参数（二元切换，播放状态驱动）
│   ├── 待机 → 呼吸动画 + 随机微动作
│   └── 点击 → 交互反馈
└── 模型管理（导入/切换）
```

## 和现有 Tachie 的关系

- 两个窗口**互斥**，用户在设置中选择用 PNG 还是 Live2D
- Tachie 代码零改动
- Dialog 通过信号控制，不感知底层是 PNG 还是 Live2D

## 语音合成复用

现有 VITS 方案直接复用，不修改：

```
VITS HTTP → MP3 → QMediaPlayer::play()  （不改）
                         ↓
         playbackState 信号 → 是否播放中
                         ↓
         Live2D 嘴参数：播放中=张嘴循环，停止=闭嘴
```

音量驱动的精细口型同步（QAudioOutput + PCM 分析）作为后续优化。

## 分阶段实施

| 阶段 | 内容 | 预估工时 |
|------|------|----------|
| 1 | CMake 集成 Cubism SDK + QOpenGLWidget 渲染 | 2-3 天 |
| 2 | 参数控制（眨眼、呼吸、表情） | 1-2 天 |
| 3 | 动作播放 + 物理 | 1 天 |
| 4 | AI 状态桥接 | 1 天 |
| 5 | 点击交互 | 2 天 |
| 6 | 模型导入 UI | 1 天 |
| 7 | 和 Tachie 共存切换 | 0.5 天 |

## 关键风险

- Cubism SDK 需要同意许可协议才能下载
- OpenGL (QOpenGLWidget) 可能与现有透明穿透窗口冲突
- 鼠标穿透 vs 可交互：需要"大部分区域穿透、模型上可点"
- 编译产物增大 ~5MB

## 参考

- [Live2D Cubism SDK for Native](https://www.live2d.com/download/cubism-sdk/)
- [Open-LLM-VTuber](https://github.com/Open-LLM-VTuber/Open-LLM-VTuber) — Live2D 集成参考实现
