<div align="center">
  <img src="assets/beacon.png" alt="Beacon" width="96">
  <h1>Beacon 中文文档</h1>
  <p>面向 Minecraft 速通的进度追踪器</p>
  <p><a href="README.md">English</a></p>
</div>

Beacon 是一个面向 Minecraft 速通的进度追踪器。它读取本地存档中的进度、统计和自定义规则，并将目标实时显示在主窗口和可选悬浮窗中。项目同时提供内置模板和可扩展的模板包格式，方便开发者制作自己的路线。

## 功能概览

- **实时追踪进度**：自动发现世界和玩家，在游戏过程中刷新当前存档。
- **读取进度与统计**：读取标准的 `stats/<uuid>.json` 和 `advancements/<uuid>.json`，不会修改存档。
- **主窗口与悬浮窗**：可以使用完整追踪界面，也可以在游戏过程中使用紧凑、可配置的悬浮窗。
- **按版本匹配模板**：根据 Minecraft 存档的 `DataVersion` 选择兼容模板。
- **稳定刷新**：文件暂时不可用或仍在写入时，保留上次成功读取的状态并自动重试。
- **自定义路线**：通过模板包定义目标、规则、语言、布局和图标。
- **跨平台桌面程序**：面向 Windows、macOS 和 Linux。

## 用户指南

### 快速开始

1. 构建或下载对应平台的 Beacon。
2. 启动 `beacon`。
3. 打开设置，选择 Minecraft 游戏目录和模板。
4. 选择世界与玩家，开始游戏；存档变化后，Beacon 会自动更新清单。

默认会自动检测以下 Minecraft 游戏目录：

| 平台 | 默认游戏目录 |
| --- | --- |
| Windows | `%APPDATA%/.minecraft` |
| macOS | `~/Library/Application Support/minecraft` |
| Linux | `~/.minecraft` |

当前内置模板包括：

| 模板 | 用途 |
| --- | --- |
| `1.16` | Minecraft 1.16 速通进度 |
| `26.1` | Minecraft 26.1 速通进度 |
| `26.2` | Minecraft 26.2 速通进度 |
| `26.3` | Minecraft 26.3 速通进度 |
| `all_potions` | 与版本无关的全药水清单 |

### 数据与异常恢复

Beacon 只读取选中的 Minecraft 存档。设置保存在程序目录下的 `config/settings.json`，头像缓存仍保存在各平台的应用数据目录下的 `cache/avatars/`。

程序会高频检查当前世界，并定期扫描其他世界的变化。如果 Minecraft 正在写入文件，Beacon 会保留上次有效进度，显示数据过期状态并自动重试。无关世界损坏或无法访问时，程序会单独提示，不会阻断当前追踪。

## 开发者指南

### 环境要求

- C++20 编译器
- CMake 3.21 或更高版本
- 支持子模块的 Git
- 由平台原生包管理器提供的 OpenSSL 和 Catch2；SDL3 可以由原生包管理器提供，
  也可以通过上游 CMake 构建

桌面程序使用 SDL3 和 OpenSSL；核心库可以在不启用桌面程序的情况下构建。Windows CI 使用 [`vcpkg.json`](vcpkg.json) 中的 manifest，Linux 和 macOS 则使用原生依赖。Ubuntu 24.04 镜像没有 SDL3 开发包，因此 CI 会使用 SDL 官方 CMake 项目构建 SDL3 3.2.6。

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
| `templates` | 内置模板包 |
| `assets` | 字体、界面资源和 Minecraft 图标 |
| `test` | 核心、文件系统、展示、界面和打包检查 |

### 自定义模板

模板是 `templates/` 下的独立目录。每个模板至少包含：

```text
templates/<name>/
├── template.json   # 目标、事实、规则和 Minecraft 兼容范围
├── lang.json       # 本地化文本
└── layout.json     # 主窗口与悬浮窗布局
```

完整的格式、校验规则和示例请参阅内置的 [`beacon-custom-template` skill](.agents/skills/beacon-custom-template/SKILL.md)。模板可以引用内置资源目录中的图标，也可以附带自己的语言和图片资源。

## Minecraft 资源与商标声明

[`assets/vender/minecraft/`](assets/vender/minecraft/) 下的图标来自 **Minecraft Java Edition 26.1 原版资源**。这些资源仅用于 Beacon 这个非官方进度追踪器中的目标识别，不构成 Minecraft 游戏或其资源包的替代品。

Minecraft 是 Microsoft Corporation 的商标。Beacon 是独立的非官方项目，与 Mojang Studios 或 Microsoft 没有关联，也未获得其认可或赞助。

## 许可证

Beacon 使用 [MIT License](LICENSE) 发布。第三方组件和资源仍适用其各自的许可证与声明，包括 [GNU Unifont 的 SIL Open Font License](assets/fonts/Unifont-LICENSE.txt)。
