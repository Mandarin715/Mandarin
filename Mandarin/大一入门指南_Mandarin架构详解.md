# Mandrin 架构详解 — 写给只学过 C 语言的大一同学

> 这份文档假设你学完了 C 语言和数据结构，还没接触过图形界面编程。
> 我们用你熟悉的 C 语言概念做类比，一步步解释这个 AI 桌宠是怎么做出来的。

---

## 第一章：从 C 语言到图形界面

### 1.1 你熟悉的 C 程序长这样

```c
#include <stdio.h>

int main() {
    printf("Hello World\n");
    return 0;
}
```

特点：从头到尾顺序执行，`main()` 返回就结束了。

### 1.2 图形界面程序长什么样

图形界面程序不是"执行完就结束"，而是**一直运行，等待用户操作**：

```
启动 → 显示窗口 → 等用户点击/输入 → 响应 → 等下一步操作 → ... → 用户点关闭 → 退出
```

这个"等待→响应→等待→响应"的循环叫做**事件循环**（event loop）。

```c
// 伪代码：图形界面程序的骨架
int main() {
    创建窗口();
    显示窗口();
    
    while (窗口还开着) {
        if (用户点击了按钮)  处理点击();
        if (用户按了键盘)    处理按键();
        if (网络数据到了)    处理数据();
    }
    
    return 0;
}
```

Mandarin 的 `main.cpp` 最后一行 `return a.exec();` 就是进入这个循环。

### 1.3 什么是 Qt

Qt 是一个 C++ 库，帮你处理了上面那个 "while 循环" 里的所有脏活。你不用自己写 `if (用户点击了按钮)`，Qt 帮你做好了。

用 Qt 写程序就像搭积木：
- **QWidget** = 一块矩形区域（窗口、按钮、文本框都是 Widget）
- **QLabel** = 显示文字的 Widget
- **QPushButton** = 可点击的 Widget
- **QTextEdit** = 可以输入文字的 Widget

---

## 第二章：信号与槽 — Qt 的核心机制

### 2.1 问题：两个模块之间怎么通信？

假设场景：用户点击了"发送"按钮，聊天消息要发给 AI。

在 C 语言里你可能这样写：

```c
// 按钮被点击时
void on_button_clicked() {
    char *msg = get_input_text();    // 拿到输入框的文字
    send_to_ai(msg);                 // 发给 AI
}
```

但问题来了——**按钮模块**怎么知道**AI 模块**的存在？如果你直接调用 `send_to_ai()`，两个模块就**耦合**在一起了，改一个就得改另一个。

### 2.2 信号与槽 = 广播与收音机

Qt 的解决方案：**信号（signal）**和**槽（slot）**。

把信号想象成**广播电台发射信号**，槽想象成**收音机接收信号**：

```
电台（信号发送者）          收音机（信号接收者）
  按钮被点击了  ──无线──→   AI模块：收到，开始处理
                         日志模块：收到，记录日志
                         动画模块：收到，播放点击动画
```

电台不在乎谁在收听，收音机不在乎信号从哪来的。**一个信号可以连接多个槽，一个槽可以连接多个信号**。

### 2.3 实际代码长这样

```cpp
// 信号声明（在按钮类里）
signals:
    void clicked();    // "我被点击了"信号

// 槽声明（在对话处理类里）
public slots:
    void sendMessage();  // "发送消息"槽
```

```cpp
// 连接：把按钮的 clicked 信号连到 dialog 的 sendMessage 槽
connect(按钮, &Button::clicked,   // 信号源
        dialog, &Dialog::sendMessage);  // 接收者
```

当按钮被点击时，Qt 自动调用 `dialog->sendMessage()`。按钮不需要知道 dialog 的存在。

### 2.4 Mandarin 里的信号示例

在 `main.cpp` 中：

```cpp
// 用户右键点击角色立绘 → 切换对话框显隐
connect(&tachieWin, &Tachie::requestToggleVisible,  // 立绘发出信号
        &dialogWin, &Dialog::ToggleVisible);         // 对话框响应

// AI 回复后要切换角色表情 → 更新立绘
connect(&dialogWin, &Dialog::requestSetCharTachie,   // 对话框发出信号
        &tachieWin, &Tachie::SetTachieImg);           // 立绘响应
```

