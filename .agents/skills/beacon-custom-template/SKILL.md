---
name: beacon-custom-template
description: 为 Beacon 创建、修改和校验可直接加载的 Minecraft 自定义模板包。
---

# Beacon 自定义模板 Skill

本 skill 面向 AI。用户要求新增或修改模板时，先阅读用户 `templates/` 下已有的模板（Beacon 不附带内置模板），
再按下面的格式生成文件；不要修改 Beacon C++ 源码，也不要凭空引入模板字段。

## 工作协议

1. 先确认目标 Minecraft `DataVersion` 和要追踪的事实；不确定时保留兼容范围并明确说明。
2. 每个模板只创建 `template.json`、`lang.json`、`layout.json` 三个必需文件；语言目录和模板专用资源按需添加。
3. 复用已有模板的命名、图标和布局风格。所有 `id`、`ref`、`completion_rule` 和布局节点引用必须闭合且唯一。
4. 生成后检查 JSON、规则引用、版本范围、布局引用和资源路径；确保每个非空 `view.icon` 都指向模板内存在的文件；优先用仓库已有测试或构建命令验证。
5. 输出变更文件、用户需要选择的模板名称，以及仍需用户确认的版本或事实假设。

模板包放在安装根目录（`beacon` 启动器所在目录）的 `templates/<name>/` 下。本 skill 随包放在安装根目录的
`.agents/skills/beacon-custom-template/SKILL.md`。版本化安装中的程序和内置资源位于 `versions/<版本>/`，模板不放进版本目录。
直接运行源码构建的 `build/beacon` 时，模板应放在 `build/templates/<name>/` 下。

## 模板格式

Beacon 的模板决定三件事：追踪什么、怎样判定完成、怎样显示。模板不会保存历史记录；Minecraft 存档变化后，Beacon 会用当前 Facts 重新计算并刷新界面。

## 创建模板包

已有模板时，最简单的做法是复制其目录再删改内容；没有模板时按本文格式从零编写。桌面程序只扫描安装根目录的 `templates/` 下一级子目录；每个模板必须包含下面三个 JSON 文件：

```text
templates/
  my-run/
    template.json
    lang.json
    langs/          可选语言目录
    layout.json
    assets/         可选的模板专用资源，可任意组织子目录
      debris.png
```

`lang.json` 是模板的默认语言。可选语言放在模板目录的 `langs/` 下，文件名使用语言代码，例如
`langs/en.json` 或 `langs/zh-CN.json`；然后可以在设置中的“模板语言”下拉框切换。语言文件只需提供要覆盖的条目，缺失条目会回退到 `lang.json`；“界面语言”独立控制 Beacon 自身的文字。

Beacon 启动时扫描这些目录。新增模板后重启 Beacon，然后在设置中的“模板”下拉框选择它。

下面是一个只追踪“解放末地”的最小模板。

`template.json`：

```json
{
  "name_key": "template.my_run",
  "minecraft": {
    "min_version": 0,
    "max_version": 99999999
  },
  "goals": [
    {
      "id": "goal/kill_dragon",
      "fact": "adv/minecraft:end/kill_dragon",
      "view": {
        "name_key": "goal.kill_dragon"
      }
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
    {
      "id": "Progress",
      "source": "nodes",
      "nodes": ["goal/kill_dragon"],
      "exclude": [],
      "x": 0,
      "y": 0,
      "width": 1,
      "height": 1, "margin": 0, "padding": 0
    }
  ],
  "overlay": []
}
```

## 选择 Fact

Fact 是 Beacon 从 Minecraft 存档读取出的整数。缺失的 Fact 按 `0` 处理；布尔事件完成后通常为 `1`。

| 要追踪的数据 | Fact 格式 | 示例 |
| --- | --- | --- |
| 进度 | `adv/<进度 ID>` | `adv/minecraft:end/kill_dragon` |
| 进度条件 | `criterion/<进度 ID>/<条件 ID>` | `criterion/minecraft:story/shiny_gear/diamond_helmet` |
| 统计 | `stat/<分类>/<对象 ID>` | `stat/minecraft:mined/minecraft:ancient_debris` |
| 配方 | `recipe/<配方 ID>` | `recipe/minecraft:diamond_pickaxe` |
| 游戏时间 | `clock/play_ticks` | `clock/play_ticks` |

Beacon 还提供一个非权威的物品数量估算 Fact：
`stat/minecraft:estimated/<物品 ID>`，计算方式是“拾取 + 合成 - 丢弃 - 使用”，不足时钳制为 0。
例如 `stat/minecraft:estimated/minecraft:ender_pearl`。它不是玩家当前背包的真实库存。

现代版本中，可以直接从存档找到名称：

- 统计：`.minecraft/saves/<世界>/stats/<UUID>.json` 的 `stats` 对象。例如 `stats["minecraft:mined"]["minecraft:stone"]` 对应 `stat/minecraft:mined/minecraft:stone`。
- 进度：`.minecraft/saves/<世界>/advancements/<UUID>.json`。顶层 key 对应 `adv/<key>`，其 `criteria` 中的 key 对应 `criterion/<顶层 key>/<条件 key>`。

