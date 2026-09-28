# Live2D 接入 · 阶段 0–1 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 Live2D 接入的两块地基打好——① Cubism Native SDK 能编进工程（阶段 0）；② 立绘窗口的「窗口层」从 `Tachie` 里干净下沉为 `CharacterWindowBase`，PNG 路径零回归（阶段 1）；③ 顺带把**不依赖 SDK** 的模型解析模块与几何模块写成有测试的独立单元（阶段 3 的前置，提前做是因为它不依赖 SDK）。

**Architecture:** 现有 `Tachie` 把「窗口关注点」（无边框/透明/鼠标穿透/交互区/拖拽/位置持久化/内心气泡/文件拖放）和「渲染关注点」（QPixmap 加载缩放、AnimePlugin 动画）揉在一个 697 行的类里。本计划先抽出纯几何与纯解析两块有测试的模块，再把窗口关注点下沉到 `CharacterWindowBase`，`Tachie` 退化为 PNG 渲染器子类。Live2D 渲染器将在阶段 2 作为同级的第二个子类加入，**不需要复制任何窗口逻辑**。渲染走离屏 FBO + 读回 RGBA，因此渲染结果就是一张 alpha `QImage`，可直接喂给既有的 `ApplyInteractiveRegionFromImage()`。

**Tech Stack:** C++20、Qt 6.6.3（Widgets/Test/OpenGL）、MSVC 2022、CMake、Live2D Cubism SDK for Native 5-r.x、Qt Test。

**Spec:** `Mandarin/docs/Live2D方案.md`（含 2026-09-28 实施前核实章节，**动手前必读**）

## Global Constraints

- 版本真源：`Mandarin/CMakeLists.txt` 的 `project(Mandarin VERSION x.y.z)`，本计划**不改版本号**。
- C++ 标准：`CMAKE_CXX_STANDARD 20`，不得降级。
- **禁止启动桌宠**：本计划全程只做构建与测试，不运行 `启动.bat` / `Mandarin.exe`。构建 `Mandarin` 目标前必须确认 `Mandarin.exe` 未运行（否则 `LNK1104`）。
- 构建目录用 `build2`；构建 `cmake --build "build2" --config Release --target <target>`；测试 `ctest --test-dir build2 -C Release`。
- **模型文件禁二传**：任何 `.moc3` / `.model3.json` / `.exp3.json` / 贴图**不得进入 git、不得进入打包产物**。测试 fixture 只能用**手写的合成 JSON**，不得复制 `Documents/Mandarin/Live2D/` 下的真实模型文件。
- **模型文件禁二改**：不得修改、重打包模型目录内任何文件。
- **SDK 入 git 禁止**：`Mandarin/3rdparty/Live2DCubismSDK/` 已加入 `.gitignore`，SDK 二进制不入库。
- 中文注释风格与现有代码一致；命名类名 PascalCase、成员 `m_` 前缀、私有成员 `_` 前缀（部分沿用）。
- 测试文件组织沿用 `tests/test_chatlogstore.cpp` 的 Qt Test 风格（`QTEST_MAIN` + `#include "xxx.moc"`）。

---

## 依赖与并行说明

| 任务 | 依赖 |
|---|---|
| Task 1（SDK 就位 + 编译冒烟） | **依赖使用者下载 SDK**（官方页需勾选同意 EULA，工具不能代接受） |
| Task 2（模型解析模块） | 无 |
| Task 3（几何模块） | 无 |
| Task 4（下沉 `CharacterWindowBase`） | Task 3 |
| Task 5（回归验收） | Task 1–4 |

**Task 2 与 Task 3 不依赖 SDK，SDK 还在下载时就可以先做。**

---

## 文件结构

| 文件 | 责任 | 动作 |
|---|---|---|
| `Mandarin/tests/test_cubismcore.cpp` | Core 能否链接运行的编译冒烟 | 新建 |
| `Mandarin/tests/test_live2dmodelinfo.cpp` | 模型资源清单/参数/水印参数/指纹的单元测试 | 新建 |
| `Mandarin/tests/test_tachiegeometry.cpp` | 立绘画布几何的 characterization 测试 | 新建 |
| `Mandarin/utils/Live2DModelInfo.{h,cpp}` | 纯解析：model3.json / cdi3.json / exp3.json / 指纹。**不读文件、不碰 SDK** | 新建 |
| `Mandarin/utils/TachieGeometry.{h,cpp}` | 纯几何：200% 画布、居中、倍率钳制 | 新建 |
| `Mandarin/windows/character/characterwindowbase.{h,cpp}` | 窗口层：透明/穿透/交互区/拖拽/位置/气泡/拖放 | 新建 |
| `Mandarin/windows/tachie/tachie.{h,cpp}` | 退化为 PNG 渲染器子类 | 修改 |
| `Mandarin/main.cpp` | 窗口创建与信号连接改为基类指针 | 修改 |
| `Mandarin/windows/setting/setting.{h,cpp}` | 构造形参与 3 条连接改为基类类型 | 修改 |
| `Mandarin/CMakeLists.txt` | SDK 接线 + 3 个新测试目标 | 修改 |

---

## Task 1: Cubism Native SDK 就位 + 编译冒烟

**Files:**
- Create: `Mandarin/tests/test_cubismcore.cpp`
- Modify: `Mandarin/CMakeLists.txt`（在 L328 之后、L330 的单元测试段之前插入 SDK 段）

**Interfaces:**
- Consumes: 使用者下载并解压的 `Mandarin/3rdparty/Live2DCubismSDK/`（内含 `Core/` 与 `Framework/`）
- Produces: CMake 目标 `CubismNativeFramework`（静态库，`PUBLIC` 暴露 `Core/include`、`Framework/src`、`CSM_TARGET_WIN_GL`）、变量 `CUBISM_CORE_LIB`

> ⚠️ **本任务第一步是核对真实目录结构**：SDK 尚未下载，下面的路径按 5-r.x 常见布局书写，**解压后必须先用 `ls` 确认真实文件名**（尤其 Core 库是 `_MD` / `_MT` 还是单一 `.lib`、是否含 `.dll`）。实测与假设不符时以实测为准改 CMake，不要改假设硬凑。

- [ ] **Step 1: 先写会失败的冒烟测试**

创建 `Mandarin/tests/test_cubismcore.cpp`：

```cpp
/*Cubism Core 能否链接并运行的最小冒烟测试。
  Core 是闭源预编译库，只有它能证明 include 路径与 .lib 选型都对。*/
#include <Live2DCubismCore.h>

#include <cstdio>

int main()
{
    const csmUint32 version = csmGetVersion();
    if (version == 0)
    {
        std::printf("FAIL: csmGetVersion() 返回 0\n");
        return 1;
    }
    // 版本号按 major<<24 | minor<<16 | patch<<8 | build 打包
    const unsigned major = (version >> 24) & 0xFF;
    const unsigned minor = (version >> 16) & 0xFF;
    const unsigned patch = (version >> 8) & 0xFF;
    std::printf("OK: Cubism Core %u.%u.%u (raw 0x%08X)\n", major, minor, patch, version);
    return 0;
}
```

> 若 `csmGetVersion` 的真实签名/返回类型与头文件不符，**以头文件为准**修改本文件——这一步的目的就是让编译器告诉我们真相。

- [ ] **Step 2: 确认测试因缺少 SDK 接线而失败**

Run: `cmake --build "build2" --config Release --target test_cubismcore`
Expected: FAIL —— 目标不存在（`test_cubismcore` 尚未加进 CMake，构建系统报 "unknown target"）。这是本任务的"红"。

- [x] **Step 3: 解压 SDK 并核对真实布局**（2026-09-28 已完成，实测结构如下）

解压到 `Mandarin/3rdparty/Live2DCubismSDK/`（官方那层 `CubismSdkForNative-5-r.5/` 保留即可，CMake 会自动下钻）。**实测真实结构**：

```
Mandarin/3rdparty/Live2DCubismSDK/CubismSdkForNative-5-r.5/
├── Core/
│   ├── include/Live2DCubismCore.h
│   ├── lib/windows/x86_64/<工具集>/*.lib      ← 实测多一层工具集目录！
│   └── dll/windows/x86_64/Live2DCubismCore.{dll,lib}   ← DLL 版（本工程不用）
└── Framework/
    ├── CMakeLists.txt        ← 官方构建脚本，target 名 = Framework (STATIC)
    └── src/  (75 个 .cpp)
        └── Rendering/{D3D9,D3D11,OpenGL,Vulkan}/   ← 4 个后端
```

**实测与计划假设的 3 处差异（已据此修正下面的 CMake）**：

1. **Core 静态库多一层「MSVC 工具集版本」目录**：`lib/windows/x86_64/{141,142,143}/`，且每个目录下有 4 个变体
   `Live2DCubismCore_{MD,MDd,MT,MTd}.lib`。141=VS2017、142=VS2019、**143=VS2022**。
   本工程生成器是 `Visual Studio 17 2022` → 取 `143`；CRT 跟配置走：**Debug→`_MDd`，其余→`_MD`**（Qt 用 `/MD`）。
   选**静态库**而非 `Core/dll/` 下的 DLL 版，少一个要随包分发的文件。