用 C 语言类比的话，信号槽就像一个**回调函数注册表**：

```c
// C 语言等价写法（简化版）
typedef void (*Callback)(void);

Callback click_callbacks[10];  // 点击事件的回调数组

void register_click_callback(Callback cb) {
    // 注册一个回调
}

void button_clicked() {
    for (int i = 0; i < 10; i++) {
        if (click_callbacks[i]) 
            click_callbacks[i]();  // 调用所有注册的回调
    }
}
```

信号槽就是这个思路的升级版——类型安全、自动管理内存、支持跨线程。

---

## 第三章：Mandarin 的整体架构

### 3.1 三个窗口

整个应用由三个窗口组成，每个窗口是一个独立的功能模块：

```
┌──────────────────────────────────────────────────┐
│                                                  │
│    Tachie（角色立绘窗口）                          │
│    - 显示角色图片                                  │
│    - 鼠标穿透（点击角色=点击桌面）                   │
│    - 播放动画（颤抖、缩放等）                       │
│    - 右键切换对话框                                 │
│                                                  │
├──────────────────────────────────────────────────┤
│                                                  │
│    Dialog（聊天气泡窗口）                           │
│    - 显示聊天内容                                  │
│    - 接收用户文字/语音输入                          │
│    - 调用 AI、TTS、搜索等所有服务                   │
│    - 这是整个程序的"大脑"                           │
│                                                  │
├──────────────────────────────────────────────────┤
│                                                  │
│    MainWindow（设置面板）                          │
│    - 只在用户点击托盘图标时才创建（懒加载）           │
│    - 配置 AI 模型、语音、角色、搜索等                │
│    - 修改配置后通知 Dialog 重新加载                  │
│                                                  │
└──────────────────────────────────────────────────┘
```

### 3.2 它们之间怎么通信

```
         Tachie                  Dialog
    ┌──────────────┐        ┌──────────────┐
    │ 右键点击立绘   │──────→│ 切换显隐      │
    │              │        │              │
    │ 切换表情图片   │←──────│ AI 返回心情    │
    └──────────────┘        └──────┬───────┘
                                   │
                            MainWindow（设置）
                            ┌──────────────┐
                            │ 修改配置      │──→ Dialog 重载配置
                            │ 修改立绘大小   │──→ Tachie 更新大小
                            │ 切换角色      │──→ Tachie 重新加载
                            └──────────────┘
```

箭头方向就是信号传递方向。三者的代码在：
- `windows/tachie/` — 立绘窗口
- `windows/dialog/` — 对话窗口
- `windows/setting/` — 设置窗口

---

## 第四章：每个模块的详细实现

### 4.1 Tachie（角色立绘）— `windows/tachie/`

**功能**：在桌面上显示一个 Galgame 风格的角色图片。

**关键实现**：

| 功能 | 怎么做的 | C 语言类比 |
|------|---------|-----------|
| 无边框 | 设置窗口属性 `Qt::FramelessWindowHint` | 相当于 Windows API `SetWindowLong` 去掉标题栏 |
| 鼠标穿透 | 检查点击位置像素的 alpha 通道，透明部分 `return` 不处理 | 相当于判断 `if (pixel_alpha < 10) pass_through()` |
| 拖拽移动 | `DragHelper` 类拦截鼠标事件，移动时重新设置窗口位置 | `case WM_MOUSEMOVE: SetWindowPos(...)` |
| 动画播放 | 读取 JSON 插件 → 用 `QPropertyAnimation` 改变位置/透明度/大小 | 相当于定时器 + `memcpy` 缩放图片 + 移动坐标 |
| 图片加载 | `QPixmap` 加载 PNG → `QLabel::setPixmap()` 显示 | 读文件头 → 解码 → `BitBlt` 绘制 |

**动画步骤**（从 `AnimePlugin.h`）：
```cpp
struct AnimePluginStep {
    Type type;         // Move（移动）、Opacity（透明度）、Scale（缩放）
    double durationSec; // 持续多少秒
    double x, y;       // Move: 横向纵向偏移
    double from, to;   // Opacity: 透明度从多少到多少
    double scaleFrom, scaleTo; // Scale: 缩放从几倍到几倍
};
```

### 4.2 Dialog（聊天气泡）— `windows/dialog/`

这是整个程序最复杂的模块。把它拆成几个子功能来理解：