模板的 `minecraft.min_version` 和 `max_version` 是存档 JSON 顶层的 `DataVersion`，范围包含两端。只支持一个版本时将两者设为相同值；确认所用 Fact 在多个版本中稳定后，才放宽范围。

## 编写规则

`goals` 中的自动规则必须且只能使用一种规则。叶子节点可以省略规则操作，此时它不会从存档读取数据，只能按住 Ctrl（macOS 为 Cmd）并用鼠标左键/右键手动标记或取消；需要显示或被其他规则引用的节点应设置唯一 `id`；`completion_rule` 必须指向一个显式 `id`。

### 单个事实

达到 `target` 即完成，省略 `target` 时默认为 `1`：

```json
{
  "id": "goal/debris",
  "fact": "stat/minecraft:mined/minecraft:ancient_debris",
  "target": 20,
  "view": {"name_key": "goal.debris", "icon": "assets/debris.png"}
}
```

`view` 只控制显示。`name_key` 是 `lang.json` 中的本地化 key；`icon` 可选，填写时必须使用模板目录内的完整相对文件路径；`type` 可选为 `task`、`goal` 或 `challenge`，用于选择 Minecraft 进度边框，省略时不显示边框。
模板资源可放在 `assets/` 下的任意子目录。路径必须与实际文件名和扩展名完全一致；Beacon 不查找全局资源名，也不会补充文件扩展名。省略 `icon`、路径为空、文件缺失、图片无法读取（包括损坏的 GIF）、非法路径或符号链接越界时，统一显示紫黑棋盘格 missingno，不影响模板加载；程序不会加载模板目录外的文件作为图标。模板目录包含其使用的全部资源，复制整个目录即可分享。

父级或会被 `layout.json`、`ref`、`completion_rule` 引用的节点应提供唯一 `id`。
收集类规则的子项不需要 `id`，直接在子项上提供 `view` 即可：

```json
{
  "fact": "criterion/minecraft:adventure/adventuring_time/minecraft:badlands",
  "view": {
    "name_key": "criterion.badlands",
    "icon": "assets/icons/check.xpm"
  }
}
```

`view` 可以用于 `all`、`any` 和 `count` 的任意子项。这样不会为仅用于展示的子项生成不会被引用的虚假 ID。
收集类子项通常使用自己的 `name_key` 和 `icon`，父级的 `view` 仍用于收集卡片标题和主图标。

对应的 `criterion.*` 标题写在同目录的 `lang.json` 中。例如：

```json
{
  "fact": "criterion/minecraft:husbandry/complete_catalogue/minecraft:all_black",
  "view": {
    "name_key": "criterion.all_black",
    "icon": "assets/icons/check.xpm"
  }
}
```

```json
{
  "criterion.all_black": "全黑"
}
```

例如，`"icon": "assets/blocks/stone.png"` 对应模板目录中的 `assets/blocks/stone.png`。程序包只带界面资源和字体；模板资源与模板配置一起复制、分享。

### 组合规则

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

可用规则：

| 规则 | 完成条件 |
| --- | --- |
| `fact` | Fact 值达到 `target` |
| `all` | 所有子规则完成 |
| `any` | 至少一个子规则完成 |
| `count` | 完成的子规则数量达到目标 |
| `prefix` | 按顺序统计从第一个开始连续完成的子规则 |
| `ref` | 使用另一个显式 ID 节点的结果 |

`count` 的格式为：

```json
{
  "id": "goal/two_of_three",
  "count": {
    "target": 2,
    "of": [
      {"fact": "adv/example:a"},
      {"fact": "adv/example:b"},
      {"fact": "adv/example:c"}
    ]
  }
}
```

`ref` 适合在总完成规则中复用已有目标，避免复制判定条件：

```json
{
  "id": "completion/all",
  "all": [
    {"ref": "goal/debris"},
    {"ref": "goal/kill_dragon"}
  ]
}
```

引用可以写在目标节点之前，但不能形成环。

主窗口顶部的目标计数目前只统计 ID 以 `goal/` 开头的 Fact 节点；组合规则和其他前缀的节点不计入该总数。`completion_rule` 独立决定整次挑战是否完成。自定义目标应遵循这个命名约定，否则规则仍可求值和显示，但不会进入顶部目标计数。

## 配置布局

`layout.json` 的 `main` 控制主窗口，`overlay` 控制悬浮窗。每个分组都必须提供完整字段：

| 字段 | 含义 |
| --- | --- |
| `id` | 非空分组名称 |
| `source` | 如何选择节点 |
| `nodes` | 节点 ID 或 ID 前缀 |
| `exclude` | 要排除的精确节点 ID |
| `x`, `y` | 左上角位置，使用任意非负数，加载时自动归一化 |
| `width`, `height` | 正数尺寸，加载时自动归一化 |
| `margin`, `padding` | 必填像素值，分别表示分组外边距和内容内边距 |

