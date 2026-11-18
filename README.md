# NX2Harmony

<p align="center"><b>Turn your own Nintendo Switch games into HarmonyOS apps on a Windows PC, and play them on your own phone.</b><br>
Every package is built from your files and keys and signed with your own Huawei developer account. The phone runs the game's own code, prepared ahead of time on the PC; there is no emulator in between.</p>

<p align="center"><b>在 Windows 电脑上把你自己的 Switch 游戏做成鸿蒙应用，装到你自己的手机上玩。</b><br>
每个安装包都用你的文件和密钥构建，用你自己的华为开发者账号签名。手机上运行的是游戏自己的代码，事先在电脑上处理好，中间没有模拟器。</p>

[English](#english) | [中文](#chinese)

# English

## Legal

**Only for games you own.** Each game you build has to be a copy you paid for and dumped yourself from your own game card or console, and the keys and firmware have to come off your own Nintendo Switch as well. NX2Harmony is a personal, unofficial project: Nintendo, The Pokémon Company, Huawei and the authors of nxnative have no part in it, and none of them has endorsed it. Game titles, Nintendo Switch, Pokémon, HarmonyOS and the other names used here are trademarks of their owners.

Nothing that belongs to a game lives in this repository, and none is handed out from it: no shaders, sound, pictures, text or code of any game, and no keys, firmware or signing files. The Releases page carries the packed app and a full package (the app plus the game backends' sources); no HAP and no game data is ever released. The repository holds the desktop app, the command line, the thin adapters that drive each game's backend, the game backends' sources in [backends/](backends/README.md) (each game's HarmonyOS pipeline: scripts, native code and the app shell, with no game content), the HarmonyOS verification table [games.tsv](games.tsv) (the Verified and Being optimized versions: title IDs, versions, backends, statuses and the files each needs; no game data) and the packaging scripts.

What a build makes from your files stays on your own PC and phone. The HAP and the game data sent to the phone hold the game itself: keep them to yourself, never upload them or pass them on.

Local law decides whether you may dump your own games, keys and firmware; that check is yours to make. This project gives no help in getting hold of games, keys or firmware.

Game files, keys and firmware have no place in this project's issues or pull requests. Posting them, or asking anyone for them, gets the issue or pull request deleted without an answer.

## About

NX2Harmony is a Windows desktop app with a command line beside it. From your own game files and keys it builds a signed HarmonyOS package (HAP) of a Switch game, installs it on your phone with hdc and pushes the game data to it. It works the way [nxnative](https://github.com/seedds/nxnative) does: a version that has been adapted becomes a playable package in one go; a game or version that has not is still recognized, and the app shows its state. The work for each game is done by a backend of its own, developed in its own project; this repository is the app, the adapters that drive those backends, a copy of each backend's sources (`backends/`), the packaging and the documentation. [How it works](docs/how-it-works.md) describes the parts.

### Games you can build

| Game | Version | Status | What you need |
|---|---|---|---|
| The Legend of Zelda: Tears of the Kingdom | 1.4.3 | Verified | Base game: the card `.xci` (1.0.0, or a card that already carries 1.2.1), the eShop `.nsp` / `.nsz`, or an `.xcz` (NSZ and XCZ are decompressed inside the build). The 1.4.3 update as `.nsz` only (the backend takes no other format; an `.nsp` you dumped can be compressed with nsz first). `prod.keys`. With an eShop or `.xcz` base, the system fonts come from firmware dumped from your own console (in a folder next to `prod.keys`) or from one of your own game card images (`.xci`, not necessarily of this game) in the game folder |
| The Legend of Zelda: Ocarina of Time | v131072 | Being optimized | The Switch 2 version's `.nup` holding the v131072 update. The system fonts are read once from one of your own game card images (`.xci`, not necessarily of this game) or from firmware dumped from your own console (needs `prod.keys`) |
| Pokémon Legends: Z-A | 2.0.2 | Being optimized | The Switch 2 Edition's `.nup` holding the 2.0.2 update. The system fonts as for Ocarina of Time |

**Verified**: played through on a phone. **Being optimized**: builds and plays; the frame rate and a few pictures are still being tuned. A version marked **Being adapted** is shown but not built. The full table, with title IDs and backends, is [games.tsv](games.tsv); every newly adapted version adds a row.

- **Tears of the Kingdom**: the first build makes the shader seed (about 3 minutes); later builds reuse what is already prepared and only pack again.
- **Pokémon Legends: Z-A**: after the first install the phone unpacks the game's shaders once (about 1 minute), then the PC translates them (about 1 hour) and sends them back to the phone.
- **Ocarina of Time**: every shader is translated on the PC.

### The desktop app

The app has three pages:

- **Library (游戏库)**: the games in your game folder, each with its icon. The icon comes from your own files first (the control data of an XCI / NSP / NSZ, the meta block of a NUP). For a game whose files carry none, the app looks the eShop icon up online by title ID, by itself when the library is scanned (and again with the 「从网上取图标」 button): first the image host tinfoil.media, then the community titledb list's link to the official icon on Nintendo's CDN, and for a few Switch 2 games the picture on Nintendo's own site. Neither the project nor the app carries any game picture; icons are kept only in the app's own `work\icons` (`data\work\icons` in the packed app). The built HAP uses the same icon: it replaces the backend's app icon while the HAP is packed, and the original is put back right after.
- **Build (构建)**: the files found, the target phone, the progress step by step, and the log.
- **Environment (环境)**: what the PC and the phone still need, a button that installs whatever can be installed (「一键安装缺少的」), and an 「打开下载页」 button beside anything you must download yourself.

The packed app is portable: settings, the backends' locations, the icon cache and the logs live in `data\` beside `NX2Harmony.exe`, so the whole folder can be copied anywhere (`%LOCALAPPDATA%\NX2Harmony` is used only when that folder is read-only). Run from source, the same files sit in the checkout (`nx2h.local.json`, `work\`, `logs\`), all ignored by git. A build still uses the PC's own Python, Docker and DevEco Studio; the Environment page checks them.

### Recommended install

> **Let an AI assistant do it.** Point an AI coding assistant (Claude Code, Codex or similar) at this checkout, give it the folders that hold your game files and `prod.keys`, and let it prepare the PC, run the build and put the game on your phone. Its instructions are the [install skill](.claude/skills/install-nx2harmony/SKILL.md): with them it waits out the slow stages, reads every result before moving on and handles the errors listed there. Some steps only you can take: installing DevEco Studio, signing in to your Huawei account in the browser, installing WSL 2 or restarting Windows if Docker Desktop asks for it on its first start, and, on the phone, turning on developer mode and USB debugging and allowing debugging when asked.
>
> **Or use the desktop app** and go through its pages yourself: [Build with the window](docs/install.md#build-with-the-window).

### Install skill

> The assistant's side of the job is written down in [`.claude/skills/install-nx2harmony/SKILL.md`](.claude/skills/install-nx2harmony/SKILL.md): the order of the work, the rules it keeps (your files stay yours, keys are never opened), what the `nx2h.py` messages mean and which steps it hands back to you. In Claude Code, `/install-nx2harmony` starts it, and Claude Code also uses it unasked; other assistants are sent to it by [AGENTS.md](AGENTS.md), or you can say in your first message: *"Use .claude/skills/install-nx2harmony/SKILL.md to build my game for my HarmonyOS phone."*

## What's new

- **2026-11-18: version 0.1.2, the backends' sources in this repository.** The three game backends are now in `backends/`, copies imported from their own projects, so a checkout builds all three games with nothing placed beside it: the Environment page clones totk-android into `backends/totk-android`, and only Ocarina of Time still needs nxnative fetched. The full package is made from the committed backends. The packed app carries its licence texts in `licenses\`, and the prebuilt libopus no longer holds build paths of the PC it was built on. The 0.1.1 note on Tears of the Kingdom is corrected: the sky-island stop was mitigated, not fixed ([CHANGELOG.md](CHANGELOG.md)).
- **2026-11-17: version 0.1.1, the full package on the Releases page.** One download now holds the app and the sources of every game backend, so the three games build without fetching their projects separately. Tears of the Kingdom's backend is build ng-fence4: two frames in flight by default, with a told-fence consistency check that mitigates the world stopping at a sky-island spot (it did not come back in testing; its cause is not known) ([CHANGELOG.md](CHANGELOG.md)).
- **2026-11-17: more ways in for Tears of the Kingdom.** The base game may now be the eShop `.nsp` / `.nsz` or an `.xcz` (decompressed inside the build). The Environment page opens the download page of anything you must install yourself with one button. The full package refuses files that would identify the PC it was packed on. This is version 0.1.0, the first numbered one ([CHANGELOG.md](CHANGELOG.md)).
- **2026-11-16: built from nothing on a clean PC.** Ocarina of Time and Tears of the Kingdom were built from scratch on a clean PC with the full package (`packaging/full_bundle.py`).
- **2026-11-15: The Legend of Zelda: Ocarina of Time (Switch 2) added**: it builds and plays on a phone; since a bug turned up it is marked being optimized, not verified.
- **2026-11-09: NX2Harmony.** One window builds Tears of the Kingdom and Pokémon Legends: Z-A, signed in the background with your Huawei account.

## Install and play

**[docs/install.md](docs/install.md)** walks through everything, starting with a PC that has nothing installed and ending with the game open on your phone. An AI assistant with the install skill can do it for you instead ([above](#recommended-install)).

- [What you need](docs/install.md#what-you-need) · [Set up the PC](docs/install.md#set-up-the-pc) · [Set up the phone](docs/install.md#set-up-the-phone)
- [Build with the window](docs/install.md#build-with-the-window) · [Build from the command line](docs/install.md#build-from-the-command-line) · [Signing](docs/install.md#signing)
- [Where things are](docs/install.md#where-things-are) · [When a build stops](docs/install.md#when-a-build-stops) · [Known issues](docs/install.md#known-issues) · [Uninstall](docs/install.md#uninstall)

In short:

- **The app.** Download `NX2Harmony-0.1.2-windows-x64-full.zip` (the full package, with the backends) from this repository's Releases page and unzip it anywhere, then run `NX2Harmony.exe`. Or pack it yourself with `python packaging/build.py`: it draws the icon, runs PyInstaller and keeps `dist\NX2Harmony\data`, and needs the packages in `packaging/requirements-build.txt` (PyInstaller, PySide6-Essentials, Pillow, cryptography). Then double-click `dist\NX2Harmony\NX2Harmony.exe` (a shortcut on the desktop works too). The folder is about 100 MB and carries its own Python and Qt; `nx2h.exe` beside it is the command line, used like `python nx2h.py` below. To run from source instead: Python 3.9 or newer (`一键安装环境.cmd` installs it when `where python` finds none; it takes the Microsoft Store alias for an installed Python, so with only that alias run `winget install Python.Python.3.12` yourself), then double-click `NX2Harmony.cmd`; the first start installs PySide6 and cryptography from `requirements.txt` (about 80 MB). The checkout carries the game backends in `backends\`.
- **The PC.** The Environment page, or a double-click on `一键安装环境.cmd`, checks the PC and the phone, installs with winget what it can and says how to do the rest: Python, Docker Desktop (its first start may ask for WSL 2 or a restart, which is yours to do), zstd for Ocarina of Time, and only when needed Git (to fetch the totk-android reference), PowerShell 7 (to build libopus when no prebuilt one is there) and Java (when the DevEco Studio in use has none of its own). DevEco Studio you install yourself: sign in with your Huawei account and open it once so it downloads the HarmonyOS SDK; hdc comes with it. The first build of a game needs about 80 GB free (70 GB for Ocarina of Time) on the drive of its backend. → [Set up the PC](docs/install.md#set-up-the-pc)
- **The phone.** Developer mode and USB debugging on, connected to the PC by cable, debugging allowed. → [Set up the phone](docs/install.md#set-up-the-phone)
- **Signing.** Each game gets its own bundle name, made by the app (for example `nx2h.uk3f9a2.totk`), and is signed in the background with your Huawei developer account (real-name verified): the first build, or the Environment page, opens the Huawei account login page in your browser once, and after that there is nothing to do. DevEco Studio must be installed but need not be open. The app uses the same Huawei service as DevEco Studio's automatic signing; that is not a documented public interface, so it may change. Your login and signing key stay in `%USERPROFILE%\.nx2h`, sealed with Windows DPAPI. If background signing fails, the app makes a small signing project and opens it in DevEco Studio to sign there. → [Signing](docs/install.md#signing)
- **Building.** In the app, or with a double-click on `一键构建.cmd`: pick the folder with your game files (every subfolder is scanned, and one folder may hold several games) and, the first time, the folder that holds `prod.keys`. The rest runs by itself: prepare the game data, compile the native libraries, pack the signed HAP, install it on the phone, push the game data (what the phone already has is skipped), open the game. The phone's screen stays on meanwhile and gets its own screen-off time back at the end. When the folder holds two or more buildable games, the double-click build stops and lists them; pick one on the command line with `--game <title id>`.

The command line:

```
python nx2h.py setup [--fix]
python nx2h.py scan <folder>...
python nx2h.py build [<folder>...] [-k <folder with prod.keys>] [--game <title id>] [--tag <tag>] [--no-install] [--screen <W>x<H>] [--progress json]
```

**Tears of the Kingdom on a wide screen.** The build lays the game's UI out for your phone's screen when it is wider than 16:9: the HUD sits at the screen's edges, backgrounds and cutscenes fill it. The resolution is read from the connected phone (`hidumper`, screen[0]); with no phone connected this part is skipped and the output says so, and `--screen 2848x1320` names the screen instead. The layout can only go into a folder the app makes itself, which it does when it starts with 「画面比例」 (aspect ratio) set to 「全面屏」 (full screen); a fresh install normally gets that folder in the same build. If it is missing (16:9 chosen in the settings, or settings made for another screen), the build goes on without the layout and the HUD stays in the middle at 16:9; choose 「全面屏」 in the settings, tap 「开始游戏」 once and build again. Choosing 「居中 16:9」 for the HUD under 「画面」 in the game's settings brings the original layout back.

`--progress json` prints one JSON event per line, in the format of nxnative's `build_ipa_from_nsp.py` (see nxnative's `docs/frontends.md`), so a graphical front end can read it directly.

## How it works

See [docs/how-it-works.md](docs/how-it-works.md). In brief: NX2Harmony recognizes your files without keys, finds the game and version in [games.tsv](games.tsv) and runs that game's backend step by step. The backend checks your files, rewrites the game's code ahead of time and prepares its data and shaders; NX2Harmony then packs the HAP with the game's bundle name, signing and icon, installs it and pushes the data. A step whose result is already there is skipped, so a stopped build continues where it stopped.

What is where:

- `nx2h.py`: the command line. `NX2Harmony.cmd` / `NX2Harmony.pyw` open the app from source; `一键安装环境.cmd` and `一键构建.cmd` are the double-click entries.
- `nx2h/scan.py`: recognizes game files (no keys needed).
- `nx2h/doctor.py`: the environment checks and the automatic installs.
- `nx2h/device.py`: the phone, through hdc.
- `nx2h/app.py`: the desktop app (Qt / PySide6); `nx2h/core.py` is the logic it uses that has nothing to do with drawing.
- `nx2h/icon.py`: the game icons. From the game files first: XCI / NSP / NSZ through `nx2h/vendor/switchfs` (totk-android's readers, MIT), which decrypts the control data inside the app's own process; NUP from its meta block. Otherwise online by title ID (see [The desktop app](#the-desktop-app)).
- `nx2h/fonts.py`: the console's system fonts, read once from one of your own card images or from your own console's firmware and kept for every game.
- `nx2h/bundle.py`, `nx2h/signing.py`, `nx2h/agc.py`: each game's bundle name and its signing (a debug certificate and profile requested from Huawei in the background).
- `nx2h/mirrors.py`: picking download sources by speed; switched off until it has been tested on a fresh PC.
- `nx2h/backends/`: one adapter per adapted version, each driving one game backend (Tears of the Kingdom: NativeGames-harmonyos; Z-A: LZA; Ocarina of Time: OOT, which also needs nxnative; all three build on totk-android at a pinned commit).
- `backends/`: the game backends' sources, copies imported from each backend's own project ([backends/README.md](backends/README.md)), with prebuilt libopus / libvpx (`prebuilt-ohos`) and `backends/SOURCES.tsv`, which records where each came from and pins totk-android and nxnative. Those two are fetched, not copied: the Environment page clones totk-android into `backends\totk-android`; nxnative goes into `backends\nxnative` or beside this repository. Another place for a backend can go into `nx2h.local.json` under `backends`. Without a backend the app recognizes its game but does not build it.
- `packaging/`: `build.py` packs the app; `full_bundle.py` makes the full package for a clean PC from the committed `backends/` (the packed app, the backends' sources, the whole totk-android tree at the pinned commit, the parts of nxnative that Ocarina of Time runs, prebuilt libraries; no game content). Before copying anything it refuses uncommitted changes under `backends/`, files that would tell who packed it or where, and pictures not listed in `packaging/bundle_pictures.tsv`. `backend_import.py` imports or updates a backend from its project.
- `games.tsv`: the HarmonyOS verification table (Verified and Being optimized versions).
- `docs/`: [install](docs/install.md), [how it works](docs/how-it-works.md), and `docs/DESIGN.md`, the design history in Chinese (stage A is the single entry point; stage B is the general core after nxnative).
- `tests/`: checks that need no game files: `python -m unittest discover -s tests`.
- `.githooks/`: a pre-commit check that stops what it can recognize: by name (game files and game data, keys, built packages, libraries and archives, signing files, this PC's settings, build and work folders), by size (any file over 20 MB) and by content (console key lines like those in `prod.keys`), letting through only a backend's module build file without signing and the prebuilt libraries in `backends/prebuilt-ohos`; turn it on once per clone with `git config core.hooksPath .githooks`.
- [CONTRIBUTING.md](CONTRIBUTING.md): how to help; [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md): the third-party parts and their licenses.

## Acknowledgements

- **[nxnative](https://github.com/seedds/nxnative)** (MIT): the model NX2Harmony follows. Adapted versions build in one go, others are recognized and shown with their state, and the build reports its progress in the event format nxnative's front ends read. The Ocarina of Time backend runs parts of it (its shader splitter, its mesh stage and its Ampere Ryujinx), and the NUP reading in `nx2h/icon.py` is adapted from its `scripts/pack_nup.py`.
- **[totk-android](https://github.com/AurelioB/totk-android)** (MIT): the Android port all the backends build on, pinned to one commit. The game-file readers in `nx2h/vendor/switchfs` come from it.
- **[d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)** (MIT): turning a Switch game into native code got its start in this project; its work reaches NX2Harmony by way of totk-android.
- **Ryujinx** (MIT): the shader translator the backends build is based on it.
- **The NUP specification** ([a55uka/nup](https://github.com/a55uka/nup)): the Switch 2 package format, which `nx2h/scan.py` reads with code of its own.
- **NX2Apple**: the Mac front end over nxnative whose flow this app follows (a Library page, an Environment page and a Build page). A community project; no code was taken from it.
- **libopus** and **libvpx** (BSD): the game's sound and its VP9 movies; prebuilt for the phone in `backends/prebuilt-ohos` (in this repository and in the full package).
- The packed app carries Qt / PySide6 (LGPL v3), cryptography, CPython and the PyInstaller bootloader. Each part and its license is listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

NX2Harmony is released under the MIT License ([LICENSE](LICENSE)); the third-party parts and their own licenses are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

---

<a id="chinese"></a>

# 中文说明

## 法律声明

**只用于你自己拥有的游戏。** 构建的每个游戏都得是你花钱买下、由你从自己的卡带或主机导出的；密钥和固件也一样，要出自你自己的 Nintendo Switch。NX2Harmony 是个人做的非官方项目，任天堂、The Pokémon Company、华为和 nxnative 的作者都没有参与，也没有任何一方为它背书。文中的游戏名称、Nintendo Switch、宝可梦（Pokémon）、HarmonyOS 等名称都是各自权利人的商标。

属于游戏的东西，这个仓库里一样都没有，从这里也拿不到：任何游戏的着色器、声音、图片、文字、代码都没有，密钥、固件、签名文件同样没有。Releases 页面上有打包好的软件和完整包（软件加各游戏后端的源码）；从不发布 HAP 或游戏数据。仓库里有的是桌面软件、命令行、驱动各游戏后端的薄适配层、[backends/](backends/README.md) 里各游戏后端的源码（每个游戏的鸿蒙流水线：脚本、原生代码和外面那层应用，不含游戏内容）、鸿蒙验证表 [games.tsv](games.tsv)（已验证和优化中的版本：title ID、版本、后端、状态和需要的文件，不含任何游戏数据）以及打包脚本。

构建用你的文件做出来的东西，只留在你自己的电脑和手机上。HAP 和推到手机上的游戏数据里就是游戏本身：留给自己用，不要上传，也不要转给别人。

各地对导出自己的游戏、密钥和固件规定不一，动手之前先查清你那里是否允许。本项目不提供任何获取游戏、密钥或固件的帮助。

issue 和 pull request 里不能出现游戏文件、密钥或固件，也不能向别人要；有这类内容的直接删掉，不作回复。

## 项目介绍

NX2Harmony 是一个 Windows 桌面软件，另带命令行。它用你自己的游戏文件和密钥构建出 Switch 游戏的鸿蒙签名安装包（HAP），用 hdc 装到你的手机上，再把游戏数据推过去。做法照 [nxnative](https://github.com/seedds/nxnative)：已经适配过的版本一步出可玩的包；没适配过的游戏或版本也会识别出来，软件会显示它的状态。每个游戏的活由它自己的后端来做，后端在各自的工程里开发；本仓库是软件本身、驱动这些后端的适配层、各后端源码的一份副本（`backends/`）、打包和文档。各部分怎么配合见[工作原理](docs/how-it-works.zh-CN.md)。

### 能构建的游戏

| 游戏 | 版本 | 状态 | 需要的文件 |
|---|---|---|---|
| 塞尔达传说 王国之泪 | 1.4.3 | 已验证 | 本体：卡带 `.xci`（1.0.0，或已含 1.2.1 更新的卡带）、eShop 版 `.nsp` / `.nsz`，或 `.xcz`（NSZ、XCZ 在构建里自动解压）。1.4.3 更新只收 `.nsz`（后端只认这种格式，自己导出的 `.nsp` 可以先用 nsz 压缩）。`prod.keys`。用 eShop 版或 `.xcz` 本体时，系统字体从你主机导出的固件（放在 `prod.keys` 旁边的文件夹里）或游戏文件夹里你自己的一张游戏卡带镜像（`.xci`，不必是这个游戏的）取 |
| 塞尔达传说 时之笛 | v131072 | 优化中 | Switch 2 版的 `.nup`（含 v131072 更新）。系统字体从你自己的一张游戏卡带镜像（`.xci`，不必是这个游戏的）或你主机导出的固件取一次（要 `prod.keys`） |
| 宝可梦传说 Z-A | 2.0.2 | 优化中 | Switch 2 Edition 的 `.nup`（含 2.0.2 更新）。系统字体同时之笛 |

**已验证**：在手机上玩通过。**优化中**：能构建、能玩，帧率和个别画面还在调。标为“适配中”的版本只显示，不能构建。完整的表（含 title ID 和后端）见 [games.tsv](games.tsv)，每适配完一个版本就加一行。

- **王国之泪**：第一次构建要做着色器种子（约 3 分钟）；之后再构建会复用已准备好的结果，只重新打包。
- **宝可梦传说 Z-A**：第一次装到手机后，手机先解包一次游戏的着色器（约 1 分钟），再在电脑上翻译（约 1 小时）并推回手机。
- **时之笛**：着色器全部在电脑上翻译。

### 桌面软件

软件有三页：

- **游戏库**：游戏文件夹里的游戏，每个都配上图标。图标先从你自己的文件里取（XCI / NSP / NSZ 的 control 数据，NUP 的 meta 块）；文件里没有的，扫描游戏库时软件会自己按 title ID 联网取 eShop 图标（「从网上取图标」按钮也能再取一次）：先查图片服务 tinfoil.media，再用社区 titledb 列表给出的任天堂 CDN 官方图标链接，少数 Switch 2 游戏用任天堂官网上的图。项目和软件都不带任何游戏图片，图标只放在软件自己的 `work\icons`（打包版在 `data\work\icons`）。构建出的 HAP 也用这个图标：打包时临时替换后端的应用图标，打完立即换回原来的。
- **构建**：识别到的文件、目标手机、分步进度和日志。
- **环境**：电脑和手机还缺什么，一个「一键安装缺少的」按钮；要你自己下载的东西旁边有「打开下载页」按钮。

打包版是便携的：设置、后端位置、图标缓存和日志都在 `NX2Harmony.exe` 旁边的 `data\` 里，整个文件夹拷到哪里都能用（只有那个文件夹不能写时才用 `%LOCALAPPDATA%\NX2Harmony`）。从源码运行时，这些文件都在仓库目录里（`nx2h.local.json`、`work\`、`logs\`），全部被 git 忽略。构建时仍要用电脑上的 Python、Docker 和 DevEco Studio，“环境”页会检查。

### 推荐的安装

> **让 AI 助手代劳。** Claude Code、Codex 这类 AI 编程助手可以接手这个仓库：把放游戏文件和 `prod.keys` 的文件夹告诉它，由它把电脑准备好、跑完构建、把游戏装进你的手机。它的做法写在[安装 skill](.claude/skills/install-nx2harmony/SKILL.md) 里：慢的阶段耐心等完，每一步的结果先看过再往下走，skill 里列出的报错按那里的办法处理。有几步只能你自己来：安装 DevEco Studio；在浏览器里登录华为账号；Docker Desktop 第一次启动时如果要装 WSL 2 或重启电脑；在手机上打开开发者模式和 USB 调试，并在手机询问时允许调试。
>
> **也可以用桌面软件**，自己按页面一步步来：[用窗口构建](docs/install.zh-CN.md#用窗口构建)。

### 安装 skill

> 助手那一侧怎么做，写在 [`.claude/skills/install-nx2harmony/SKILL.md`](.claude/skills/install-nx2harmony/SKILL.md) 里：先做什么后做什么、要守的规矩（你的文件只归你，密钥从不打开）、`nx2h.py` 各条提示的意思、哪些步骤要交回给你。在 Claude Code 里输入 `/install-nx2harmony` 就会用上它，不输入它也会自己用；其他助手由 [AGENTS.md](AGENTS.md) 引过去，你也可以第一句就说：*“照 .claude/skills/install-nx2harmony/SKILL.md 把我的游戏构建好，装到我的鸿蒙手机上。”*

## 更新说明

- **2026-11-18：0.1.2，后端源码进了本仓库。** 三个游戏后端现在在 `backends/` 里，是从各自工程导入的副本，所以从源码运行时仓库旁边什么都不用放就能构建这三个游戏：totk-android 由“环境”页克隆到 `backends/totk-android`，只有时之笛还要另取 nxnative。完整包用已提交的后端来做。打包好的软件在 `licenses\` 里带上了各部分的许可原文，预编译的 libopus 里不再有编译它的那台电脑上的路径。更正 0.1.1 关于王国之泪的说法：空岛停住的问题是缓解了，不是修好了（[CHANGELOG.md](CHANGELOG.md)）。
- **2026-11-17：0.1.1，Releases 上有完整包了。** 一次下载就有软件和各游戏后端的源码，不用另外取后端工程就能构建这三个游戏。王国之泪的后端是 ng-fence4：默认双帧，配有 told-fence 一致性检查，缓解了在一处空岛世界停住的问题（测试中没再出现，根本原因还不清楚）（[CHANGELOG.md](CHANGELOG.md)）。
- **2026-11-17：王国之泪能用的本体更多了。** 本体现在也可以是 eShop 版 `.nsp` / `.nsz` 或 `.xcz`（在构建里解压）。“环境”页里要你自己装的东西，一个按钮就能打开下载页。完整包会拒收能看出是哪台电脑打的包的文件。这是第一个带版本号的版本 0.1.0（[CHANGELOG.md](CHANGELOG.md)）。
- **2026-11-16：在干净的电脑上从零构建。** 用完整包（`packaging/full_bundle.py`）在一台干净的电脑上从头构建了时之笛和王国之泪。
- **2026-11-15：新增塞尔达传说 时之笛（Switch 2）**：能构建、能在手机上玩；发现 bug 后标为优化中，不再标已验证。
- **2026-11-09：NX2Harmony。** 一个窗口构建王国之泪和宝可梦传说 Z-A，用你的华为账号在后台签名。

## 安装与游玩

**[docs/install.zh-CN.md](docs/install.zh-CN.md)** 从一台什么都没装的电脑讲起，一直讲到游戏在手机上打开。也可以让 AI 助手按安装 skill 替你做（见[上文](#推荐的安装)）。

- [需要准备什么](docs/install.zh-CN.md#需要准备什么) · [准备电脑](docs/install.zh-CN.md#准备电脑) · [准备手机](docs/install.zh-CN.md#准备手机)
- [用窗口构建](docs/install.zh-CN.md#用窗口构建) · [用命令行构建](docs/install.zh-CN.md#用命令行构建) · [签名](docs/install.zh-CN.md#签名)
- [文件在哪](docs/install.zh-CN.md#文件在哪) · [构建停下时](docs/install.zh-CN.md#构建停下时) · [已知问题](docs/install.zh-CN.md#已知问题) · [卸载](docs/install.zh-CN.md#卸载)

简单说：

- **软件。** 从本仓库的 Releases 页面下载 `NX2Harmony-0.1.2-windows-x64-full.zip`（完整包，带后端），解压到任意位置，运行 `NX2Harmony.exe`。也可以自己用 `python packaging/build.py` 打包：它会画图标、运行 PyInstaller，并保留 `dist\NX2Harmony\data`；需要 `packaging/requirements-build.txt` 里的包（PyInstaller、PySide6-Essentials、Pillow、cryptography）。然后双击 `dist\NX2Harmony\NX2Harmony.exe`（也可以发送到桌面做快捷方式）。整个文件夹约 100 MB，自带 Python 和 Qt；同一个文件夹里的 `nx2h.exe` 是命令行版，用法和下面的 `python nx2h.py` 相同。也可以从源码运行：先装 Python 3.9 或更新版本（`where python` 找不到 Python 时，`一键安装环境.cmd` 会装；只有微软商店的 python 别名时它会当作已经装好，这时自己运行 `winget install Python.Python.3.12`），再双击 `NX2Harmony.cmd`；第一次打开时会装 `requirements.txt` 里的 PySide6 和 cryptography（约 80 MB）。游戏后端就在仓库的 `backends\` 里。
- **电脑。** “环境”页，或双击 `一键安装环境.cmd`，会检查电脑和手机，能用 winget 装的都装好，装不了的告诉你怎么做：Python、Docker Desktop（第一次启动可能要装 WSL 2 或重启电脑，这一步要你自己来），构建时之笛还有 zstd；只在需要时才装 Git（取 totk-android 参考仓库用）、PowerShell 7（没有预编译的 libopus 时编译它）和 Java（所用的 DevEco Studio 不带 Java 时）。DevEco Studio 要你自己装：用华为账号登录，打开一次让它下载 HarmonyOS SDK；hdc 随它一起装好。第一次构建一个游戏时，它的后端所在的盘要有约 80 GB 空闲（时之笛约 70 GB）。→ [准备电脑](docs/install.zh-CN.md#准备电脑)
- **手机。** 打开开发者模式和 USB 调试，用数据线连上电脑，允许调试。→ [准备手机](docs/install.zh-CN.md#准备手机)
- **签名。** 每个游戏有自己的包名，由软件生成（如 `nx2h.uk3f9a2.totk`），用你的华为开发者账号（需实名认证）在后台签名：第一次构建时（或在“环境”页）浏览器会打开一次华为账号登录页，之后就不用再管。DevEco Studio 要装着，但不用打开。软件用的是 DevEco Studio 自动签名背后的同一个华为服务；它不是公开文档里的接口，以后可能会变。你的登录信息和签名密钥放在 `%USERPROFILE%\.nx2h`，用 Windows DPAPI 加密保护。后台签名不成功时，软件会生成一个小的签名工程，并在 DevEco Studio 里打开，在那里签。→ [签名](docs/install.zh-CN.md#签名)
- **构建。** 在软件里，或双击 `一键构建.cmd`：选择放游戏文件的文件夹（所有子文件夹都会扫描，一个文件夹里可以放多个游戏），第一次还要选 `prod.keys` 所在的文件夹。之后自动依次完成：准备游戏数据、编译原生库、打包签名的 HAP、装到手机、推送游戏数据（手机上已有的同一份会跳过）、打开游戏。这期间手机保持亮屏，结束后恢复原来的息屏时间。文件夹里有两个以上可构建的游戏时，双击构建会停下并列出它们，这时用命令行加 `--game <title id>` 选一个。

命令行：

```
python nx2h.py setup [--fix]
python nx2h.py scan <文件夹>...
python nx2h.py build [<文件夹>...] [-k <prod.keys 所在文件夹>] [--game <title id>] [--tag <标签>] [--no-install] [--screen <宽>x<高>] [--progress json]
```

**王国之泪的全面屏布局。** 手机屏幕比 16:9 宽时，构建会按这块屏幕生成一套界面布局：HUD 贴到屏幕两边，背景和过场铺满。分辨率从连着的手机读（`hidumper` 的 screen[0]）；没连手机时这一步先跳过，输出里会说明；也可以用 `--screen 2848x1320` 直接指定。这套布局只能放进应用自己建的文件夹，应用在「画面比例」为「全面屏」时启动才会建它；全新安装时，通常同一次构建里就会建好。缺这个文件夹时（设置里选了 16:9，或设置是按另一块屏幕做的），构建照常继续，只是不推这套布局，HUD 先在中间 16:9；在设置里选「全面屏」、点一次「开始游戏」，再构建一次就会补上。游戏设置「画面」里的 HUD 选「居中 16:9」，就回到原来的布局。

`--progress json` 每行输出一个 JSON 事件，格式与 nxnative 的 `build_ipa_from_nsp.py` 相同（见 nxnative 的 `docs/frontends.md`），图形前端可以直接读取。

## 工作原理

见 [docs/how-it-works.zh-CN.md](docs/how-it-works.zh-CN.md)。简单说：NX2Harmony 不用密钥就能识别你的文件，在 [games.tsv](games.tsv) 里找到对应的游戏和版本，再一步步运行这个游戏的后端。后端核对你的文件、提前改写游戏代码、准备游戏数据和着色器；然后 NX2Harmony 用这个游戏的包名、签名和图标打出 HAP，装到手机并推送数据。结果已经在的步骤会跳过，所以中途停下的构建会从停下的地方接着做。

各部分在哪：

- `nx2h.py`：命令行入口。`NX2Harmony.cmd` / `NX2Harmony.pyw` 从源码打开软件；`一键安装环境.cmd` 和 `一键构建.cmd` 是双击入口。
- `nx2h/scan.py`：识别游戏文件（不需要密钥）。
- `nx2h/doctor.py`：环境检查和自动安装。
- `nx2h/device.py`：通过 hdc 操作手机。
- `nx2h/app.py`：桌面软件（Qt / PySide6）；`nx2h/core.py` 是它用到的、与界面绘制无关的逻辑。
- `nx2h/icon.py`：游戏图标。先从游戏文件里取：XCI / NSP / NSZ 用 `nx2h/vendor/switchfs`（totk-android 的读取代码，MIT）在软件自己的进程里解密 control 数据；NUP 读 meta 块。文件里没有时按 title ID 联网取（见[桌面软件](#桌面软件)）。
- `nx2h/fonts.py`：主机的系统字体，从你自己的一张卡带镜像或你主机导出的固件里取一次，所有游戏共用。
- `nx2h/bundle.py`、`nx2h/signing.py`、`nx2h/agc.py`：每个游戏的包名和签名（在后台向华为申请调试证书和 Profile）。
- `nx2h/mirrors.py`：按测速选择下载源；在新电脑上实测之前一直关着。
- `nx2h/backends/`：每个已适配版本一个适配层，各自驱动一个游戏后端（王国之泪：NativeGames-harmonyos；Z-A：LZA；时之笛：OOT，另需 nxnative；三个后端都建在固定提交的 totk-android 上）。
- `backends/`：各游戏后端的源码，是从各后端自己的工程导入的副本（见 [backends/README.md](backends/README.md)），另有预编译的 libopus / libvpx（`prebuilt-ohos`）和 `backends/SOURCES.tsv`：记着每一份从哪来，并固定 totk-android 和 nxnative 的提交。这两个用时再取，不拷进来：“环境”页把 totk-android 克隆到 `backends\totk-android`；nxnative 放到 `backends\nxnative` 或本仓库旁边。后端也可以放在别处，在 `nx2h.local.json` 的 `backends` 里写它的位置。缺后端时，软件能识别对应的游戏，但不能构建。
- `packaging/`：`build.py` 打包软件；`full_bundle.py` 用已提交的 `backends/` 为干净的电脑做完整包（打包好的软件、各后端的源码、固定提交的整个 totk-android、时之笛要用的那部分 nxnative、预编译库，不含游戏内容）。拷贝任何东西之前，它会拒绝 `backends/` 下没提交的改动、能看出是谁、在哪打的包的文件，以及 `packaging/bundle_pictures.tsv` 里没登记的图片。`backend_import.py` 从后端自己的工程导入或更新后端。
- `games.tsv`：鸿蒙验证表（已验证和优化中的版本）。
- `docs/`：[安装](docs/install.zh-CN.md)、[工作原理](docs/how-it-works.zh-CN.md)，以及 `docs/DESIGN.md`（中文的设计历程：A 段是统一入口，B 段是照 nxnative 做的通用内核）。
- `tests/`：不需要游戏文件的检查：`python -m unittest discover -s tests`。
- `.githooks/`：提交前拦下它能认出来的东西：按名字认（游戏文件和游戏数据、密钥、打好的包、库和压缩包、签名文件、本机设置、构建和工作文件夹），按大小认（超过 20 MB 的文件），按内容认（像 `prod.keys` 里那样的主机密钥行），只放行不含签名的后端模块构建文件和 `backends/prebuilt-ohos` 里的预编译库；每个克隆运行一次 `git config core.hooksPath .githooks` 启用。
- [CONTRIBUTING.md](CONTRIBUTING.md)：怎么参与；[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)：第三方组件和各自的许可。

## 致谢

- **[nxnative](https://github.com/seedds/nxnative)**（MIT）：NX2Harmony 照着它的思路做。已适配的版本一步出包，其余的识别出来并显示状态，构建进度用 nxnative 前端读取的事件格式输出。时之笛后端运行它的一部分（着色器拆分、mesh 阶段和它的 Ampere 版 Ryujinx），`nx2h/icon.py` 读 NUP 的代码改编自它的 `scripts/pack_nup.py`。
- **[totk-android](https://github.com/AurelioB/totk-android)**（MIT）：所有后端都建在这个安卓移植之上，固定在一个提交。`nx2h/vendor/switchfs` 里读取游戏文件的代码来自它。
- **[d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)**（MIT）：把 Switch 游戏变成原生代码，是从这个项目起步的；它的成果经由 totk-android 来到本项目。
- **Ryujinx**（MIT）：后端构建的着色器翻译器以它为基础。
- **NUP 规范**（[a55uka/nup](https://github.com/a55uka/nup)）：Switch 2 的包格式，`nx2h/scan.py` 用自己写的代码读取。
- **NX2Apple**：建在 nxnative 之上的 Mac 前端，本软件的流程照它来（“游戏库”页、“环境”页、“构建”页）。它是社区项目，没有拷它的代码。
- **libopus** 和 **libvpx**（BSD）：游戏的声音和 VP9 影片；为手机预编译好的版本在 `backends/prebuilt-ohos`（本仓库和完整包里都有）。
- 打包好的软件里带有 Qt / PySide6（LGPL v3）、cryptography、CPython 和 PyInstaller 的启动器，各部分和它们的许可列在 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

## 许可证

本项目以 MIT 许可发布（[LICENSE](LICENSE)）；第三方部分及其各自的许可列在 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
