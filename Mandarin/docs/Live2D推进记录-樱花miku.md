# Live2D 推进记录：樱花miku

日期：2026-09-30。接手基线为 `Live2D交接.md`，本轮完成其 §4.1 的独立预设，并修复 §4.3 的四处测试信息格式问题。

## 实现与范围

- 新增 `assets/live2d-presets/樱花miku/parameter-map.json`、`presets/moods.json`、`presets/idle.json`。
- 映射有 31 个语义参数，范围/默认值来自本次 `dumpsDeclaredParameterTable` 实际加载 moc 后导出的 141 条声明，逐项核对。共有轴的 cdi3 显示名也与原映射语义一致。
- 14 个情绪原型、26 个心情别名、5 条待机摆动轴。别名沿用亚托莉角色词表；参数表属于樱花模型。
- 共享面部/全身轴数值经樱花模型独立出图复核后保留；删除原型中未采用的 `bodyZ`。没有照搬 miku 的道具表：樱花 `Param133` 是「哭」，`Param89/90` 也不是 miku 的 `Param134/135`。
- 演出开关保留给 B 阶段，情绪不触发哭道具、QQ人或换形态。换装/道具的触发方式仍待用户决定。
- `Live2DMoodPreset::load()` 增加可选的显式目录参数，供离线校准与测试读取仓库候选；默认应用路径不变。测试无需先写 Documents。
- 新增 `validatesSakuraRepositoryPresets`：检查候选装载、非空数据、原型范围、驱动器排除、演出开关隔离、角色别名一致性、moc 声明一致性，并导出表情/摆动图。本地模型缺失时只跳过 moc 和画图部分。
- 修复 `test_live2doffscreen.cpp` 三处、`test_live2dwindow.cpp` 一处 `QString::arg` 混用 printf 占位符。

## 源码核对更正

交接文档将 moods 原型称为「相对 neutral 的增量」，与实际代码不一致：
`parametersForArchetype()` 先填 neutral，再用原型的**绝对目标值**覆盖；没有相加。
本轮更正交接文档及加载器注释，保持渲染行为。旧文档入口也标明最新记录位置。

## 目视复核

测试产物位于 `build2/tests/live2d-probe/sakura-review/`：

- `moods.png`：14 个原型的全身对照。
- `faces-2x.png`：同一裁切区的最近邻两倍脸部放大。
- `idle.png`：6 张全身相位帧。

已实际查看这些图：睡觉近乎闭眼、惊讶张嘴、尴尬腮红、悲伤半闭眼均可辨认。
眉毛变化含蓄，高兴与中立的区分也较轻；后续可以按用户观感细调。
全身图可见小幅胯部/上身摆动与头发物理，人物完整，未见裁切或水印。
这套 rig 没有屈膝迈步；不能把全身轴的摆动称作腿部关节动作。
物理持有的轴以物理输出为基准，情绪中的身体目标不保证最终保留。

## 验证证据

- Release 主程序与两套 Live2D 测试目标构建成功。沙箱内 MSBuild 在调用 CL 前遇到重复 `Path/PATH`，经用户批准在沙箱外构建成功。
- 全量 `ctest --test-dir build2 -C Release --output-on-failure`：10/10 通过，总耗时 371.59 秒；离屏目标 149.08 秒，窗口目标 221.80 秒。此轮全量运行时 Documents 尚只有 atri/miku 预设，新增仓库候选由新用例直接覆盖；安装后的检查另列下方。
- 新用例独立运行：31 条映射、137 个原型条目、14 个原型通过，3 passed / 0 failed / 0 skipped。
- RED：临时移走樱花 `parameter-map.json`，复现交接时未提供预设的状态，新用例在装载断言失败；恢复后通过。日志 `build2/sakura-red-absent.log`。
- 数据变异：将映射改成 `{}`，装载断言失败；将 `eyeLOpen.neutral` 从 1 改为 0，范围断言失败，数字为 `declared=0/1/1 map=0/0/1`。两次均用 finally 恢复原始字节。日志 `build2/sakura-red-missing.log`、`build2/sakura-red-range.log`。
- 本轮没有修改渲染器驱动逻辑，也没有新增需要通过生产代码变异验证的驱动器防御行为；以上变异验证的是预设数据断言。
- 真实 `config.ini` 的 SHA256 基线为 `6B4A6BD2032B6307E609D5B682288CAF3080DB9383AFA3A972035F53B26AD7AF`，全量测试后相同。

