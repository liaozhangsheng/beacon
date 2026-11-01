---
name: beacon-custom-template
description: 帮 Beacon 用户新建、修改或排查 Minecraft 追踪模板（templates/ 下的 template.json、lang.json、layout.json）。用户想追踪新的进度、统计或手动任务，调整主窗口或悬浮窗布局，添加翻译和图标，或模板加载失败、显示不对时使用。
---

# Beacon 自定义模板

Beacon 是一个读取 Minecraft 存档、实时显示挑战进度的桌面程序。**模板**决定它追踪什么、什么时候算完成、界面怎样排列。
这份 skill 用来帮助 Beacon 的使用者（通常是玩家，不是程序员）编写和修改模板。

## 与用户沟通

- 用游戏里的说法交流：问“想追踪哪些进度、要挖多少远古残骸、哪些完成才算通关”，不要让用户填写 `DataVersion`、Fact、`prefix` 这类字段名。
- 能从本地文件确认的事情自己查：已有模板、存档里的进度和统计名称、存档版本。只在需要用户做决定时提问。
- 只改模板目录里的文件。不要修改 Beacon 程序本身、`versions/` 下的文件、用户的设置或 Minecraft 存档。
- 模板做不到的事情直说（例如检查背包里当前有几个物品），并给出替代做法（估算数量或手动打勾）。
- 交付时用一两句话说明模板会显示什么，再说明怎样在 Beacon 里看到它；文件细节放在后面。

## 工作流程

1. **找到模板目录。** 模板放在 Beacon 安装目录（`beacon` 启动程序所在目录，这份 skill 位于其中的 `.agents/skills/` 下）的 `templates/<模板名>/` 里，不要放进 `versions/`。Beacon 自带 `templates/1.16.1`（1.16.1 全进度，中英文），可作为参考或起点；先看 `templates/` 下已有哪些，修改已有模板时先读完它的三个 JSON 文件、翻译和图标。
2. **弄清需求。** 把用户的描述整理成下表，能从本地查到的不再问：

   | 要弄清的内容 | 例子 |
   | --- | --- |
   | 游戏版本 | 1.20.1，或者直接从存档读取（见“确定游戏版本”） |
   | 追踪哪些东西 | 进度、某个进度里的子条件、挖掘/拾取/击杀次数、游戏时长、只能自己打勾的任务 |
   | 每项的目标 | 获得进度即可，还是要累计到某个数量 |
   | 什么时候算通关 | 全部完成、任选其一、完成其中几项，还是按顺序完成 |
   | 怎样显示 | 主窗口如何分区，悬浮窗显示哪些，统计是否常驻，哪些项按阶段或轮换显示 |
   | 名称与图标 | 模板名、每一项显示的文字、需要的语言、用户手头有没有图标图片 |

3. **从存档核对名称。** 进度和统计的写法必须与存档完全一致，按“从存档查找名称”查实，不要凭记忆拼写。
4. **编写文件。** 新模板可以从“最小模板”或“完整示例”开始；已有模板只改需要改的部分，不顺手改动用户的其他目标、布局或翻译。改名或删除某项时，同时更新引用它的地方。
5. **自检。** 运行“检查模板”里的脚本，修完所有问题。
6. **让用户加载并确认。** 新模板需要重启 Beacon；修改已有模板可以重启，或在设置里先切到其他模板再切回（直接重选当前模板不会重新读取文件）。请用户确认设置中能看到模板、窗口显示正确。
7. **交付说明。** 见“完成后告诉用户”。

## 模板文件

Beacon 启动时扫描 `templates/` 下的每个子目录，每个模板目录必须有三个 JSON 文件：

```text
templates/
  my-run/
    template.json   追踪什么、怎样算完成、适用的游戏版本
    lang.json       默认语言的显示文字
    layout.json     主窗口和悬浮窗的布局
    langs/          可选：其他语言，例如 en.json、zh-CN.json
    assets/         可选：图标等资源，子目录随意
```

- 设置里显示的模板名来自 `template.json` 的 `name_key` 经 `lang.json` 翻译后的文字，目录名只用于存放文件。
- 三个文件都用 UTF-8 编码和标准 JSON：双引号、不写注释、没有末尾多余的逗号、同一对象里不重复字段。Beacon 会拒绝它不认识的字段。
- 不要把 Markdown 代码围栏或说明文字写进 JSON 文件。
- 整个模板目录（包括图标）复制给别人就能直接使用。