2. **`Framework/src` 里含 4 个渲染后端**（D3D9 7 个、D3D11 8 个、OpenGL 5 个、Vulkan 6 个）。
   **绝不能 `file(GLOB_RECURSE .../Framework/src/*.cpp)` 全量编译** —— 会连带编译 Vulkan/D3D 后端（Vulkan 还要 Vulkan SDK）。
   官方做法：父级先设 `FRAMEWORK_SOURCE=OpenGL`（`Rendering/CMakeLists.txt` 里 `add_subdirectory(${FRAMEWORK_SOURCE})`），再 `add_subdirectory(Framework)`。
3. **OpenGL 后端在 Windows 上 `#include <GL/glew.h>`**（`CubismRenderer_OpenGLES2.hpp` L33-35，条件 `CSM_TARGET_WIN_GL`）
   —— **SDK 不自带 GLEW**，这是阶段 2 必须补的外部依赖（见"后续计划"）。

> `csmGetVersion()` 实测返回 **6.0.0**（raw `0x06000001`），`csmGetLatestMocVersion()` = 6。
> **Core 的版本号与 SDK 包名 `5-r.5` 不是一回事**，不要用包名去推断 Core 版本。
> 另：`typedef unsigned int csmVersion;` / `csmMocVersion`，都是普通 typedef（不是 enum class）。

- [x] **Step 4: 在 CMakeLists.txt 里接线 SDK**（2026-09-28 已完成并实测通过）

先在 `Mandarin/CMakeLists.txt` 的 `project(...)` 之后加一条**全局编译选项**（实测发现：不链 Qt 的目标不会被 Qt 带上 `/utf-8`，MSVC 会按 GBK 解析源码，导致中文注释报 `C4819 + C1071`）：

```cmake
# MSVC 默认按系统代码页解析源文件（中文 Windows 上是 936/GBK），会把中文注释与字面量读坏，
# 表现为 C4819 + C1071（"在注释中遇到意外的文件结束"）。
# 链接 Qt 的目标会由 Qt6::Core 带上 /utf-8，**不链接 Qt 的目标不会**，所以这里全局显式指定；
# 源文件一律以 UTF-8（无 BOM）保存。
if(MSVC)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/utf-8>")
endif()
```

然后在 `qt_finalize_executable(Mandarin)` 之后、`# ---- 单元测试（Qt Test）----` 之前插入：

```cmake
# ---- Live2D Cubism SDK for Native（专有许可，目录不入版本库）----
# 官方 zip 解压后会多一层 CubismSdkForNative-5-r.x/，这里自动下钻一层，省得手工改名。
set(CUBISM_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/Live2DCubismSDK"
    CACHE PATH "Live2D Cubism SDK for Native 解压根目录")
if(NOT EXISTS "${CUBISM_SDK_DIR}/Core/include/Live2DCubismCore.h")
    file(GLOB _cubism_candidates "${CUBISM_SDK_DIR}/CubismSdkForNative-*")
    foreach(_cubism_candidate ${_cubism_candidates})
        if(EXISTS "${_cubism_candidate}/Core/include/Live2DCubismCore.h")
            set(CUBISM_SDK_DIR "${_cubism_candidate}")
            break()
        endif()
    endforeach()
endif()

set(CUBISM_CORE_DIR "")
set(CUBISM_CORE_LIB "")
if(WIN32 AND EXISTS "${CUBISM_SDK_DIR}/Core/include/Live2DCubismCore.h")
    # Core 预编译库按 MSVC 工具集版本分目录（143 = VS2022，142 = VS2019，141 = VS2017），
    # 取第一个存在的；本工程生成器是 "Visual Studio 17 2022"，命中 143。
    foreach(_cubism_toolset 143 142 141)
        if(EXISTS "${CUBISM_SDK_DIR}/Core/lib/windows/x86_64/${_cubism_toolset}")
            set(CUBISM_CORE_DIR "${CUBISM_SDK_DIR}/Core/lib/windows/x86_64/${_cubism_toolset}")
            break()
        endif()
    endforeach()

    if(CUBISM_CORE_DIR)
        # 选静态库而不是 Core/dll 下的 DLL 版：少一个要随包分发的文件。
        # CRT 必须与配置匹配（Qt 用 /MD）：Debug -> _MDd，其余 -> _MD。
        set(CUBISM_CORE_LIB
            "$<$<CONFIG:Debug>:${CUBISM_CORE_DIR}/Live2DCubismCore_MDd.lib>"
            "$<$<NOT:$<CONFIG:Debug>>:${CUBISM_CORE_DIR}/Live2DCubismCore_MD.lib>")
    endif()

    message(STATUS "Live2D Cubism SDK: base=${CUBISM_SDK_DIR}")
    message(STATUS "Live2D Cubism Core: ${CUBISM_CORE_DIR}")
else()
    message(STATUS "Live2D Cubism SDK 未就位，跳过（Live2D 相关目标不可用）")
endif()
```

> **Framework 静态库不在本任务里建**：它需要先补 GLEW，且必须走官方 `add_subdirectory(Framework)` + 预设 `FRAMEWORK_SOURCE=OpenGL` 的路线（不能 glob）。这段留到阶段 2，见「后续计划」。
> 实测配置输出：
> ```
> -- Live2D Cubism SDK: base=.../3rdparty/Live2DCubismSDK/CubismSdkForNative-5-r.5
> -- Live2D Cubism Core: .../Core/lib/windows/x86_64/143
> ```

- [x] **Step 5: 加冒烟测试目标**

在 `Mandarin/CMakeLists.txt` 的 `if(BUILD_TESTING)` 段内、`endif()` 之前追加（`CUBISM_CORE_LIB` 是含生成器表达式的**列表**，不要再加引号包成单个字符串）：

```cmake
    # Cubism Core 编译/链接冒烟（不依赖 Qt；用静态库，无需拷贝 DLL）
    if(WIN32 AND CUBISM_CORE_LIB)
        add_executable(test_cubismcore tests/test_cubismcore.cpp)
        target_include_directories(test_cubismcore PRIVATE
            "${CUBISM_SDK_DIR}/Core/include")
        target_link_libraries(test_cubismcore PRIVATE ${CUBISM_CORE_LIB})
        set_target_properties(test_cubismcore PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
        add_test(NAME test_cubismcore COMMAND test_cubismcore)
    endif()
```

- [x] **Step 6: 重新 configure 并构建**（已完成）

Run: `cmake -S Mandarin -B build2` 然后 `cmake --build "build2" --config Release --target test_cubismcore`
实测结果：配置阶段打印 `Live2D Cubism SDK: base=...` / `Live2D Cubism Core: .../143`；`test_cubismcore.vcxproj -> ...\tests\Release\test_cubismcore.exe`。

- [x] **Step 7: 运行冒烟测试**（已完成）

Run: `ctest --test-dir build2 -C Release -R test_cubismcore --output-on-failure`
实测输出（**注意是 6.0.0，不是 5.x**）：

```
OK: Cubism Core 6.0.0 (raw 0x06000001), latest moc version 6
```

- [x] **Step 8: 验证 `/utf-8` 全局改动无回归**（新增步骤，实测通过）

`/utf-8` 是全局编译选项，会影响主程序全部源文件，必须单独验证：

```powershell
cmake --build "build2" --config Release --target Mandarin   # 主程序重编，期望零 error/warning
ctest --test-dir build2 -C Release --output-on-failure      # 期望 4/4 通过
```

实测：`Mandarin.vcxproj -> build2\Release\Mandarin.exe`，无 error/warning；`100% tests passed, 0 tests failed out of 4`。

- [x] **Step 9: 确认 SDK 未进版本库**（已完成）

Run: `git status --short` → 只出现 `Mandarin/CMakeLists.txt`、`tests/test_cubismcore.cpp`、计划文档；`git check-ignore` 命中 `.gitignore:121`。

- [ ] **Step 10: Commit**

```bash
git add Mandarin/CMakeLists.txt Mandarin/tests/test_cubismcore.cpp Mandarin/docs/Live2D实施计划-阶段0-1.md
git commit -m "build(live2d): wire Cubism Core (v143/_MD) into CMake with a link smoke test

- auto-descend into CubismSdkForNative-*, pick toolset dir 143 for VS2022
- per-config CRT: Debug -> _MDd, otherwise _MD; static lib, no DLL to ship
- add global /utf-8 for MSVC: non-Qt targets would otherwise parse UTF-8 as GBK (C4819/C1071)
- smoke test asserts csmGetVersion() and csmGetLatestMocVersion(); Core reports 6.0.0"
```

---

## Task 2: 模型解析模块 `Live2DModelInfo`

**Files:**
- Create: `Mandarin/utils/Live2DModelInfo.h`
- Create: `Mandarin/utils/Live2DModelInfo.cpp`
- Create: `Mandarin/tests/test_live2dmodelinfo.cpp`
- Modify: `Mandarin/CMakeLists.txt`

