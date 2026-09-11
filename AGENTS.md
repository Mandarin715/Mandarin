# Mandarin — AI 桌面宠物

> Qt 6.6.3 + C++20 跨平台 AI 桌宠应用，Galgame 风格立绘 + AI 对话 + 语音交互

## 项目架构

```
三窗口架构 (main.cpp)：
├── Tachie（角色立绘）—— 无边框、鼠标穿透、动画播放
├── Dialog（聊天气泡）—— 核心业务枢纽
└── MainWindow（设置面板）—— ElaWindow 导航，懒加载

信号绑定（main.cpp + setting.cpp）：
  Tachie::requestToggleVisible → Dialog::ToggleVisible
  Tachie::requestFileDrop       → Dialog::handleFileDrop
  Dialog::requestSetCharTachie  → Tachie::SetTachieImg
  Dialog::requestShowInnerThought / requestHideInnerThought → Tachie 内心气泡
  配置变更 → MainWindow 路由 → Dialog::Reload*Config() / Tachie 槽
```

## 源码目录

```
Mandarin/
├── main.cpp                       # 入口，窗口创建，托盘，配置迁移
├── CMakeLists.txt                 # Qt6 Widgets/Network/Multimedia/Svg
├── GlobalConstants.h              # 路径常量 (Documents/Mandarin/...)
├── docs/                          # 功能设计方案（待实施）
├── windows/
│   ├── tachie/                    # 立绘窗口（鼠标穿透，动画）
│   ├── dialog/                    # 聊天窗口（核心业务枢纽，dialog.h ~320行 / dialog.cpp ~4800行）
│   │   └── history/               # 对话历史界面
│   └── setting/                   # 设置（11个子页面，ElaWindow 导航）
│       └── child/                 # general/llm/screenCapture/search/speech/
│                                  #   appLauncher/vits/plugin/char/memory/about
└── utils/
    ├── SearchProvider.h/cpp           # 联网搜索（百度千帆 + 通用 GET / SearXNG）
    ├── WakeWordDetector.h/cpp         # sherpa-onnx 离线唤醒词（KWS）
    ├── OfflineSpeechRecognizer.h/cpp  # sherpa-onnx SenseVoice 离线语音识别
    ├── ChatLogStore.h/cpp             # 聊天记录持久化（chat.jsonl 即写即存）
    ├── FaceDetector.h/cpp             # WinRT 人脸检测（摄像头视觉感知，仅 Windows）
    ├── AnimePlugin.h/cpp              # 动画数据结构+JSON解析
    ├── AnimePluginManager.h/cpp       # 插件发现+索引
    ├── DragHelper.h/cpp               # 无边框窗口拖拽
    └── CustomScrollBinder.h/cpp       # 自定义滚动条绑定
```

## 关键技术细节

### Dialog 消息提交优先级链
0. 日程提醒意图拦截 → 1. 屏幕捕获关键词 → 2. 搜索关键词 / 否定重搜 → 3. 应用启动匹配 → 4. AI 意图分类（除纯寒暄外都判；缺主题时先反问确认） → 5. 普通聊天

### AI 回复格式约定
系统提示词要求 AI 以 `心情|中文回复|日语回复|内心独白` 固定格式回复，`|` 分隔符解析；
心情段映射立绘表情（requestSetCharTachie），内心独白段显示在立绘气泡。

### 线程模型
主要以**主线程**为主，无 QThread。网络用 QNetworkAccessManager 异步信号，音频用 QIODevice::readyRead 事件驱动，ONNX 推理同步（INT8 量化）。
例外：**语音识别解码**与**截屏 JPEG 编码**使用 `std::thread` 工作线程执行，完成后经 `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` 回主线程（避免解码/编码期间冻结 UI；sherpa-onnx recognizer 非线程安全，须保证同时只有一次解码）。

