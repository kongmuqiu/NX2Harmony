---
name: install-nx2harmony
description: Prepares a Windows PC and turns the player's own Switch game files and keys into a signed app on their own HarmonyOS phone with NX2Harmony (Tears of the Kingdom 1.4.3, Ocarina of Time v131072 for Switch 2, Pokémon Legends Z-A 2.0.2 Switch 2 Edition), working through whatever stops the build on the way. Use it when a player wants one of these games on a HarmonyOS phone, or pastes NX2Harmony output (a [RED] line, a ✗ row of nx2h.py setup, a signing message).
---

# Install a game with NX2Harmony

The job: start from game files and keys that the player dumped from their **own** console, and end with the game
running on **their own** HarmonyOS phone, from a Windows 10/11 x64 PC. In short: `python nx2h.py setup` checks the PC,
`python nx2h.py scan` identifies the files, the phone gets connected, and a single `python nx2h.py build` prepares the
game data, compiles the native libraries, builds, signs and installs the HAP, sends the game data and opens the game. A
first build is long, and the PC does nearly all of it unattended; Pokémon Legends: Z-A adds a shader stage that the
build itself labels as about an hour. Later builds reuse whatever is already prepared. Take the steps one at a time and
look at each result before starting the next. For the player there is a matching guide, `docs/install.md` (Chinese:
`docs/install.zh-CN.md`); `docs/how-it-works.md` describes the pipeline.

The commands below are for PowerShell, run from the repository root. With the packed app, use
`nx2h.exe <command> ...` from the folder that holds `NX2Harmony.exe` in place of `python nx2h.py <command> ...`.

## Rules

- **Nothing but the player's own dumps.** Every game file, `prod.keys` and firmware used here was taken by the player
  from their own console, and nothing else is a source: not another person, not a website, not a link you could fetch,
  and you never hint at one. A gap is closed only by a dump the player makes; say which one is still needed and go no
  further until they have it.
- **Keys and signing material stay unread.** The build gets the folder holding `prod.keys` through `-k`; never open,
  print, copy or move the file itself (no `Get-Content`, no `type`). The same goes for `build-profile.json5`, `*.p12`,
  `*.p7b`, `*.cer` and everything under `%USERPROFILE%\.nx2h`. Of `nx2h.local.json` you may name the settings it holds,
  never their values.
- **Checks are not negotiable.** A failed check is never answered by editing `games.tsv`, the code under `nx2h/`, a
  backend's hash lists or HAP audit, or `.gitignore`. When a hash or module check fails, the player's dump is damaged
  or comes from a different release of the game; say so. Any other failure is handled the way its message says, or the
  way step 6's table says where the two differ (the table gives the command-line form of a fix, for example).