**Interfaces:**
- Consumes: 无（纯解析）
- Produces:
  - `class Live2DModelInfo`
  - `bool Live2DModelInfo::parse(const QByteArray &model3Json, const QByteArray &cdi3Json, QString *error = nullptr)`
  - `QString Live2DModelInfo::mocPath() const` / `physicsPath()` / `displayInfoPath()`
  - `QStringList Live2DModelInfo::texturePaths() const`
  - `QStringList Live2DModelInfo::groupIds(const QString &groupName) const`
  - `bool Live2DModelInfo::hasParameter(const QString &id) const` / `QString parameterDisplayName(const QString &id) const` / `int parameterCount() const`
  - `static QString Live2DModelInfo::expressionParamId(const QByteArray &exp3Json)`
  - `static QString Live2DModelInfo::fingerprint(const QByteArray &moc3Bytes)`

> **为什么全是字节入参**：模型目录与文件名可能含中文（`樱花miku.moc3`）。Cubism SDK 内部走 `csmString`→`fopen`，Windows 下打不开 UTF-8 中文路径，所以**路径永远不交给 SDK**；本模块连文件都不读，只解析字节，由调用方用 `QFile` 读好再传进来。

- [ ] **Step 1: 写失败的测试**

创建 `Mandarin/tests/test_live2dmodelinfo.cpp`：

```cpp
#include <QtTest>

#include "../utils/Live2DModelInfo.h"

class TestLive2DModelInfo : public QObject
{
    Q_OBJECT

  private slots:
    void parseKeepsChinesePathsAndGroups();
    void parseRejectsMissingMoc();
    void expressionParamIdReadsWatermarkParam();
    void fingerprintChangesWithBytes();
};

/*模型路径/贴图清单原样保留中文，Groups 正确读出*/
void TestLive2DModelInfo::parseKeepsChinesePathsAndGroups()
{
    const QByteArray model3 = R"({
        "Version": 3,
        "FileReferences": {
            "Moc": "樱花miku.moc3",
            "Textures": ["樱花miku.4096/texture_00.png", "樱花miku.4096/texture_01.png"],
            "Physics": "樱花miku.physics3.json",
            "DisplayInfo": "樱花miku.cdi3.json"
        },
        "Groups": [
            { "Target": "Parameter", "Name": "LipSync", "Ids": [] },
            { "Target": "Parameter", "Name": "EyeBlink", "Ids": ["ParamEyeROpen", "ParamEyeLOpen"] }
        ]
    })";
    const QByteArray cdi3 = R"({
        "Parameters": [
            { "Id": "ParamMouthOpenY", "Name": "嘴　张开和闭合", "GroupId": "" }
        ]
    })";

    Live2DModelInfo info;
    QString error;
    QVERIFY2(info.parse(model3, cdi3, &error), qPrintable(error));

    QCOMPARE(info.mocPath(), QStringLiteral("樱花miku.moc3"));
    QCOMPARE(info.physicsPath(), QStringLiteral("樱花miku.physics3.json"));
    QCOMPARE(info.displayInfoPath(), QStringLiteral("樱花miku.cdi3.json"));
    QCOMPARE(info.texturePaths().size(), 2);
    QCOMPARE(info.texturePaths().first(), QStringLiteral("樱花miku.4096/texture_00.png"));

    // miku 的 LipSync 组是空的（方案里必须显式指定口型参数）
    QVERIFY(info.groupIds(QStringLiteral("LipSync")).isEmpty());
    QCOMPARE(info.groupIds(QStringLiteral("EyeBlink")),
             QStringList({QStringLiteral("ParamEyeROpen"), QStringLiteral("ParamEyeLOpen")}));

    QVERIFY(info.hasParameter(QStringLiteral("ParamMouthOpenY")));
    QCOMPARE(info.parameterDisplayName(QStringLiteral("ParamMouthOpenY")),
             QStringLiteral("嘴　张开和闭合"));
    QCOMPARE(info.parameterCount(), 1);
}

/*缺少 Moc 必须明确失败，不能静默返回一个空模型*/
void TestLive2DModelInfo::parseRejectsMissingMoc()
{
    Live2DModelInfo info;
    QString error;
    QVERIFY(!info.parse(QByteArrayLiteral("{\"Version\":3,\"FileReferences\":{}}"),
                        QByteArray(), &error));
    QVERIFY(!error.isEmpty());

    // 连 JSON 都不是
    Live2DModelInfo broken;
    QString brokenError;
    QVERIFY(!broken.parse(QByteArrayLiteral("not json"), QByteArray(), &brokenError));
    QVERIFY(!brokenError.isEmpty());
}

/*水印参数从表情文件里读出来，不硬编码 Param137*/
void TestLive2DModelInfo::expressionParamIdReadsWatermarkParam()
{
    const QByteArray watermark = R"({
        "Type": "Live2D Expression",
        "Parameters": [ { "Id": "Param137", "Value": 1.0, "Blend": "Add" } ]
    })";
    QCOMPARE(Live2DModelInfo::expressionParamId(watermark), QStringLiteral("Param137"));
    QVERIFY(Live2DModelInfo::expressionParamId(QByteArrayLiteral("{}")).isEmpty());
    QVERIFY(Live2DModelInfo::expressionParamId(QByteArrayLiteral("not json")).isEmpty());
}

/*指纹随字节变化——换模型后参数映射不匹配要能查出来*/
void TestLive2DModelInfo::fingerprintChangesWithBytes()
{
    const QString a = Live2DModelInfo::fingerprint(QByteArrayLiteral("moc3-A"));
    const QString b = Live2DModelInfo::fingerprint(QByteArrayLiteral("moc3-A"));
    const QString c = Live2DModelInfo::fingerprint(QByteArrayLiteral("moc3-B"));
    QCOMPARE(a, b);
    QVERIFY(a != c);
    QVERIFY(a.startsWith(QStringLiteral("sha256:")));
    QCOMPARE(a.size(), 7 + 64);
}

QTEST_MAIN(TestLive2DModelInfo)
#include "test_live2dmodelinfo.moc"
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cmake --build "build2" --config Release --target test_live2dmodelinfo`
Expected: FAIL —— 目标不存在（尚未加进 CMake）。

- [ ] **Step 3: 写头文件**

创建 `Mandarin/utils/Live2DModelInfo.h`：

```cpp
#ifndef LIVE2DMODELINFO_H
#define LIVE2DMODELINFO_H

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

/*Live2D 模型资源清单（纯解析：不依赖 Cubism SDK，也不读文件——便于单元测试）
  设计前提：模型目录/文件名可能含中文或空格，一律由调用方用 QFile 读成字节再交进来；
  绝不把路径交给 Cubism SDK（其内部走 csmString/fopen，Windows 下打不开 UTF-8 中文路径）。*/
class Live2DModelInfo
{
  public:
    /*解析 model3.json；cdi3Json 可为空。失败时写 error 并返回 false。*/
    bool parse(const QByteArray &model3Json, const QByteArray &cdi3Json,
               QString *error = nullptr);

    QString mocPath() const { return m_mocPath; }
    QString physicsPath() const { return m_physicsPath; }
    QString displayInfoPath() const { return m_displayInfoPath; }
    QStringList texturePaths() const { return m_texturePaths; }

    /*model3.json 的 Groups（如 LipSync / EyeBlink）；未声明则返回空表*/
    QStringList groupIds(const QString &groupName) const;

    /*cdi3.json 里的参数 ID → 显示名*/
    bool hasParameter(const QString &id) const { return m_paramNames.contains(id); }
    QString parameterDisplayName(const QString &id) const { return m_paramNames.value(id); }
    int parameterCount() const { return m_paramNames.size(); }

    /*取 .exp3.json 驱动的参数 ID（水印.exp3.json → Param137）。
      用途：水印必须"显式置 0"关闭，而不是靠"不加载该表情"。*/
    static QString expressionParamId(const QByteArray &exp3Json);

    /*对 .moc3 字节算 sha256，供模型指纹校验*/
    static QString fingerprint(const QByteArray &moc3Bytes);

  private:
    QString m_mocPath;
    QString m_physicsPath;
    QString m_displayInfoPath;
    QStringList m_texturePaths;
    QHash<QString, QStringList> m_groups;
    QHash<QString, QString> m_paramNames;
};

#endif // LIVE2DMODELINFO_H
```

- [ ] **Step 4: 写实现**

创建 `Mandarin/utils/Live2DModelInfo.cpp`：