#### 4.2.1 AI 对话流程

```
用户输入"你好" →
  ↓
检查是否触发屏幕捕获关键词（如"看看屏幕"）
  ↓ 不是
检查是否触发搜索关键词（如"帮我搜"）
  ↓ 不是
检查是否匹配应用启动词（如"打开微信"）
  ↓ 不是
AI 判断是否需要联网搜索
  ↓ 不需要
发送给大语言模型（DeepSeek/OpenAI/Custom）
  ↓
收到流式回复 → 逐字显示在聊天气泡中
  ↓
解析回复格式：心情|中文|日语
  ├→ 心情 → 切换立绘表情
  ├→ 中文 → 显示在聊天框
  └→ 日语 → 发去 TTS 合成语音
```

#### 4.2.2 AI 回复格式约定

系统提示词要求 AI 按 `心情|中文|日语` 格式回复：

```
用户问：今天天气真好
AI 回：快乐|是呀，阳光明媚的！|そうですね、いい天気ですね！
       ↑       ↑                    ↑
     心情      中文显示              日语合成语音
```

为什么用 `|` 分隔？因为解析简单——`str.section('|', 0, 0)` 取心情，`.section('|', 1, 1)` 取中文，`.section('|', 2, 2)` 取日语。类似于 C 语言的 `strtok()` 按分隔符切字符串。

#### 4.2.3 消息优先级链

用户输入一条消息后，Dialog 按固定顺序检查它属于哪种类型：

```
优先级 1：屏幕捕获  → "看看屏幕"、"截图" → 截屏发去视觉 AI 分析
优先级 2：手动搜索  → "帮我搜"、"查一下" → 调搜索引擎
优先级 3：应用启动  → "打开微信" → QProcess 启动程序
优先级 4：AI 分类   → 用户没明说但可能需要搜索 → AI 判断
优先级 5：普通聊天  → 直接发给大模型
```

类似于 C 语言的 `if-else if-else` 链。这样做的好处是**不会两个处理器同时响应同一条消息**。

#### 4.2.4 流式显示

大模型不是一次性返回完整回复，而是一个字一个字吐出来的（就像 ChatGPT 打字效果）。这叫做 **SSE（Server-Sent Events）**。

```cpp
// AiProvider 收到一个 chunk → 发出信号
connect(ai, &AiProvider::replyChunkReceived, [=](const QString &chunk) {
    m_streamRawReply += chunk;          // 追加到缓冲区
    // 每 100ms 更新一次界面，避免太频繁刷新
    if (!m_streamDisplayTimer->isActive())
        m_streamDisplayTimer->start();
});

// 100ms 定时器触发 → 更新聊天框显示
connect(m_streamDisplayTimer, &QTimer::timeout, [=]() {
    ui->textEdit->setText(m_streamDisplayedChinese);
});
```

这类似于串口通信中的**环形缓冲区 + 定时刷新**模式：数据来了先存着，定时批量显示。

#### 4.2.5 语音流程

```
说话 → QAudioSource 麦克风采集成 PCM 数据
     → 静音检测（连续 2.5s 无声 = 说完）
     → Base64 编码 → HTTP 发给百度语音识别 API
     → 拿到识别文字 → 填入输入框
     → 自动发送（或手动 Enter）

AI 回复的日语部分：
     → 按句号/感叹号/问号 切分句子
     → 逐句 HTTP 发给 VITS 服务器
     → 并发 2 路（前一句还在合成时下一句已经发出）
     → HTTP 返回 MP3 → QTemporaryFile 暂存
     → QMediaPlayer 按顺序播放
```

语音处理的本质是一串**数据格式转换**：

```
声波 → PCM 数字信号 → Base64 字符串 → HTTP JSON → 百度识别 → 中文文本
日文文本 → HTTP 请求 → VITS 合成 → MP3 → QMediaPlayer 播放 → 声波
```

#### 4.2.6 联网搜索

```
用户说"帮我搜一下最近有什么好玩的游戏"
  → extractSearchQuery() 提取查询词："最近有什么好玩的游戏"
  → SearchProvider::search() HTTP 发给百度千帆/SearXNG/Bing
  → 解析搜索结果 → 构建摘要文本
  → 注入到用户消息中：
    "最近有什么好玩的游戏\n\n[联网搜索结果]：xxx..."
  → 发去大模型，大模型基于搜索结果回答
```