### `template.json`

顶层只有下面四个字段，全部必填：

| 字段 | 内容 |
| --- | --- |
| `name_key` | 模板名的文字 key，建议写 `template.<目录名>`，在 `lang.json` 里给出文字 |
| `minecraft` | `{"min_version": 整数, "max_version": 整数}`，适用的存档版本号范围，两端都包含 |
| `goals` | 规则数组：要显示的目标、统计，以及不直接显示的通关规则 |
| `completion_rule` | 决定“整个挑战是否完成”的规则 `id` |

没有 `version`、`description`、`assets`、`stats`、`language` 等其他顶层字段。

### 最小模板

只追踪“解放末地”：

`template.json`：

```json
{
  "name_key": "template.my_run",
  "minecraft": {"min_version": 3465, "max_version": 3465},
  "goals": [
    {
      "id": "goal/kill_dragon",
      "fact": "adv/minecraft:end/kill_dragon",
      "view": {"name_key": "goal.kill_dragon"}
    }
  ],
  "completion_rule": "goal/kill_dragon"
}
```

`lang.json`：

```json
{
  "template.my_run": "末影龙实时追踪",
  "goal.kill_dragon": "解放末地"
}
```

`layout.json`：

```json
{
  "main": [
    {"id": "Progress", "nodes": ["goal/kill_dragon"]}
  ]
}
```

示例中的 `3465` 只是占位，必须换成用户存档的实际版本号。

## 确定游戏版本

`min_version` / `max_version` 填写的是**存档版本号**（`DataVersion`，一个整数，例如 1.20.1 是 `3465`），不是 `1.20.1` 这样的版本名。
最可靠的来源是用户的存档：打开下文的统计文件，顶层 `DataVersion` 就是它。只支持一个版本时两端写同一个数；确认用到的进度和统计在多个版本里都存在后，才放宽范围。
不确定的版本号要告诉用户“尚未核实”，不要写一个很大的范围当作兼容承诺。

存档版本不在模板范围内时，Beacon 不会对这个存档使用该模板。

## 从存档查找名称

### 存档在哪里

| 系统 | 默认游戏目录 |
| --- | --- |
| Windows | `%APPDATA%\.minecraft` |
| macOS | `~/Library/Application Support/minecraft` |
| Linux | `~/.minecraft` |

使用 HMCL、PCL、Prism 等启动器并开启版本隔离时，游戏目录在启动器的实例或版本文件夹里；Beacon 设置中填写的游戏目录也可以作参考。
世界存档在游戏目录的 `saves/<世界名>/` 下：

- 统计：`stats/<玩家UUID>.json`，`stats` 对象里按分类列出，例如 `stats["minecraft:mined"]["minecraft:stone"]`。
- 进度：`advancements/<玩家UUID>.json`，顶层 key 是进度 ID，其中 `criteria` 列出已达成的子条件。

一个世界里有多个玩家文件时，确认用的是同一个玩家的统计和进度。存档里还没有某个条目，可能只是还没发生；最好在一个已经完成过该进度的存档里查，或请用户在游戏里先做一次。
只读取存档，不要修改它。

### 能追踪的数据

模板通过 **Fact**（从存档读出的整数）获取数据。缺失的 Fact 视为 `0`，达成的进度为 `1`。

| 要追踪的东西 | 写法 | 例子 |
| --- | --- | --- |
| 进度 | `adv/<进度 ID>` | `adv/minecraft:end/kill_dragon` |
| 进度的子条件 | `criterion/<进度 ID>/<条件 ID>` | `criterion/minecraft:story/shiny_gear/diamond_helmet` |
| 统计 | `stat/<分类>/<对象 ID>` | `stat/minecraft:mined/minecraft:ancient_debris` |
| 配方解锁 | `recipe/<配方进度路径>` | `recipe/minecraft:tools/diamond_pickaxe` |
| 游戏时长 | `clock/play_ticks` | `clock/play_ticks` |
| 物品数量估算 | `stat/minecraft:estimated/<物品 ID>` | `stat/minecraft:estimated/minecraft:ender_pearl` |

写法要点：

