[README.md](https://github.com/user-attachments/files/28984655/README.md)
<!--
  注意：这是 ZcChat2 的个人分支版本，由 Mandarin715 维护。
  原项目地址：https://github.com/Zao-chen/ZcChat2
-->

# Mandarin — AI 桌面宠物

🌟 一个模仿 Galgame 演出效果的 AI 桌宠

<img width="1045" height="593" alt="screenshot" src="https://github.com/user-attachments/assets/49439b92-308f-4ecd-b8cc-a1538153752c" />

[![GitHub Release](https://img.shields.io/github/v/release/Mandarin715/ZcChat2?color=22c55e&style=for-the-badge)](https://github.com/Mandarin715/ZcChat2/releases)
[![GitHub License](https://img.shields.io/github/license/Mandarin715/ZcChat2?color=ef4444&style=for-the-badge)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20macOS%20%7C%20Linux-6366f1?style=for-the-badge)]()


---

## 🎯 项目介绍

让你的桌面上住进一位 Galgame 风格的角色。她可以和你聊天、展示丰富的表情动作、用语音回复你，还能记住关于你的信息。

### ✨ 核心特性

| 特性 | 说明 |
|------|------|
| 😊 **立绘演出** | Galgame 风格立绘，支持多表情、多动作组合动画 |
| 🎬 **动态效果** | 立绘动画（颤抖、靠近）和粒子特效（气泡等） |
| 🎙 **语音交互** | 语音输入唤醒，支持直接对话和打断 |
| 🔊 **语音合成** | 接入多种 TTS 引擎（VITS 等），还原角色声音 |
| 💻 **操作系统** | 通过系统级 API 让桌宠操作你的电脑 |
| 🧠 **长期记忆** | AI 自动提取并持久化用户信息与对话上下文，越聊越懂你 |
| 🔌 **插件扩展** | 动画、粒子素材以插件方式加载，支持二次开发 |

### 🎗️ 相比 ZcChat 的改进

- **更轻量** — 后台内存占用从 40MB 降至 8MB
- **更流畅** — LLM 和 TTS 均采用流式传输，响应更快
- **更易用** — 一键导入角色，配置流程大幅简化
- **更灵活** — 动画、粒子等素材以插件化方式管理
- **更深入** — 专注桌面端，支持系统级操作
- **更智能** — 长期记忆系统，角色能记住用户偏好和历史对话

---

## 🗺 开发进度

- [x] 基础功能移植
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
- [ ] 本地应用调用
---

## 🚀 快速入门

### Step 1：下载 & 启动

1. 在 [Release](https://github.com/Mandarin715/ZcChat2/releases) 下载最新 `ZcChat2-v*-portable.zip`
2. 解压到任意目录
3. 双击 `启动.bat`，桌宠直接启动

### Step 2：（可选）语音功能

> 仅文字对话可跳过此步。

1. 下载 [vits-simple-api](https://github.com/Artrajz/vits-simple-api/releases)（Windows CPU 版本）
2. 解压到桌宠目录下的 `vits-simple-api/` 文件夹
3. 在 [Release](https://github.com/Mandarin715/ZcChat2/releases) 下载 `SomeGalActor_Vits.zip`（语音模型）
4. 解压到 `vits-simple-api/models/SomeGalActor_Vits/`
5. 重新双击 `启动.bat`，脚本自动检测并开启语音模式

### Step 3：配置 API Key

1. 右键系统托盘图标 → 设置
2. 「对话模型」→ 选择服务商 → 填入 API Key → 点击「获取」
3. （语音输入需）「语音输入设置」→ 填入百度语音识别 API Key

### Step 4：导入角色

1. 在 [Release](https://github.com/Mandarin715/ZcChat2/releases) 下载角色包（如 Atri.zip）
2. 设置 → 角色设置 → 导入

> 更多角色可在 [Discussions](https://github.com/Mandarin715/ZcChat2/discussions) 分享和获取

---

## 🤗 参与贡献

欢迎任何形式的贡献：

- **提交 PR** — 改进代码、修复 Bug、新增功能
- **报告问题** — 通过 [Issues](https://github.com/Mandarin715/ZcChat2/issues) 提交 Bug 或功能建议
- **分享角色** — 在 [Discussions](https://github.com/Zao-chen/ZcChat2/discussions/categories/%E8%A7%92%E8%89%B2%E5%88%86%E4%BA%AB) 分享自创角色
- **Star ⭐** — 觉得有用就点个 Star 吧

---

## 🔗 相关链接

| 项目 | 说明 |
|------|------|
| [Zao-chen/ZcAILib](https://github.com/Zao-chen/ZcAILib) | 高性能 Qt AI 请求库 |
| [Zao-chen/ZcWidgetTools](https://github.com/Zao-chen/ZcWidgetTools) | ElaWidgetTools 扩展 |
| [Zao-chen/ZcJsonLib](https://github.com/Zao-chen/ZcJsonLib) | 轻量 JSON 键值库 |
| [Liniyous/ElaWidgetTools](https://github.com/Liniyous/ElaWidgetTools) | Fluent UI for Qt |
| [Artrajz/vits-simple-api](https://github.com/Artrajz/vits-simple-api) | VITS 语音合成 |
| [Qt](https://www.qt.io/) | 跨平台 C++ 框架 |

---

## 📄 许可证

本项目基于 [GPL v3](LICENSE) 开源协议发布。