### 配置双存储
| 文件 | 格式 | 用途 |
|------|------|------|
| `Documents/Mandarin/config.json` | JSON | 可迁移配置（API Key等） |
| `Documents/Mandarin/config.ini` | INI | 本地配置（窗口位置等） |

### 数据文件布局
```
Documents/Mandarin/
├── config.json / config.ini
├── schedules.json                 # 日程提醒
├── Character/Assets/<角色>/       # 角色资源（立绘+配置）
├── Character/UserConfig/<角色>/   # config.json + chat.jsonl（聊天记录）+ memory.json
└── Plugin/Anime/*.json            # 动画插件
```

## 第三方依赖

| 库 | 位置 | 说明 |
|----|------|------|
| ZcAiLib | 3rdparty/（优先外部 P:/Qt/Project/ZcAILib） | AI SDK，自研 |
| ZcJsonLib | 3rdparty/ | JSON 封装，自研 |
| ZcWidgetTools | 3rdparty/ | 自定义控件，自研 |
| ElaWidgetTools | 3rdparty/ | Fluent Design UI |
| sherpa-onnx | 3rdparty/ | 离线唤醒词（KWS）+ SenseVoice 离线识别 |

> VITS 语音合成依赖本地 vits-simple-api HTTP 服务（仓库根 `vits-simple-api-windows-cpu-*`），Dialog 通过 QNetworkAccessManager 拉取 MP3 播放。

## 构建

- **Windows**: CMake + MSVC 2022，构建目录 `build/`（旧，Release）和 `build2/`（当前，Debug + Release）
- **启动桌宠**: 运行 `build2/Release/启动.bat`（自动 cd 到正确目录 → 查找 vits-simple-api 并后台拉起语音服务 → 启动 `Mandarin.exe`；找不到语音服务则纯文本模式）。**不要直接运行 exe、不要从其他目录启动**
- **⚠️ AI 不要自动启动桌宠**：构建/验证完即可，**启动由用户自己来**。AI 只在用户明确要求时才运行 `启动.bat`；不要为了"验证"或"方便"擅自拉起。（需要验证编译结果时用构建产物存在性/报错来判断，不必启动 GUI）
- **跨平台**: 同时支持 macOS（Bundle）和 Linux（X11）
- **CI**: GitHub Actions，Inno Setup 打包
- **Windows 构建注意**: 摄像头视觉感知用 C++/WinRT（Windows SDK 10.0.26100.0），需 `#include <coroutine>` + `/std:c++20` + 显式 `#include <winrt/Windows.Foundation.Collections.h>`，并链接 `windowsapp.lib ole32.lib oleaut32.lib`（见 `CMakeLists.txt` 的 `MANDARIN_WINRT_INC`）。链接报 `LNK1104` 说明 `Mandarin.exe` 还开着，先结束进程
- **测试**: `ctest --test-dir build2 -C Release`（测试产物在 `build2/tests/Release/`）
- **当前版本**: 见 `Mandarin/CMakeLists.txt` 的 `project(Mandarin VERSION x.y.z)`（唯一真源，重新 configure 后生成 `build2/Version.h`）；`build2/Release/Mandarin-v<版本>-portable.zip` 为对应打包产物
- **打包**: portable zip = `build2/Release/**` 全量（排除 `*.zip`），打包前确认 `models/` 里没有多余的大文件（曾误带 fp32 `model.onnx` 导致体积膨胀到 1 GB）

## 代码风格

- C++20（`CMakeLists.txt` 里 `CMAKE_CXX_STANDARD 20`，为 C++/WinRT 协程所需），Qt 信号/槽机制，`Q_OBJECT` 宏
- 配置写入：各设置页面在值变更时立即写文件，然后 emit 信号通知 Dialog 重载
- 命名：类名 PascalCase，成员变量 `m_` 前缀，私有成员 `_` 前缀（部分）
- JSON 操作使用 ZcJsonLib（`load()`/`save()`/`value()`）