```cpp
#include "Live2DModelInfo.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

bool Live2DModelInfo::parse(const QByteArray &model3Json, const QByteArray &cdi3Json,
                            QString *error)
{
    m_mocPath.clear();
    m_physicsPath.clear();
    m_displayInfoPath.clear();
    m_texturePaths.clear();
    m_groups.clear();
    m_paramNames.clear();

    QJsonParseError parseError{};
    const QJsonDocument modelDoc = QJsonDocument::fromJson(model3Json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !modelDoc.isObject())
    {
        if (error)
            *error = QStringLiteral("model3.json 解析失败：%1").arg(parseError.errorString());
        return false;
    }

    const QJsonObject root = modelDoc.object();
    const QJsonObject refs = root.value(QStringLiteral("FileReferences")).toObject();

    m_mocPath = refs.value(QStringLiteral("Moc")).toString();
    if (m_mocPath.isEmpty())
    {
        if (error)
            *error = QStringLiteral("model3.json 缺少 FileReferences.Moc");
        return false;
    }
    m_physicsPath = refs.value(QStringLiteral("Physics")).toString();
    m_displayInfoPath = refs.value(QStringLiteral("DisplayInfo")).toString();

    const QJsonArray textures = refs.value(QStringLiteral("Textures")).toArray();
    for (const QJsonValue &texture : textures)
        m_texturePaths.append(texture.toString());

    const QJsonArray groups = root.value(QStringLiteral("Groups")).toArray();
    for (const QJsonValue &groupValue : groups)
    {
        const QJsonObject group = groupValue.toObject();
        const QString name = group.value(QStringLiteral("Name")).toString();
        if (name.isEmpty())
            continue;
        QStringList ids;
        const QJsonArray idArray = group.value(QStringLiteral("Ids")).toArray();
        for (const QJsonValue &id : idArray)
            ids.append(id.toString());
        m_groups.insert(name, ids);
    }

    if (!cdi3Json.isEmpty())
    {
        const QJsonDocument cdiDoc = QJsonDocument::fromJson(cdi3Json);
        const QJsonArray params = cdiDoc.object().value(QStringLiteral("Parameters")).toArray();
        for (const QJsonValue &paramValue : params)
        {
            const QJsonObject param = paramValue.toObject();
            const QString id = param.value(QStringLiteral("Id")).toString();
            if (!id.isEmpty())
                m_paramNames.insert(id, param.value(QStringLiteral("Name")).toString());
        }
    }
    return true;
}

QStringList Live2DModelInfo::groupIds(const QString &groupName) const
{
    return m_groups.value(groupName);
}

QString Live2DModelInfo::expressionParamId(const QByteArray &exp3Json)
{
    const QJsonDocument doc = QJsonDocument::fromJson(exp3Json);
    if (!doc.isObject())
        return QString();
    const QJsonArray params = doc.object().value(QStringLiteral("Parameters")).toArray();
    if (params.isEmpty())
        return QString();
    return params.first().toObject().value(QStringLiteral("Id")).toString();
}

QString Live2DModelInfo::fingerprint(const QByteArray &moc3Bytes)
{
    return QStringLiteral("sha256:")
           + QString::fromLatin1(
               QCryptographicHash::hash(moc3Bytes, QCryptographicHash::Sha256).toHex());
}
```

- [ ] **Step 5: 加测试目标**

在 `Mandarin/CMakeLists.txt` 的 `if(BUILD_TESTING)` 段内追加（沿用 `test_chatlogstore` 的产物隔离与 DLL 拷贝做法）：

```cmake
    add_executable(test_live2dmodelinfo
        tests/test_live2dmodelinfo.cpp
        utils/Live2DModelInfo.cpp
    )
    target_include_directories(test_live2dmodelinfo PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}
    )
    target_link_libraries(test_live2dmodelinfo PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Core
    )
    set_target_properties(test_live2dmodelinfo PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    add_custom_command(TARGET test_live2dmodelinfo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt${QT_VERSION_MAJOR}::Test>"
            "$<TARGET_FILE_DIR:test_live2dmodelinfo>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt${QT_VERSION_MAJOR}::Core>"
            "$<TARGET_FILE_DIR:test_live2dmodelinfo>"
    )
    add_test(NAME test_live2dmodelinfo COMMAND test_live2dmodelinfo)
```

- [ ] **Step 6: 重新 configure 并构建**

Run: `cmake -S Mandarin -B build2` 然后 `cmake --build "build2" --config Release --target test_live2dmodelinfo`
Expected: PASS（编译成功）。

- [ ] **Step 7: 运行测试确认通过**

Run: `ctest --test-dir build2 -C Release -R test_live2dmodelinfo --output-on-failure`
Expected: PASS —— 4 个用例全过。

- [ ] **Step 8: Commit**

```bash
git add Mandarin/utils/Live2DModelInfo.h Mandarin/utils/Live2DModelInfo.cpp Mandarin/tests/test_live2dmodelinfo.cpp Mandarin/CMakeLists.txt
git commit -m "feat(live2d): add SDK-free model info parser (paths/groups/params/watermark param/fingerprint)"
```

---

## Task 3: 几何模块 `TachieGeometry`

**Files:**
- Create: `Mandarin/utils/TachieGeometry.h`
- Create: `Mandarin/utils/TachieGeometry.cpp`
- Create: `Mandarin/tests/test_tachiegeometry.cpp`
- Modify: `Mandarin/CMakeLists.txt`
- Modify: `Mandarin/windows/tachie/tachie.cpp`（`SetTachieSize` L430-456 与缩放动画 L368-377 改为调用新模块）

**Interfaces:**
- Consumes: 无
- Produces:
  - `namespace TachieGeometry`
  - `struct CanvasLayout { QSize canvasSize; QPoint imageTopLeft; };`
  - `CanvasLayout canvasForScaledSize(const QSize &scaledSize, double canvasScale = 2.0)`
  - `double clampScaleFactor(double factor)`
  - `QRect centeredRect(const QSize &canvasSize, const QSize &imageSize)`

> **这是 characterization 测试**：断言值全部取自**改造前** `tachie.cpp` 的实际算法（整数除法、`qRound`、`qBound(0.05, …, 2.0)`），目的是把现有行为钉住，保证 PNG 路径重构后逐像素等价。

- [ ] **Step 1: 写失败的测试**

创建 `Mandarin/tests/test_tachiegeometry.cpp`：

```cpp
#include <QtTest>

#include "../utils/TachieGeometry.h"

class TestTachieGeometry : public QObject
{
    Q_OBJECT

  private slots:
    void canvasForScaledSizeMatchesCurrentBehavior();
    void clampScaleFactorBounds();
    void centeredRectRounds();
};

/*200% 画布与居中（值取自改造前 SetTachieSize 的算法）*/
void TestTachieGeometry::canvasForScaledSizeMatchesCurrentBehavior()
{
    const TachieGeometry::CanvasLayout even =
        TachieGeometry::canvasForScaledSize(QSize(300, 500));
    QCOMPARE(even.canvasSize, QSize(600, 1000));
    QCOMPARE(even.imageTopLeft, QPoint(150, 250));

    // 奇数差：整数除法向下取整，与改造前一致
    const TachieGeometry::CanvasLayout odd =
        TachieGeometry::canvasForScaledSize(QSize(301, 501));
    QCOMPARE(odd.canvasSize, QSize(602, 1002));
    QCOMPARE(odd.imageTopLeft, QPoint(150, 250));

    // 退化尺寸不允许出现 0 画布
    const TachieGeometry::CanvasLayout tiny =
        TachieGeometry::canvasForScaledSize(QSize(0, 0));
    QCOMPARE(tiny.canvasSize, QSize(1, 1));

    // 自定义画布倍率
    const TachieGeometry::CanvasLayout custom =
        TachieGeometry::canvasForScaledSize(QSize(100, 100), 1.5);
    QCOMPARE(custom.canvasSize, QSize(150, 150));
    QCOMPARE(custom.imageTopLeft, QPoint(25, 25));
}

/*倍率钳制（改造前是 qBound(0.05, factor, 2.0)）*/
void TestTachieGeometry::clampScaleFactorBounds()
{
    QCOMPARE(TachieGeometry::clampScaleFactor(0.001), 0.05);
    QCOMPARE(TachieGeometry::clampScaleFactor(1.5), 1.5);
    QCOMPARE(TachieGeometry::clampScaleFactor(99.0), 2.0);
}

/*居中矩形的 round 行为（改造前是 qRound(center - size / 2.0)）*/
void TestTachieGeometry::centeredRectRounds()
{
    QCOMPARE(TachieGeometry::centeredRect(QSize(600, 1000), QSize(301, 501)),
             QRect(QPoint(150, 250), QSize(301, 501)));
    QCOMPARE(TachieGeometry::centeredRect(QSize(601, 1001), QSize(300, 500)),
             QRect(QPoint(151, 251), QSize(300, 500)));
}

QTEST_MAIN(TestTachieGeometry)
#include "test_tachiegeometry.moc"
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cmake --build "build2" --config Release --target test_tachiegeometry`
Expected: FAIL —— 目标不存在。

- [ ] **Step 3: 写头文件**

创建 `Mandarin/utils/TachieGeometry.h`：

```cpp
#ifndef TACHIEGEOMETRY_H
#define TACHIEGEOMETRY_H

#include <QPoint>
#include <QRect>
#include <QSize>

/*立绘窗口的纯几何计算。
  从 Tachie 里抽出来：一是便于单元测试，二是 PNG 与 Live2D 两个渲染器共用同一套画布规则。*/
namespace TachieGeometry
{
/*PNG 立绘的 200% 画布：窗口尺寸 = 缩放后图片 × canvasScale，图片在画布内居中*/
struct CanvasLayout
{
    QSize canvasSize;
    QPoint imageTopLeft;
};

CanvasLayout canvasForScaledSize(const QSize &scaledSize, double canvasScale = 2.0);

/*缩放动画倍率钳制（避免异常值把图片瞬间放大或缩到看不见）*/
double clampScaleFactor(double factor);

/*把 imageSize 以 canvasSize 为画布居中摆放（四舍五入到整像素）*/
QRect centeredRect(const QSize &canvasSize, const QSize &imageSize);
} // namespace TachieGeometry

#endif // TACHIEGEOMETRY_H
```

- [ ] **Step 4: 写实现**

创建 `Mandarin/utils/TachieGeometry.cpp`：