- 名称按存档原样照抄，保留命名空间、大小写和中间的 `/`。条件 ID 本身可能包含 `minecraft:` 和 `/`，不要猜测拆分位置。
- 统计分类如 `minecraft:mined`（挖掘）、`minecraft:picked_up`（拾取）、`minecraft:crafted`（合成）、`minecraft:killed`（击杀）、`minecraft:custom`（杂项），以存档中出现的为准。
- `recipe/` 表示配方**已解锁**，不代表合成过。它来自存档中 `minecraft:recipes/` 开头的进度，保留分类路径：`minecraft:recipes/tools/diamond_pickaxe` 写作 `recipe/minecraft:tools/diamond_pickaxe`，不一定等于物品 ID。
- `clock/play_ticks` 是这个玩家累计的游戏时长，单位是 tick（每秒 20）。“累计玩 5 分钟”写作 `"target": 6000`。它不是倒计时，也不是现实时间。
- 物品数量估算 = 拾取 + 合成 − 丢弃 − 使用，最小为 0。它**不是**背包里的真实数量，交付时要向用户说明。
- “获得过”“现在拿着”“累计拾取过几次”是不同的事：拾取统计在重复捡起同一物品时也会增加。

没有背包、箱子、坐标、生命值、记分板或 NBT 数据，也没有 `inventory/`、`scoreboard/`、`nbt/` 之类的写法。
Beacon 只读取保存下来的进度和统计，不运行数据包。做不到自动追踪的目标，改用“手动打勾的任务”。

## 编写规则

`goals` 里的每一项是一条**规则**。规则可以有：

- `id`：被其他地方引用时必填，且在整个模板里唯一。只作为某个分组子项显示的规则可以不写。
- `view`：显示方式（见下文）。
- 一种判定方式：`fact`、`all`、`any`、`count`、`prefix`、`ref` 六选一；都不写就是手动打勾的任务。

建议给真正的目标用 `goal/` 开头的 ID，纯统计用 `counter/`，通关规则用 `completion/`。主窗口顶部的“已完成/总目标”只统计 `goal/` 开头的规则（不管它是否在布局中出现）；子项也用 `goal/` 开头会被重复计数。

### 单项数据：`fact`

```json
{
  "id": "goal/debris",
  "fact": "stat/minecraft:mined/minecraft:ancient_debris",
  "target": 20,
  "view": {"name_key": "goal.debris", "icon": "assets/debris.png"}
}
```

- 数值达到 `target` 即完成。`target` 必须是至少为 1 的整数（不能写 `0`、负数、`"20"` 或 `20.0`）。
- 进度和条件省略 `target` 时默认为 1。
- 统计（`stat/`）省略 `target` 时只显示数字，永远不会自动完成。想让“挖 7 块石头”算完成，必须写 `"target": 7`。不要把没有目标的统计放进需要完成的组合规则里。

只显示数量的计数器：

```json
{
  "id": "counter/stone",
  "fact": "stat/minecraft:mined/minecraft:stone",
  "view": {"name_key": "counter.stone"}
}
```

### 手动打勾的任务

不写判定方式即可；不要写 `"fact": ""`、`"manual": true` 或编造的 Fact：

```json
{
  "id": "goal/manual_check",
  "view": {"name_key": "goal.manual_check"}
}
```

在 Beacon 里按住 Ctrl（macOS 为 Cmd）点击图标：左键标记完成，右键撤销；统计项则是加一、减一。
手动标记只保存在本次运行中，不写入存档，重启 Beacon 或按 Ctrl+R / Cmd+R 重开本轮会清除。
组合规则不能直接打勾，标记它的子项即可。已经自动完成的进度不会因为右键撤销而变回未完成；统计的手动调整叠加在存档数值上，最低显示 0。

### 组合多项

| 写法 | 完成条件 |
| --- | --- |
| `"all": [...]` | 全部子项完成 |
| `"any": [...]` | 任一子项完成 |
| `"count": {"target": n, "of": [...]}` | 完成至少 n 项（n 在 1 到子项数之间） |
| `"prefix": [...]` | 按顺序完成：只数从第一项开始连续完成的项，全部完成才算完成 |
| `"ref": "<id>"` | 与另一条规则的结果相同，用于复用 |

```json
{
  "id": "goal/route",
  "all": [
    {"fact": "adv/minecraft:story/enter_the_nether"},
    {
      "any": [
        {"fact": "adv/minecraft:end/kill_dragon"},
        {"fact": "adv/minecraft:end/dragon_egg"}
      ]
    }
  ],
  "view": {"name_key": "goal.route"}
}
```

