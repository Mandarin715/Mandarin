# Mandarin — AI 桌面宠物

> Qt 6.6.3 + C++17 跨平台 AI 桌宠应用，Galgame 风格立绘 + AI 对话 + 语音交互

## 项目架构

```
三窗口架构 (main.cpp)：
├── Tachie（角色立绘）—— 无边框、鼠标穿透、动画播放
├── Dialog（聊天气泡）—— 核心业务枢纽
└── MainWindow（设置面板）—— ElaWindow 导航，懒加载

信号绑定：
  Tachie::requestToggleVisible → Dialog::ToggleVisible
  Dialog::requestSetCharTachie → Tachie::SetTachieImg
  配置变更 → MainWindow 路由 → Dialog::Reload*Config()
```

## 源码目录

```
Mandarin/
├── main.cpp                       # 入口，窗口创建，托盘，配置迁移
├── CMakeLists.txt                 # Qt6 Widgets/Network/Multimedia/Svg
├── GlobalConstants.h              # 路径常量 (Documents/Mandarin/...)
├── windows/
│   ├── tachie/                    # 立绘窗口（鼠标穿透，动画）
│   ├── dialog/                    # 聊天窗口（核心逻辑 ~200行头）
│   │   └── history/               # 对话历史界面
│   └── setting/                   # 设置（10个子页面）
│       └── child/                 # llm/char/vits/speech/screenCapture/
│                                  #   search/appLauncher/plugin/general/about
└── utils/
    ├── SearchProvider.h/cpp       # 联网搜索（百度千帆/SearXNG/Bing/SerpAPI）
    ├── WakeWordDetector.h/cpp     # sherpa-onnx 离线唤醒词
    ├── AnimePlugin.h/cpp          # 动画数据结构+JSON解析
    ├── AnimePluginManager.h/cpp   # 插件发现+索引
    ├── DragHelper.h/cpp           # 无边框窗口拖拽
    └── CustomScrollBinder.h/cpp   # 自定义滚动条绑定
```

## 关键技术细节

### Dialog 消息提交优先级链
1. 屏幕捕获关键词 → 2. 搜索关键词 → 3. 应用启动匹配 → 4. AI 意图分类 → 5. 普通聊天

### AI 回复格式约定
系统提示词要求 AI 以 `心情|中文回复|日语回复` 固定格式回复，`|` 分隔符解析。

### 线程模型
**全部主线程**，无 QThread。网络用 QNetworkAccessManager 异步信号，音频用 QIODevice::readyRead 事件驱动，ONNX 推理主线程同步（INT8 量化）。

### 配置双存储
| 文件 | 格式 | 用途 |
|------|------|------|
| `Documents/Mandarin/config.json` | JSON | 可迁移配置（API Key等） |
| `Documents/Mandarin/config.ini` | INI | 本地配置（窗口位置等） |

### 数据文件布局
```
Documents/Mandarin/
├── config.json / config.ini
├── Character/Assets/<角色>/       # 角色资源（立绘+配置）
├── Character/UserConfig/<角色>/   # context.json + memory.json
└── Plugin/Anime/*.json            # 动画插件
```

## 第三方依赖

| 库 | 位置 | 说明 |
|----|------|------|
| ZcAiLib | 3rdparty/（优先外部 P:/Qt/Project/ZcAILib） | AI SDK，自研 |
| ZcJsonLib | 3rdparty/ | JSON 封装，自研 |
| ZcWidgetTools | 3rdparty/ | 自定义控件，自研 |
| ElaWidgetTools | 3rdparty/ | Fluent Design UI |
| sherpa-onnx | 3rdparty/ | 离线 KWS 唤醒词 |

## 构建

- **Windows**: CMake + MSVC 2022，构建目录 `build/`（Release）和 `build2/`（Debug）
- **跨平台**: 同时支持 macOS（Bundle）和 Linux（X11）
- **CI**: GitHub Actions，Inno Setup 打包
- **当前版本**: v1.7.1

## 代码风格

- C++17，Qt 信号/槽机制，`Q_OBJECT` 宏
- 配置写入：各设置页面在值变更时立即写文件，然后 emit 信号通知 Dialog 重载
- 命名：类名 PascalCase，成员变量 `m_` 前缀，私有成员 `_` 前缀（部分）
- JSON 操作使用 ZcJsonLib（`load()`/`save()`/`value()`）