## 运行时安装

经用户批准安装三份数据到 `Documents/Mandarin/Character/Assets/亚托莉/Live2D/樱花miku/`，与仓库副本逐文件 SHA256 一致。
没有修改模型本体、配置选项，没有启动桌宠。当前选中的模型仍由用户原配置决定。
安装后补跑窗口范围对账、三模型情绪校准、全链路帧成本：5 passed / 0 failed / 0 skipped，68.157 秒。
再跑三模型摆动规则、全身相位帧、新仓库候选测试：5 passed / 0 failed / 0 skipped，35.449 秒。
运行时三模型校准总表 `_mood-sheet-atri-miku-樱花miku.png` 已查看；本轮未再次重复整套 ctest。

`reportsFullPipelineFrameCost` 的本次测量（60 帧平均，当前机器，非实时持续帧率保证）：

| 模型 | 档位 | ms/帧 |
|---|---|---:|
| miku（用户当前档位） | 120fps / 1.5× | 6.17 |
| miku | 120fps / 1.0× | 4.42 |
| atri | 120fps / 1.5× | 4.13 |
| atri | 120fps / 1.0× | 2.42 |

日志：`build2/sakura-runtime-window.log`、`build2/sakura-runtime-offscreen.log`。
安装后测试结束再次检查真实 config.ini，哈希仍与上面的基线一致。
三份最终文件与运行时副本一致：

- parameter-map.json：`C3443370D05E2715DF1BAB37ECDF22DC95B9E0934A15DC2AA0CDC7A8E6F36C2F`
- presets/moods.json：`A0EF93AB634A6565D04EF1B03D06E5A467FD028B1456F556392B4AE41DB6245C`
- presets/idle.json：`154E20FABAB7B46AC75EC43CA1320145381E10CB6B5068559AA9DFA0EE6EC12D`

## 后续

B 阶段先决定道具/换装的触发入口，再实施互斥组及形态开关。
渲染线程与 Dialog 音频胶水值类型仍是独立后续任务，本轮未展开。

## 用户后续指定：切换 miku 角色

2026-09-30，用户要求将角色替换成 miku，语音沿用亚托莉。
已创建独立 `Character/Assets/miku` 和 `Character/UserConfig/miku`，设置 miku 角色提示词及 miku Live2D 预设；`CharSelect=miku`，`renderMode=live2d`、`live2dModel=miku`、120fps/1.5× 保持不变。
语音为 `vitsEnable=true`、`vitsMasSelect=VITS - 4 - ATRI`，与亚托莉配置相同；未复制亚托莉聊天记录或记忆。
Tachie 下保留26个心情文件名供当前提示词代码枚举；PNG降级图使用本地 miku 中立帧，表情由 Live2D 预设驱动。PNG 不入库、不分发。
原 config.ini 备份在 `build2/character-switch-miku/config.ini.before-miku`。
专项验证：角色心情词表、当前模型范围、表情切换均通过；验证前后新 config.ini 哈希不变。没有启动桌宠。

### 后续切回亚托莉

2026-09-30 用户要求换回亚托莉，并指定后续主要维护该角色。已设置 CharSelect=亚托莉、live2dModel=atri，保持 Live2D 模式、120fps/1.5× 和 ATRI 语音；miku 目录保留为备用。切换前配置备份在 build2/character-switch-atri/config.ini.before-atri。
