# Beacon

Minecraft 速通进度追踪器。读取本地存档的统计和进度文件，在主窗口和悬浮窗中显示模板定义的目标。

## 从源码构建

需要 C++20 编译器、CMake 3.21+、Git；初始化子模块后安装 SDL3 和 OpenSSL（可使用仓库的 vcpkg manifest）。

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/beacon
```

使用 vcpkg 时，在配置命令中增加 `-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake`。
资源随可执行文件复制；设置和头像缓存保存在各平台的用户数据目录中（分别位于 `config/settings.json` 和 `cache/avatars/`）。
配置只接受当前完整格式；旧配置需要重新设置游戏目录、模板和外观。

[自定义模板 Skill](.agents/skills/beacon-custom-template/SKILL.md)

## 第三方资源与声明

`assets/fonts/Unifont.ttf` 使用 GNU Unifont，按 SIL Open Font License 1.1
分发；完整许可证见
[`assets/fonts/Unifont-LICENSE.txt`](assets/fonts/Unifont-LICENSE.txt)。

`assets/vender/minecraft/` 中的图标来自 Minecraft 原版资源，仅用于本项目的
非官方进度追踪功能。Minecraft 是 Microsoft Corporation 的商标。本项目不是
Mojang Studios 或 Microsoft 的官方产品，也未获其批准、关联或认可。

存档发现、文件变化检测和规则求值在后台执行。当前存档每 250 毫秒检查一次，其他世界每秒扫描一次；新增世界会触发提前扫描。统计和进度文件分别缓存，未变化的版本判定也会复用。
读取失败时界面标明数据过期，显示错误和上次读取时间，保留上次成功的进度并重试；恢复前暂停手动操作及完成动画。无关损坏世界会单独提示，不阻断正常世界。
切换语言或更新相同规则的展示资源会保留本轮手动进度；切换游戏目录、世界、玩家或规则时开始新一轮。同路径替换世界目录或游戏时间回退也会重开一轮；也可点击主窗口的“重开本轮”清除手动进度并重新读取存档。
切换语言只加载文本，复用规则、布局和图片；编辑模板或图片后，点击设置中的“保存”重新加载。
设置先验证资源并保存，成功后才切换运行配置。

头像下载在后台线程上执行，切换玩家或退出时取消正在进行的网络请求。
不需要桌面界面时可使用 `-DBEACON_BUILD_DESKTOP=OFF` 构建核心库；核心库不依赖 SDL 或 OpenSSL。回归测试通过 `-DBUILD_TESTING=ON` 启用，需要 Catch2 3，然后运行 `ctest --test-dir build --output-on-failure`。
