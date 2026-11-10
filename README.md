> [!TIP]
> **Recommended: [Advancely](https://github.com/LNXSeus/Advancely)** — a highly customizable Minecraft progress tracker that goes beyond advancements.
>
> **Beacon's development drew on LNXSeus's Advancely as a reference. Thank you to the author for sharing the project.**

<div align="center">
  <img src="assets/app-icon.png" alt="Beacon" width="96">
  <h1>Beacon</h1>
  <p><strong>An advancement tracker for Minecraft speedruns.</strong></p>
  <p>
    <a href="https://github.com/liaozhangsheng/beacon/actions/workflows/ci.yml"><img src="https://github.com/liaozhangsheng/beacon/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
    <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-2ea44f.svg" alt="MIT License"></a>
  </p>
  <p><a href="#beacon">English</a> · <a href="README_zh.md">中文</a></p>
</div>

Beacon reads Minecraft save data and turns advancements, statistics, and custom rules into a live speedrun checklist. It provides a main window, an optional overlay, and a template package format for describing your own runs.

<p align="center">
  <img src="docs/overlay-en.gif" alt="The Beacon overlay with the bundled 1.16.1 template: unfinished advancements and collection entries scroll past while statistics stay fixed">
  <br><sub>50 fps · 5 seconds</sub>
</p>
<p align="center">
  <img src="docs/mainwindow.png" alt="The Beacon main window with the bundled 1.16.1 template: advancements by category, collection checklists, statistics, the leaderboard, and the status bar" width="720">
</p>

## Highlights

- **Live advancement tracking** — discovers worlds and players, then refreshes the active save while you play.
- **Advancements and statistics** — reads the standard `stats/<uuid>.json` and `advancements/<uuid>.json` files without modifying the save.
- **Main window and overlay** — use the full tracker or a compact, configurable overlay during a run.
- **Version-aware templates** — each template declares the Minecraft `DataVersion` range it supports.
- **Resilient refresh** — keeps the last successful state visible when a file is temporarily unavailable or still being written.
- **Customizable runs** — define goals, rules, languages, layouts, and icons in a template package.
- **Cross-platform desktop app** — designed for Windows, macOS, and Linux.

## For users

### Getting started

1. Build or download Beacon for your platform.
2. Launch `beacon`. It starts with the bundled **1.16.1 All Advancements** template; open settings to choose your Minecraft game directory, or another template from `templates/` in the installation root (see [Custom templates](#custom-templates)).
3. Select a world and player, then start playing. Beacon updates the checklist as the save changes.

The default Minecraft directories are detected automatically:

| Platform | Default game directory |
| --- | --- |
| Windows | `%APPDATA%/.minecraft` |
| macOS | `~/Library/Application Support/minecraft` |
| Linux | `~/.minecraft` |

The installation root is the directory containing the `beacon` launcher. Beacon ships the `templates/1.16.1` template, which tracks every Minecraft Java Edition 1.16.1 advancement in Chinese and English. Without any template, it exits with `no templates found`.

The interface defaults to Chinese. In settings, change **界面语言** (Interface language) to **English** to switch the app's labels.

The **Auto detect** setting is on by default. When you switch to the Minecraft window, Beacon reads the game's `--gameDir` argument and looks for the active world under `saves`. If detection fails, Beacon keeps the current game directory. Automatic detection is supported on Windows, macOS, and Linux builds with X11 support; on Wayland, choose the directory manually. Detection requires an absolute `--gameDir` path.

### Shortcuts

| Action | Shortcut |
| --- | --- |
| Restart the run (clear manual marks and re-read the save; the save itself is not modified) | Windows/Linux: `Ctrl+R`; macOS: `Cmd+R` |
| Mark a goal done / undo a manual mark | Hold `Ctrl` on Windows/Linux or `Cmd` on macOS, then left / right click its icon |
| Move or resize the overlay | Drag the overlay / drag its edges |
| Close settings | `Esc` |

The restart and manual-mark shortcuts are also listed at the bottom of the settings window.

Settings let you set the main window and overlay width and height independently (1–16384 logical pixels). Click **Save** to apply them. Resizing either window by its edges automatically saves its size after about 500 ms; Beacon restores those sizes on the next launch. Maximizing or minimizing does not replace the normal size. The overlay enforces a minimum height based on its layout and scale, growing only when needed and keeping taller sizes. Small widths and main-window heights can clip content and controls.

### Data and recovery behavior

Beacon reads save files without modifying them. Settings are stored next to the `beacon` launcher under `config/settings.json`; templates belong under `templates/` in that same directory. Installed program files and built-in assets live under `versions/<version>/`, separately from templates and settings. Downloaded skins and leaderboards are cached in `cache/` in that same directory; skins are looked up through Mojang's session server.
Downloads go through `HTTPS_PROXY` or `ALL_PROXY` (honouring `NO_PROXY`) when set, and otherwise through the system's HTTPS proxy on macOS and Windows; the proxy must be an HTTP proxy that supports `CONNECT`.

The active world is checked frequently; other worlds are scanned periodically for changes. If Minecraft is still writing a file, Beacon keeps the last successfully loaded state, indicates that the data could not be refreshed, and retries. A damaged or inaccessible world is reported without interrupting tracking of the active world.

### Application updates

Full packages with update support check for signed updates in the background once at startup. If a newer version is available, an **Update to &lt;version&gt;** button appears next to Settings at the bottom left of the main window; click it to exit Beacon and apply the update. Beacon does not restart automatically. You can also close Beacon and run `updater/beacon-updater --check` followed by `--apply` (`updater\beacon-updater.exe` on Windows). Program files and assets update separately; unchanged assets are not downloaded again. Templates and user settings are never managed by the updater. Each version is installed alongside the previous one, and Beacon switches to it atomically. Failures before the switch leave the current version untouched; `--rollback` returns to the previous usable version.

## For developers

### Requirements

- A C++20 compiler
- CMake 3.21 or newer
- Git with submodule support
- Ninja for the build commands below
- Desktop app: SDL3 3.2 or newer, FreeType, and OpenSSL development libraries
- Standalone updater: OpenSSL and libarchive development libraries
- Tests: Catch2 3 or newer, Python 3, and the `openssl` command-line tool

Windows and macOS release builds take their libraries from the dependency manifest in [`vcpkg.json`](vcpkg.json), whose `builtin-baseline` pins every port version; CI checks out vcpkg at that commit (triplets `x64-windows` and `arm64-osx-dynamic`). Linux uses Ubuntu 24.04 packages and builds SDL3 3.2.6 from source. The [CI workflow](.github/workflows/ci.yml) lists the platform dependencies and compiler setup, including LLVM 18 on macOS. For local macOS builds, Homebrew's `freetype libarchive openssl@3 sdl3` also work; on Ubuntu, the corresponding packages are `libfreetype-dev` and `libarchive-dev`.

Core-only builds do not require SDL3, FreeType, OpenSSL, or libarchive. Python 3 is also used to prepare update packages; the `openssl` command-line tool is used to generate release keys and sign manifests.

### Build from source

The examples use Bash syntax. On Windows, configure the MSVC build environment and set `VCPKG_ROOT` first. Clone the repository with its submodules, then configure it with CMake:

```sh
git clone --recurse-submodules https://github.com/liaozhangsheng/beacon.git
cd beacon

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

On Windows, configure with the vcpkg toolchain as well (on macOS, use `-DVCPKG_TARGET_TRIPLET=arm64-osx-dynamic` to build against the pinned libraries):

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=x64-windows
```

Building copies the bundled templates to `build/templates/`; put your own template directories beside them. Then run:

```sh
./build/beacon
```

On Windows, run `build/beacon.exe` instead. If the submodules were cloned without `--recurse-submodules`, initialize them manually:

```sh
git submodule update --init --recursive
```

To build only the non-desktop libraries:

```sh
cmake -S . -B build-core -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBEACON_BUILD_DESKTOP=OFF \
  -DBEACON_BUILD_UPDATER=OFF
cmake --build build-core --parallel
```

### Update packages and signing

Generate an Ed25519 release key pair and embed the public key when configuring the build:

```sh
openssl genpkey -algorithm ED25519 -out release-private.pem
openssl pkey -in release-private.pem -pubout -out release-public.pem
cmake -S . -B build -DBEACON_UPDATE_PUBLIC_KEY_FILE=/absolute/path/release-public.pem
```

Keep the private key out of the repository and installation packages. For GitHub Actions releases, set the repository variable `BEACON_UPDATE_PUBLIC_KEY` to the public-key PEM and the secret `BEACON_UPDATE_SIGNING_KEY` to the matching private-key PEM. The release workflow verifies that the keys match before signing manifests.

Normal branch builds produce downloadable CI artifacts. To publish a GitHub release, push a `v<version>` tag that matches the version in `CMakeLists.txt` and `vcpkg.json`, such as `v1.0.0`. CI runs [`scripts/package_updates.py`](scripts/package_updates.py) on the CPack ZIP to produce program and assets components, a release manifest, and a full package with version metadata. The release job signs the manifest when a signing key is configured; otherwise, it omits update manifests from the release.

`cmake --build build --target package` creates a raw CPack ZIP. That ZIP lacks the metadata required for updates; distribute the generated full package instead. Builds without an embedded public key cannot download or apply updates; local rollback remains available.

### Tests and formatting

Configure with the test feature enabled, build, and run the regression suite:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

On Windows, add the vcpkg toolchain and `-DVCPKG_TARGET_TRIPLET=x64-windows` to the configure command.

With `clang-format` installed, check formatting with:

```sh
cmake --build build --target format-check
```

The continuous integration workflow covers Linux, Windows, and macOS: [`.github/workflows/ci.yml`](.github/workflows/ci.yml).

### Project layout

| Path | Responsibility |
| --- | --- |
| `src/core` | Template compilation, rule evaluation, and shared data models |
| `src/io` | Safe filesystem and file-change handling |
| `src/minecraft` | Save discovery plus stats and advancement parsing |
| `src/app` | Runtime refresh, persistence, and manual progress |
| `src/ui` | Presentation, layout, rendering, and desktop interaction |
| `assets` | App UI resources: `fonts/`, `widgets/`, `progress/`, `app-icon.png`, `default-avatar.png`, and `settings-background.png` |
| `test` | Core, filesystem, presentation, UI, and package checks |

### Custom templates

Templates are self-contained directories under `templates/`. The three JSON files below are required; `langs/` and `assets/` are optional:

```text
templates/<name>/
├── template.json   # goals, facts, rules, and Minecraft compatibility
├── lang.json       # localized text
├── layout.json     # main-window and overlay layout
├── langs/          # optional translations, e.g. en.json or zh-CN.json
└── assets/         # optional resources used by this template
```

The program bundle contains only app UI resources and fonts. Keep template-specific images in the template directory so they can be shared with it.
The bundled [`beacon-custom-template` skill](.agents/skills/beacon-custom-template/SKILL.md) covers icon paths, validation, and the full schema.

`lang.json` provides the default text. The **Template language** setting selects a translation from `langs/`, with missing entries falling back to `lang.json`; **Interface language** controls Beacon's labels. Restart after adding a template. To reload an edit, restart or switch to another template and back.

Statistic rules can use `prefix` for cumulative stages; `view.unit` formats distance, time, and damage statistics while targets stay in raw units.
Main-window layouts can also show a community leaderboard from a public Google Sheet.

## Minecraft trademark

Minecraft is a trademark of Microsoft Corporation. Beacon is an independent, unofficial project and is not affiliated with, endorsed by, or sponsored by Mojang Studios or Microsoft. The bundled textures under `assets/` and `templates/` come from Minecraft and are copyright Mojang AB / Microsoft; they are used under the [Minecraft Usage Guidelines](https://www.minecraft.net/usage-guidelines) and are not covered by Beacon's license. [NOTICE.txt](NOTICE.txt) lists them.

## License

Beacon is released under the [MIT License](LICENSE). Third-party components and resources retain their own licenses and notices, including [GNU Unifont's SIL Open Font License](assets/fonts/Unifont-LICENSE.txt). Packages include the license of every bundled library under `versions/<version>/licenses/` and `updater/licenses/`; [NOTICE.txt](NOTICE.txt) summarizes them.

This software is based in part on the work of the [FreeType Project](https://freetype.org), used under the [FreeType License](https://freetype.org/license.html).