- 子项数组不能为空。组合规则数的是**完成了几项**，不会把数值相加：“挖 10 块石头”和“挖 20 块石头”组成的 `all` 显示 `0/2`～`2/2`，不是 `0/30`。
- `target` 只能紧跟 `fact`，或写在 `count` 对象里。
- 没有“非”、等于、区间、算式或脚本之类的写法。
- `prefix` 不记录事情发生的先后，只按当前存档判断：后面的项先完成，也要等前面的项完成后才计入。

通关规则通常用 `ref` 把已有目标组合起来，不必重复写判定：

```json
{
  "id": "completion/all",
  "all": [
    {"ref": "goal/debris"},
    {"ref": "goal/kill_dragon"}
  ]
}
```

`ref` 可以引用写在后面的规则，但不能绕回自己（直接或间接）。`ref` 只复用结果，不继承对方的名称、图标或子项；要显示它就单独写 `view`。

### 显示方式：`view`

| 字段 | 内容 | 省略时 |
| --- | --- | --- |
| `name_key` | 必填，显示文字的 key，在 `lang.json` 里给出文字 | 不能省略 |
| `icon` | 模板目录内图片的相对路径，例如 `assets/items/apple.png` | 显示紫黑棋盘格占位图 |
| `type` | `task`、`goal` 或 `challenge`，对应游戏里三种进度边框 | 无边框 |
| `enchanted` | `true` 时在图标上叠加附魔紫光 | 无光效 |
| `show` | `self`、`stages` 或 `cycle`，见“分阶段显示与轮换显示” | `prefix` 为 `stages`，其余为 `self` |

组合规则的子项也可以有自己的 `view`（不需要 `id`），用作收集组里每个成员的名称和图标：

```json
{
  "fact": "criterion/minecraft:adventure/adventuring_time/minecraft:badlands",
  "view": {"name_key": "criterion.badlands", "icon": "assets/icons/badlands.png"}
}
```

### 图标

- 路径相对模板目录，使用 `/`，要与实际文件名、扩展名和大小写完全一致；不能用绝对路径、网址、盘符、反斜杠、`.` 或 `..`。Beacon 不会补扩展名，也不会读取模板目录之外的文件。
- 图标缺失、路径写错或图片读不出来时显示紫黑棋盘格，模板仍能加载。用户没有图标时省略 `icon`，并告诉用户会显示占位图；不要写指向不存在文件的路径。
- 优先使用透明背景的 PNG。GIF 动图可以用，但请用户在 Beacon 里实际看一下。
- 在 Windows 或 macOS 上能显示，不代表在区分大小写的系统上也能显示；分享模板前核对大小写。

**附魔光效**：在 `view` 里写 `"enchanted": true`（只接受 `true` / `false`）。图片不需要自带光效，Beacon 会沿图标的不透明部分绘制紫光；不透明的方形背景也会被覆盖。光效只是装饰，和完成状态无关，没有颜色、速度等选项。每一项各自设置，不会从父项继承。

### 分阶段显示与轮换显示

一个位置可以显示一组规则中“当前那一项”，由 `view.show` 决定：

| `show` | 可用于 | 显示内容 |
| --- | --- | --- |
| `self` | 任意规则 | 自己（非 `prefix` 的默认值） |
| `stages` | `prefix`、`all` | 第一个未完成的子项；全部完成后停在最后一项（`prefix` 的默认值） |
| `cycle` | `all`、`any`、`count` | 每 1.5 秒轮换子项的图标；未完成时只轮换未完成的，完成后只轮换已完成的 |

`show` 写在不适用的规则上会导致模板加载失败。

**分阶段统计**，例如“挖石头 10 → 20 → 64”显示在同一个位置：

```json
{
  "id": "counter/stone_stages",
  "prefix": [
    {"fact": "stat/minecraft:mined/minecraft:stone", "target": 10, "view": {"name_key": "stage.stone_10"}},
    {"fact": "stat/minecraft:mined/minecraft:stone", "target": 20, "view": {"name_key": "stage.stone_20"}},
    {"fact": "stat/minecraft:mined/minecraft:stone", "target": 64, "view": {"name_key": "stage.stone_64"}}
  ],
  "view": {"name_key": "counter.stone_stages"}
}
```

- 每个阶段显示自己的名称；没写图标时沿用上一层的图标。统计阶段的名称下方会多一行 `12/20`。
- 各阶段读的是同一个累计数：挖了 12 块时显示 `12/20`，不会从 0 重新数。
- 主窗口里，Ctrl/Cmd 左键推进当前阶段，右键回退（当前阶段为 0 时回到上一阶段）。

