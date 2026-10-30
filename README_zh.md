<div align="center">
  <img src="assets/app-icon.png" alt="Beacon" width="96">
  <h1>Beacon 中文文档</h1>
  <p>面向 Minecraft 速通的进度（Advancements）追踪器</p>
  <p><a href="README.md">English</a></p>
</div>

Beacon 是一个面向 Minecraft 速通的进度追踪器。它读取本地存档中的进度（Advancements）和统计数据，结合模板中的自定义规则，在主窗口和可选悬浮窗中实时展示目标的完成情况。你可以通过模板包配置自己的速通路线。

## 功能概览

- **实时追踪进度**：自动发现世界和玩家，在游戏过程中刷新当前存档。
- **读取进度与统计**：读取标准的 `stats/<uuid>.json` 和 `advancements/<uuid>.json`，不会修改存档。
- **主窗口与悬浮窗**：可以使用完整追踪界面，也可以在游戏过程中使用紧凑、可配置的悬浮窗。
- **模板版本检查**：每个模板声明支持的 Minecraft 存档 `DataVersion` 范围。
- **稳定刷新**：文件暂时不可用或仍在写入时，保留上次成功读取的状态并自动重试。
- **自定义路线**：通过模板包定义目标、规则、语言、布局和图标。
- **跨平台桌面程序**：面向 Windows、macOS 和 Linux。

## 用户指南

### 快速开始