```cpp
#include "TachieGeometry.h"

#include <QtGlobal>

#include <algorithm>

namespace TachieGeometry
{
CanvasLayout canvasForScaledSize(const QSize &scaledSize, double canvasScale)
{
    CanvasLayout layout;
    const int canvasWidth = std::max(1, qRound(scaledSize.width() * canvasScale));
    const int canvasHeight = std::max(1, qRound(scaledSize.height() * canvasScale));
    layout.canvasSize = QSize(canvasWidth, canvasHeight);
    // 整数除法：与 Tachie::SetTachieSize 改造前的行为逐位一致
    layout.imageTopLeft = QPoint((canvasWidth - scaledSize.width()) / 2,
                                 (canvasHeight - scaledSize.height()) / 2);
    return layout;
}

double clampScaleFactor(double factor)
{
    return qBound(0.05, factor, 2.0);
}

QRect centeredRect(const QSize &canvasSize, const QSize &imageSize)
{
    const int x = qRound(canvasSize.width() / 2.0 - imageSize.width() / 2.0);
    const int y = qRound(canvasSize.height() / 2.0 - imageSize.height() / 2.0);
    return QRect(QPoint(x, y), imageSize);
}
} // namespace TachieGeometry
```

- [ ] **Step 5: 加测试目标**

在 `Mandarin/CMakeLists.txt` 的 `if(BUILD_TESTING)` 段内追加：

```cmake
    add_executable(test_tachiegeometry
        tests/test_tachiegeometry.cpp
        utils/TachieGeometry.cpp
    )
    target_include_directories(test_tachiegeometry PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}
    )
    target_link_libraries(test_tachiegeometry PRIVATE
        Qt${QT_VERSION_MAJOR}::Test
        Qt${QT_VERSION_MAJOR}::Core
    )
    set_target_properties(test_tachiegeometry PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    add_custom_command(TARGET test_tachiegeometry POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt${QT_VERSION_MAJOR}::Test>"
            "$<TARGET_FILE_DIR:test_tachiegeometry>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt${QT_VERSION_MAJOR}::Core>"
            "$<TARGET_FILE_DIR:test_tachiegeometry>"
    )
    add_test(NAME test_tachiegeometry COMMAND test_tachiegeometry)
```

- [ ] **Step 6: 构建并运行测试**

Run: `cmake -S Mandarin -B build2` 然后 `ctest --test-dir build2 -C Release -R test_tachiegeometry --output-on-failure`
Expected: PASS —— 3 个用例全过。

- [ ] **Step 7: 让 `tachie.cpp` 改用新模块**

在 `Mandarin/windows/tachie/tachie.cpp` 顶部加 `#include "../../utils/TachieGeometry.h"`，然后：

**(a) `SetTachieSize`（约 L430-456）**：把
```cpp
    constexpr double kCanvasScale = 2.0;
    ...
    const int canvasW = qMax(1, qRound(scaledPixmap.width() * kCanvasScale));
    const int canvasH = qMax(1, qRound(scaledPixmap.height() * kCanvasScale));
    const int imgX = (canvasW - scaledPixmap.width()) / 2;
    const int imgY = (canvasH - scaledPixmap.height()) / 2;

    this->resize(canvasW, canvasH);
    ui->label_tachie1->setPixmap(scaledPixmap);
    ui->label_tachie1->setGeometry(imgX, imgY, scaledPixmap.width(),
                                   scaledPixmap.height());

    _scaledImg = scaledPixmap.toImage();
    _scaledImgTopLeft = QPoint(imgX, imgY);
```
改为
```cpp
    const TachieGeometry::CanvasLayout layout =
        TachieGeometry::canvasForScaledSize(scaledPixmap.size());

    this->resize(layout.canvasSize);
    ui->label_tachie1->setPixmap(scaledPixmap);
    ui->label_tachie1->setGeometry(layout.imageTopLeft.x(), layout.imageTopLeft.y(),
                                   scaledPixmap.width(), scaledPixmap.height());

    _scaledImg = scaledPixmap.toImage();
    _scaledImgTopLeft = layout.imageTopLeft;
```
（删除原 `kCanvasScale`、`canvasW/canvasH/imgX/imgY` 五个局部量。）

**(b) 缩放动画帧（约 L365-393）**：把
```cpp
                const double safeFactor = qBound(0.05, factor, 2.0);
                const int w = qMax(
                    1, qRound(scaleSequenceState->baseImageRect.width() * safeFactor));
                const int h = qMax(
                    1, qRound(scaleSequenceState->baseImageRect.height() * safeFactor));

                // 固定窗口，仅在画布中心缩放，保证始终从中心放大/缩小。
                const QPointF center(width() / 2.0, height() / 2.0);
                const int x = qRound(center.x() - w / 2.0);
                const int y = qRound(center.y() - h / 2.0);
```
改为
```cpp
                const double safeFactor = TachieGeometry::clampScaleFactor(factor);
                const int w = qMax(
                    1, qRound(scaleSequenceState->baseImageRect.width() * safeFactor));
                const int h = qMax(
                    1, qRound(scaleSequenceState->baseImageRect.height() * safeFactor));

                // 固定窗口，仅在画布中心缩放，保证始终从中心放大/缩小。
                const QRect frame =
                    TachieGeometry::centeredRect(size(), QSize(w, h));
                const int x = frame.x();
                const int y = frame.y();
```

> `size()` 与改造前的 `width()/height()` 等价（都是本窗口尺寸），行为不变。

- [ ] **Step 8: 构建主程序并确认无回归**

先确认 `Mandarin.exe` 未运行（否则 `LNK1104`），然后：
Run: `cmake --build "build2" --config Release --target Mandarin`
Expected: PASS —— 编译链接成功，无警告新增。

- [ ] **Step 9: 重跑全部测试**

Run: `ctest --test-dir build2 -C Release --output-on-failure`
Expected: PASS —— `test_chatlogstore`、`test_live2dmodelinfo`、`test_tachiegeometry`（以及 SDK 就位时的 `test_cubismcore`）全过。

- [ ] **Step 10: Commit**

```bash
git add Mandarin/utils/TachieGeometry.h Mandarin/utils/TachieGeometry.cpp Mandarin/tests/test_tachiegeometry.cpp Mandarin/windows/tachie/tachie.cpp Mandarin/CMakeLists.txt
git commit -m "refactor(live2d): extract Tachie canvas geometry into tested TachieGeometry module"
```

---

## Task 4: 下沉窗口层到 `CharacterWindowBase`

**Files:**
- Create: `Mandarin/windows/character/characterwindowbase.h`
- Create: `Mandarin/windows/character/characterwindowbase.cpp`
- Modify: `Mandarin/windows/tachie/tachie.h`
- Modify: `Mandarin/windows/tachie/tachie.cpp`
- Modify: `Mandarin/main.cpp:130-149`（窗口创建 + 5 条信号）
- Modify: `Mandarin/windows/setting/setting.h:22`、`Mandarin/windows/setting/setting.cpp:16,66-70`
- Modify: `Mandarin/CMakeLists.txt`（源码清单加两个新文件）

**Interfaces:**
- Consumes: `TachieGeometry`（Task 3）
- Produces:
  - `class CharacterWindowBase : public QWidget`，含
    - 信号 `requestToggleVisible()`、`requestFileDrop(QStringList)`
    - 槽 `ShowInnerThought(QString)`、`HideInnerThought()`、`ResetTachieLoc()`
    - `protected: void updateRenderedImage(const QImage &image, const QPoint &topLeft)`
    - `protected: void ApplyInteractiveRegion(const QRegion &)` / `ApplyInteractiveRegionFromImage()` / `ApplyInteractiveRegionFullWindow()`
    - `protected: void SaveTachieLoc()` / `RestoreTachieLoc()` / `RepositionInnerThoughtBubble()`
    - 纯虚 `void relayoutContent()` / `void reloadContent(const QString &contentName)` / `QSize contentSize() const`
    - 成员 `QImage m_scaledImg`、`QPoint m_scaledImgTopLeft`、`bool m_tachiePosRestoreDone`
  - `class Tachie : public CharacterWindowBase`（PNG 渲染器）

> **已核实可干净下沉**：`tachie.cpp` 里 `ui->` 只出现在构造（L39-43）与渲染缩放（L295、L388-389、L451-452）；本任务要搬的窗口层方法**完全不引用 `ui->`**。

- [ ] **Step 1: 写基类头文件**

创建 `Mandarin/windows/character/characterwindowbase.h`：