- **Build output is the game itself and stays with the player:** the HAP, the data sent to the phone, and a backend's
  `prepared\` and `artifacts\` folders. None of it is uploaded, committed, attached to anything or passed to anyone,
  and you never propose that.
- **Some steps need the player's own hands:** logging in to the Huawei account in the browser (a developer account with
  real-name verification), installing DevEco Studio, answering UAC prompts (winget, Docker Desktop, WSL 2), restarting
  the PC, turning on developer mode and USB debugging, allowing debugging on the phone, and everything inside the game.
  Never type a password for them. A player using Claude Code can run a command in this session by starting it with `!`.
- **The app holds the saves.** They live inside the app on the phone and vanish when it is uninstalled, so ask before any
  step that uninstalls it. Do not change the phone's settings yourself; the build keeps the screen awake while it runs
  and restores the screen-off time afterwards on its own.
- **One build at a time, never cut short.** Leave a running build alone and start no second one next to it. Should one
  end early all the same, the steps it completed are not lost: running the unchanged command again resumes after the
  last of them.
- **The player's language, the PC's facts.** Reply in the language the player writes in. Look up yourself what `setup`
  and `scan` can report; only the player can say which folder holds their files and which phone to use.

## 0. What does the player want?

| Case | Go to |
| --- | --- |
| The first game on this PC | steps 1–5 |
| Another game on a PC that has built one | steps 2, 4 and 5 |
| Build again after updating NX2Harmony, or remove a game | step 7 |
| An error message | the table in step 6, then the step it came from |

## 1. Check the PC

```powershell
python -V
python -c "import PySide6, cryptography; print('ok')"
python nx2h.py setup
```

- `python -V` has to print Python 3.9 or newer. If it prints nothing, opens the Microsoft Store or says Python was not
  found, the `python` on this PC is only the Microsoft Store alias: Python is missing. The player then runs
  `winget install -e --id Python.Python.3.12 --source winget` and opens a new terminal. `一键安装环境.cmd` does not help here: it installs
  Python only when `where python` finds none, and the Store alias counts as found, so it skips the install and then fails.
- When the import line fails: `python -m pip install -r requirements.txt` (PySide6 and cryptography, about 80 MB;
  `NX2Harmony.cmd` installs the same on its first start). `setup` does not check these two, but the build needs them:
  cryptography for the signing, PySide6 for putting the game's icon into the HAP.
- `setup` prints one row per check: `✓` ready, `✗` missing (with a `→` line under it saying what to do), `·` information
  only. Its last line is `环境就绪，可以构建。` or `还有 N 项没准备好` (exit code 1). That count includes the Huawei
  account row, which the first build settles by itself, and the `prod.keys` row, for which `-k` on the build is enough
  (see the table).

`python nx2h.py setup --fix` installs or starts what it can and then shows the rows again. It can take a long time
(the WSL download, the Docker Desktop install, up to 5 minutes for Docker Desktop to come up): run it detached or
with a long timeout and never kill it midway, or what it installed is not recorded. It runs winget (the UAC
prompts are the player's to answer), waits up to 5 minutes for Docker Desktop to come up and, once the tools the signing
needs are there, opens the Huawei login page in the browser when the account is not logged in yet. Installing Docker
Desktop can take a while. Without WSL 2 it installs WSL first (`wsl --install --no-distribution`, a UAC prompt) and
then asks for a restart of Windows before Docker Desktop can start; Docker Desktop's first start asks the player to
accept its service agreement in its window.

| Row | When it shows `✗` |
| --- | --- |
| `Windows 10/11 x64` | No other system is supported. Stop. |
| `程序位置` (only on a network location) | Copy the whole NX2Harmony folder to a local disk and run it from there. |
| `Python 3.9 或更新（运行构建脚本）` | `setup --fix` (winget `Python.Python.3.12`). From source the row reports the Python that runs `setup`; it matters mostly for the packed app. |
| `WSL 2（Docker Desktop 要用）` (only while Docker Desktop is not running) | `setup --fix` runs `wsl --install --no-distribution` (`wsl --update` for one too old) as administrator: the player answers the UAC prompt. Then the row ends in `重启电脑后才能用`: the player restarts Windows and runs `setup --fix` again. |
| `Docker Desktop 在运行` | `setup --fix` installs Docker Desktop and starts it, or only starts it when it is installed (`已安装但没在运行`), and waits up to 5 minutes. `（还没接受它的服务协议）`: the player presses Accept in Docker Desktop's window (signing in can be skipped). If it still does not come up, the player opens it and does what its window says. |
| `DevEco Studio 和 HarmonyOS SDK` | The player installs DevEco Studio from <https://developer.huawei.com/consumer/cn/deveco-studio/>, logs in with the Huawei account and opens it once so it downloads the HarmonyOS SDK, which can take a while. hdc comes with it. `缺 SDK`: open DevEco Studio again and let the download finish. |
| `Java（签名和打包用）` (only with a toolchain that has no Java of its own) | `setup --fix` (winget `Microsoft.OpenJDK.21`). |
| `<game> 后端（<folder>）` | That game's backend is missing: see "The backends" below. |
| `Git（取参考仓库用）` (only when the totk-android checkout is missing and so is Git) | `setup --fix` (winget `Git.Git`). |
| `参考仓库 totk-android 在固定提交` | `setup --fix` clones the checkout (from source: into `backends\totk-android`) or moves it to the pinned commit. It refuses when that checkout has local changes: ask the player before anything discards them. In the full package this copy cannot be repaired here: the player unpacks their own copy of the package again, completely, in the same place (if they no longer have it, ask where it came from; never look for one online). |
| `华为账号（签名用）` | Stays ✗ until the first login, but does not stop a build: the first build (or `setup --fix`) opens the Huawei login page in the browser and the player logs in once. |
| `nxnative（塞尔达传说 时之笛的着色器工具）` | Ocarina of Time only. nxnative (MIT) belongs in `backends\nxnative` (or beside this repository folder), or `nx2h.local.json` names its place as `"nxnative"`. With the player's go-ahead, in this repository's folder: `git clone https://github.com/seedds/nxnative backends/nxnative`, then `git -C backends/nxnative checkout <commit>` with the commit on the nxnative row of `backends/SOURCES.tsv`. The full package carries its own copy. |
| `zstd（塞尔达传说 时之笛的着色器清单用）` | `setup --fix` (winget `Meta.Zstandard`). |
| `PowerShell 7（编译 libopus 用）` (only when no prebuilt libopus is there) | `setup --fix` (winget `Microsoft.PowerShell`). |
| `prod.keys（你自己的 Switch 导出）` | Looked for only in `%USERPROFILE%\.switch` and in the folder used last time; passing `-k` to the build is enough. Never open the file to check it. |
| `磁盘空间（<drive> 至少 N GB）` | The drive that holds the backends needs 80 GB free for a game still to prepare (70 GB for Ocarina of Time; 5 GB once its data is prepared), and Docker Desktop's images need room too. Free space, or move NX2Harmony with its backends to a bigger drive. |
| `鸿蒙手机已连接（开发者模式 + USB 调试）` | Step 3. Only needed to install. |