`source` 有三种：

| source | `nodes` 的含义 |
| --- | --- |
| `nodes` | 精确节点 ID，按数组顺序显示 |
| `prefix` | 节点 ID 前缀，按模板声明顺序显示匹配项 |
| `children` | 显式节点 ID，显示它们的直接子规则 |

自定义统计应和其他规则一样定义在 `template.json` 的 `goals` 中，再由 `layout.json` 使用 `source: "nodes"` 按 ID 引用。统计的 `fact`、`target`、`view.name_key`、`view.icon` 和 `view.type` 都属于模板配置：

```json
{
  "id": "counter/ancient_debris",
  "fact": "stat/minecraft:mined/minecraft:ancient_debris",
  "target": 20,
  "view": {
    "name_key": "counter.ancient_debris",
    "icon": "assets/blocks/stone.png",
    "type": "challenge"
  }
}
```

例如，将全部 `goal/nether_` 节点放进四列网格：

```json
{
  "id": "Nether",
  "source": "prefix",
  "nodes": ["goal/nether_"],
  "exclude": ["goal/nether_hidden"],
  "x": 0,
  "y": 0,
  "width": 1,
  "height": 1, "margin": 0, "padding": 0
}
```

### 主窗口子项状态样式

仅 `layout.json` 的 `main` 中、`source: "children"` 的分组允许可选字段 `item_style`。
它作用于该分组的直接子项，不改变规则求值、收集组标题或总进度。嵌套布局组分别配置，不继承父组样式。
不要将此字段写入 `template.json`、`view`、`overlay` 或 `source: "nodes"` / `"prefix"` 的分组，否则加载失败。
悬浮窗沿用原有的完成遮盖动画和轮播移除行为，不应用这些样式。

例如，未完成子项高亮，完成后变暗并绘制贯穿图标与文字的删除线：

```json
{
  "main": [
    {
      "id": "Collections",
      "source": "children",
      "nodes": ["goal/route"],
      "exclude": [],
      "x": 0,
      "y": 0,
      "width": 1,
      "height": 1,
      "margin": 0,
      "padding": 8,
      "item_style": {
        "pending": {"emphasis": "highlight"},
        "done": {"emphasis": "muted", "decoration": "strikethrough"}
      }
    }
  ],
  "overlay": []
}
```

此示例的 `goal/route` 必须在 `template.json` 中定义，并为需要展示的子规则提供 `view`。
`pending` 表示未完成，`done` 表示已完成；两种状态及其属性均可省略。支持的属性如下：

| 属性 | 可选字符串值 | 省略时的默认值 |
| --- | --- | --- |
| `emphasis` | `highlight` 高亮、`muted` 变暗，同时影响图标与文字 | `pending` 为 `muted`，`done` 为 `highlight` |
| `decoration` | `none` 无装饰、`strikethrough` 删除线 | `none` |
| `visibility` | `visible` 显示、`blank` 留空 | `visible` |

不配置 `item_style`、省略状态或仅配置删除线等其他属性时，均默认未完成暗色、完成高亮；显式 `emphasis` 配置覆盖对应状态的默认值。

删除线从图标贯穿到首行文字右侧，多行文字的后续行分别绘制删除线，不改变排版尺寸。
不支持按图标和文字分别配置装饰，也不支持叉线、装饰数组、自定义颜色或线宽。

完成后留空时，将上例的 `done` 对象替换为：

```json
"done": {"visibility": "blank"}
```

`blank` 保留原有位置和尺寸，不会让后续子项前移；绘制前直接跳过图标、文字、光晕、装饰与点击处理。
它优先于 `emphasis` 和 `decoration`，即使同时配置删除线也不绘制。留空项无法直接点击撤销完成。
当用户希望完成后还能直接手动撤销时，应选择可见的变暗或删除线样式。

生成配置时只填写用户需要的属性，不要补齐所有默认值。未知状态、属性、枚举值和非字符串属性值都会被拒绝；
`item_style` 及 `pending` / `done` 必须是对象，不能写成 `null` 或数组。
修改后重启 Beacon，或先切换到其他模板再切回当前模板。

## 检查模板

修改 JSON 或资源文件后重启 Beacon，或先切换到其他模板再切回；直接选择当前模板不会重新加载。加载失败时优先检查：

1. 三个必需文件是否都在同一个模板目录中。
2. JSON 是否有尾随逗号、重复字段或拼写错误；Beacon 会拒绝未知字段。
3. 每个 `id` 是否唯一，`ref` 和 `completion_rule` 指向的 ID 是否存在。
4. `layout.json` 的 `nodes` 是否与 `template.json` 中的 ID 一致。
5. 当前存档的 `DataVersion` 是否落在模板范围内。
6. 每个非空 `view.icon` 是否是模板目录内的完整相对文件路径，且对应文件确实存在。
7. `item_style` 是否仅用于 `main` 的 `source: "children"` 分组，状态、属性和值是否符合上表。