```cpp
#ifndef CHARACTERWINDOWBASE_H
#define CHARACTERWINDOWBASE_H

#include <QImage>
#include <QPoint>
#include <QRegion>
#include <QWidget>

class QDragEnterEvent;
class QDropEvent;
class QTimer;

/*立绘窗口的「窗口层」：无边框透明、鼠标穿透与交互区、拖拽、位置持久化、内心气泡、文件拖放。
  渲染层交由子类实现（Tachie = PNG，后续 Live2D = 离屏帧），
  目的是 Live2D 接入时窗口行为零复制、PNG 路径零回归。*/
class CharacterWindowBase : public QWidget
{
    Q_OBJECT

  public:
    explicit CharacterWindowBase(QWidget *parent = nullptr);
    ~CharacterWindowBase() override;

  signals:
    void requestToggleVisible();                //切换对话框显示状态
    void requestFileDrop(QStringList filePaths); //文件拖放到立绘

  public slots:
    void ShowInnerThought(QString text);
    void HideInnerThought();
    void ResetTachieLoc();
    /*立绘大小（百分比）：基类只记住值并触发 relayoutContent()，具体缩放交由子类*/
    void SetTachieSize(int size);
    /*按角色/心情名重载内容（由 Dialog::requestSetCharTachie 驱动，必须是 public 才能 connect）*/
    virtual void reloadContent(const QString &contentName) = 0;

  protected:
    /*子类渲染完成后调用：登记 alpha 图及其窗口内位置，并重算交互区*/
    void updateRenderedImage(const QImage &image, const QPoint &topLeft);

    void ApplyInteractiveRegion(const QRegion &region);
    void ApplyInteractiveRegionFromImage();
    void ApplyInteractiveRegionFullWindow();

    void SaveTachieLoc();
    void RestoreTachieLoc();
    void RepositionInnerThoughtBubble();

    /*子类实现：把当前内容按窗口尺寸重新布局（PNG 缩放贴图 / Live2D 重设画布）*/
    virtual void relayoutContent() = 0;
    /*子类实现：当前内容尺寸（用于气泡定位等）*/
    virtual QSize contentSize() const = 0;

    void contextMenuEvent(QContextMenuEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

    QImage m_scaledImg;              //当前内容的 alpha 图（原 Tachie::_scaledImg）
    QPoint m_scaledImgTopLeft{0, 0}; //内容在窗口内的左上角（原 Tachie::_scaledImgTopLeft）
    bool m_tachiePosRestoreDone = false; //位置恢复完成后才允许自动保存
    int m_tachieSizePercent = 100;       //立绘大小百分比（原 Tachie::SetTachieSize 的入参）

    QWidget *m_innerThoughtBubble = nullptr;
    QTimer *m_innerThoughtTimer = nullptr;
};

#endif // CHARACTERWINDOWBASE_H
```

- [ ] **Step 2: 写基类实现（原样搬迁，不改逻辑）**

创建 `Mandarin/windows/character/characterwindowbase.cpp`，内容为下列方法的**原样搬迁**（把 `Tachie::` 换成 `CharacterWindowBase::`，`_scaledImg` → `m_scaledImg`，`_scaledImgTopLeft` → `m_scaledImgTopLeft`，`_tachiePosRestoreDone` → `m_tachiePosRestoreDone`）：

| 方法 | 源位置（`tachie.cpp`） |
|---|---|
| 构造函数里窗口属性那部分 | L44-58（`WA_TranslucentBackground`、`new DragHelper(this)`、`setAcceptDrops`、`setWindowFlags` 等；**排除 L39-43 的 `ui->setupUi` 与 label 重父化**） |
| `ApplyInteractiveRegion` | L85-122 |
| `ApplyInteractiveRegionFromImage` | L123-132 |
| `ApplyInteractiveRegionFullWindow` | L133-137 |
| `mousePressEvent` | L469-495 |
| `mouseReleaseEvent` | L496-511 |
| `dragEnterEvent` | L512-517 |
| `dropEvent` | L518-533 |
| `ResetTachieLoc` | L534-540 |
| `SaveTachieLoc` | L541-551 |
| `RestoreTachieLoc` | L552-577 |
| `ShowInnerThought` | L578-635 |
| `RepositionInnerThoughtBubble` | L636-672 |
| `HideInnerThought` | L673-696 |
| 析构 | 原 `~Tachie()` 中与气泡/定时器/动画无关的清理（气泡 `deleteLater`） |

新增实现：

```cpp
#include "characterwindowbase.h"

#include "../../utils/DragHelper.h"

#include <QDropEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QTimer>

CharacterWindowBase::CharacterWindowBase(QWidget *parent)
    : QWidget(parent)
{
    /*窗口属性部分从原 Tachie 构造函数原样搬迁*/
}

CharacterWindowBase::~CharacterWindowBase()
{
    /*原 ~Tachie() 中与内心气泡/定时器相关的清理原样搬迁*/
}

void CharacterWindowBase::updateRenderedImage(const QImage &image, const QPoint &topLeft)
{
    m_scaledImg = image;
    m_scaledImgTopLeft = topLeft;
    RepositionInnerThoughtBubble();
}

void CharacterWindowBase::SetTachieSize(int size)
{
    // 原 Tachie::SetTachieSize 的开头（含 "NowTachie 为空则直接返回" 的守卫）留在子类
    // relayoutContent() 里，因为"有没有内容"只有子类知道。
    m_tachieSizePercent = (size <= 0) ? 100 : size;
    qInfo() << "设置立绘大小为" << m_tachieSizePercent;
    relayoutContent();
}
```

> `mousePressEvent` 里的 alpha 命中判定（原 L472-480）**必须原样保留**：它读的就是 `m_scaledImg`，这是"透明处穿透、模型处可点"的唯一实现。

- [ ] **Step 3: 让 `Tachie` 继承基类**

修改 `Mandarin/windows/tachie/tachie.h`：

```cpp
#ifndef TACHIE_H
#define TACHIE_H

#include "../character/characterwindowbase.h"

#include "../../utils/AnimePluginManager.h"

#include <QPixmap>
#include <QHash>

namespace Ui
{
class Tachie;
}

/*PNG 立绘渲染器：窗口层（穿透/拖拽/位置/气泡/拖放）全部继承自 CharacterWindowBase*/
class Tachie : public CharacterWindowBase
{
    Q_OBJECT

  public:
    explicit Tachie(QWidget *parent = nullptr);
    ~Tachie() override;

  public slots:
    void SetTachieImg(QString TachieName = "default");
    void reloadContent(const QString &contentName) override; //原 SetTachieImg

  protected:
    void relayoutContent() override;   //原 SetTachieSize 的布局部分（用基类的 m_tachieSizePercent）
    QSize contentSize() const override; //当前贴图尺寸

  private:
    Ui::Tachie *ui;
    QPixmap NowTachie;
    AnimePluginManager m_animePluginManager;
    QHash<QString, QPixmap> m_pixmapCache;
    QHash<QString, qint64> m_pixmapCacheStamp;
    QPixmap loadTachiePixmapCached(const QString &filePath);
    QSequentialAnimationGroup *m_activeAnimationGroup = nullptr;
    void TryPlayAnimationForAction(const QString &actionName);
};

#endif // TACHIE_H
```

> 从 `tachie.h` **删除**（已上移基类）：`requestToggleVisible`/`requestFileDrop` 两个信号、`ResetTachieLoc`/`ShowInnerThought`/`HideInnerThought` 三个槽、`_scaledImg`/`_scaledImgTopLeft`/`_tachiePosRestoreDone` 三个成员、`SaveTachieLoc`/`RestoreTachieLoc`/`RepositionInnerThoughtBubble`/`ApplyInteractiveRegion*` 五个方法、以及 `contextMenuEvent`/`mousePressEvent`/`mouseReleaseEvent`/`dragEnterEvent`/`dropEvent` 五个事件覆写。

- [ ] **Step 4: 收敛 `tachie.cpp`**

- 删除已搬到基类的那些方法定义；
- `Tachie::Tachie` 只保留 `ui->setupUi(this)`、label 重父化（原 L39-43），基类构造已处理窗口属性；
- `SetTachieImg` 改名为 `reloadContent(const QString &contentName)`（内部 `TachieName` 用 `contentName`），并在末尾调 `updateRenderedImage(m_scaledImg, m_scaledImgTopLeft)` 之后**不再需要**自己调 `ApplyInteractiveRegion*`（`updateRenderedImage` 内部会做）；
- `SetTachieSize(int size)` **上移到基类**（Live2D 渲染器同样要支持"立绘大小"），基类只记 `m_tachieSizePercent` 并调 `relayoutContent()`；原实现里"`NowTachie` 为空则直接返回"的前置守卫、以及 `constexpr double kCanvasScale` 与"预留 200% 画布"的注释都搬进 `Tachie::relayoutContent()` 开头；
- `reloadContent(const QString &contentName)` 由 `SetTachieImg` 改名而来（内部变量名同步），并在末尾改调 `updateRenderedImage(...)`（内部会重算交互区）；
- 原 L458-463 的平台分支（Linux/macOS 调 `ApplyInteractiveRegionFromImage()`，Windows 调 `clearMask()`）搬进 `relayoutContent()` 末尾，行为不变。

- [ ] **Step 5: 改 `main.cpp`**

`Mandarin/main.cpp` 的 L130-149 改为：

```cpp
    /*窗口创建*/
    Dialog dialogWin;
    dialogWin.show();
    CharacterWindowBase *characterWin = new Tachie();
    characterWin->show();
    MainWindow *settings = nullptr;

    /*一些绑定*/
    //对话框的开启和关闭
    QObject::connect(characterWin, &CharacterWindowBase::requestToggleVisible, &dialogWin,
                     &Dialog::ToggleVisible);
    //文件拖放到立绘
    QObject::connect(characterWin, &CharacterWindowBase::requestFileDrop, &dialogWin,
                     &Dialog::handleFileDrop);
    //修改立绘图片
    QObject::connect(&dialogWin, &Dialog::requestSetCharTachie, characterWin,
                     &CharacterWindowBase::reloadContent);
    QObject::connect(&dialogWin, &Dialog::requestShowInnerThought, characterWin,
                     &CharacterWindowBase::ShowInnerThought);
    QObject::connect(&dialogWin, &Dialog::requestHideInnerThought, characterWin,
                     &CharacterWindowBase::HideInnerThought);
```

