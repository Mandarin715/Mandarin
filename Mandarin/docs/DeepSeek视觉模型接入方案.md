# DeepSeek 视觉模型接入方案（待实施）

> 2026-08-22 设计。给屏幕识别（屏幕捕获/视觉分析）的 Server 下拉增加 **DeepSeek** 内置预置项，一键选用 `deepseek-v4-flash-vision-exp` 视觉模型（OpenAI 兼容接口，与现有 Kimi/OpenAI 调用格式一致）。

## 目标

屏幕识别设置里能像 Kimi/OpenAI 一样**点选即用** DeepSeek 视觉模型，不用手动填 Custom BaseUrl。

## 现状分析

屏幕识别目前三处硬编码了服务商：

| 文件 | 位置 | 现状 |
|------|------|------|
| `settingchild_screenCapture.cpp:19-21` | 构造函数 | `comboBox_ServerSelect` 只加 Kimi / OpenAI / Custom |
| `settingchild_screenCapture.cpp:119-135` | `updateModelPresets()` | 按服务商填充模型下拉预设（Kimi → moonshot-v1-*-vision-preview；OpenAI → gpt-4o*；Custom → 无） |
| `settingchild_screenCapture.cpp:137-145` | `updateBaseUrlVisibility()` | 仅 Custom 显示 BaseUrl 输入框；Kimi/OpenAI 只设占位符 |
| `dialog.cpp:2800-2825` | `analyzeScreenWithVision()` | Server 分支决定 API 端点（Kimi → api.moonshot.cn；OpenAI → api.openai.com；Custom → baseUrl + /v1/chat/completions） |
| `settingchild_screenCapture.ui:134` | 提示标签 | "Kimi 推荐…，OpenAI 推荐 gpt-4o-mini。API Key 与对话模型分开配置" |

**已有先例**：LLM 设置页（`settingchild_llm.cpp:225`、`AiProvider::DeepSeek`）已经把 DeepSeek 作为内置服务商支持，端点知识项目内已有，视觉接入照此办理。

## 设计方案

### 1. 设置页加「DeepSeek」预置项

`settingchild_screenCapture.cpp` 构造函数服务商列表加一项：

```cpp
ui->comboBox_ServerSelect->addItem("Kimi");
ui->comboBox_ServerSelect->addItem("DeepSeek");   // ✨ 新增
ui->comboBox_ServerSelect->addItem("OpenAI");
ui->comboBox_ServerSelect->addItem("Custom");
```

### 2. 模型预设

`updateModelPresets()` 加 DeepSeek 分支：

```cpp
else if (server == "DeepSeek")
{
    ui->comboBox_ModelSelect->addItem("deepseek-v4-flash-vision-exp");
}
```

> 模型名以 [DeepSeek 视觉 API 文档](https://api-docs.deepseek.com/zh-cn/guides/vision/) 为准；若后续有更新型号，在此追加即可。

### 3. BaseUrl 可见性

`updateBaseUrlVisibility()`：DeepSeek 非 Custom → BaseUrl 输入框保持隐藏，设占位符：

```cpp
else if (!isCustom && server == "DeepSeek")
    ui->lineEdit_BaseUrl->setPlaceholderText("https://api.deepseek.com/v1");
```

### 4. 运行时端点（dialog.cpp）

`analyzeScreenWithVision()` 的端点选择加 DeepSeek 分支：

```cpp
if (serverSelect == "Kimi")
    apiUrl = QStringLiteral("https://api.moonshot.cn/v1/chat/completions");
else if (serverSelect == "DeepSeek")                          // ✨ 新增
    apiUrl = QStringLiteral("https://api.deepseek.com/v1/chat/completions");
else if (serverSelect == "OpenAI")
    apiUrl = QStringLiteral("https://api.openai.com/v1/chat/completions");
```

请求体（base64 `image_url`、`max_tokens=500`、stream=false）**无需改动**——DeepSeek 视觉接口与 OpenAI 兼容。

### 5. 提示文案（可选）

`settingchild_screenCapture.ui:134` 提示标签补一句："…DeepSeek 推荐模型 deepseek-v4-flash-vision-exp"。

## 与现有功能关系

- 原 Kimi/OpenAI/Custom 分支**不动**，纯增量加一项；
- 配置沿用 `screenCapture/Server` + `screenCapture/Model`，切换服务商时模型下拉自动换预设（已有逻辑 `updateModelPresets` 自动处理）；
- 未来智能助手方案的 `buildVisionRequest` 抽取后，此端点分支一并受益。

## 代码改动清单

| 文件 | 改动 | 行数 |
|------|------|------|
| `settingchild_screenCapture.cpp` | 服务商列表 + `updateModelPresets` + `updateBaseUrlVisibility` 各加 DeepSeek 分支 | ~6 行 |
| `settingchild_screenCapture.ui` | 提示标签补 DeepSeek 文案 | 1 行 |
| `dialog.cpp` | `analyzeScreenWithVision` 端点分支加 DeepSeek | 2 行 |

总计约 **10 行，0.5 天内**。

## 实施步骤

| 阶段 | 内容 | 验收 |
|------|------|------|
| 1 | 三处代码改动 | 设置页下拉出现 DeepSeek；选中后模型下拉出现 vision-exp 预设；配置落盘 `screenCapture/Server=DeepSeek` |
| 2 | 运行验证 | 屏幕识别用 DeepSeek 返回正常描述；切回 Kimi/OpenAI 行为不变 |

## 风险

| 风险 | 对策 |
|------|------|
| 模型名/端点与官方文档漂移 | 以 [DeepSeek 视觉文档](https://api-docs.deepseek.com/zh-cn/guides/vision/) 为准；模型下拉预设集中一处易改 |
| 视觉 API 未开通/Key 无效 | 与 Kimi 相同提示（无 Key 时 `analyzeScreenWithVision` 已回退纯文本，`dialog.cpp:2792-2798`） |
| 图片格式不兼容 | base64 image_url 为 OpenAI 标准格式，与现有 Kimi 请求一致，无需改动 |