**先获取再使用**，例如锻造模板收集中的一项：

```json
{
  "prefix": [
    {
      "fact": "stat/minecraft:picked_up/minecraft:rib_armor_trim_smithing_template",
      "view": {"name_key": "stage.rib_obtain"}
    },
    {
      "fact": "criterion/minecraft:adventure/trim_with_all_exclusive_armor_patterns/armor_trimmed_minecraft:rib_armor_trim_smithing_template_smithing_trim",
      "view": {"name_key": "stage.rib_use"}
    }
  ],
  "view": {
    "name_key": "criterion.rib_armor_trim_smithing_template",
    "icon": "assets/icons/rib_armor_trim_smithing_template.png"
  }
}
```

放进收集组的 `all` 里，只有“使用”阶段完成后这一项才算完成。拾取统计只在捡起物品时增加，合成得到的不算；需要时把第一阶段改成 `any` 并列多个来源。
`all` 配 `"show": "stages"` 也按顺序显示，但后面的步骤先完成也会计入；要求严格顺序时用 `prefix`。

**任选其一轮换**，例如“吃生牛肉或熟牛肉都算”：

```json
{
  "any": [
    {"fact": "criterion/minecraft:husbandry/balanced_diet/beef", "view": {"name_key": "food.beef", "icon": "assets/icons/beef.png"}},
    {"fact": "criterion/minecraft:husbandry/balanced_diet/cooked_beef", "view": {"name_key": "food.cooked_beef", "icon": "assets/icons/cooked_beef.png"}}
  ],
  "view": {"name_key": "food.any_beef", "show": "cycle"}
}
```

未完成时两个图标轮换，名称固定显示 `food.any_beef` 的文字；吃过一种后固定显示那一种。两种都要吃就把 `any` 换成 `all`。
轮换时不显示外层自己的图标，可以省略。示例中的条件名仅作示意，使用前按存档核对。

## 布局 `layout.json`

`main` 是主窗口，`overlay` 是悬浮窗（悬浮窗还需用户在设置中开启），两者都可省略。每个窗口是一个分组数组。
分组的字段都有默认值，**只写与默认值不同的部分**：

| 字段 | 含义 | 默认 |
| --- | --- | --- |
| `id` | 分组名，只为方便阅读，不显示 | 空 |
| `x`, `y` | 左上角位置（相对比例，见下文） | `0` |
| `width`, `height` | 尺寸（相对比例，必须大于 0） | `1` |
| `margin`, `padding` | 外边距、内边距，像素，随窗口缩放 | `0` |
| `nodes` / `prefix` / `collections` / `children` | 分组内容，四选一 | 空面板 |
| `exclude` | 从选中的项里排除的规则 ID | `[]` |
| `spacing` | 组内间距 | 见“间距” |
| `item_style` | 仅主窗口的 `collections` 分组：完成/未完成的样式 | 未完成变暗、完成高亮 |
| `scroll` | 仅悬浮窗：是否滚动轮播 | `true` |

数值都不能是负数。写错字段或把字段写在不支持的地方时，模板加载失败，错误信息会给出 `layout.json/main/0/children/1/x` 这样的位置。

### 分组内容

| 字段 | 填写 | 显示 |
| --- | --- | --- |
| `nodes` | 规则 ID 列表 | 这些规则，按列表顺序 |
| `prefix` | ID 开头的字符串列表 | 所有以它开头的规则，按 `template.json` 中的顺序 |
| `collections` | 规则 ID 列表 | 每条规则的直接子项，组成带标题的收集组 |
| `children` | 子分组数组 | 不显示项目，子分组在本组范围内排列 |

- 这里只能填规则的 `id`，不能填 Fact 或文字 key。
- `prefix` 是普通的开头匹配：写 `goal/nether_`，不要写 `goal/nether_*`。
- `collections` 只展开一层，收集组标题来自父规则的 `view`。
- 某项没放进布局只是不显示，仍然影响通关判定和顶部计数。
- **填错的 ID 不会让加载失败，只会让那一项不显示**，所以必须用“检查模板”的脚本核对。
- 没有 `columns`、`rows`、字体大小之类的字段，项目排列由 Beacon 根据空间自动决定。

```json
{"id": "Nether", "prefix": ["goal/nether_"], "exclude": ["goal/nether_hidden"]}
```

### 位置与嵌套