1. 构建或下载对应平台的 Beacon。
2. 在安装根目录的 `templates/` 下放入至少一个模板包，格式见[自定义模板](#自定义模板)。
3. 启动 `beacon`，打开设置，选择 Minecraft 游戏目录和模板。
4. 选择世界与玩家，开始游戏；存档变化后，Beacon 会自动更新清单。

默认会自动检测以下 Minecraft 游戏目录：

| 平台 | 默认游戏目录 |
| --- | --- |
| Windows | `%APPDATA%/.minecraft` |
| macOS | `~/Library/Application Support/minecraft` |
| Linux | `~/.minecraft` |

安装根目录是 `beacon` 启动器所在目录。Beacon 不附带模板；没有模板时，会以 `no templates found` 退出。

设置中的“自动探测”默认开启。切换到 Minecraft 窗口时，Beacon 会读取游戏进程的 `--gameDir` 启动参数，并在其 `saves` 目录中查找正在游玩的世界。探测失败时保留当前游戏目录。自动探测支持 Windows、macOS 和启用 X11 支持的 Linux 构建；Wayland 请手动填写目录。探测要求 `--gameDir` 使用绝对路径。

### 快捷操作

| 操作 | 方式 |
| --- | --- |
| 重开本轮（清除手动标记并重新读取存档，不修改存档） | Windows/Linux：`Ctrl+R`；macOS：`Cmd+R` |
| 手动标记完成 / 撤销手动标记 | Windows/Linux 按住 `Ctrl`，macOS 按住 `Cmd`，左键 / 右键点击图标 |
| 移动悬浮窗或调整大小 | 拖动悬浮窗 / 拖动其边缘 |
| 关闭设置窗口 | `Esc` |

设置窗口底部也列出了重开本轮和手动标记的快捷键提示。

### 数据与异常恢复

Beacon 只读取选中的 Minecraft 存档。设置保存在安装目录（`beacon` 启动器所在目录）下的 `config/settings.json`，头像缓存仍保存在各平台的应用数据目录下的 `cache/avatars/`。

程序会频繁检查当前世界，并定期扫描其他世界的变化。如果 Minecraft 正在写入文件，Beacon 会保留上次成功读取的状态，提示数据暂时无法刷新，并自动重试。其他世界损坏或无法访问时，程序会单独提示，不会中断当前世界的追踪。

### 程序更新

支持更新的完整发布包会在启动时后台检查一次签名更新。发现新版本后，主窗口左下角“设置”旁会出现“更新到 <版本>”按钮；点击即可退出 Beacon 并应用更新，更新后需要手动重新启动 Beacon。也可以关闭 Beacon 后手动运行 `updater/beacon-updater --check` 再运行 `--apply`；Windows 使用 `updater\beacon-updater.exe`。程序与资源分别更新，未变化的资源不会重复下载；模板和用户设置不由更新器管理。每个版本安装在上一版本旁边并原子切换。切换前失败时当前版本保持不变；`--rollback` 可回到上一个可用版本。

## 开发者指南

### 环境要求

- C++20 编译器
- 独立更新器使用仓库内置的 YLT/coro_http 客户端，并需要 OpenSSL 和 libarchive 开发库（macOS：`brew install libarchive`；Ubuntu：`libarchive-dev`）
- CMake 3.21 或更高版本
- 支持子模块的 Git
- 由平台原生包管理器提供的 OpenSSL 和 Catch2；SDL3 可以由原生包管理器提供，
  也可以通过上游 CMake 构建

桌面程序使用 SDL3、FreeType（macOS：`brew install freetype`；Ubuntu：`libfreetype-dev`）和 OpenSSL；核心库可以在不启用桌面程序的情况下构建。Windows CI 使用 [`vcpkg.json`](vcpkg.json) 中的依赖清单，Linux 和 macOS 则使用系统包管理器提供的依赖。Ubuntu 24.04 镜像没有 SDL3 开发包，因此 CI 会使用 SDL 官方 CMake 项目构建 SDL3 3.2.6。

### 从源码构建

先克隆仓库及其子模块，再使用 CMake 配置：

```sh
git clone --recurse-submodules https://github.com/liaozhangsheng/beacon.git
cd beacon

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Windows 下还需要在配置时加入 vcpkg 工具链：

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=x64-windows
```

运行程序：

```sh
./build/beacon
```

Windows 下运行 `build/beacon.exe`。如果克隆时没有使用 `--recurse-submodules`，可以手动初始化子模块：

```sh
git submodule update --init --recursive
```

只构建不依赖桌面的核心库：

```sh
cmake -S . -B build-core -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBEACON_BUILD_DESKTOP=OFF
cmake --build build-core --parallel
```

### 更新包与签名

生成 Ed25519 发布密钥，并在构建配置中嵌入公钥：

```sh
openssl genpkey -algorithm ED25519 -out release-private.pem
openssl pkey -in release-private.pem -pubout -out release-public.pem
cmake -S . -B build -DBEACON_UPDATE_PUBLIC_KEY_FILE=/absolute/path/release-public.pem
```

私钥不得提交到仓库或放入安装包。使用 GitHub Actions 发布时，将公钥 PEM 保存到仓库变量 `BEACON_UPDATE_PUBLIC_KEY`，将私钥 PEM 保存到 Secret `BEACON_UPDATE_SIGNING_KEY`。CI 用 [`scripts/package_updates.py`](scripts/package_updates.py) 处理 CPack ZIP，生成程序组件、资源组件、发布清单和带版本元数据的完整包，再对清单签名。发布时使用生成的完整包；原始 CPack 包缺少更新所需的元数据。未嵌入公钥的构建会拒绝更新，但仍可回滚；未配置签名私钥时，CI 只发布完整包，不发布更新清单。

### 测试与格式检查

启用测试特性、构建并运行回归测试：

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Windows 下请在配置命令中同时加入 vcpkg 工具链和 `-DVCPKG_TARGET_TRIPLET=x64-windows`。

Linux 下可以检查代码格式：

```sh
cmake --build build --target format-check
```

持续集成工作流覆盖 Linux、Windows 和 macOS：[`.github/workflows/ci.yml`](.github/workflows/ci.yml)。

### 项目结构

| 路径 | 职责 |
| --- | --- |
| `src/core` | 模板编译、规则求值和共享数据模型 |
| `src/io` | 安全文件访问与文件变化处理 |
| `src/minecraft` | 存档发现、统计和进度解析 |
| `src/app` | 运行时刷新、持久化和手动进度 |
| `src/ui` | 展示、布局、渲染和桌面交互 |
| `assets` | 程序界面资源：`fonts/`、`widgets/`、`progress/`、`app-icon.png`、`default-avatar.png` 和 `settings-background.png` |
| `test` | 核心、文件系统、展示、界面和打包检查 |

### 自定义模板

模板是 `templates/` 下的独立目录。下面三个 JSON 文件是必需的，`assets/` 目录可选：

```text
templates/<name>/
├── template.json   # 目标、事实、规则和 Minecraft 兼容范围
├── lang.json       # 本地化文本
├── layout.json     # 主窗口与悬浮窗布局
└── assets/         # 此模板使用的可选资源
```

推荐将模板专用资源放在 `assets/` 下，也可以使用模板内部的其他目录。`view.icon` 必须填写相对于 `template.json` 所在目录的精确文件路径，例如 `"assets/apple.png"`。Beacon 不查询全局资源名，也不会自动补充扩展名，路径不能越出模板目录。模板加载时会检查所引用的文件是否存在；某项不需要图标时，可以省略 `icon` 或将其设为空字符串。复制整个模板目录即可一并分享其中的资源。

程序包只包含界面所需资源和字体。完整格式、校验规则和示例请参阅内置的 [`beacon-custom-template` skill](.agents/skills/beacon-custom-template/SKILL.md)。

统计计数器可用 `prefix` 配置有序的多个阶段。每个直接子项使用一个 `stat/` 事实、正整数 `target` 和自己的 `view`，
布局继续通过 `source: "nodes"` 引用父节点。同一个位置显示首个未完成阶段的图标、名称和数量，全部完成后保留最后阶段并高亮；
主窗口的手动加一增加当前阶段的计数，全部完成后不再增加；手动减一减少当前阶段的计数，当前计数为 0 时重新打开上一阶段。计数器在悬浮窗中保持固定位置。普通计数器可与多阶段计数器放在同一组。
各阶段使用存档中的累计统计，消耗物品不会减少计数，也无需单独持久化阶段状态。
重复捡起同一物品也会增加拾取统计，因此这些计数不代表当前持有量。各阶段必须依次完成。

`layout.json` 的 `main` 中，`source: "children"` 的分组可以配置子项状态样式：

```json
"item_style": {
  "pending": { "emphasis": "highlight" },
  "done": { "emphasis": "muted", "decoration": "strikethrough" }
}
```

`emphasis` 支持 `highlight`、`muted`，同时影响图标和文字；默认未完成（`pending`）为 `muted` 暗色，完成（`done`）为 `highlight` 高亮。显式配置可覆盖默认值。
`decoration` 支持 `none`（默认）和 `strikethrough`；删除线贯穿图标和文字。
`visibility` 支持 `visible`（默认）和 `blank`。例如 `"done": { "visibility": "blank" }` 会在完成后保留空位，
跳过图标、文字、光晕、装饰和点击处理。`overlay` 不支持此配置，悬浮窗通过自己的完成动画和轮播逻辑处理收集项。
这些样式不作用于收集组的标题和总进度；嵌套布局组需要各自配置，不继承父组样式。
`pending` 和 `done` 及其属性均可省略，省略时使用上述默认值；`blank` 优先于明暗和删除线。
留空的子项无法直接点击撤销完成。不要把 `item_style` 放在 `template.json`、`overlay` 或其他 `source` 的分组中，
否则加载会报错；未知状态、属性、枚举值以及非字符串属性值同样会被拒绝。修改后重新选择模板或重启 Beacon。

## Minecraft 商标声明

Minecraft 是 Microsoft Corporation 的商标。Beacon 是独立的非官方项目，与 Mojang Studios 或 Microsoft 没有关联，也未获得其认可或赞助。

## 许可证

Beacon 使用 [MIT License](LICENSE) 发布。第三方组件和资源仍适用其各自的许可证与声明，包括 [GNU Unifont 的 SIL Open Font License](assets/fonts/Unifont-LICENSE.txt)。

本软件部分基于 [FreeType 项目](https://freetype.org)的工作，并按照 [FreeType License](licenses/FreeType-LICENSE.txt) 使用。
