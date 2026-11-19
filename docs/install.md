# Install

[← README](../README.md) · [中文](install.zh-CN.md)

On a Windows PC, the steps below take **your own** game files to a signed HarmonyOS app on your phone, with the game data sent over to it. The first build of a game is long and mostly runs by itself; later builds of the same game reuse what was prepared and only pack and install again. How the parts fit together: [how-it-works.md](how-it-works.md). An AI assistant works from the [install skill](../.claude/skills/install-nx2harmony/SKILL.md) (`/install-nx2harmony`) instead, a checklist version of this page.

Contents: [What you need](#what-you-need) · [Set up the PC](#set-up-the-pc) · [Set up the phone](#set-up-the-phone) · [Build with the window](#build-with-the-window) · [Build from the command line](#build-from-the-command-line) · [Signing](#signing) · [Where things are](#where-things-are) · [When a build stops](#when-a-build-stops) · [Known issues](#known-issues) · [Uninstall](#uninstall)

Before you start:
- Game files, keys and firmware: only your own dumps, from games you own and a console you own. NX2Harmony fetches none of them, and nothing here points to a source.
- What a build makes (the HAP and the game data) holds the game: install it on your own phone and keep it off the internet and away from others.
- A failed check names its cause in its message. The way past it is never an edit to `games.tsv`, to a backend's `expected-*.json` hash files or to its scripts.
- The program's window and messages are in Chinese. This page gives each label as you will see it, with its meaning.

## What you need

**A PC** with Windows 10 or 11, 64-bit (x64). Run the program from a local disk: from a network share or a mapped network drive, cmd.exe, CMake and ohpm fail or hang. The program's Environment (环境) page checks this.

**Free disk space** on the drive that holds the game backends ([Where things are](#where-things-are)): about 80 GB for Tears of the Kingdom or Pokémon Legends: Z-A, about 70 GB for Ocarina of Time, while that game's data is still to be prepared; 5 GB once it is. Docker Desktop keeps its images in a disk image of its own, on top of that.

**A HarmonyOS phone** and a USB data cable ([Set up the phone](#set-up-the-phone)).

**A Huawei account** registered as a developer, with real-name verification. The program signs each game with it ([Signing](#signing)).

**Your own game files**, per game:

| Game | Version | Status | Files |
|---|---|---|---|
| The Legend of Zelda: Tears of the Kingdom (`0100F2C0115B6000`) | 1.4.3 | Verified (已验证) | The base game: the card `.xci` (1.0.0, or a card that already carries 1.2.1), the eShop `.nsp` / `.nsz`, or an `.xcz`; NSZ and XCZ are decompressed inside the build. The 1.4.3 update as `.nsz` only (an `.nsp` you dumped yourself can be compressed to `.nsz` with the nsz tool first). `prod.keys`. |
| The Legend of Zelda: Ocarina of Time, Nintendo Switch 2 version (`0400486024DEC000`) | v131072 | Being optimized (优化中) | The game's `.nup` that holds the v131072 update. `prod.keys`, for the system fonts. |
| Pokémon Legends: Z-A, Nintendo Switch 2 Edition (`0100F43008C44000`) | 2.0.2 | Being optimized (优化中) | The game's `.nup` that holds the 2.0.2 update (v393216). `prod.keys`, for the system fonts. |

Verified: played through on a phone. Being optimized: it builds and plays, but the frame rate and a few pictures are still being tuned. The full table is [games.tsv](../games.tsv). Other versions and games, and a version marked Being adapted (适配中), are recognized and shown, but not built.

**File names.** The program finds games without keys, from the files' own headers, with two exceptions that it reads from the file name: a card image (`.xci` / `.xcz`) needs its title ID in brackets, such as `[0100F2C0115B6000]`, and an `.nsp` / `.nsz` gives its version as `[v…]`, so the 1.4.3 update must carry `[v720896]`. A `.nup` lists its contents itself.

**`prod.keys`**, dumped from your own console. Put it in `%USERPROFILE%\.switch\prod.keys`, where it is found by itself, or choose its folder in the program (`-k` on the command line). The program never copies it and never shows its contents; only the steps that decrypt your files read it.

**System fonts.** Every game asks the console for its shared fonts. A card image carries them; an eShop package and a NUP do not. They are read once, with your `prod.keys`, from either:
- one of your own game card images (`.xci`, not necessarily of this game) in the game folder, or
- the firmware dumped from your own console, unpacked (a folder of `.nca` files), inside or next to the folder that holds `prod.keys`, or named as `"firmware"` in `nx2h.local.json`.

Once read they are kept for every game. With a Tears of the Kingdom `.xci` as the base, nothing more is needed; an `.xcz` base, like an eShop one, needs one of the two.

## Set up the PC

Two ways to the same checks:
- **The Environment (环境) page** of the program. Every row is a check; **一键安装缺少的** (install what is missing) installs, with winget, everything that needs no account, and **重新检查** (check again) runs the checks again. A row you have to download yourself has an **打开下载页** (open the download page) button.
- **`一键安装环境.cmd`**, double-clicked in the checkout when you run from source (the packed program has no such file: use its Environment page or `nx2h.exe setup --fix`). When `where python` finds no Python, it first installs Python 3.12 with winget and asks you to close the window and double-click it again; then it runs the same checks and installs (`python nx2h.py setup --fix`). The Microsoft Store's `python` alias counts as found, so on a PC where `python -V` prints nothing, opens the Store or says Python was not found, install Python yourself first (`winget install -e --id Python.Python.3.12 --source winget` in a terminal), then double-click the file again.

What is installed for you, when it is missing:

| Item | When | How |
|---|---|---|
| Python 3.9 or newer | always: it runs the backends' scripts (the packed program carries a Python only for itself) | `winget install -e --id Python.Python.3.12 --source winget` |
| Docker Desktop, running | always: the Linux tools run in containers | `winget install -e --id Docker.DockerDesktop --source winget`; the next **一键安装缺少的** (or `setup --fix`) starts it and waits up to 3 minutes. If it is still not ready, open it: it may need WSL 2 installed or Windows restarted |
| zstd | Ocarina of Time (its shader list) | `winget install -e --id Meta.Zstandard --source winget` |
| PowerShell 7 | only to build libopus when no prebuilt one is there (`backends\prebuilt-ohos` brings one, in a checkout and in the full package) | `winget install -e --id Microsoft.PowerShell --source winget` |
| Git | only for a source checkout whose reference project totk-android must be cloned | `winget install -e --id Git.Git --source winget` |
| Java | only when the DevEco Studio in use has no Java of its own | `winget install -e --id Microsoft.OpenJDK.21 --source winget` |
| totk-android at its pinned commit | source checkouts, cloned into `backends\totk-android` (the full package brings its own copy) | the backend's `native-harmonyos/upstream.py` |
| Huawei account login | for signing; offered once DevEco Studio is installed | opens the login page in your browser |

What you do yourself:
- **DevEco Studio.** The **打开下载页** button opens Huawei's download page (https://developer.huawei.com/consumer/cn/deveco-studio/). Install it, log in with your Huawei account and open it once, so that it downloads the HarmonyOS SDK. hdc, the tool that talks to the phone, comes with it. When several are installed, the one with the newest SDK is used.
- **nxnative**, for Ocarina of Time when you run from source: clone [nxnative](https://github.com/seedds/nxnative) into `backends\nxnative` or beside this repository and check out the commit `backends/SOURCES.tsv` names, or name its place as `"nxnative"` in `nx2h.local.json`. The game backends themselves, NativeGames-harmonyos (Tears of the Kingdom), LZA (Z-A) and OOT (Ocarina of Time), are part of the checkout, in `backends\`; the full package carries them, nxnative and totk-android in its own `backends\`. Without a backend, its game is recognized but not built.

Starting the program:
- **Packed:** `NX2Harmony.exe` in the NX2Harmony folder: the full package from this repository's Releases page (`NX2Harmony-0.1.3-windows-x64-full.zip`, unzip it anywhere), or `dist\NX2Harmony` after packing. The folder is portable: copy it anywhere and its settings go with it in `data\`. `nx2h.exe` beside it is the command line.
- **From source:** with Python 3.9 or newer installed, double-click `NX2Harmony.cmd`. Its first start installs PySide6 and cryptography from `requirements.txt` (about 80 MB).
- **Packing it yourself:** `python packaging/build.py`, in a Python with the packages of `packaging/requirements-build.txt` (PyInstaller, PySide6-Essentials, Pillow, cryptography). The result is `dist\NX2Harmony`; its `data\` is kept across packings. A full package for a clean PC: `python packaging/full_bundle.py --out <folder> [--zip]` ([how-it-works.md](how-it-works.md#the-full-package)).

## Set up the phone

1. Settings → About phone: tap the version number several times until developer mode turns on.
2. Settings → System → Developer options: turn on USB debugging.
3. Connect the phone to the PC with a data cable and allow debugging when the phone asks.

The top bar of the program then shows the phone's model and system (**手机未连接**, phone not connected, while there is none); it looks again every 5 seconds. Connect one phone at a time: with several, the program cannot tell which to use (unless `nx2h.local.json` names one under `"hdc"` → `"target"`). When DevEco Studio runs its own hdc server, the program finds the phone there too and remembers that server.

You do not have to keep the screen awake yourself: from signing until the game opens, the build wakes the phone every 20 seconds and holds its screen on; afterwards, also after an error or **停止** (stop), the phone gets its own screen-off time back. Keep the cable in until the game opens.

Leave room on the phone for the game data: about 17 GB for Tears of the Kingdom, about 8 GB for Z-A and more than 26 GB for Ocarina of Time (its RomFS image alone is about 26 GB). Z-A's first install needs about 7.5 GB more for a while, for its shader unpacking, which is deleted again at the end.

## Build with the window

Open NX2Harmony. Its three pages are along the top.

**Library (游戏库)**
1. **游戏文件夹** (game folder) → **更换…** (change): choose the folder with your game files. It is scanned with all its subfolders for `.nsp`, `.nsz`, `.xci`, `.xcz` and `.nup`; one folder may hold several games.
2. **主机密钥 prod.keys** (console keys) → **选择…** (choose): choose the folder that holds `prod.keys`. A game whose data is already prepared builds without it, but the icons from your files need it.
3. Every game gets a card with its icon and status. The icon comes from your own files first. For a game whose files carry none, the program looks the eShop icon up online by title ID, by itself when the library is scanned (and at a build): first on the image host tinfoil.media, then through the community titledb list's link to the official icon on Nintendo's CDN, and for a few Switch 2 games the picture on Nintendo's own site. Only the title ID goes out (the titledb list is downloaded whole and searched on the PC). Icons are kept in the program's `work\icons` ([Where things are](#where-things-are)). **刷新** (refresh) scans again after you add files; **从网上取图标** (get the icon online) appears for a game still without one.
4. Pick a game and press **构建游戏** (build the game). The line under its title says what is still missing, or that its data is ready and a build only packs again. A game not in `games.tsv` shows **未适配** (not adapted) and cannot be built.

**Build (构建)**: the files found for the game and **在哪里玩** (where to play) with the phone. **构建完装到手机并推送游戏数据** (install on the phone and send the game data) is ticked while a phone is connected; without it the build ends with the signed HAP. **开始构建** (start) starts, **停止** (stop) stops. The page shows each stage with its time, the running step with the newest line of its tool's output, a progress bar (gigabytes and minutes left while data is sent) and the build log. At the end, **打开 HAP 所在文件夹** opens the HAP's folder and **打开日志文件夹** the log folder.

**Environment (环境)**: see [Set up the PC](#set-up-the-pc). When something other than the phone or the keys is missing, the program opens on this page.

What a build does:
1. **准备游戏数据** (prepare the game data): unpack, rewrite the game code, translate the shaders. By far the longest the first time.
2. **编译原生库** (build the native libraries).
3. **打包 HAP** (pack the HAP): signed, every entry audited, with the game's icon.
4. **安装到手机** (install on the phone), and mark it as a game for the system.
5. **推送游戏数据** (send the game data): files the phone already has are skipped.
6. Z-A's first install only: **着色器种子** (shader seed), see below.

Then the game opens on the phone. Before stage 1 the build gets the signing in the background when this game has none yet, or when it does not cover this phone ([Signing](#signing)).

Stage 1 per game, and what takes long the first time:
- **Tears of the Kingdom:** unpack the 1.4.3 update, merge the base and update RomFS (checked against 1.4.3), RomFS patches (with the screen layout, see [Known issues](#known-issues)), system fonts, shader translator, rewrite the game code, guest data, shader seed (about 3 minutes the first time), third-party libraries. The translator is built in a .NET SDK container pulled from mcr.microsoft.com, and the code tools run in an image (`nativegames-tools:1`) built once on this PC.
- **Pokémon Legends: Z-A:** unpack the NUP, rewrite the game code, runtime tables, pack the RomFS, system fonts, shader translator, guest library (with the wide-screen camera patch), game data, third-party libraries. Its shader seed is made after the first install: the app starts once on the phone to unpack the game's shaders with the game's own decoder (about 1 minute), the PC fetches them, translates them (about 1 hour) and sends the seed back, and the unpacked files are deleted from the phone.
- **Ocarina of Time:** unpack the NUP, check the game modules, store them uncompressed, rewrite the game code, runtime tables, pack the RomFS (about 12 minutes), system fonts, guest library (frame step and dynamic resolution patches), game data, shader translator, then nine shader steps, all on the PC: extract every shader (about 10 minutes), SPIRV-Cross, translate (about 10 minutes), validate, SSA (about 8 minutes), the variants and shared-memory rewrites found in testing (about 2 minutes), task and mesh shaders as compute kernels, wrap them, pack the seed; then libopus / libvpx and dav1d (the AV1 movies).

Each step that finds its output already there says **已有，跳过** (already there, skipped). Closing the window during a build asks first and stops it; finished steps are skipped next time.

## Build from the command line

From a checkout `python nx2h.py`, beside the packed `NX2Harmony.exe` `nx2h.exe`; the commands are the same.

```
python nx2h.py setup [--fix]
python nx2h.py scan <folder>...
python nx2h.py build [<folder>...] [-k <prod.keys folder>] [--game <title id>] [--tag <tag>] [--no-install] [--screen <W>x<H>] [--progress json]
```

- `setup` checks this PC and the phone; `--fix` installs what winget can, as **一键安装缺少的** does.
- `scan` lists the games and versions in the folders, each game's files and what it still needs.
- `build` prepares, packs the signed HAP, installs it, sends the game data and opens the game.
  - Without a folder, a dialog asks for one and remembers it; this is what `一键构建.cmd` does (it passes its arguments on).
  - `-k`: the folder that holds `prod.keys` (or the file itself). Without it: the folder chosen before, else `%USERPROFILE%\.switch`; when the keys are still missing and needed, a dialog asks.
  - `--game <title id>`: needed when the folders hold more than one game that can be built; the build stops and lists them. `0100F2C0115B6000` Tears of the Kingdom, `0400486024DEC000` Ocarina of Time, `0100F43008C44000` Z-A.
  - `--no-install`: only the signed HAP. No phone is needed once your Huawei account has one registered; a build with the phone connected registers it when it signs. With `--no-install` the connected phone is not registered, so when the account has no phone yet the signing stops with `这个华为账号还没有登记任何手机` (this Huawei account has no phone registered). To put the HAP on the phone later, connect the phone and run the same build without `--no-install`: it reuses everything, installs and sends the data.
  - `--screen <W>x<H>`: for Tears of the Kingdom, the screen its interface is laid out for, such as `2848x1320` (either order). Without it the connected phone is asked.
  - `--tag`: the HAP's tag, by default `nx2h-<date>-<time>`. The HAP is `<backend>\artifacts\release\<tag>\totk-harmonyos-<tag>-signed.hap`, the same pattern for every game.
  - `--progress json`: one JSON event per line, for a front end ([how-it-works.md](how-it-works.md#progress-events)).

Exit codes: `0` done; `1` stopped on an error (the last line starting with `[RED]` says why); `2` bad arguments, or the folder dialog was closed; `130` interrupted (Ctrl+C).

```
python nx2h.py scan D:\Games
python nx2h.py build D:\Games -k D:\keys --game 0100F2C0115B6000
```

## Signing

A HarmonyOS app installs only when it is signed, and a debug signing names one bundle name and the phones it may go on. Each game gets its own bundle name, made by the program: `nx2h.<this PC>.<game>`, such as `nx2h.uk3f9a2.totk`. The middle part is derived from this Windows installation, so the window, the command line and every copy of the packed program use the same names; a name, once made, is kept in `nx2h.local.json` under `"bundles"`. The app on the phone and its saves belong to that name.

**In the background (the default).** The first time a game needs signing, the build opens the Huawei account login page in your browser (or press **一键安装缺少的** beforehand: the Huawei account row does the same). After you log in, the program, by itself:
- makes one key and one debug certificate for this PC (with the keytool of DevEco Studio's own Java), shared by every game;
- adds the connected phone to your account's devices;
- asks for a debug profile for the game's bundle name that covers every phone of your account.

DevEco Studio must be installed but does not have to be open. This is the same service DevEco Studio's automatic signing uses; it is not a documented public interface, so Huawei may change it. A new profile is asked for when the old one is about to expire or does not cover the connected phone. With `--no-install` no phone is added: the profile covers the phones already in your account, and with none there the signing stops ([Build from the command line](#build-from-the-command-line)).

Kept in `%USERPROFILE%\.nx2h`, which belongs to your Windows user: `account.dat` (the login) and `keys\key.dat` (the key's passwords) are sealed with Windows DPAPI, so only this Windows user on this PC can open them; `keys\` also holds the key store, the certificate, the profiles (`profiles\<bundle name>.p7b`) and the `material\` that hvigor decrypts the passwords with.

**In DevEco Studio (the fallback).** When the background signing fails and DevEco Studio is installed, the program makes a small signing project, `%USERPROFILE%\.nx2h\signing\NX2H_<game>` (`<system drive>\nx2h-signing\NX2H_<game>` when your user folder's path has spaces or non-English characters), opens it in DevEco Studio and stops with **打开 DevEco Studio 签名** (open signing in DevEco Studio). There: log in with your Huawei account, File → Project Structure → Signing Configs, tick Automatically generate signature, OK, with the phone connected. Then start the build again. The project is a copy of the backend's app shell (its configuration with the same permissions, the shell's own sources and resources, the hvigor files), with no game code, no built libraries and no game data; DevEco Studio keeps what it signs in `%USERPROFILE%\.ohos\config`.

**During the build** the backend's `AppScope\app.json5` (bundle name) and `build-profile.json5` (signing) are swapped for this game's and put back right after; the originals wait in the backend's `prepared\nx2h-stage\signing`, and a build that was stopped puts them back at the start of the next one.

**Never shown or copied.** Signing files and passwords are read inside the program only: never printed, logged, copied off this PC or put in a repository. The login's own record, `%USERPROFILE%\.nx2h\login.log`, keeps request lines and field names, never values. No HAP, game data or signing material is ever published with this project.

Your Huawei account has limits: when it says the number of devices or debug profiles is at the limit, delete unused ones in AppGallery Connect (device management, profile management) and build again.

## Where things are

| What | Packed program | From source |
|---|---|---|
| Settings: game folder, keys folder, backend places, bundle names, phone connection | `data\nx2h.local.json` | `nx2h.local.json` |
| Logs, one per step: `<date>-<time>-<game>-<step>.log` | `data\logs\` | `logs\` |
| Game icons | `data\work\icons\` | `work\icons\` |
| System fonts read from your files, for every game | `data\work\shared-fonts\` | `work\shared-fonts\` |

The packed program's `data\` sits beside `NX2Harmony.exe`; when that folder cannot be written (for example under Program Files), `%LOCALAPPDATA%\NX2Harmony` is used. From source these files sit in the checkout, and git ignores them.

- **Backends:** in the full package, `backends\` beside `NX2Harmony.exe` (also totk-android, nxnative and the prebuilt libraries); from source, the checkout's own `backends\` (totk-android is cloned into it, and nxnative may go there too); or where `nx2h.local.json` says under `"backends"`. A place remembered there is used while that folder exists, so a checkout that used backends beside it before 0.1.2 keeps using them until you remove the entry.
- **Game data made from your files**, tens of GB: each backend's `prepared\`. Built libraries, the shader translator and the HAPs: its `artifacts\`.
- **While a step reads a game file**, Tears of the Kingdom may hard-link it into a folder of its own (the backend's `prepared\nx2h-stage`, the program's `work\stage`, or `.nx2h-stage` beside the file); only where hard links are impossible (exFAT, a network or virtual-machine shared folder) is it copied. These are removed once the step is done.
- **Signing and the Huawei login:** `%USERPROFILE%\.nx2h\` ([Signing](#signing)).
- **`prod.keys`:** wherever you keep it; `%USERPROFILE%\.switch\prod.keys` is found by itself.
- **On the phone:** the app (`nx2h.<this PC>.<game>`) and, in its own storage (`/data/storage/el2/base/haps/entry/files`), the game data: `game-v143/`, `game-za/` or `game-oot/` (runtime tables, RomFS image, fonts), `android/shaders/<version>/` (the shader seed), and for Tears of the Kingdom `android/extracted/v143/romfs-patches*/` (patched files and the screen layout). The saves are in the app's storage too.

## When a build stops

Read the last line that starts with `[RED]`. In the window it is in the build log, and the note shown at the end points to it; the command line prints it last. It says what is wrong, and when a tool failed it names that step's log, whose last lines usually give the cause. Fix it, then press **开始构建** again or run the same command: steps whose output is there are skipped, so the build goes on where it stopped.

The first build downloads container images (`python:3.14` from Docker Hub, the .NET SDK from mcr.microsoft.com) and packages. On a slow or unstable network a download or an image build can break off; nothing is lost, build again when the network is better. An image is built up to four times in all before the build gives up; the third and fourth tries use other Debian mirrors.

The messages are in Chinese; the table gives how each begins.

| The `[RED]` line | What to do |
|---|---|
| `这些文件夹里没有已适配、可一键构建的游戏` (no adapted game) | `python nx2h.py scan <folder>` shows what was found. Check the file names ([What you need](#what-you-need)) and that the version is in `games.tsv`. |
| `找到不止一个可构建的游戏` (more than one) | Add `--game <title id>` (the window does it itself). |
| `缺本体…`, `缺 1.4.3 更新 .nsz…`, `缺…的 NUP…` (missing file) | Put the named file in the game folder, with the tags in its name. |
| `…盘剩余 … GB，准备…约要 … GB` (disk) | Free space on that drive, or move the program with its backends to a drive with room. |
| `需要 prod.keys` | Choose its folder (`-k`), or put it in `%USERPROFILE%\.switch`. |
| `没有找到连接的手机` (no phone) | [Set up the phone](#set-up-the-phone), or build with `--no-install`. |
| `构建要用本机的 Python 3.9 或更新版本` | Install Python on the Environment page, then open the program again. |
| `自动安装没成功：退出码 0x8A15005E…` (install failed: certificate) | Something on the PC replaces HTTPS certificates (an accelerator, an antivirus's HTTPS scanning, a proxy) and winget's Microsoft Store source refuses it. From 0.1.3 the program installs from the winget source only; with an older one, run the command the row names in a terminal (`winget install -e --id <package> --source winget`), or switch that scanning off. |
| `后台签名没成功` (background signing failed) | Check the Huawei account (developer, real-name verified) and build again, or sign in DevEco Studio ([Signing](#signing)). |
| `Docker 镜像 … 都没拉下来`, `… 建了 … 次都没成功` (image) | The network: build again later. |
| `…这一步要用 Docker Desktop` | Start Docker Desktop (the Environment page installs and starts it). |
| `本体是 eShop 版…不带系统字体` (no fonts) | Put the firmware dumped from your own console next to the `prod.keys` folder, or one of your own game card images (`.xci`, not necessarily of this game) in the game folder. |
| `还没有参考仓库 totk-android`, `…不在固定提交` (reference) | On the Environment page, fix the row **参考仓库 totk-android 在固定提交** (reference repository at its pinned commit), or run `python native-harmonyos/upstream.py --update` in the backend. |
| `软件包自带的参考仓库 totk-android 缺失或不完整` | Unpack your copy of the full package again, completely, to the same place (or download it again from this repository's Releases page). |
| `找不到…的后端` (backend missing) | Packed: unpack your copy of the full package again, completely, to the same place. From source: the backend is part of the checkout; restore it with `git checkout -- backends`. Or write its place in `nx2h.local.json` under `"backends"`. |
| `缺 nxnative 仓库` | Clone nxnative into `backends\nxnative` or beside this repository, or name it as `"nxnative"` in `nx2h.local.json`. |
| `抽出着色器要用 zstd 程序`, `编译 libopus 需要 PowerShell 7` | Install it on the Environment page. |
| `…和 assets/expected-oot-v131072.json 对不上` | These are not the v131072 game files: check the NUP. |
| `HarmonyOS SDK 不完整` | Open DevEco Studio once and let the SDK finish downloading. |
| `…里还有别的游戏文件，建不了硬链接…` | Move that file into a folder of its own, or free the space it names. |
| `安装失败` (install failed) | Check the phone: unlocked, enough space, debugging allowed. An app of the same bundle name already on the phone but signed with another certificate (by DevEco Studio, or with a key since deleted from `%USERPROFILE%\.nx2h`) must be uninstalled first; export its saves before that. |
| `手机上的着色器解包 15 分钟没结束`, `手机上没有生成 bnsh.pack` (Z-A) | Build again with the phone connected. |
| `<step> 失败（退出码 N），日志：…` (step failed) | Open that log; its last lines give the reason. |

Still stuck? Open an issue in this repository with the build problem form. Give the game and version, the Windows version, how you run the program (packed, or from source with its commit: `git rev-parse --short HEAD`), the phone model and system (as the top bar shows them), the `[RED]` line and the last lines of the log it names. Before you paste, shorten paths that show your Windows user name, and replace with `***` the phone's serial number, your Huawei account and team name, e-mail addresses and the middle part of the bundle name (`nx2h.***.totk`). Never attach `prod.keys`, firmware, game files, a HAP, game data, signing files or `nx2h.local.json`.

## Known issues

- **Pokémon Legends: Z-A is Being optimized (优化中):** it builds and plays; the frame rate and a few pictures are still being tuned.
- **Ocarina of Time is Being optimized (优化中):** it builds and plays, but a bug turned up in testing, so it is no longer marked Verified.
- **First builds are long:** the long steps are listed under [Build with the window](#build-with-the-window); Z-A's shader translation alone takes about an hour. Later builds of the same game only pack and install again.
- **Tears of the Kingdom's screen layout:** the build lays the interface out for the phone's screen (the HUD at the screen's edges, backgrounds and cutscenes filling it) when the screen is more than 1% wider than 16:9. It asks the connected phone (or takes `--screen`); without a phone it lays out nothing this time and the HUD stays in the middle 16:9 until a build with the phone connected. The layout can only go into a folder that the app makes itself, which it does when it starts with 画面比例 (aspect ratio) set to 全面屏 (full screen). On a fresh install that is normally the case and the layout goes over in the same run. When the output says the phone has no folder for it (16:9 chosen, or the app last started for another screen size), choose 全面屏 in the app's settings, tap 开始游戏 (start the game) once, then build again: only the missing files are sent. In the game's settings, 画面 (display) → HUD 居中 16:9 (centered) brings back the original layout.
- **Shaders:** a seed may lack a few shaders; they are filled in while you play. Fetching the phone's list of missing shaders back to the PC from the program is planned, not done ([DESIGN.md](DESIGN.md)).
- **Saves** live in the app's own storage on the phone. Uninstalling the app deletes them, and NX2Harmony cannot back them up or restore them yet (planned). Tears of the Kingdom's launcher has a 存档 (saves) tab that exports them as a zip; use it before uninstalling. A build made on another PC gets another bundle name: it installs as a separate app with saves of its own.
- **Signing** uses a Huawei service that is not a documented public interface; if it changes, sign in DevEco Studio ([Signing](#signing)).
- **Downloads:** choosing the fastest download source (`nx2h/mirrors.py`) is written but switched off until it has been tested on a fresh PC.
- **One phone at a time**, and the program must run from a local disk ([What you need](#what-you-need)).
- The window and the messages are in Chinese only.

## Uninstall

**On the phone:** uninstall the app like any other, or `hdc uninstall <bundle name>`. The bundle name is in the build log (`包名：…`) and in `nx2h.local.json` under `"bundles"`. Its game data and its saves go with it: export the saves first.

**On the PC:**
1. **The program:** delete its folder, `data\` included (or the checkout, with `nx2h.local.json`, `work\` and `logs\`); also `%LOCALAPPDATA%\NX2Harmony` if it was used.
2. **The game data:** each backend's `prepared\` and `artifacts\` (tens of GB made from your files, and the HAPs), or the backend folders themselves (`backends\` in the full package).
3. **Signing:** `%USERPROFILE%\.nx2h` (key, certificate, profiles, the saved login) and, if it was used, `<system drive>\nx2h-signing`. What DevEco Studio signed itself is in `%USERPROFILE%\.ohos\config`.
4. **Docker:** the images and the volume the backends use, when nothing else of yours needs them:

   ```
   docker image rm nativegames-tools:1 python:3.14 mcr.microsoft.com/dotnet/sdk:10.0-noble-aot
   docker volume rm totk-venv
   ```

   The backends' own scripts can make further volumes; `docker volume ls` lists them.
5. **Tools installed for the build:** Windows Settings → Apps, or `winget uninstall --id <id>` with the IDs from [Set up the PC](#set-up-the-pc). DevEco Studio uninstalls like any program.

**In your Huawei account** the debug certificate, the profiles and the registered phone stay; delete them in AppGallery Connect if you no longer want them.

Your game files, `prod.keys` and firmware are never changed by the program; they stay where you keep them.