同一层的分组按 `x`、`y`、`width`、`height` 的比例铺满它所在的区域（顶层就是整个窗口）。
例如并排的两个分组，一个 `"width": 1`，另一个 `"x": 1, "width": 2`，就分别占窗口宽度的三分之一和三分之二。
想在某处留空，就放一个不写内容的分组占位。

用 `children` 嵌套时，子分组的位置相对父分组计算，父分组只当作背景面板：

```json
{
  "main": [
    {
      "id": "Container",
      "children": [
        {"id": "Left", "padding": 8, "nodes": ["goal/kill_dragon"]},
        {"id": "Right", "x": 1, "padding": 8, "nodes": ["counter/stone"]}
      ]
    }
  ]
}
```

布局的 `children` 是分组数组，规则里的 `all`、`any`、`prefix`、`count.of` 是规则数组，两者不能混用。主窗口和悬浮窗的布局各写各的，不会自动复制。

### 悬浮窗滚动

悬浮窗只有一行。分组默认按设置中的方向和速度滚动轮播，完成的项从轮播中移除。
统计等需要一直显示的内容，给它的分组写 `"scroll": false`，项目会固定排成一行：

```json
{"id": "Stats", "y": 1, "padding": 4, "nodes": ["counter/ancient_debris"], "scroll": false}
```

悬浮窗的分组全部占满宽度、从上到下排列时，各行按 `height` 的比例分配高度，`y` 留出的空隙保留为空行；某行内容放不下时，该行保持内容所需的高度，其余行按比例分剩下的空间，窗口最小高度也会随之增大。

### 间距

两个分组之间的距离等于它们的 `margin` 之和。组内间距用 `spacing` 对象调整（像素，只写要改的字段）：

| 字段 | 含义 | 主窗口 `nodes`/`prefix` | 主窗口 `collections` | 悬浮窗 |
| --- | --- | --- | --- | --- |
| `item` | 相邻项目的横向间距 | 0 | 16 | `collections` 为 32，其余 0 |
| `row` | 行距 | 0 | 6 | 不支持 |
| `label` | 图标到下方文字的距离 | 4 | 3 | 2 |
| `collection` | 相邻收集组的距离 | 不支持 | 0 | 48（仅 `collections`） |

```json
{"id": "Route", "margin": 4, "padding": 8, "collections": ["goal/route"], "spacing": {"item": 24, "row": 10}}
```

### 收集组的完成样式

主窗口的 `collections` 分组可以用 `item_style` 设置子项在未完成（`pending`）和完成（`done`）时的样子：

| 属性 | 可选值 | 默认 |
| --- | --- | --- |
| `emphasis` | `highlight` 高亮、`muted` 变暗 | 未完成 `muted`，完成 `highlight` |
| `decoration` | `none`、`strikethrough` 删除线 | `none` |
| `visibility` | `visible`、`blank` 留空 | `visible` |

```json
{
  "id": "Collections",
  "padding": 8,
  "collections": ["goal/route"],
  "item_style": {
    "pending": {"emphasis": "highlight"},
    "done": {"emphasis": "muted", "decoration": "strikethrough"}
  }
}
```

`"done": {"visibility": "blank"}` 会让完成的项留出空位、不再显示，同时也**无法再点击撤销**；用户希望能撤销时，建议用变暗或删除线。
`item_style` 只能写在主窗口的 `collections` 分组里；悬浮窗有自己的完成动画。

## 文字与翻译

- `lang.json` 是默认语言，必须是平铺的 `"key": "文字"` 对象，不能嵌套。要覆盖模板名和所有 `view.name_key`；缺少的 key 会直接把 key 原文显示在界面上。
- 其他语言放在 `langs/<语言代码>.json`（例如 `langs/en.json`），只写需要翻译的条目，没写的回退到 `lang.json`。文件名只用字母、数字、`_`、`-`。不要再套一层 `{"en": {...}}`。
- 用户在 Beacon 设置的“模板语言”中切换模板文字；“界面语言”只控制 Beacon 自己的按钮和菜单。
- 文字 key 和规则 ID 是两回事，建议用 `goal.route`、`stage.stone_20` 这样有含义的 key。

## 完整示例：路线、补给与手动确认

三个目标都完成才算通关：进入下界并击败末影龙、累计挖 32 块石头、手动确认准备完成。不需要额外图标。
`3465` 需换成用户存档的实际版本号。

`template.json`：

