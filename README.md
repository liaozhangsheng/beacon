<div align="center">
  <img src="assets/beacon.png" alt="Beacon" width="96">
  <h1>Beacon</h1>
  <p><strong>A focused progress tracker for Minecraft speedruns.</strong></p>
  <p>
    <a href="https://github.com/liaozhangsheng/beacon/actions/workflows/ci.yml"><img src="https://github.com/liaozhangsheng/beacon/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
    <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-2ea44f.svg" alt="MIT License"></a>
  </p>
  <p><a href="#beacon">English</a> · <a href="README_zh.md">中文</a></p>
</div>

Beacon reads Minecraft save data and turns advancements, statistics, and custom rules into a live speedrun checklist. It provides a main window, an optional overlay, bundled templates, and a small template system for runs that do not fit the defaults.

## Highlights

- **Live progress tracking** — discovers worlds and players, then refreshes the active save while you play.
- **Advancements and statistics** — reads the standard `stats/<uuid>.json` and `advancements/<uuid>.json` files without modifying the save.
- **Main window and overlay** — use the full tracker or a compact, configurable overlay during a run.
- **Version-aware templates** — the bundled templates are selected by Minecraft `DataVersion`.
- **Resilient refresh** — keeps the last successful state visible when a file is temporarily unavailable or still being written.
- **Customizable runs** — define goals, rules, languages, layouts, and icons in a template package.
- **Cross-platform desktop app** — designed for Windows, macOS, and Linux.

## For users

### Getting started

1. Build or download Beacon for your platform.
2. Launch `beacon`.
3. Open settings and choose your Minecraft game directory and template.
4. Select a world and player, then start playing. Beacon updates the checklist as the save changes.

The default Minecraft directories are detected automatically:

| Platform | Default game directory |
| --- | --- |
| Windows | `%APPDATA%/.minecraft` |
| macOS | `~/Library/Application Support/minecraft` |
| Linux | `~/.minecraft` |

The bundled templates currently include:

| Template | Purpose |
| --- | --- |
| `1.16` | Minecraft 1.16 speedrun advancements |
| `26.1` | Minecraft 26.1 speedrun advancements |
| `26.2` | Minecraft 26.2 speedrun advancements |
| `26.3` | Minecraft 26.3 speedrun advancements |
| `all_potions` | A version-independent all-potions checklist |

The **自动探测 (Auto detect)** setting is on by default. When enabled, focusing Minecraft reads its process’s `--gameDir` argument and uses the existing active-world discovery under `saves`. Failed detection keeps the current directory. Supported on Windows, macOS, and Linux builds with X11 support; use a manual directory on Wayland. Relative paths and launches without `--gameDir` are not supported.

### Shortcuts

| Action | Shortcut |
| --- | --- |
| Restart the run (clear manual marks and re-read the save; the save itself is not modified) | Windows/Linux: `Ctrl+R`; macOS: `Cmd+R` |
| Mark a goal done / undo a manual mark | Hold `Ctrl` on Windows/Linux or `Cmd` on macOS, then left / right click its icon |
| Move or resize the overlay | Drag the overlay / drag its edges |
| Close settings | `Esc` |

The restart and manual-mark shortcuts are also listed at the bottom of the settings window.

### Data and recovery behavior

Beacon only reads the selected Minecraft save. Settings are stored in the installation directory (next to the `beacon` launcher) under `config/settings.json`; the avatar cache remains in the platform-specific application data directory under `cache/avatars/`.

The active world is checked frequently; other worlds are scanned periodically for changes. If Minecraft is still writing a file, Beacon keeps the last valid progress, reports the stale data state, and retries. A damaged or inaccessible unrelated world is reported without stopping the active tracker.

### Standalone updates