**The backends.** Each game's pipeline is developed in a project of its own: NativeGames-harmonyos (Tears of the
Kingdom), LZA (Z-A) and OOT (Ocarina of Time). NX2Harmony builds with the copies in `backends\`: part of this repository
when run from source, and beside `NX2Harmony.exe` in the full package. All three build on the totk-android reference
checkout at a pinned commit (from source `setup --fix` clones it into `backends\totk-android`; the full package carries
its own), and Ocarina of Time needs nxnative as well (its row above). A place that `nx2h.local.json` gives under
`"backends"` (keys `totk_v143`, `za_202`, `oot_v131072`) comes first while that folder exists. Without a backend `scan`
still recognizes its game, but nothing builds. If one is missing from a checkout, `git status -- backends` shows what is
gone and `git checkout -- backends` restores it (ask the player first when that would discard changes they made there).
If one is missing from the full package, the package from this repository's Releases page carries all of them: have the
player unpack it again (or download it again from that page); never look for one anywhere else.

## 2. Find the player's files

First ask the player which folder holds the game files and `prod.keys`. Look only inside the folder they name, and only
at file names, never at contents:

```powershell
Get-ChildItem -LiteralPath "<folder the player named>" -Recurse -Depth 4 -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -eq 'prod.keys' -or $_.Extension -in '.xci', '.xcz', '.nsp', '.nsz', '.nup' } | Select-Object Name, @{n='GB';e={[math]::Round($_.Length/1GB,1)}}
python nx2h.py scan "<game folder>"
```

`scan` reads only the files' headers and names (no keys) in that folder and every subfolder (hidden and system folders
are skipped), and prints each game with its files and a `状态：` line:

- `文件齐全，可以一键构建`: go on.
- `缺...`: something is still missing; step 6's table says what to do.
- `状态：未适配` (the game or version is not in `games.tsv`) or `状态：适配中（<version>）。还不能一键构建` (listed, not
  buildable yet): NX2Harmony builds only versions marked Verified (已验证) or Being optimized (优化中). Tell the player,
  and stop.
- `无法识别：<file>（文件名里没有 [title id] 标签，卡带内容需要密钥才能识别）`: a card image (`.xci` / `.xcz`) is
  recognized only by the title ID in brackets in its file name. Ask the player to rename it, for example
  `Tears of the Kingdom [0100F2C0115B6000].xci`.

| Game (status in `games.tsv`) | Title ID | Files the build takes |
| --- | --- | --- |
| Tears of the Kingdom 1.4.3 (Verified) | `0100F2C0115B6000`, update `0100F2C0115B6800` | The base game: the card `.xci` (1.0.0, or a card that already carries 1.2.1), the eShop `.nsp` / `.nsz`, or an `.xcz` (NSZ and XCZ are decompressed inside the build). The 1.4.3 update, as `.nsz` only, with `[v720896]` in its file name: the version is read from the name, the title ID from the ticket inside (a `[0100F2C0115B6800]` tag in the name counts only when there is no ticket). Only this update works, no other version of it. `prod.keys`. |
| Ocarina of Time v131072, Switch 2 version (Being optimized) | `0400486024DEC000` | The game's `.nup` holding the v131072 update. `prod.keys`, for the system fonts only. |
| Pokémon Legends: Z-A 2.0.2, Switch 2 Edition (Being optimized) | `0100F43008C44000` | The game's `.nup` holding the 2.0.2 update (`v393216`). `prod.keys`, for the system fonts only. |

- An update the player has as `.nsp` must first be compressed to `.nsz` by the player with nsz; the build takes the
  update as `.nsz` only.
- **System fonts** are read once with `prod.keys` and then kept for every game. They come from one of the player's own
  game card images (`.xci`, not necessarily of this game) in the game folder, or from firmware dumped from the player's
  own console, unpacked (a folder of `.nca` files) near `prod.keys`, for example a `firmware` folder next to it. A Tears
  of the Kingdom `.xci` carries them; with an eShop or `.xcz` base, and for Z-A and Ocarina of Time, one of the two is
  needed.
- A `prod.keys` made while the console ran older firmware can lack the keys a newer card or firmware dump needs.
- **Disk:** checked before the first step, on the drive that holds the backend (step 1). **Phone:** the game data takes
  about 17 GB for Tears of the Kingdom and about 8 GB for Z-A (sizes from a test install in `docs/DESIGN.md`), plus about
  7.5 GB on the phone for as long as Z-A's shaders are unpacked there; Ocarina of Time needs more (its RomFS image alone
  is about 26 GB).
- One folder may hold several games: the build then needs `--game <title id>`. Several folders can be given at once.

## 3. Connect the phone (the player)

The player turns on developer mode and USB debugging, in the words `setup` uses: 设置 → 关于手机 → 连点版本号打开开发者模式；
系统 → 开发者选项 → 打开 USB 调试；用数据线连电脑并允许调试 (Settings → About phone → tap the version number repeatedly;
System → Developer options → USB debugging; connect the cable and allow debugging). Then check:

```powershell
python nx2h.py setup   # the last row: 鸿蒙手机已连接 ... with the phone's model
$hdc = "<DevEco Studio folder>\sdk\default\openharmony\toolchains\hdc.exe"   # the folder the DevEco row of setup shows
& $hdc list targets
```

When `setup`'s last row shows the phone, it is ready. In the `hdc` listing a line with a device key means the same;
`[Empty]` means the phone is not connected or not authorized: the player unlocks it, accepts the debugging prompt and
tries another cable or port (a charge-only cable shows nothing), and you check again. NX2Harmony also finds a phone that
another hdc server on this PC already sees (DevEco Studio's own, for example), which this plain `hdc` call may not
show. With more than one phone connected the build uses none of them: unplug the others, or set
`"hdc": {"target": "<key>"}` in `nx2h.local.json`. Leave the cable in until the build is done.

## 4. Build

```powershell
python nx2h.py build "<game folder>" -k "<folder with prod.keys>"
#   several games in the folder:                  --game <title id>      e.g. --game 0100F2C0115B6000
#   the signed HAP only, nothing installed:       --no-install           (see below)
#   Tears of the Kingdom, screen size unknown:    --screen 2848x1320     (the phone's resolution in pixels)
```

Give the game folder and `-k` every time: without a folder the build opens a folder dialog, and without `-k` it does the
same when it finds no `prod.keys` in the places step 1 names. Once a game's data is prepared, `-k` is no longer needed.

`--no-install` makes only the signed HAP, in three stages (`[1/3]` to `[3/3]`). Its signing registers no phone: a game
signed for the first time this way gets a profile for the phones the Huawei account already has, and on an account
without any it stops with `后台签名没成功：这个华为账号还没有登记任何手机`. To install later, connect the phone and run
the same build without `--no-install`: it installs and sends the data, reusing the rest.

A build outlives a normal command timeout. Point its output at a file outside the repository, launch it detached (or
with a limit of several hours), and look into that file from time to time:

```powershell
cmd /c 'python nx2h.py build "<game folder>" -k "<keys folder>" > "%TEMP%\nx2h-build.txt" 2>&1'
```

The output names stages and steps, for example:

```
游戏：塞尔达传说 王国之泪 1.4.3（0100F2C0115B6000）→ <where the HAP goes>
[1/5] 准备游戏数据 · 解包、改写、着色器
  - 解开 1.4.3 更新
    完成
  - 合并本体和更新的 RomFS