本质是把搜索结果作为**附加上下文**塞到 AI 请求里，让 AI 能参考最新信息作答。

### 4.3 MainWindow（设置面板）— `windows/setting/`

**功能**：配置一切。

采用 `ElaWindow` 导航布局，左边导航栏 + 右边内容区，有 10 个子页面：

| 页面 | 类名 | 作用 |
|------|------|------|
| LLM | `SettingChild_LLM` | 配置 AI 服务商（OpenAI/DeepSeek/Custom）|
| 角色 | `SettingChild_Char` | 导入/管理角色、绑定动画 |
| VITS | `SettingChild_Vits` | 语音合成服务配置 |
| 语音输入 | `SettingChild_Speech` | 百度识别 API、热键、唤醒词 |
| 屏幕捕获 | `SettingChild_ScreenCapture` | 截图分析用的视觉模型 |
| 联网搜索 | `SettingChild_Search` | 搜索引擎 API 配置 |
| 应用启动 | `SettingChild_AppLauncher` | 关键词→程序的映射 |
| 插件 | `SettingChild_Plugin` | 导入/管理动画插件 |
| 通用 | `SettingChild_General` | 窗口大小、开机自启 |
| 关于 | `SettingChild_About` | 版本号、检查更新 |

每个设置页面的工作模式一样：

```
1. 构造函数：从 config.json / config.ini 读取已保存的值 → 填入界面控件
2. 用户修改 → 立即写入配置文件
3. emit 信号通知 Dialog::Reload*Config() 刷新运行时状态
```

**懒加载**：设置窗口在用户点击托盘图标前都不创建，节省启动内存。

---

## 第五章：关键 C++ 概念解释

### 5.1 `emit` 是什么

```cpp
emit requestSetCharTachie("happy");  // 发射信号
```

`emit` 是 Qt 的宏，展开后等于空。它只是用来向读代码的人标注"这里在发信号"。实际效果等价于：

```c
// C 语言等价理解
void emit_requestSetCharTachie(const char *mood) {
    for (int i = 0; i < callback_count; i++) {
        callbacks[i](mood);  // 调用所有连接到这个信号的回调函数
    }
}
```

### 5.2 `Q_OBJECT` 宏

```cpp
class Dialog : public QWidget {
    Q_OBJECT  // ← 这一行必须写
```

`Q_OBJECT` 是 Qt 的"魔法宏"。它告诉 Qt 的代码生成器（MOC, Meta-Object Compiler）:"这个类用了信号和槽，帮我生成额外代码"。如果你写了 `signals:` 或 `slots:` 但不加 `Q_OBJECT`，编译会报奇怪的错误。

### 5.3 `QTimer::singleShot(500, this, &Dialog::f)`

```cpp
QTimer::singleShot(500, this, &Dialog::fetchLocation);
```

含义：**500 毫秒后**，调用 `this->fetchLocation()`。只执行一次。

类似于：`sleep(500); fetchLocation();` 但不阻塞主线程。

### 5.4 `QtConcurrent::run` vs 主线程

Mandarin 中**全部代码在主线程运行**，没有多线程。网络、音频、ONNX 推理全在主线程。

这是故意的——因为 Qt 的 GUI 操作必须在主线程，而单线程避免了锁、竞态条件等复杂问题。代价是任何耗时操作（如 ONNX 推理）会短暂卡住界面。

C 语言类比：
```c
// 单线程事件循环
while (running) {
    event = get_next_event();
    handle(event);   // 处理每个事件要快，不然界面卡住
}
```

### 5.5 `QNetworkAccessManager` 异步网络

```cpp
QNetworkReply *reply = manager->get(request);
connect(reply, &QNetworkReply::finished, [=]() {
    // 网络请求完成后，自动回调这里
});
```

你熟悉的 C 语言写法可能是：

```c
char *result = http_get(url);  // 阻塞，等网络返回
process(result);
```

而 Qt 的异步方式是：**发出请求后立刻返回，不等待，网络完成后通过信号通知你**。类似于注册了一个回调函数。

---

## 第六章：数据是怎么存的

### 6.1 两个配置文件

