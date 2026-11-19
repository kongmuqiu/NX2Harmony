# 更新记录 · Changelog

中文 · [English](#english)

版本号格式：主版本.次版本.修订。每个版本注明它能构建的游戏版本（见 `games.tsv`）。

## 0.1.3 · 2026-11-19 · 一键安装只走 winget 社区源

- **一键安装 Python、Docker Desktop（以及 Git、zstd、PowerShell、Java）加上 `--source winget`**：以前不指定源，winget 会连同微软商店源一起查；商店源会把服务器证书和固定值比对，玩家电脑上有加速器、杀毒软件的 HTTPS 扫描或代理替换了证书时，整次安装就以退出码 0x8A15005E（服务器证书与任何预期值都不匹配）失败，虽然要装的包本来就在 winget 社区源里。“环境”页、命令行和 `一键安装环境.cmd` 给玩家看的手动安装命令也都带上了 `--source winget`；安装说明的错误对照表加了这一行。

## 0.1.2 · 2026-11-18 · 后端源码进了本仓库

- **三个游戏后端的源码在本仓库的 `backends/` 里**：`NativeGames-harmonyos`（王国之泪）、`LZA`（Z-A）、`OOT`（时之笛），都是从各自工程导入的副本。游戏本身的工作仍在各后端自己的工程里做，再用 `packaging/backend_import.py` 导入；NX2 自己的改动记成补丁，放在维护者电脑上的 `backends/patches/`（不进提交：补丁里引着原工程被改掉的行）。`backends/SOURCES.tsv` 记着每个文件夹从哪来，并固定 totk-android 和 nxnative 的提交（这两个用时再取，不拷进来）；预编译的 libopus / libvpx 在 `backends/prebuilt-ohos`。说明见 [backends/README.md](backends/README.md)。
- **从源码运行，仓库旁边不用再放东西**：totk-android 由“环境”页克隆到 `backends/totk-android`；只有时之笛要的 nxnative 还得自己取，放到 `backends/nxnative` 或本仓库旁边。`nx2h.local.json` 的 `backends` 里以前记下的位置，只要那个文件夹还在就照旧优先；删掉这一项就改用 `backends/`。从源码运行时缺后端，提示改为用 git 恢复（`git checkout -- backends`），不再让人重新解压完整包。
- **完整包用已提交的后端来做**：`packaging/full_bundle.py` 取当前提交里的 `backends/`，`backends/` 下有没提交的改动就拒绝打包，并把这个提交记在包里的 `backends/NX2HARMONY-COMMIT.txt`。
- **发布前的检查**：libopus 重新编译，库里不再带打包电脑上的构建路径（代码和数据逐字节相同，只有源文件名字符串变了）；各后端的源码里不再出现打包电脑上的文件夹和维护者自己其他工程的名字；许可补全：打包好的软件在 `NX2Harmony.exe` 旁边多了 `licenses\` 文件夹（LGPL-3.0、GPL-3.0、Qt 的第三方声明、Qt / PySide6 源码的获取方式，以及 Python、PyInstaller、cffi、cryptography 的许可），各后端的 THIRD_PARTY_NOTICES 和应用里的许可文件夹现在列出了 d2r-mobile、nxnative、dav1d 和其他编进应用的部分（时之笛的应用里还缺 pokopia-native 的许可原文，Z-A 的应用里还缺生成的 Khronos Vulkan 枚举名表的）；不再带 opengl32sw.dll（Mesa 的软件 OpenGL，没用到）；提交钩子和 `.gitignore` 认得更多私密文件类型（`.npdm`、`.key`、`.env`、`.log`、`.pem` 等）。
- **Z-A 第一次构建能走完了**：最后一步「在手机上解包着色器」以前只打开了启动器，游戏运行时没启动，这一步会等满 15 分钟后失败；现在用 `--ps command start` 启动运行时，解包完会自己退出，再接着翻译着色器；电脑上翻译前读游戏代码的那一步也不再需要 Python 模块 capstone（新电脑上没有它，这一步会失败）。
- **0.1.0 和 0.1.1 的下载已撤下**：Releases 页面上只留 0.1.2，请用它。
- **更正 0.1.1 的说法**：王国之泪在一处空岛世界停住的问题没有修好，只是缓解了：默认双帧加上 told-fence 一致性检查之后，测试中没再出现，根本原因还不清楚。

## 0.1.1 · 2026-11-17 · 完整包上 Releases

- **完整包作为 Releases 附件发布**（`NX2Harmony-0.1.1-windows-x64-full.zip`）：打包好的软件，加上王国之泪、Z-A、时之笛三个后端的源码、固定提交的 totk-android、时之笛要用的那部分 nxnative 和预编译的 libopus / libvpx。不含任何游戏内容；打包前照常拦下会带出打包人信息的文件和没登记的图片。只要软件本体的也可以下 `NX2Harmony-0.1.1-windows-x64.zip`。
- **王国之泪的后端是 ng-fence4**：默认双帧（两帧并行），配有 told-fence 一致性检查，缓解了在一处空岛世界停住的问题（测试中没再出现，根本原因还不清楚）；需要时可在手机上的 env-dev.txt 里写 `TOTK_OHOS_SYNC_LAG=0` 回到单帧。
- 文档说明完整包从 Releases 下载；后端缺文件时的提示改为重新解压手上的完整包。

## 0.1.0 · 2026-11-17 · 第一个版本

- **桌面软件 NX2Harmony**（Qt / PySide6，PyInstaller 打成便携文件夹）和命令行 `nx2h.py setup / scan / build`：“游戏库”“构建”“环境”三页，设置和日志放在 exe 旁边的 `data\`。
- **能一键构建**：塞尔达传说 王国之泪 1.4.3（“已验证”）、塞尔达传说 时之笛 v131072（“优化中”：发现 bug，不再标“已验证”）、宝可梦传说 Z-A 2.0.2（“优化中”）。王国之泪的本体可以是卡带 XCI，也可以是 eShop 版 NSP / NSZ 或 XCZ（NSZ、XCZ 在构建里自动解压）。
- **识别游戏文件**：NSP / NSZ / XCI / XCZ / NUP，不需要密钥；图标先从游戏文件里取，取不到时按 title ID 联网取。
- **环境检查和一键安装**：Python 和 Docker Desktop 用 winget 装（Docker Desktop 第一次启动可能要装 WSL 2 或重启电脑，这一步由玩家来做）；构建时之笛时装 zstd；Git（要克隆 totk-android 参考仓库时）、PowerShell 7（没有预编译的 libopus、要自己编译时）和 Java（所用的 DevEco Studio 不带 Java 时）只在检查发现需要时才装；DevEco Studio 旁边有「打开下载页」按钮；检查后端工程、prod.keys、磁盘空间和手机。
- **签名**：每个游戏自动生成自己的包名，用玩家的华为账号在后台申请调试证书和 Profile；失败时可用 DevEco Studio 签。
- **安装和推送**：hdc 安装、标记为游戏、推送游戏数据（手机上已有的同一份跳过），构建期间手机保持亮屏。
- **完整包**（`packaging/full_bundle.py`）：打包好的软件、各后端的源码、固定提交的整个 totk-android、时之笛要用的那部分 nxnative、预编译的 libopus / libvpx，不含游戏内容；拷贝前拦下会带出打包人信息的文件和没登记的图片。
- 下载源的测速选择（`nx2h/mirrors.py`）已写好，默认关闭，等新电脑上实测后再打开。

<a id="english"></a>

## English

Versions are major.minor.patch. Each names the game versions it can build (see `games.tsv`).

### 0.1.3 · 2026-11-19 · one-click installs from the winget source only

- **The one-click installs of Python, Docker Desktop (and Git, zstd, PowerShell, Java) add `--source winget`**: without a source winget searched the Microsoft Store source too, which checks its server certificate against fixed values; on a player's PC where an accelerator, an antivirus's HTTPS scanning or a proxy replaces certificates the whole install failed with exit code 0x8A15005E (the server certificate did not match any of the expected values), although the package is in the winget source. The commands the Environment page, the command line and `一键安装环境.cmd` give the player for a manual install carry `--source winget` too, and the install guide's error table has the row.

### 0.1.2 · 2026-11-18 · the backends' sources in this repository

- **The three game backends' sources are in this repository, under `backends/`**: `NativeGames-harmonyos` (Tears of the Kingdom), `LZA` (Z-A) and `OOT` (Ocarina of Time), each a copy imported from its own project. Work on a game itself is still done in that backend's own project and then imported with `packaging/backend_import.py`; NX2's own changes are kept as patches in `backends/patches/` on the maintainer's PC (not committed: they quote the projects' original lines). `backends/SOURCES.tsv` records where each folder comes from and pins totk-android and nxnative, which are fetched, not copied; prebuilt libopus / libvpx are in `backends/prebuilt-ohos`. See [backends/README.md](backends/README.md).
- **From a checkout nothing has to sit beside the repository any more**: the Environment page clones totk-android into `backends/totk-android`; only nxnative, for Ocarina of Time, is still yours to fetch, into `backends/nxnative` or beside this repository. A place remembered under `backends` in `nx2h.local.json` still comes first while that folder exists; remove the entry to use `backends/`. When a backend is missing from a checkout, the message now says to restore it with git (`git checkout -- backends`) instead of unpacking the full package again.
- **The full package is made from the committed backends**: `packaging/full_bundle.py` takes `backends/` as the current commit holds it, refuses when `backends/` has uncommitted changes, and names that commit in the package's `backends/NX2HARMONY-COMMIT.txt`.
- **Release checks**: libopus rebuilt so that no build path of the packing PC is inside (its code and data are byte for byte the same; only the source-file name strings changed); the backends' sources no longer name the packing PC's folders or the maintainer's other projects; licences completed: the packed app has a `licenses\` folder beside `NX2Harmony.exe` (LGPL-3.0, GPL-3.0, Qt's third-party notices, where to get the Qt / PySide6 sources, and the licences of Python, PyInstaller, cffi and cryptography), and the backends' THIRD_PARTY_NOTICES and in-app licence folders now list d2r-mobile, nxnative, dav1d and the other components compiled in (pokopia-native's text is still missing in Ocarina of Time's app, and the generated Khronos Vulkan name table's in Z-A's); opengl32sw.dll (Mesa's software OpenGL, unused) is no longer shipped; the pre-commit hook and `.gitignore` know more private file types (`.npdm`, `.key`, `.env`, `.log`, `.pem` and others).
- **Z-A's first build gets through**: its last step, unpacking the shaders on the phone, only opened the launcher and never started the game's runtime, so it waited 15 minutes and failed; it now starts the runtime (`--ps command start`), which unpacks, exits, and the shaders are translated; reading the game's code before the translation no longer needs the Python module capstone (a new PC has none, and the step failed).
- **The 0.1.0 and 0.1.1 downloads are withdrawn**: the Releases page keeps 0.1.2 only; use it.
- **Correction to 0.1.1**: Tears of the Kingdom's world stopping at a sky-island spot was not fixed but mitigated: with two frames in flight by default and the told-fence consistency check it did not come back in testing, and its cause is not known.

### 0.1.1 · 2026-11-17 · the full package on Releases

- **The full package is a Releases asset** (`NX2Harmony-0.1.1-windows-x64-full.zip`): the packed app plus the sources of the three backends (Tears of the Kingdom, Z-A, Ocarina of Time), totk-android at the pinned commit, the parts of nxnative that Ocarina of Time runs and prebuilt libopus / libvpx. No game content; files that would tell who packed it and pictures nobody has reviewed are refused as before. The app alone is `NX2Harmony-0.1.1-windows-x64.zip`.
- **Tears of the Kingdom's backend is build ng-fence4**: two frames in flight by default, with a told-fence consistency check that mitigates the world stopping at a sky-island spot (it did not come back in testing; its cause is not known); `TOTK_OHOS_SYNC_LAG=0` in the env-dev.txt file on the phone goes back to one frame.
- The docs say the full package comes from the Releases page; when a backend file is missing, the messages say to unpack the player's full package again.

### 0.1.0 · 2026-11-17 · first version

- **NX2Harmony desktop app** (Qt / PySide6, packed by PyInstaller as a portable folder) and the command line `nx2h.py setup / scan / build`: Library (游戏库), Build (构建) and Environment (环境) pages; settings and logs in `data\` beside the exe.
- **One-click builds**: The Legend of Zelda: Tears of the Kingdom 1.4.3 (Verified), The Legend of Zelda: Ocarina of Time v131072 (Being optimized: a bug turned up, so it is no longer marked Verified), Pokémon Legends: Z-A 2.0.2 (Being optimized). Tears of the Kingdom's base game may be the card XCI, the eShop NSP / NSZ or an XCZ (NSZ and XCZ are decompressed inside the build).
- **Game files recognized** without keys: NSP / NSZ / XCI / XCZ / NUP; icons from the game files first, else looked up online by title ID.
- **Environment checks and one-click installs**: Python and Docker Desktop through winget (Docker Desktop's first start may ask for WSL 2 or a restart, which the player does); zstd for Ocarina of Time; Git (to clone the totk-android reference), PowerShell 7 (to build libopus when no prebuilt one is there) and Java (when the DevEco Studio in use has none of its own) only when the checks find them needed; an 「打开下载页」 (open the download page) button for DevEco Studio; checks for the backend projects, prod.keys, disk space and the phone.
- **Signing**: every game gets its own bundle name; a debug certificate and profile are requested in the background with the player's Huawei account; DevEco Studio signing as the fallback.
- **Install and push**: hdc install, marked as a game, game data sent (what the phone already has is skipped), the phone's screen kept on during the build.
- **Full package** (`packaging/full_bundle.py`): the packed app, the backends' sources, the whole totk-android tree at the pinned commit, the parts of nxnative that Ocarina of Time runs, prebuilt libopus / libvpx, no game content; files that would tell who packed it and pictures nobody has reviewed are refused before anything is copied.
- Download-source selection by speed test (`nx2h/mirrors.py`) is written but off until it has been tested on a fresh PC.
