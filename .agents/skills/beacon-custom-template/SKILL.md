---
name: beacon-custom-template
description: 为 Beacon 创建、修改和校验可直接加载的 Minecraft 自定义模板包。
---

# Beacon 自定义模板 Skill

本 skill 面向 AI。用户要求新增或修改模板时，先阅读仓库中的 `templates/` 示例，
再按下面的格式生成文件；不要修改 Beacon C++ 源码，也不要凭空引入模板字段。

## 工作协议

1. 先确认目标 Minecraft `DataVersion` 和要追踪的事实；不确定时保留兼容范围并明确说明。
2. 每个模板只创建 `template.json`、`lang.json`、`layout.json` 三个必需文件；语言目录和图标按需添加。
3. 复用已有模板的命名、图标和布局风格。所有 `id`、`ref`、`completion_rule` 和布局节点引用必须闭合且唯一。
4. 生成后检查 JSON、规则引用、版本范围、布局引用和图标路径；优先用仓库已有测试或构建命令验证。
5. 输出变更文件、用户需要选择的模板名称，以及仍需用户确认的版本或事实假设。

模板包应放在程序根目录的 `templates/<name>/` 下；打包后的本文件位于程序根目录的
`.agents/skills/beacon-custom-template/SKILL.md`，与 `beacon`、`assets/` 和 `templates/`
同级。不要把 skill 文件放进模板目录，否则会被当成模板内容扫描。

## 模板格式

Beacon 的模板决定三件事：追踪什么、怎样判定完成、怎样显示。模板不会保存历史记录；Minecraft 存档变化后，Beacon 会用当前 Facts 重新计算并刷新界面。

## 创建模板包

最简单的做法是复制现有模板目录，再删改内容。桌面程序只扫描可执行文件所在目录的 `templates/` 下一级子目录；每个模板必须包含下面三个文件：

```text
templates/
  my-run/
    template.json
    lang.json
    langs/          可选语言目录
    layout.json
    icons/          可选
```

`lang.json` 是模板的默认语言。可选语言放在模板目录的 `langs/` 下，文件名使用语言代码，例如
`langs/en.json` 或 `langs/zh-CN.json`；然后可以在设置中的“语言”下拉框切换。语言文件只需提供要覆盖的条目，缺失条目会回退到 `lang.json`。

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

`goals` 中的自动规则必须且只能使用一种规则。叶子节点可以省略规则操作，此时它不会从存档读取数据，只能通过 Ctrl+鼠标左键/右键手动标记或取消；需要显示或被其他规则引用的节点应设置唯一 `id`；`completion_rule` 必须指向一个显式 `id`。

### 单个事实

达到 `target` 即完成，省略 `target` 时默认为 `1`：

```json
{
  "id": "goal/debris",
  "fact": "stat/minecraft:mined/minecraft:ancient_debris",
  "target": 20,
  "view": {"name_key": "goal.debris", "icon": "icons/debris.png"}
}
```

`view` 只控制显示。`name_key` 是 `lang.json` 中的本地化 key；`icon` 是可选的图标路径；`type` 可选为 `task`、`goal` 或 `challenge`，用于选择 Minecraft 进度边框，省略时默认为 `task`。
普通图标必须是模板目录内的规范化相对路径，不能指向目录外。

父级或会被 `layout.json`、`ref`、`completion_rule` 引用的节点应提供唯一 `id`。
收集类规则的子项不需要 `id`，直接在子项上提供 `view` 即可：

```json
{
  "fact": "criterion/minecraft:adventure/adventuring_time/minecraft:badlands",
  "view": {
    "name_key": "criterion.badlands",
    "icon": "icons/check.xpm"
  }
}
```

`view` 可以用于 `all`、`any` 和 `count` 的任意子项。这样不会为仅用于展示的子项生成不会被引用的虚假 ID。
收集类子项通常使用自己的 `name_key` 和 `icon`，父级的 `view` 仍用于收集卡片标题和主图标。

`templates/26.2/template.json` 已采用这个格式；对应的 `criterion.*` 标题位于同目录的
`lang.json`，并且已经提供中文翻译。例如：

```json
{
  "fact": "criterion/minecraft:husbandry/complete_catalogue/minecraft:all_black",
  "view": {
    "name_key": "criterion.all_black",
    "icon": "icons/check.xpm"
  }
}
```

```json
{
  "criterion.all_black": "全黑"
}
```

也可以使用全局图标命名空间：`minecraft/block/stone` 查找程序根目录的
`assets/vender/minecraft/block/stone.png`。命名空间可替换为 `assets/vender/` 下的任意文件夹；命名空间图标可省略 `.png`，模板目录内的路径按实际文件名填写。标准模板位于程序根目录的
`templates/` 下，因此这里的“程序根目录”就是包含 `templates/` 和 `assets/` 的目录。

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
    "icon": "minecraft/block/stone",
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

## 检查模板

修改 JSON 后重启 Beacon 或重新选择模板。加载失败时优先检查：

1. 三个必需文件是否都在同一个模板目录中。
2. JSON 是否有尾随逗号、重复字段或拼写错误；Beacon 会拒绝未知字段。
3. 每个 `id` 是否唯一，`ref` 和 `completion_rule` 指向的 ID 是否存在。
4. `layout.json` 的 `nodes` 是否与 `template.json` 中的 ID 一致。
5. 当前存档的 `DataVersion` 是否落在模板范围内。
6. 普通 icon 是否存在于模板目录内；命名空间 icon 是否存在于程序根目录的 `assets/vender/`。

可运行的大型示例见 `templates/26.2/`。