Update-ready full packages check for signed updates in the background once at startup. If a newer version is available, an **更新到 &lt;version&gt;** (Update to) button appears next to Settings at the bottom left of the main window; click it to exit Beacon and apply the update. Beacon does not restart automatically. You can also close Beacon and run `updater/beacon-updater --check` followed by `--apply` (`updater\beacon-updater.exe` on Windows). Runtime and assets update separately; unchanged assets are not downloaded again. Templates and user settings are never managed by the updater. Each version is installed beside the previous one and selected atomically. Failures before the switch leave the current version untouched; `--rollback` returns to the previous usable version.

## For developers

### Requirements

- A C++20 compiler
- OpenSSL and libarchive development libraries for the standalone updater (macOS: `brew install libarchive`; Ubuntu: `libarchive-dev`)
- CMake 3.21 or newer
- Git with submodule support
- OpenSSL and Catch2 from the platform's native package manager; SDL3 from the
  native package manager or an upstream CMake build

The desktop application uses SDL3, FreeType (macOS: `brew install freetype`; Ubuntu: `libfreetype-dev`), and OpenSSL; the core libraries can be built without them. The Windows CI job uses the manifest in [`vcpkg.json`](vcpkg.json); Linux and macOS use native dependencies instead. On Ubuntu 24.04, the CI job builds SDL3 3.2.6 with SDL's upstream CMake project because that image does not provide an SDL3 development package.

### Build from source

Clone the repository with its submodules, then configure it with CMake:

```sh
git clone --recurse-submodules https://github.com/liaozhangsheng/beacon.git
cd beacon

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

On Windows, configure with the vcpkg toolchain as well:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=x64-windows
```

Run the application:

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
  -DBEACON_BUILD_DESKTOP=OFF
cmake --build build-core --parallel
```

### Update packages and signing

Generate an Ed25519 release key pair and embed the public key when configuring the build:

```sh
openssl genpkey -algorithm ED25519 -out release-private.pem
openssl pkey -in release-private.pem -pubout -out release-public.pem
cmake -S . -B build -DBEACON_UPDATE_PUBLIC_KEY_FILE=/absolute/path/release-public.pem
```

Keep the private key out of the repository and installation packages. For GitHub Actions releases, set the repository variable `BEACON_UPDATE_PUBLIC_KEY` to the public-key PEM and the secret `BEACON_UPDATE_SIGNING_KEY` to the private-key PEM. CI runs [`scripts/package_updates.py`](scripts/package_updates.py) on the CPack ZIP to produce runtime and assets components, a release manifest, and a full package with version metadata, then signs the manifest. Publish the generated full package; raw CPack archives lack the metadata required for updates. Builds without a public key reject updates; rollback remains available. Without a signing key, CI publishes full packages without update manifests.

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

On Linux, formatting can be checked with:

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
| `templates` | Bundled template packages |
| `assets` | Fonts, UI assets, and Minecraft icons |
| `test` | Core, filesystem, presentation, UI, and package checks |

### Custom templates

Templates are self-contained directories under `templates/`. Each template must contain:

```text
templates/<name>/
├── template.json   # goals, facts, rules, and Minecraft compatibility
├── lang.json       # localized text
└── layout.json     # main-window and overlay layout
```

Use the bundled [`beacon-custom-template` skill](.agents/skills/beacon-custom-template/SKILL.md) for the complete schema, validation rules, and examples. A template may reference icons from the bundled asset directory or include its own language and image resources.

## Minecraft resources and trademarks

The icons under [`assets/vender/minecraft/`](assets/vender/minecraft/) are derived from the **vanilla Minecraft Java Edition 26.1 resource assets**. They are bundled only to identify goals in Beacon's unofficial progress tracker; they are not a replacement for the Minecraft game or its resource pack.

Minecraft is a trademark of Microsoft Corporation. Beacon is an independent, unofficial project and is not affiliated with, endorsed by, or sponsored by Mojang Studios or Microsoft.

## License

Beacon is released under the [MIT License](LICENSE). Third-party components and resources retain their own licenses and notices, including [GNU Unifont's SIL Open Font License](assets/fonts/Unifont-LICENSE.txt).

This software is based in part on the work of the [FreeType Project](https://freetype.org), used under the [FreeType License](licenses/FreeType-LICENSE.txt).