```

What a step itself prints goes to a log of its own, `logs\<date>-<time>-<game>-<step>.log` in the repository folder
(packed: `data\logs\` beside `NX2Harmony.exe`, or `%LOCALAPPDATA%\NX2Harmony\logs` when that folder is read-only); the
build output shows only the step names. A long silence is therefore normal: while the newest log keeps growing, the
step is working, so leave it running. A step finished on an earlier run prints `已有，跳过`, so a restarted build moves
quickly past everything already done.

**Signing, the first time for each game.** Before the first stage the build prints
`签名：<bundle name> 还没签名，在后台向华为申请（第一次会在浏览器里打开华为账号登录页）` (or
`签名：<bundle name> 的签名里没有这台手机` when a phone the profile does not cover is connected). On the very first
signing the browser opens the Huawei account login page; the player logs in there, and the build waits up to 10 minutes
for that. The rest needs nobody: once per PC it makes a signing key and debug certificate, it registers the connected
phone when the account does not have it yet (`把连着的手机登记为调试设备`), asks for a debug profile for the game's
bundle name and prints `签名就绪：<bundle name>`. It talks to the same service DevEco Studio's automatic signing uses,
which Huawei does not document as a public interface, so it may change. The login and the keys stay in
`%USERPROFILE%\.nx2h`, sealed with Windows DPAPI for this Windows user. Each game gets a bundle name of its own,
`nx2h.<this PC's part>.<slug>` such as `nx2h.uk3f9a2.totk`, kept in `nx2h.local.json` under `"bundles"`.