> `reloadContent` 在基类里已声明为 **public 纯虚槽**（见 Task 4 Step 1），所以这里能直接 `connect` 到 `CharacterWindowBase::reloadContent`；`relayoutContent` / `contentSize` 仍为 `protected`，只由基类内部调用，不需要外部可见。
>
> 同时在 `main.cpp` 头部把 `#include "windows/tachie/tachie.h"` 之后补 `#include "windows/character/characterwindowbase.h"`。
>
> 生命周期：原先是栈对象。改为堆对象后，用 `characterWin` 作为 `MainWindow` 的父对象管理，或在 `main` 返回前 `delete characterWin;`（`MainWindow` 由 `settings` 指针持有且未删除，保持现状）。二选一，**不要引入新的泄漏**。

- [ ] **Step 6: 改 `setting.{h,cpp}`**

`Mandarin/windows/setting/setting.h`：
- `#include "../tachie/tachie.h"` → `#include "../character/characterwindowbase.h"`
- L22 构造声明改为
```cpp
    MainWindow(Dialog *dialog, CharacterWindowBase *characterWin, QWidget *parent = nullptr);
```

`Mandarin/windows/setting/setting.cpp`：
- L16 定义形参同步改名；
- L66-70 三条连接的目标类型改为 `CharacterWindowBase`：
```cpp
    // ⚠️ 第一个信号的真实名字是 requestReloadCharSelect，不是 requestSetCharTachie
    //    （后者是 Dialog 的信号名，容易抄错）。
    connect(settingchild_charWin, &SettingChild_Char::requestReloadCharSelect,
            characterWin, &CharacterWindowBase::reloadContent); //设置立绘图像（重载角色）
    connect(settingchild_charWin, &SettingChild_Char::requestSetTachieSize,
            characterWin, &CharacterWindowBase::SetTachieSize); //设置立绘大小
    connect(settingchild_charWin, &SettingChild_Char::requestResetTachieLoc,
            characterWin, &CharacterWindowBase::ResetTachieLoc); //重置立绘位置
```
> `SetTachieSize` 已在 Task 4 Step 1 上移为 `CharacterWindowBase` 的公开槽，这里连的就是基类版本；`CharacterWindowBase` 是不完整类型之外的真实基类，`connect` 到基类槽对 `Tachie` 实例同样生效。

- [x] **Task 4 执行记录（2026-09-28 已完成并验证）**

实际执行与计划的差异，均为**计划本身的缺口**而非设计变更：

| # | 差异 | 处理 |
|---|---|---|
| 1 | 计划 Step 6 写的信号名 `requestSetCharTachie` **不存在**于 `SettingChild_Char` | 用真实名 `requestReloadCharSelect`（上面代码块已更正） |
| 2 | 计划 `tachie.h` 骨架漏了 `class QSequentialAnimationGroup;` 前向声明，但保留了 `m_activeAnimationGroup` 指针 | 补上前向声明（否则编不过） |
| 3 | 计划基类头漏了 `#include <QStringList>`，而 `requestFileDrop(QStringList)` 需要它（原先靠 `AnimePluginManager.h` 间接引入） | 补上 include |
| 4 | **Task 3 只做了「模块 + 测试」，漏了 Step 7 的接线**：`utils/TachieGeometry.cpp` 只加进了**测试**目标、没加进 `Mandarin` 目标，`tachie.cpp` 也仍在用内联算法 | 已补：把 `utils/TachieGeometry.h/.cpp` 加入 `Mandarin` 目标源码表，并把 `kCanvasScale` 内联数学换成 `TachieGeometry::canvasForScaledSize`；缩放动画帧改用 `clampScaleFactor`/`centeredRect` |

> **教训（写给下一个执行者）**：新增的 `.cpp` **必须同时加进主程序目标**，只加进测试目标的话，测试全绿而主程序在链接期才报 `LNK2019`。
> 另外，搬迁类重构完成后应做一次**方法体字节级比对**（把 `git show HEAD:<file>` 里的方法体与搬过去后的内容、仅套用改名表后逐字节比较），这比人工读 diff 可靠得多 —— 本次 13/13 方法体确认逐字节一致。

- [ ] **Step 7: 把新文件加进主程序源码清单**

在 `Mandarin/CMakeLists.txt` 的 `Mandarin` 目标源码列表中，`windows/tachie/tachie.cpp` 附近加入：

```cmake
    windows/character/characterwindowbase.cpp
    windows/character/characterwindowbase.h
```

- [ ] **Step 8: 构建**

先确认 `Mandarin.exe` 未运行。
Run: `cmake -S Mandarin -B build2` 然后 `cmake --build "build2" --config Release --target Mandarin`
Expected: PASS —— 编译链接成功。若出现 "no member named `_scaledImg`" 之类，说明搬迁时漏改成员名，按改名表修正。

- [ ] **Step 9: 重跑全部测试**

Run: `ctest --test-dir build2 -C Release --output-on-failure`
Expected: PASS —— 既有测试全过（本任务是纯搬迁，不应有任何测试变化）。

- [ ] **Step 10: Commit**

```bash
git add Mandarin/windows/character/ Mandarin/windows/tachie/ Mandarin/windows/setting/ Mandarin/main.cpp Mandarin/CMakeLists.txt
git commit -m "refactor(live2d): sink window layer into CharacterWindowBase, Tachie becomes PNG renderer"
```

---

## Task 5: PNG 路径回归验收

**Files:** 无（只做验证与文档）

**Interfaces:**
- Consumes: Task 1–4 的产物
- Produces: 一份回归结论（写进 `Mandarin/docs/Live2D方案.md` 的验收清单下方）

> **本任务不启动桌宠**。下列人工项由使用者自行启动验证（AGENTS.md 规则：AI 不自动启动）。

- [ ] **Step 1: 自动化部分（AI 执行）**

Run: `cmake --build "build2" --config Release --target Mandarin` 然后 `ctest --test-dir build2 -C Release --output-on-failure`
Expected: 主程序构建成功；全部测试 PASS。

- [ ] **Step 2: 产物卫生检查（AI 执行）**

Run:
```powershell
git status --short
Get-ChildItem "build2\tests\Release" -ErrorAction SilentlyContinue | Select-Object Name
```
Expected: 工作区只有本计划的预期改动；测试产物在 `build2/tests/Release/`，**没有落进 `build2/Release/`**（否则会被打进便携包）。

- [ ] **Step 3: 人工回归清单（交给使用者）**

需要使用者自己 `启动.bat` 后逐项确认，把结果回填到方案文档：

1. 立绘正常显示，位置与改造前一致（同一角色应恢复到同一坐标）；
2. 立绘**透明处鼠标穿透**、模型处可点击；
3. 拖动立绘可移动，松手后位置被记住，重启后恢复；
4. 托盘/右键菜单能切换对话框显示；
5. 把图片文件拖到立绘上能触发对话框的拖放处理；
6. 内心气泡（思考中）能出现在头顶，结束后消失；
7. 设置页里「设置立绘大小」「重置立绘位置」「重载角色」三个按钮均生效；
8. 缩放动画（AnimePlugin 的 scale 步骤）从中心缩放、不抖动。

- [ ] **Step 4: 把回归结论写回方案文档**

在 `Mandarin/docs/Live2D方案.md` 的「验收清单」章节后追加：

```markdown
### 阶段 0–1 回归结论（填写日期：____）

- [ ] 上述 8 项人工回归全部通过（日期：____，执行人：____）
- [ ] `ctest --test-dir build2 -C Release` 全绿
- [ ] `build2/Release/` 内无测试产物
- [ ] `git status` 中无 SDK / 模型文件
```

- [ ] **Step 5: Commit**

```bash
git add Mandarin/docs/Live2D方案.md
git commit -m "docs(live2d): record stage 0-1 regression checklist and results"
```

---

## 后续计划（不在本计划范围）

**阶段 2–10 需等 SDK 到手后再写独立计划**，原因：`CubismNativeFramework` 的渲染器 API（`CubismRenderer_OpenGLES2`、`CubismUserModel`、`CubismModelMatrix` 等）签名必须以**实际解压出的头文件**为准，凭记忆写会产出错误代码。阶段 2 的第一件事应是"通读 SDK 头文件并记录真实入口签名"，再据此写任务。