```
Documents/Mandarin/
├── config.json   ← JSON 格式，存 API Key、模型选择等（可以迁移到其他电脑）
└── config.ini    ← INI 格式，存窗口位置、最近使用的角色（本机专属）
```

**为什么分两个**？你换电脑时，`config.json` 里的 API Key 要带走，但 `config.ini` 里的窗口坐标在新屏幕上可能不适用。

### 6.2 角色数据

```
Documents/Mandarin/Character/
├── Assets/Atri/              ← 角色资源（开发者提供）
│   ├── Tachie/               ← 立绘图片
│   │   ├── default.png       ← 默认表情
│   │   ├── happy.png         ← 开心表情
│   │   └── angry.png         ← 生气表情
│   └── config.json           ← 角色设定 + 动画绑定
│
└── UserConfig/Atri/          ← 用户数据（用户生成）
    ├── config.json           ← 用户选择的模型/TTS配置
    ├── context.json          ← 最近的对话记录
    └── memory.json           ← AI 提取的长期记忆
```

---

## 第七章：一个完整请求的全过程

以"用户输入'今天天气好吗'并得到回复"为例：

```
[1] 用户在聊天框输入"今天天气好吗"，按 Enter
     ↓
[2] submitCurrentInput() 开始优先级检查
     → 不是屏幕捕获、不是手动搜索、不是应用启动
     → classifyAndSearch()：问 AI "这需要搜索吗？"
     → AI 回复 "YES|天气"
     ↓
[3] SearchProvider::search("天气") → HTTP 发百度千帆 API
     → 返回搜索结果 → buildSummary() 构建摘要文字
     ↓
[4] doSubmitCurrentInput() 构建完整消息：
     系统提示词（角色设定 + 用户记忆 + 所在地）
     + 对话历史
     + "今天天气好吗\n\n[搜索结果]：上海今天晴，22-28℃..."
     ↓ 发给大模型
[5] AI 流式返回：
     replyChunkReceived("开心")  → chunk by chunk
     replyChunkReceived("|今天")
     replyChunkReceived("天气不错")
     ...
     → 每 100ms 更新聊天框显示
     ↓
[6] replyReceived("开心|今天天气不错呢！|今日はいい天気ですね！")
     → 解析：心情=开心, 中文=今天天气不错呢, 日语=今日は...
     → emit requestSetCharTachie("开心") → Tachie 切 happy.png
     → 日语"今日はいい天気ですね!" → VitsGetAndPlay() → TTS 合成语音
     ↓
[7] extractAndStoreMemory()
     → 调 AI 提取记忆 → 存入 memory.json
     ↓
[8] scheduleContextSave()
     → 对话写入 context.json（2 秒延迟防抖）
```

---

## 第八章：如果你要改代码

### 8.1 改立绘

在 `tachie.cpp` 的 `SetTachieImg()` 里改图片加载逻辑。

### 8.2 改聊天行为

在 `dialog.cpp` 的 `submitCurrentInput()` 或 `doSubmitCurrentInput()` 里改。

### 8.3 加新的设置项

1. 在 `settingchild_xxx.ui` 加界面控件
2. 在 `settingchild_xxx.h` 加槽声明
3. 在 `settingchild_xxx.cpp` 实现读写配置 + emit 信号
4. 在 `dialog.h/cpp` 添加 `Reload*Config()` 方法响应

### 8.4 加新的 AI 服务商

```
1. AiProvider.h → ServiceType 枚举加一个
2. AiProvider.cpp → 在 setServiceType 和 chat 里处理新服务商
3. settingchild_llm.cpp → 加 UI 和配置逻辑
```

---

## 总结

| 你学过的 | Mandarin 里的对应 |
|---------|-----------------|
| C 语言的 `while` 循环 | Qt 的 `a.exec()` 事件循环 |
| 函数指针/回调 | 信号与槽 `connect()` |
| `strtok()` 分割字符串 | `QString::section('|', ...)` |
| `fopen`/`fread`/`fwrite` | `QFile` / `ZcJsonLib` |
| `socket` + `send`/`recv` | `QNetworkAccessManager` 异步 HTTP |
| 结构体 | `class`（C++ 里带函数的结构体） |
| `malloc`/`free` | `new`/`delete`（Qt 里常用 `parent` 自动管理） |
| 指针 | `QObject` 的 parent-child 树管理生命周期 |