```json
{
  "name_key": "template.route_demo",
  "minecraft": {"min_version": 3465, "max_version": 3465},
  "goals": [
    {
      "id": "goal/route",
      "all": [
        {"fact": "adv/minecraft:story/enter_the_nether", "view": {"name_key": "step.nether"}},
        {"fact": "adv/minecraft:end/kill_dragon", "view": {"name_key": "step.dragon"}}
      ],
      "view": {"name_key": "goal.route", "type": "challenge"}
    },
    {
      "id": "goal/supplies",
      "fact": "stat/minecraft:mined/minecraft:stone",
      "target": 32,
      "view": {"name_key": "goal.supplies"}
    },
    {
      "id": "goal/manual_check",
      "view": {"name_key": "goal.manual_check"}
    },
    {
      "id": "completion/all",
      "all": [
        {"ref": "goal/route"},
        {"ref": "goal/supplies"},
        {"ref": "goal/manual_check"}
      ]
    }
  ],
  "completion_rule": "completion/all"
}
```

`lang.json`：

```json
{
  "template.route_demo": "路线与补给示例",
  "goal.route": "完成路线",
  "step.nether": "进入下界",
  "step.dragon": "击败末影龙",
  "goal.supplies": "累计挖掘 32 块石头",
  "goal.manual_check": "手动确认准备完成"
}
```

`layout.json`：

```json
{
  "main": [
    {
      "id": "Route",
      "width": 2,
      "height": 2,
      "padding": 8,
      "collections": ["goal/route"],
      "item_style": {
        "pending": {"emphasis": "highlight"},
        "done": {"emphasis": "muted", "decoration": "strikethrough"}
      }
    },
    {"id": "Supplies", "x": 2, "padding": 8, "nodes": ["goal/supplies"]},
    {"id": "Manual", "x": 2, "y": 1, "padding": 8, "nodes": ["goal/manual_check"]}
  ],
  "overlay": [
    {"id": "RemainingGoals", "width": 2, "padding": 4, "nodes": ["goal/route", "goal/manual_check"]},
    {"id": "SupplyCounter", "x": 2, "padding": 4, "nodes": ["goal/supplies"], "scroll": false}
  ]
}
```

主窗口左侧三分之二是路线收集组，右侧上下是挖石头和手动确认；悬浮窗里路线和手动任务轮播，挖石头计数固定显示。

## 检查模板

写完后先自检，再请用户在 Beacon 里加载。下面的脚本检查 JSON 语法、重复字段、ID 是否重复、`ref` / `completion_rule` / 布局引用是否存在、文字 key 是否都有翻译、图标文件是否存在。
把它保存到模板目录以外的临时位置（不要放进 `templates/`），用 `python3 -I <脚本> <模板目录>` 运行。用户电脑上没有 Python 时，按脚本逻辑逐项人工核对。

```python
import json, sys
from pathlib import Path

root = Path(sys.argv[1])
problems = []

def load(path):
    def no_duplicates(pairs):
        keys = [k for k, _ in pairs]
        dups = sorted({k for k in keys if keys.count(k) > 1})
        if dups:
            problems.append(f"{path.relative_to(root)}: 重复字段 {dups}")
        return dict(pairs)
    try:
        return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=no_duplicates)
    except FileNotFoundError:
        problems.append(f"缺少文件 {path.relative_to(root)}")
    except ValueError as error:
        problems.append(f"{path.relative_to(root)}: JSON 无法解析：{error}")
    return None

template = load(root / "template.json") or {}
lang = load(root / "lang.json") or {}
layout = load(root / "layout.json") or {}
for extra in sorted((root / "langs").glob("*.json")):
    load(extra)

ids, refs, name_keys, icons = [], [], [template.get("name_key")], []

def walk(rule):
    if not isinstance(rule, dict):
        problems.append(f"规则不是对象：{rule!r}")
        return
    if "id" in rule:
        ids.append(rule["id"])
    if "ref" in rule:
        refs.append(rule["ref"])
    view = rule.get("view") or {}
    name_keys.append(view.get("name_key"))
    if view.get("icon"):
        icons.append(view["icon"])
    children = rule.get("all") or rule.get("any") or rule.get("prefix") or (rule.get("count") or {}).get("of") or []
    for child in children:
        walk(child)

for goal in template.get("goals", []):
    walk(goal)

for rule_id in sorted({i for i in ids if ids.count(i) > 1}):
    problems.append(f"ID 重复：{rule_id}")
for target in refs + [template.get("completion_rule")]:
    if target not in ids:
        problems.append(f"引用了不存在的 ID：{target}")
for key in name_keys:
    if key and key not in lang:
        problems.append(f"lang.json 缺少文字：{key}")
for icon in icons:
    if not (root / icon).is_file():
        problems.append(f"图标文件不存在：{icon}")

def walk_layout(groups, where):
    for index, group in enumerate(groups):
        place = f"{where}/{index}"
        for field in ("nodes", "collections", "exclude"):
            for rule_id in group.get(field, []):
                if rule_id not in ids:
                    problems.append(f"layout.json {place}: {field} 中的 ID 不存在：{rule_id}")
        for start in group.get("prefix", []):
            if not any(i.startswith(start) for i in ids):
                problems.append(f"layout.json {place}: prefix 没有匹配任何 ID：{start}")
        walk_layout(group.get("children", []), place + "/children")

for window in ("main", "overlay"):
    walk_layout(layout.get(window, []), window)

print("\n".join(problems) if problems else "未发现问题")
```