建议拆分：
- **计划 B（阶段 2）**：SDK 渲染管线 —— 离屏 FBO + 读回 RGBA `QImage` + `Live2DCharacterWindow` 子类 + 接 `updateRenderedImage()`。
  **已实测确定的前置与路线（Task 1 执行时发现，务必照此写）**：
  1. ~~必须先补 GLEW~~ **已完成（2026-09-28）**：`CubismRenderer_OpenGLES2.hpp` 在 `CSM_TARGET_WIN_GL` 下
     `#include <GL/glew.h>`，SDK 不含 GLEW。已从官方发布包取 **GLEW 2.3.1 的静态库**，落地为：
     ```
     Mandarin/3rdparty/glew/
     ├── include/GL/{glew.h,wglew.h,glxew.h,eglew.h}
     ├── lib/x64/glew32s.lib      # 静态库，不必随包分发 glew32.dll
     └── LICENSE.txt
     ```
     取包内 `lib/Release/x64/glew32s.lib`；**不要**取 `glew32.lib`（那是 DLL 的导入库）。
     注意官方 win32 包只有 Release 版静态库，Debug 配置会链接 Release CRT 版 —— 实测可用（GLEW 内部只做少量分配）。
  2. **Framework 库用官方构建脚本，不要 glob**（**已完成并实测通过**，下面就是可用的接线）：
     ```cmake
     # GLEW 静态库（imported target）
     add_library(glew_static STATIC IMPORTED)
     set_target_properties(glew_static PROPERTIES
         IMPORTED_LOCATION "${GLEW_ROOT}/lib/x64/glew32s.lib"
         INTERFACE_INCLUDE_DIRECTORIES "${GLEW_ROOT}/include"
         INTERFACE_COMPILE_DEFINITIONS "GLEW_STATIC")

     set(FRAMEWORK_SOURCE OpenGL)                                # 选后端
     set(FRAMEWORK_DEFINITIOINS CSM_TARGET_WIN_GL GLEW_STATIC)   # 上游变量名就是这个拼写，勿"修正"
     set(RENDER_INCLUDE_PATH "${GLEW_ROOT}/include")
     add_subdirectory("${CUBISM_SDK_DIR}/Framework"
                      "${CMAKE_CURRENT_BINARY_DIR}/cubism_framework")   # 产出目标名 = Framework (STATIC)

     # 框架把上面两个定义设成 PRIVATE，而我们要直接 include 它的 GL 渲染头，所以自己再补一次；
     # Core 的头与库框架同样只 PRIVATE 引了，必须显式 PUBLIC 传递给消费方，否则报
     # "Core: 不是类或命名空间名称" 等一串错。
     target_compile_definitions(Framework PUBLIC CSM_TARGET_WIN_GL)
     target_include_directories(Framework PUBLIC "${CUBISM_SDK_DIR}/Core/include")
     target_link_libraries(Framework PUBLIC ${CUBISM_CORE_LIB} glew_static opengl32)
     ```
     实测产物 `build2/cubism_framework/Release/Framework.lib`（2535 KB），
     验证目标 `tests/test_cubismframework.cpp` 通过：它同时 include 了 GL 渲染头与 `CubismMath.hpp`，
     可钉住"只编 OpenGL 后端 / GLEW 路径对 / `CSM_TARGET_WIN_GL` 生效"三件事。

     > ⚠️ 写测试/注释时注意：**别在块注释里出现 `*/`**。像 `Framework/src/**/*.cpp` 里的 `**/`
     > 会提前闭合注释（`C4138`），后续内容被打成代码，还会连带在 SDK 头里报出一堆
     > 与真实原因无关的错（如 `Core: 不是类或命名空间名称`），极难排查。
  3. 渲染器类名是 **`CubismRenderer_OpenGLES2`**（桌面 GL 也走这套 ES2 风格接口），配套离屏类是
     `CubismOffscreenManager_OpenGLES2` / `CubismOffscreenRenderTarget_OpenGLES2` —— 正对应方案里的离屏管线。

**阶段 2 已完成「离屏渲染一帧」的实证，以下 6 条是实测踩出来的，务必照抄**（实现见
`Mandarin/utils/Live2DCubismRuntime.{h,cpp}` / `Live2DOffscreenRenderer.{h,cpp}`，
验证见 `Mandarin/tests/test_live2doffscreen.cpp`；产物 `build2/tests/live2d-probe/*.png`）：

4. **必须把框架着色器拷到可执行文件旁**。桌面 GL 下框架从 **CWD 相对路径**读着色器源码
   （`"FrameworkShaders/VertShaderSrc.vert"` 等，`CubismShader_OpenGLES2.cpp`），
   而 SDK 里**没有**现成的 `FrameworkShaders/` 目录 —— 源在
   `Framework/src/Rendering/OpenGL/Shaders/Standard`。不拷过去的现象是**什么都不画**，
   日志里只有 `[CSM][E]File loader is not set.` 这类误导信息。资源读取钩子还要**带回退解析**
   （CWD 找不到就试可执行文件目录），因为 ctest 的 CWD 与 exe 目录不同。

5. **`Option` 必须有进程级生命周期**。`CubismFramework::StartUp` 只存 `option` 的**指针**，不拷贝。
   把 `Option` 放在函数栈上 → 悬空 → 现象同样是 `File loader is not set.`，且**不崩溃**，极难排查。
   官方样例把它做成 `LAppAppDelegate` 的成员就是这个原因。

6. **贴图必须 `glGenerateMipmap`**。框架每次绘制会把 `GL_TEXTURE_MIN_FILTER` 设成
   `GL_LINEAR_MIPMAP_LINEAR`；贴图没有 mipmap 就是"不完整纹理"，GL 规定采样返回 `(0,0,0,1)`，
   表现为 **形状完全正确、纯黑的剪影**（因为 Live2D 的 drawable 是贴合美术的多边形网格）。
   官方 `LAppTextureManager` 同样在 `glTexImage2D` 后调了 `glGenerateMipmap`。

7. **`QSurfaceFormat` 请求 2.0，不要请求 3.0**。实测请求 `3.0` 时驱动给出精确的 3.0 上下文，
   Cubism 绘制阶段会返回 `GL_INVALID_OPERATION` 且**整帧什么都没画**（全透明）；
   请求 `2.0` 反而拿到 **4.6.0 兼容上下文**，一切正常。

8. **读回用 `glReadPixels`，不要用 `glGetTexImage`**。后者在本机 NVIDIA 驱动上**直接崩溃**
   （`0xC0000005`，栈里是 `DrvPresentBuffers`）。用 `BeginDraw()` 绑回 FBO →
   `glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,...)` → `EndDraw()`，再**垂直镜像**（GL 原点左下）。

9. **参数覆盖要夹在 `LoadParameters()` 之后、`SaveParameters()` 之前**。顺序是
   `LoadParameters → 施加覆盖 → physics->Evaluate → SaveParameters → model->Update()`；
   放在外面第一帧就会被还原冲掉。另外 `_renderer` 在 `CubismUserModel` 里是 **private**，
   只能经 `GetRenderer<T>()` 取。

10. **测试断言不要用"两帧像素差异"证明参数生效**：帧间物理动画（头发/呼吸）本来就会变，
    像素差异无法区分"参数生效"和"动画推进"。要断言就**回读参数值**（`GetParameterValue`），
    视觉正确性另行**导出 PNG 目视确认** —— 阶段 2 的水印极性错误就是靠看图才发现的。
- **计划 C（阶段 3–7）**：`parameter-map.json` 生成（用 Task 2 的 `Live2DModelInfo` + 模型自带 `vtube.json`）、指纹校验、表情/动作外部加载、水印显式关闭、`PresentationController` 状态机与口型三要素。
- **计划 D（阶段 8–10）**：点击交互、模型导入 UI、打包合规检查。

---

## Self-Review

**1. Spec coverage（对照 `Live2D方案.md`）**

| 方案条目 | 本计划覆盖 |
|---|---|
| 阶段 1 抽 `CharacterWindowBase` | Task 4 ✓（含方案遗漏的 `main.cpp` / `setting.*` 影响面） |
| 阶段 0 SDK 集成前置 | Task 1 ✓（含 "Core 不在开源仓库、需人工同意 EULA"） |
| 模型指纹校验（优化点 7） | Task 2 的 `fingerprint()` ✓（校验时机与拒绝逻辑属阶段 3） |
| 参数映射集中化（优化点 5） | 部分：Task 2 解析出参数 ID↔显示名；**生成 `parameter-map.json` 属阶段 3**（计划 C） |
| 水印默认关闭 | Task 2 的 `expressionParamId()` 提供"显式置 0"所需的参数 ID；**实际置 0 属阶段 2** |
| 中文路径风险 | Task 2 的"字节入参、路径不进 SDK"设计 ✓ |
| 模型禁二传/禁二改、SDK 不入库 | Global Constraints + Task 1 Step 8 + Task 5 Step 2 ✓ |
| 阶段 2–10 | 明确划出，见「后续计划」 |

**2. Placeholder scan**：计划内每个代码步骤都给了可编译的完整代码或精确的"原样搬迁 + 行号 + 改名表"。两处显式声明的未知量（Core 库文件名、Cubism API 签名）都以"Step 必须实测/以头文件为准"的方式给出可执行动作，而不是 TODO。

**3. Type consistency**：`m_scaledImg` / `m_scaledImgTopLeft` / `m_tachiePosRestoreDone` 在基类定义、Task 4 Step 4 改名表、以及 `updateRenderedImage()` 三处命名一致；`TachieGeometry::CanvasLayout` 的成员名 `canvasSize` / `imageTopLeft` 在 Task 3 的头文件、实现、测试、以及 Task 4 Step 4 的调用点一致；`Live2DModelInfo` 的 9 个方法在头文件、实现、测试三处一致。
