# Mandarin — AI 桌面宠物

 一个模仿 Galgame 演出效果的 AI 桌宠

<img width="678" height="591" alt="屏幕截图 2026-06-25 170727" src="https://github.com/user-attachments/assets/bb3da77c-8959-4417-ad09-e27dde804561" />

[![GitHub Release](https://img.shields.io/github/v/release/Mandarin715/Mandarin?color=22c55e&style=for-the-badge)](https://github.com/Mandarin715/Mandarin/releases)


---

##  项目介绍

让你的桌面上住进一位 Galgame 风格的角色。她可以和你聊天、展示丰富的表情动作、用语音回复你，还能记住关于你的信息。

###  系统架构

```
┌──────────────────────────────────────────────────┐
│                    Mandarin                       │
│  ┌─────────┐  ┌──────────┐  ┌─────────────────┐ │
│  │ 角色立绘 │  │ 聊天气泡  │  │    设置面板      │ │
│  │(Tachie)  │  │ (Dialog)  │  │  (MainWindow)   │ │
│  └────┬─────┘  └────┬─────┘  └───────┬─────────┘ │
│       │              │               │            │
│  ┌────┴──────────────┴───────────────┴──────────┐ │
│  │              C++ 业务核心                      │ │
│  │  AI对话 │ VITS语音 │ SenseVoice │ 唤醒词检测   │ │
│  └────────────────────┬──────────────────────────┘ │
│                       │                            │
│  ┌────────────────────┼──────────────────────────┐ │
│  │ Qt 6.6.3 · ElaWidgetTools · sherpa-onnx       │ │
│  │ Windows 11 / Linux / macOS                    │ │
│  └──────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────┘
```

###  使用场景

| 场景 | 说明 |
|------|------|
|  **日常聊天** | 随时和她对话，她会记住你的名字、喜好和习惯 |
|  **语音交流** | 说出"萝卜子"唤醒她，用语音聊天，她会用日语回复 |
|  **办公辅助** | 说"打开某某某"直接启动应用，说"看看屏幕"让她识别屏幕内容 |
|  **角色收集** | 导入不同的角色包，每个角色有独立的立绘、声音和性格 |
|  **持续陪伴** | 连续对话模式下自动轮流发言，像和真人聊天一样自然 |

###  技术栈

- **前端 UI**：Qt 6.6.3 + ElaWidgetTools（Windows 11 Fluent Design 风格）
- **AI 对话**：支持 DeepSeek / OpenAI / 自定义兼容 API，流式 SSE 传输
- **语音合成**：VITS 日语 TTS，HTTP 流式分词合成
- **语音识别**：SenseVoice 离线语音识别（sherpa-onnx），无需联网、无需 API Key
- **唤醒词**：sherpa-onnx 离线关键词识别（KWS），无需联网
- **构建工具**：CMake + MSVC 2022，GitHub Actions 自动发布

###  核心特性

| 特性 | 说明 |
|------|------|
|  **立绘演出** | Galgame 风格立绘，支持多表情、多动作组合动画 |
|  **动态效果** | 立绘动画（颤抖、靠近）和粒子特效（气泡等） |
|  **语音交互** | 语音输入唤醒，支持直接对话和打断 |
|  **语音合成** | 接入多种 TTS 引擎（VITS 等），还原角色声音 |
|  **操作系统** | 通过系统级 API 让桌宠操作你的电脑 |
|  **长期记忆** | AI 自动提取并持久化用户信息与对话上下文，越聊越懂你 |
|  **插件扩展** | 动画、粒子素材以插件方式加载，支持二次开发 |

---

##  开发进度

- [x] 立绘系统
- [x] 语音合成接入
- [x] 上下文与历史记录
- [x] 一键导入角色
- [x] LLM 流式传输
- [x] 语音切分流式合成
- [x] 立绘动画与插件
- [x] 语音输入
- [x] 对话长期记忆
- [x] 语音唤醒
- [x] 连续对话
- [x] 屏幕识别
- [x] 本地应用调用
- [x] 联网搜索
- [ ] 主动对话
- [ ] 视觉感知
- [x] 内心独白
- [ ] 聊天记录持久化（可切换至以前的对话进程）
- [ ] live2D模型支持
---

##  部署方式

### Step 1：下载 & 启动

1. 在 [Release](https://github.com/Mandarin715/Mandarin/releases) 下载最新 `Mandarin-v*-portable.zip`
2. 解压到任意目录
3. 双击 `启动.bat`，桌宠直接启动

### Step 2：（可选）语音功能

> 仅文字对话可跳过此步。

**语音识别（SenseVoice 离线引擎）**

便携包已内置语音识别模型（`models/sense-voice/`），无需额外操作即可使用语音输入。

如未下载便携包或需手动安装模型：
1. 下载 SenseVoice 模型：[hf-mirror.com](https://hf-mirror.com/csukuangfj/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17)（点击文件与版本）（选 `model.int8.onnx` + `tokens.txt`）
2. 放入 Mandarin 目录下的 `models/sense-voice/` 文件夹
3. 重启 Mandarin，设置 → 语音输入设置 → 确认引擎状态为「已就绪」

**语音合成（VITS TTS）**

1. 下载 [vits-simple-api](https://github.com/Artrajz/vits-simple-api/releases)（Windows CPU 版本）
2. 解压到 Mandarin 安装目录（保留原文件夹名，如 `vits-simple-api-windows-cpu-v0.6.16`）
3. 在 [Release](https://github.com/Mandarin715/Mandarin/releases) （1.6.1版本及以前）下载 `SomeGalActor_Vits.zip`（语音模型）
4. 解压到刚才的 vits-simple-api 文件夹下的 `models/SomeGalActor_Vits/`
5. 双击 `启动.bat`，脚本自动检测并开启语音模式

### Step 3：配置 API Key

1. 右键系统托盘图标 → 设置
2. 「对话模型」→ 选择服务商 → 填入 API Key → 点击「获取」
3. 语音识别已内置 SenseVoice 离线引擎，无需额外配置

### Step 4：联网搜索（可选）

> 仅文字对话可跳过此步。开启后桌宠会自动判断是否需要联网查询。

1. 申请搜索 API（任选一个）：
   - [百度千帆 AI 搜索](https://console.bce.baidu.com/qianfan/ais/console/applicationConsole/application) — 免费额度，国内首选
   - [SearXNG](https://docs.searxng.org/) — 自部署，无限制
   - [Bing Web Search](https://portal.azure.com/) — Azure 订阅
   - [SerpAPI](https://serpapi.com/) — 付费，稳定
2. 设置 → 「联网搜索」→ 填入 API Key / Base URL
3. 对话中直接说"帮我搜一下xxx"即可触发

### Step 5：屏幕识别（可选）

> 桌宠可以"看到"你的屏幕内容并和你讨论。

1. 申请 [Kimi 视觉模型 API Key](https://platform.moonshot.cn/)（默认，推荐）
2. 或使用 OpenAI 兼容的视觉 API（如 GPT-4V）
3. 设置 → 「屏幕捕获」→ 填入 API Key
4. 对话中说"看看屏幕"或点击截图按钮触发

### Step 6：应用调用（可选）

> 让桌宠帮你打开本地应用。

1. 设置 → 「应用调用」
2. 添加关键词（如"微信"）和程序路径（如 `C:\Program Files\Tencent\WeChat\WeChat.exe`）
3. 对话中说"打开微信"即可启动

### Step 7：导入角色

1. 在 [Release](https://github.com/Mandarin715/Mandarin/releases) （1.6.1版本及以前）下载角色包（如 Atri.zip）
2. 设置 → 角色设置 → 导入

> 更多角色可在 [Discussions](https://github.com/Mandarin715/Mandarin/discussions) 分享和获取

---

##  参与贡献

欢迎任何形式的贡献：

- **提交 PR** — 改进代码、修复 Bug、新增功能
- **报告问题** — 通过 [Issues](https://github.com/Mandarin715/Mandarin/issues) 提交 Bug 或功能建议
- **分享角色** — 在 [Discussions](https://github.com/Mandarin715/Mandarin/discussions)分享自创角色
- **Star ⭐** — 觉得有用就点个 Star 吧

---

##  相关链接

| 项目 | 说明 |
|------|------|
| [Liniyous/ElaWidgetTools](https://github.com/Liniyous/ElaWidgetTools) | Fluent UI for Qt |
| [Artrajz/vits-simple-api](https://github.com/Artrajz/vits-simple-api) | VITS 语音合成 |
| [Qt](https://www.qt.io/) | 跨平台 C++ 框架 |
| [DeepSeek API](https://platform.deepseek.com/api_keys) | AI 对话（默认） |
| [OpenAI API](https://platform.openai.com/api-keys) | AI 对话（可选） |
| [百度千帆 AI 搜索](https://console.bce.baidu.com/qianfan/ais/console/applicationConsole/application) | 联网搜索 |
| [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) | 语音识别 + 语音唤醒 |
| [Kimi 视觉模型](https://platform.moonshot.cn/) | 屏幕识别 |

---
