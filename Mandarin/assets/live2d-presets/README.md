# Live2D 表现预设（仓库内版本化副本）

这些是**我们自己的映射数据**（只含语义名、参数 ID、范围与预设数值），**不含模型本身**——
模型受作者授权约束（禁二传/禁二改），永远不入库，见 `.gitignore` 与 `docs/Live2D方案.md`。

## 运行时读取位置（唯一真源）

```
Documents/Mandarin/Character/Assets/<角色>/Live2D/<模型>/
├── parameter-map.json          语义名 → {参数 ID, min, max, neutral}
└── presets/
    ├── moods.json              情绪原型 + 「立绘心情名 → 原型」别名表
    └── idle.json               待机摆动：每条轴的幅度/周期/相位
```

**本目录只是版本化备份**：应用**仍然只从上面那个路径读**，所以这里的文件改了不会生效。
它的用途是「防止丢失」——这几份数据是靠**逐张看图校准**出来的，重建成本很高。

## 为什么两份、以及漂移风险

因为运行时路径在 `Documents/`（用户数据区），所以本目录与运行时副本**可能漂移**。
目前的约定是：**校准改运行时那份 → 提交时同步回本目录**（本次同步已逐一校验哈希一致）。

若希望**仓库成为唯一真源**（彻底消除漂移），需要一处小改动：让加载器在运行时路径找不到时
**回退到本目录**，于是 `Documents/` 那份变成「用户覆盖」而本目录成为默认值。
这是一个独立的小改动，尚未实施——需要时再定。

## 每个模型的语义名必须与它自己的 moc 对账

`parameter-map.json` 的 `min`/`max`/`neutral` **只能来自模型自己声明的值**
（`Live2DOffscreenRenderer::declaredParameterRanges()`，测试 `parameterMapRangesMatchModelDeclarations`
会遍历所有带映射表的模型逐项核对）。**不要从 `*.vtube.json` / `*.cdi3.json` 手抄**：
那两个是作者/VTS 的配置，不是 moc 的能力边界——atri 的映射表最初就是这么错的（27 条里错 4 条：
`mouthForm` 的 max 与 neutral、`eyeLOpen/ROpen` 的 max、`mouthOpen` 的 max）。

## 当前各模型的要点

| | atri | miku |
|---|---|---|
| 参数数 | 27 语义 | 31 语义（含 `wholeBodyX/Y`、`upperBodyZ`、`lowerBodyZ`） |
| `mouthForm` | 声明 `[-1, 0]`、默认 `-0.5` → **嘴笑不出来**，情绪靠 `eye*Smile` + `cheek` | 声明 `[-1, +1]`、默认 `0` → **嘴能上扬** |
| 下半身 | **无**腿部部件/参数，只能整体旋转 | `z下身`(Param13) 是**胯部旋转**（非屈伸，无膝/踝参数） |
| 死绑定 | — | `Param21` / `Param27` / `ParamBodyAngleX/Y/Z` 写进去零像素变化 |
| 校准状态 | ✅ 已逐张目视校准 | ✅ 已目视判读（数值随 miku 的 `[-1,+1]` 嘴形而调整） |

`樱花miku` 与 miku 同一套绑定（声明值、可见顶点边界完全一致），但**行为未验证**：
它的表情参数名与 miku 不同（`Param133` 在它是「哭」，在 miku 是「大葱」），
且验证被一个既有渲染器缺陷挡住（同一进程里第二个渲染器渲染空白）。
