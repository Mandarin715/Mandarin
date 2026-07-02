# 更新日志

本项目遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.0.0/) 格式。

## [1.7.0] - 2026-07-02

### 新增
- **联网搜索功能**：AI 自主判断是否需要联网搜索，支持显式和隐式触发
  - `SearchProvider` 类：封装搜索 API，支持百度千帆/SearXNG/Bing/SerpAPI 多后端自动识别
  - 两段式 AI 搜索决策：分类器快速判断搜索意图 → 搜索 → 结果注入对话上下文
  - 屏幕捕获后自动搜索：视觉分析结果触发联想搜索
  - 手动搜索关键词触发："帮我搜"、"搜一下"等
  - IP 定位（ip.sb）：自动获取省/市/区，本地化查询（天气/新闻等）

### 优化
- VITS 语音合成从串行改为 2 路并发，首批语音就绪时间约减半

## [1.6.0] - 2026-06-16

### 新增
- **对话记忆功能**：AI 自动从对话中提取用户个人信息和求助记录，持久化存储并在后续对话中注入上下文
  - `extractAndStoreMemory()`：异步调用 AI 提取对话中的记忆信息（个人信息、求助摘要），不阻塞主界面
  - `buildMemoryContext()`：将记忆数据构建为上下文文本，注入系统提示词
  - `loadMemory()` / `saveMemory()`：记忆文件的读写，存储路径为 `Documents/Mandarin/Character/UserConfig/<角色名>/memory.json`
- 记忆数据包含两类：`personal_info`（用户个人信息键值对）和 `help_summaries`（历史求助摘要，保留最近20条）

### 修复
- 修复 `memory.json` 首次使用时不自动创建的问题：`loadMemory()` 在文件不存在时主动创建空文件
- 修复 `saveMemory()` 目录创建失败时静默返回的问题：增加错误日志
- 修复记忆提取 AI 调用失败时错误信息被丢弃的问题：通过 `qWarning()` 输出错误详情

### 变更
- `GlobalConstants.h`：新增 `ReadCharacterMemoryPath()` 函数
- `dialog.h`：新增 `m_memoryData`、`loadMemory()`、`saveMemory()`、`buildMemoryContext()`、`extractAndStoreMemory()` 声明
- `dialog.cpp`：集成记忆功能到对话流程，含 `#include <QDebug>` 用于错误诊断

---

## 版本说明

格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.0.0/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。