When the background signing fails, the build stops with `后台签名没成功：<why>` (step 6's table). If DevEco Studio is
installed, that line also names a small signing project (`%USERPROFILE%\.nx2h\signing\NX2H_<slug>`, or
`<system drive>\nx2h-signing\NX2H_<slug>` when the user folder's path has spaces or non-ASCII characters) and opens it in
DevEco Studio. Either remove the reason and build again, or have the player sign that project by hand: phone connected,
logged in, File → Project Structure → Signing Configs → tick Automatically generate signature → OK. Then run the same
build again.

**What runs** the first time (a later build reuses what is there). Tears of the Kingdom:

| Stage | Steps, as printed |
| --- | --- |
| `[1/5] 准备游戏数据` | `解开 1.4.3 更新` (the first run also fetches the `python:3.14` image); `合并本体和更新的 RomFS` (an NSZ / XCZ base is decompressed here); `RomFS 补丁文件` (for a screen wider than 16:9 also the UI laid out for it); `系统字体`; `着色器翻译器` (the first run fetches the .NET SDK image from mcr.microsoft.com and builds the translator); `改写游戏代码` and `客体数据` (the first run builds the tools image `nativegames-tools:1`); `着色器种子（首次约 3 分钟）`; `第三方静态库` (the package's prebuilt libopus / libvpx, or built once) |
| `[2/5] 编译原生库` | the two native libraries |
| `[3/5] 打包 HAP · 签名并审计` | prints `包名：<bundle name>`, puts the game's icon in, then builds, signs and audits every entry of the HAP |
| `[4/5] 安装到手机` | `hdc install`, then one start so the app makes its folders |
| `[5/5] 推送游戏数据` | the game data; files the phone already has are skipped |

The icon at `[3/5]` comes from the game files (a card or eShop package needs `prod.keys` for it; a `.nup` may carry one).
When they have none, it is looked up online by title ID: the image host tinfoil.media first, then the community titledb
list's link to the official icon on Nintendo's CDN, and for a few Switch 2 games the picture on Nintendo's own site.
Only the title ID goes out. With no icon at all the HAP keeps its default one
(`这个游戏没取到图标（文件里没有，网上也没查到）：HAP 用默认图标`).

Ocarina of Time: the same five stages. Stage 1 has more steps, among them `打包 RomFS（约 12 分钟）`, the translator
`着色器翻译器（Ampere）` and nine shader steps translated on the PC (`着色器 1/9` to `着色器 9/9`; the labels of steps 1,
3, 5 and 6 give about 10, 10, 8 and 2 minutes), then libopus / libvpx and dav1d. It sends the most data of the three.

Pokémon Legends: Z-A: stage 1 has nine steps (from `解开 NUP（2.0.2 更新）` to `第三方静态库`), and the first build adds
`[6/6] 着色器种子`: SPIRV-Cross once, `在手机上解包着色器（约 1 分钟）` (the app opens on the phone by itself and closes
when it is done; the player leaves it alone), `拉回电脑、翻译并发送种子（约 1 小时）`, then `删除手机上的解包文件`.

The build succeeded when its last line reads `完成：<game> <version> 已装到手机并打开。` (exit code 0); with
`--no-install` it ends at `完成：<HAP>`. The line `没能标记为游戏（不影响游玩）` is harmless.

## 5. Check the result on the phone (the player)

The app is now open on the phone, with the game's icon, on its own start page. The player starts the game from there
(in Tears of the Kingdom with 开始游戏, Start game), loads a save or begins a new game and plays briefly: does the
picture look right, is there sound, do the controls respond? Z-A and Ocarina of Time are Being optimized (优化中):
playable, while the frame rate and a few pictures are still being tuned (Ocarina of Time: a bug turned up in testing). From the PC, the app is installed when
`& $hdc shell bm dump -n <bundle name>` prints its bundleName (the build printed the name after `包名：`).

Tears of the Kingdom on a screen wider than 16:9: the build lays the game's UI out for that screen, with the HUD at the
screen's edges (the 画面 (display) setting can put it back to centered 16:9). A fresh install normally gets that layout
in the same run. If the output says `手机上还没有 romfs-patches-<W>x<H> 文件夹`, the player sets 画面比例 (aspect
ratio) to 全面屏 (full screen) in the app's settings and taps 开始游戏 (Start game) once; then run the same build again
(everything else is reused). If it says `读不出手机屏幕的分辨率（hidumper）` or `还不知道手机屏幕（没有连接手机）`, pass
`--screen <width>x<height>`.

## 6. When a build stops

The cause is in the last `[RED]` line (exit code 1). A failed step also prints `<step> 失败（退出码 N），日志：<log file>`
followed by the log's last lines: open that log and read its end. Once the cause is gone, the unchanged command resumes
the build.

| What the output says | What it means and what to do |
| --- | --- |
| `这些文件夹里没有已适配、可一键构建的游戏` | `python nx2h.py scan "<folder>"` shows why: another game or version, or a file it cannot identify (step 2). |
| `找到不止一个可构建的游戏，用 --game <title id> 选一个` | Add `--game` with one of the title IDs the message lists. |
| `找不到<game>的后端 <folder>` | That game's backend is missing (step 1, "The backends"). |
| `缺本体（卡带 .xci，或 eShop 版 .nsp / .nsz）` | No Tears of the Kingdom base game was found; a card image (`.xci` / `.xcz`) needs `[0100F2C0115B6000]` in its name (step 2). |
| `缺 1.4.3 更新 .nsz（[0100F2C0115B6800][v720896]）` | The update has to be an `.nsz` with `[v720896]` in its file name (the version is read from the name; the title ID comes from the ticket inside, and `[0100F2C0115B6800]` in the name is only a fallback). No other version of the update works. |
| `缺宝可梦传说 Z-A 的 NUP（要含 2.0.2 更新 [v393216]）`, `缺塞尔达传说 时之笛的 NUP（要含 v131072 更新）` | The `.nup` that holds that update is needed; other versions are not supported. |
| `剩余 <N> GB，准备<game>约要 <N> GB：先清理空间，或把软件放到空间更大的盘` | Free space on the drive that holds the backends (step 1). |
| `需要 prod.keys：用 -k 指定它所在的文件夹` | Pass `-k "<folder with prod.keys>"`. |
| `没有找到连接的手机` | Step 3; or `--no-install` for the HAP alone (step 4 says what that means for the signing). |
| `取系统字体需要 prod.keys` | Pass `-k "<folder with prod.keys>"`. |
| `系统字体要从你自己的一张游戏卡带镜像（.xci）或你主机导出的固件里取`, `本体是 eShop 版（.nsp / .nsz）或 .xcz，不带系统字体` | Put one of the player's own game card images (`.xci`, not necessarily of this game) into the game folder, or the player's own unpacked firmware dump near `prod.keys` (step 2). |
| `这些卡带 / 固件里没取到系统字体（密钥太旧，或文件损坏）` | The keys predate that card or firmware, or the file is damaged: the player makes a new `prod.keys` dump on their own console after updating it, or makes a fresh dump of the card or firmware. |
| `还没有参考仓库 totk-android`, `参考仓库 totk-android 不在固定提交`, `检查参考仓库 totk-android 失败` | `python nx2h.py setup --fix`. Ask before anything discards local changes in that checkout. |
| `软件包自带的参考仓库 totk-android 缺失或不完整` | The full package's own copy is damaged or incomplete: the player unpacks their copy of the package again, completely, in the same place, or downloads it again from this repository's Releases page; never look for one anywhere else. |
| `这一步要用 Docker Desktop（在“环境”页安装并启动）` | Docker Desktop is not installed or not running: `setup --fix`. |
| `没有 WSL 2…`, or Docker Desktop shows `WSL not installed` | `setup --fix` installs WSL 2 (a UAC prompt), then the player restarts Windows. |
| `WSL 刚装好，要重启电脑后才能用`, `Docker Desktop 安装时打开了 Windows 的虚拟化组件…`, `Windows 在等重启` (`setup --fix` prints `等重启：`) | Not a failure: the player restarts Windows, then `setup --fix` again, which starts Docker Desktop. |
| `Windows 的“虚拟机平台”没打开`, `Windows 的“适用于 Linux 的 Windows 子系统”没打开` (Windows 10) | `setup --fix` (the WSL row turns it on), then the player restarts Windows. |
| `Windows 的虚拟机监控程序没在运行` | The player restarts Windows; if it persists, an Android emulator or a game turned it off and the player runs `bcdedit /set hypervisorlaunchtype auto` as administrator and restarts. Never run it for them. |
| `Windows 的 Server 服务（LanmanServer）被禁用了` | The player sets the Server service to Automatic in `services.msc` and starts it. Never change it for them. |
| `Docker Desktop 的服务协议还没接受` | The player presses Accept in Docker Desktop's window. |
| `电脑的 BIOS/UEFI 里没开虚拟化` | The player turns on Intel VT-x or AMD SVM in the firmware settings; never change it for them. |
| `Docker Desktop 5 分钟内没有就绪`, `Docker Desktop 打开后又退出了` (from `setup --fix`) | The player opens Docker Desktop and does what its window says. |
| `Docker 镜像 <image> 建了 <N> 次都没成功`, `Docker 镜像 <image> 从 <N> 个下载源都没拉下来`, or a step whose log ends in download errors | The network: run again once it works (finished steps are skipped). A proxy for Docker is the player's setting in Docker Desktop. |
| `<step> 失败（退出码 N），日志：<log file>` | Read the end of that log. With `hap 失败`, an SDK error means DevEco Studio's SDK is incomplete; a failed audit is never worked around: report it. |
| `运行结束但没有产出预期的文件` | Read that step's log and run once more; if it happens again, report it. |
| `里还有别的游戏文件，建不了硬链接` | The build needs that file in a folder of its own and could neither link nor copy it there: ask the player to put that file alone in a folder (or free space for the copy). |
| `不是 v131072 的游戏文件` | Ocarina of Time's modules do not match: another version or a damaged NUP. The message names the unpacked folder to delete; then build with the right NUP. |
| `要解开的 NUP 没找到` | The `.nup` is not in the folders given: pass the folder that holds it. |
| `的 Python，本机的 Python 都装不上` | Ocarina of Time: no Python on this PC could install the packages the message names. The player installs Python 3.12 (`winget install -e --id Python.Python.3.12 --source winget`); then run the build again. |
| `抽出着色器要用 zstd 程序` | `setup --fix` (winget `Meta.Zstandard`), then build again. |
| `缺 nxnative 仓库` | Ocarina of Time needs nxnative (step 1). |
| `编译 libopus 需要 PowerShell 7` | `setup --fix` (winget `Microsoft.PowerShell`). |
| `HarmonyOS SDK 不完整（缺 sysroot` | Open DevEco Studio once and let the SDK download finish. |
| `spirv-val 校验：`, `个翻译容器失败`, `抽出着色器时有` | Ocarina of Time's shader steps: read that step's log, then repeat the build (the modules set aside are translated anew). |
| `手机上的着色器解包 15 分钟没结束`, `手机上没有生成 bnsh.pack` | Z-A's shader unpack on the phone: keep the phone connected, leave the app alone and repeat the build. |
| `客体库源码指向别的位置` | Prepared data copied from another PC or folder without what is needed to remake it here: ask the player before deleting that game's prepared data in its backend, then build from the game files again. |
| `后台签名没成功：连不上华为的服务器` | The network or a proxy. |
| `后台签名没成功：这个华为账号还没有开发者团队` | The player registers as a developer at developer.huawei.com (real-name verification), then builds again. |
| `后台签名没成功：这个华为账号还没有登记任何手机` | Connect the phone (step 3) and build again without `--no-install`. |
| `账号登记的设备数量已到上限`, `调试 Profile 数量已到上限` | The player removes unused devices or profiles in AppGallery Connect. |
| `等了 10 分钟没有完成登录`, `登录被取消了`, `华为账号登录已失效` | Build again and let the player finish the login in the browser. |
| `没找到 Java（keytool）` | Install DevEco Studio (it brings its own Java), or `setup --fix` (Microsoft OpenJDK 21). |
| `Windows 数据保护（DPAPI）打不开签名数据` | The signing data belongs to another Windows user: build as that user. |
| `这台电脑还没有为 <bundle> 签过名` | Run the build again: it asks for the signing first. |
| `安装失败：<hdc's message>` | A signature or profile message: the phone is not in the profile (connect it and build again), or an app with the same bundle name but another signature is installed. Uninstalling that one deletes its saves: ask the player first. |
| `缺少要推送的文件`, `发送 <file> 失败` | Run again with the phone on the cable, and check that the phone has room (step 2). |
| `构建要用本机的 Python 3.9 或更新版本来运行后端脚本` | Packed app only: install Python (`setup --fix`, or `winget install -e --id Python.Python.3.12 --source winget`), then open the app again. |
| `自动安装没成功：退出码 0x8A15005E` | winget's Microsoft Store source refused a replaced HTTPS certificate (an accelerator, an antivirus's HTTPS scanning, a proxy on the player's PC). 0.1.3 and later install from the winget source only; with an older version have the player run `winget install -e --id <package> --source winget` (the package the row names) in a terminal. |
| `要写成 宽x高（像素），比如 2848x1320` | Give `--screen` as width x height in pixels (exit code 2). |

If step 6 has no answer, the player can open an issue with this project's build-problem form
(`.github/ISSUE_TEMPLATE/build-problem.yml`); help them through its fields in the form's order: game and version, the
file forms they gave the build, phone model and HarmonyOS version, Windows version and NX2Harmony version (from source:
the commit, `git rev-parse --short HEAD`; packed: the version they got), how they built it (on the command line, the
command too, with the user folder cut out of its paths), where it stopped and the last step line shown, then for the
log field every `[RED]` line and the last few dozen lines of the newest log (the failed step's log, when one is
named). Before anything is posted, put `***` in place of the Windows user name, IP addresses, the phone's serial
number, the Huawei account and team name (`setup` prints them), e-mail addresses and the PC part of the bundle name
(`nx2h.***.<slug>`). The HAP, game data (from `prepared\` or the phone), game files, firmware, keys, signing files,
`nx2h.local.json` and anything from `%USERPROFILE%\.nx2h` stay out of the report, zipped or not.

## 7. Later

- **After updating NX2Harmony** (from source: `git pull`, which brings the backends in `backends\` along, unless `nx2h.local.json` still names an older place under `"backends"`: remove that entry to use `backends\`) or a backend:
  run the same build command. Prepared data is reused; the native libraries, the HAP and the install are done again,
  and the push skips what the phone already has. Saves stay, because the app is replaced in place (`hdc install -r`),
  not uninstalled.
- **Another game:** steps 2, 4 and 5. The system fonts and the Huawei login carry over.
- **The window instead of the command line:** `NX2Harmony.cmd` (from source) or `NX2Harmony.exe` (packed) offers the
  same on three pages: Library (游戏库), Build (构建) and Environment (环境). `一键安装环境.cmd` runs `setup --fix` (after
  the Python check in step 1); `一键构建.cmd` runs `build` and asks for the folders.
- **Another PC** gets another bundle name (part of it comes from the PC), so a build there installs as a separate app
  with its own saves.
- **Where things are:** from source, `nx2h.local.json`, `work\` (system fonts, icons) and `logs\` in the repository
  folder, all ignored by git; packed, the same under `data\` beside `NX2Harmony.exe`. The prepared game data and the
  HAPs are in each backend folder (`backends\<folder>\`), under `prepared\` and `artifacts\release\<tag>\`; the signing
  is in `%USERPROFILE%\.nx2h`. The player's version of this list is in `docs/install.md` ("Where things are").
- **Removing a game:** `docs/install.md` ("Uninstall"). Uninstalling the app from the phone deletes its saves and game
  data, so ask first (Tears of the Kingdom's launcher has a 存档 (Saves) tab that exports the saves as a zip; NX2Harmony
  itself cannot back them up yet). Deleting a backend's prepared data frees tens of GB, but the next build then needs the
  game files and `prod.keys` again.