脚本通过只说明文件之间能对上，字段写法是否被 Beacon 接受仍以实际加载为准。请用户加载后确认：

1. 设置的“模板”下拉框里出现了模板名（不是 key 原文）。
2. 主窗口、需要时还有悬浮窗，显示了预期的项目、文字和图标。
3. 有条件时用一个已知进度的存档看看完成状态和数字是否对得上。

不要为了测试而修改用户的存档。

### 常见问题

| 现象 | 先检查 |
| --- | --- |
| 设置里找不到模板 / `no templates found` | 是否放在安装目录的 `templates/<名称>/template.json`，有没有误放进 `versions/` 或多套了一层目录；新模板是否重启了 Beacon |
| `invalid template JSON` / `invalid resource JSON` | JSON 语法、重复字段 |
| `unknown field` | 是否写了不支持的字段，或把 `target`、`item_style`、`scroll`、`spacing` 写错了位置 |
| `duplicate rule ID` / `missing Ref target` | ID 是否唯一，引用拼写是否一致 |
| `dependency cycle` | `ref` 是否直接或间接引用了自己 |
| `completion_rule does not name an explicit rule ID` | `completion_rule` 是否填了 Fact、文字 key 或没有 `id` 的规则 |
| `a layout group takes only one of ...` | 同一分组是否同时写了 `nodes`、`prefix`、`collections`、`children` 中的两个以上 |
| 窗口空白或少了某项 | 布局里的 ID 是否拼对，`prefix` 是否匹配，`collections` 是否只展开了一层，是否被 `exclude` 或 `blank` 隐藏 |
| 数字在变但一直不完成 | 统计是否没写 `target`，组合规则里是否包含没有目标的统计 |
| 顶部总数不对 | 所有 `goal/` 开头的规则都会计数，包括没显示的和组合规则的子项 |
| 不切换阶段或不轮换 | `view.show` 是否写成了 `self`，轮换是否写了 `"show": "cycle"` |
| 统计在悬浮窗里跟着滚走 | 统计所在分组是否写了 `"scroll": false` |
| 紫黑棋盘格图标 | 文件是否存在，扩展名和大小写是否完全一致，图片能否打开 |
| 没有附魔光效 | 实际显示的那一项是否写了 `"enchanted": true`，图标是否能正常显示 |
| 改了文件但界面没变 | 是否重启或切到其他模板再切回，改的是否是 Beacon 实际使用的那个 `templates/` |
| 存档没有被识别 | 存档版本号是否在模板范围内，游戏目录是否正确 |

### 规模限制

单个 JSON 文件不超过 16 MiB、嵌套不超过 128 层；规则总数（含所有子项）、布局分组总数、单个列表的 ID 数和每份语言文件的条目数都不超过 10,000；单个字符串不超过 4,096 字节。
重复出现的目标用 `ref` 复用，不要堆出巨大的嵌套结构。

## 完成后告诉用户

1. 模板做了什么：追踪哪些内容、什么时候算通关、主窗口和悬浮窗怎样显示。
2. 怎样使用：模板文件夹的位置、在设置“模板”下拉框里选哪个名称、需要重启 Beacon 还是切换模板后切回、悬浮窗需要在设置中开启。
3. 已经检查了什么（自检脚本、存档核对），还需要用户在 Beacon 里确认什么。
4. 限制和未核实的地方：估算数量不是真实库存、手动标记重启后清除、哪些版本号或进度名称还没在存档中核实、哪些项没有图标会显示占位图。
