# NativeGames-harmonyos

中文 · [English](#english)

在鸿蒙手机上原生运行《塞尔达传说 王国之泪》1.4.3。

游戏自己的 ARM64 代码在电脑上提前改写，在手机上直接执行；游戏的 NVN 图形调用换成 Vulkan，由手机的 Maleoon GPU 绘制。没有模拟器，也没有指令翻译。你用自己的游戏文件构建，仓库和任何下载里都没有游戏内容。

## 在 NX2Harmony 里安装

这里是 NX2Harmony 带的一份拷贝。在 NX2Harmony 里，安装走 NX2Harmony：步骤见 [NX2Harmony 的 README](../../README.md) 和它的[安装说明](../../docs/install.zh-CN.md)；交给 AI 助手时，它照 NX2Harmony 的[安装 skill](../../.claude/skills/install-nx2harmony/SKILL.md)（`/install-nx2harmony`）来做，王国之泪 1.4.3 也在其中。

- 独立的 NativeGames-harmonyos 工程自带的 AI 安装 skill（`.claude/skills/install-totk-harmonyos/`）和 `AGENTS.md` 没有带进 NX2Harmony，这份拷贝里没有它们。
- 签名和手机上的调试授权也按 NX2Harmony 的安装说明来做。

## 现状（构建 ng-fence4，2026-11-17；版本号仍为 0.5.0）

| | |
| --- | --- |
| 游戏版本 | 1.4.3（只支持这一版，输入按哈希校验） |
| 验证过的手机 | 一台：麒麟 9030 Pro、Maleoon 935、20 GB 内存、HarmonyOS 7（API 26） |
| 帧率 | 上限 30、默认两帧并行：开放世界大多 25–30 fps（ng-fence4 实玩约 8 分钟，24.5–30 fps）；手机发热、GPU 降频后会低一些。上限 60 约 39–40 fps |
| 能用的 | 读档 / 存档、全面屏和贴边的界面、游戏里的中文、触屏按键（游戏定制或标准手柄）与划屏视角、游戏菜单、读取进度、设置页和预设、过场动画与音频 |
| 0.5.0 新加、待真机验证 | 实体手柄、手机震动、陀螺仪体感瞄准、游戏内键盘、存档导出导入和自动备份（ng-fence4 修好了其中建文件夹出错的问题） |
| 还没在真机上从头走过 | 本仓库自己的打包、安装和推送（`build_hap.ps1` → `TotZ.NativeGames.harmonyos` → `push_v143.ps1`）：真机上跑的是同一份代码、由另一个打包工具装的包 |
| 还不完善的 | 两帧并行时空岛上一个位置出现过两次世界停住，加了检查后没再出现，但原因没确定，可以退回单帧；宽屏上过场视频横向拉伸约 21%；跟着物体走的界面标记还没挪到宽屏的位置；新区域要在电脑上补一次着色器；偶尔一帧近处阴影变暗；读档时有几秒卡顿 |

变化见 [CHANGELOG.md](CHANGELOG.md)。**从 0.5.0 升级**：包名从 `com.macnativegames.harmonyprobe` 改成了 `TotZ.NativeGames.harmonyos`，新包装上是另一个应用，游戏数据要重新送、存档要搬过去，步骤见 CHANGELOG。

## 怎么玩

**这里没有可下载的安装包。** 安装包里有改写后的游戏代码，只能用你自己的游戏文件、在你自己的电脑上生成，再用你自己的华为开发者账号签名装到你的手机上。完整步骤见 **[docs/INSTALL.md](docs/INSTALL.md)**，大致是：

1. 准备：你自己的基础游戏 1.0.0（XCI）、1.4.3 更新（NSZ）和 `prod.keys`；Windows 电脑装好 Docker、Python、Git、PowerShell 7、DevEco Studio。
2. `python native-harmonyos/prepare_v143.py --base … --update … --keys …`：一条命令把游戏数据、代码、着色器和原生库都准备好。手机屏幕比 16:9 宽时加上 `--aspect <宽>x<高>`，界面就能贴到屏幕两边。
3. 在 DevEco Studio 里设置签名，然后 `native-harmonyos\build_hap.ps1 -Tag mine` 打包，`hdc install` 装到手机（包名 `TotZ.NativeGames.harmonyos`）。
4. `native-harmonyos\v143\push_v143.ps1 -RomFS -GameData -Seed -Patches`：把游戏数据（连同中文要用的字体和按屏幕生成的界面布局）送到手机，打开应用开始玩。

## 仓库里有什么

| 目录 | 内容 |
| --- | --- |
| `entry/`、`AppScope/` | 鸿蒙应用（包名 `TotZ.NativeGames.harmonyos`）：启动页（设置和预设、存档、关于）、读取画面、游戏页和游戏菜单、触屏按键 |
| `native-harmonyos/ohos/` | 原生前端：窗口与 Vulkan 呈现、音频、触控和手柄、震动和体感、线程调度、BC→ETC2 纹理转码 |
| `native-harmonyos/overlay/` | 对 totk-android 运行时与 Vulkan 后端的鸿蒙改动（整文件替换） |
| `native-harmonyos/*.py, *.sh, *.ps1` | 准备、构建、安装的脚本（着色器种子由 `build_seed.py` 在几个容器里并行生成） |
| `native-harmonyos/v143/` | 1.4.3 专用的步骤：更新解包和合并、界面布局（含按屏幕生成的贴边布局）、把数据送到手机 |
| `native-harmonyos/third_party/etcpak/` | ETC2 编码器（BSD-3） |
| `.claude/skills/`、`AGENTS.md` | 独立工程里给 AI 助手的安装步骤；NX2Harmony 不带它们，这份拷贝里没有（用 NX2Harmony 的安装说明） |
| `tools/` | `docker/`：准备步骤用的容器；`ui_icons.py`：生成应用里的图标 |
| `docs/`、`tests/` | 安装说明；不需要游戏文件的检查 |

本项目建立在开源的 [totk-android](https://github.com/AurelioB/totk-android)（MIT）之上：构建时按 `native-harmonyos/UPSTREAM_PIN` 固定的提交取用它的运行时、NVN-Vulkan 后端、代码改写器和着色器翻译器，再叠加 `native-harmonyos/overlay/` 里的鸿蒙改动。

## 法律

- 只用你自己购买的游戏、在你自己的 Switch 上导出的文件。本项目不提供、不帮助获取任何游戏文件、密钥或固件。
- 仓库里没有游戏内容：没有游戏代码、资源、着色器、存档或密钥。`prepared/`、`artifacts/` 等存放你的游戏衍生数据的目录都被 git 忽略。
- 你构建出的安装包含有你的游戏代码，不要分享。

代码以 MIT 许可发布（[LICENSE](LICENSE)），第三方代码的许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

---

<a id="english"></a>

# English

[中文](#nativegames-harmonyos) · English

The Legend of Zelda Tears of the Kingdom 1.4.3, running natively on HarmonyOS phones.

The game's own ARM64 code is rewritten ahead of time on the PC and runs directly on the phone; its NVN graphics calls go
to Vulkan on the phone's Maleoon GPU. There is no emulator and no instruction translation. You build it from your own
game files; nothing of the game is in this repository or in any download.

## Installing inside NX2Harmony

This is the copy NX2Harmony carries. Inside NX2Harmony, installation goes through NX2Harmony: the steps are in
[NX2Harmony's README](../../README.md) and its [install guide](../../docs/install.md); an AI assistant follows
NX2Harmony's [install skill](../../.claude/skills/install-nx2harmony/SKILL.md) (`/install-nx2harmony`), which covers
Tears of the Kingdom 1.4.3.

- The standalone NativeGames-harmonyos project's own AI install skill (`.claude/skills/install-totk-harmonyos/`) and its
  `AGENTS.md` are not carried into NX2Harmony, so this copy has neither.
- Signing and the debugging prompt on the phone follow NX2Harmony's install guide as well.

## Status (build ng-fence4, 2026-11-17; the version number is still 0.5.0)

| | |
| --- | --- |
| Game version | 1.4.3 only; every input is checked by hash |
| Tested phone | One: Kirin 9030 Pro, Maleoon 935, 20 GB RAM, HarmonyOS 7 (API 26) |
| Frame rate | Capped at 30 with two frames in flight (the default): mostly 25–30 fps in the open world (ng-fence4 played for about 8 minutes at 24.5–30 fps); lower once the phone heats up and the GPU slows down. About 39–40 fps capped at 60 |
| Working | Loading and saving, full-screen aspect with the HUD at the screen edges, Chinese text in the game, touch buttons (the game's own layout or a standard pad) and swipe camera, the game menu, loading progress, settings with presets, cutscenes and audio |
| New in 0.5.0, not yet tested on a phone | Physical controllers, phone rumble, gyro aiming, the in-game keyboard, save export and import with automatic backups (ng-fence4 fixed their folder error) |
| Not yet run end to end on a phone | This repository's own build, install and push (`build_hap.ps1` → `TotZ.NativeGames.harmonyos` → `push_v143.ps1`): the phone ran a package of the same code installed by another packaging tool |
| Not yet right | With two frames in flight the world stopped twice at one spot on a sky island; it has not come back since a check was added, but the cause is not known, and one frame is the fallback. Cutscene videos stretched sideways by about 21% on a wide screen; HUD markers that follow objects in the world not yet moved for the wide view; new areas need one shader fill on the PC; now and then one frame with darker shadows nearby; a few seconds of stutter when loading |

Changes are in [CHANGELOG.md](CHANGELOG.md). **Upgrading from 0.5.0**: the bundle name changed from
`com.macnativegames.harmonyprobe` to `TotZ.NativeGames.harmonyos`, so the new package installs as a separate app; the
game data has to be sent again and the saves moved over, as the changelog describes.

## How to play

**There is no package to download.** The package holds the game's rewritten code, so it can only be made from your own
game files on your own PC and signed with your own Huawei developer account for your own phone. The full steps are in
**[docs/INSTALL.en.md](docs/INSTALL.en.md)**; in short:

1. Have your own base game 1.0.0 (XCI), the 1.4.3 update (NSZ) and `prod.keys`; a Windows PC with Docker, Python, Git,
   PowerShell 7 and DevEco Studio.
2. `python native-harmonyos/prepare_v143.py --base … --update … --keys …` prepares the game data, code, shaders and
   native libraries in one command. For a phone screen wider than 16:9, add `--aspect <W>x<H>` so the HUD can sit at
   the screen's edges.
3. Set up signing in DevEco Studio, then `native-harmonyos\build_hap.ps1 -Tag mine` builds the package and `hdc install`
   puts it on the phone (bundle name `TotZ.NativeGames.harmonyos`).
4. `native-harmonyos\v143\push_v143.ps1 -RomFS -GameData -Seed -Patches` sends the game data (with the fonts Chinese
   text needs and the UI laid out for your screen); open the app and play.

## What is in the repository

| Folder | Contents |
| --- | --- |
| `entry/`, `AppScope/` | The HarmonyOS app (bundle name `TotZ.NativeGames.harmonyos`): launcher (settings with presets, saves, about), loading screen, game page and game menu, touch controls |
| `native-harmonyos/ohos/` | The native front end: window and Vulkan presentation, audio, touch and controllers, rumble and motion, thread scheduling, BC→ETC2 texture transcoding |
| `native-harmonyos/overlay/` | HarmonyOS changes to the totk-android runtime and Vulkan backend (whole-file replacements) |
| `native-harmonyos/*.py, *.sh, *.ps1` | Preparation, build and install scripts (`build_seed.py` makes the shader seed in several containers at once) |
| `native-harmonyos/v143/` | The steps for 1.4.3: unpacking and merging the update, the UI layouts (including the per-screen layouts with the HUD at the edges), sending the data to the phone |
| `native-harmonyos/third_party/etcpak/` | ETC2 encoder (BSD-3) |
| `.claude/skills/`, `AGENTS.md` | The standalone project's install steps for AI assistants; NX2Harmony leaves them out, so this copy has neither (use NX2Harmony's install guide) |
| `tools/` | `docker/`: the container used by the preparation steps; `ui_icons.py`: generates the app's icons |
| `docs/`, `tests/` | Install guide; checks that need no game files |

The project builds on the open-source [totk-android](https://github.com/AurelioB/totk-android) (MIT): the build takes its
runtime, NVN-on-Vulkan backend, code rewriter and shader translator at the commit pinned in
`native-harmonyos/UPSTREAM_PIN`, and applies the HarmonyOS changes in `native-harmonyos/overlay/` on top.

## Legal

- Use only a game you bought, dumped from your own Switch. This project does not provide game files, keys or firmware and
  does not help obtain them.
- The repository holds no game content: no game code, assets, shaders, saves or keys. `prepared/`, `artifacts/` and the
  other folders that hold data derived from your game are ignored by git.
- The package you build contains your game's code. Do not share it.

The code is released under the MIT license ([LICENSE](LICENSE)); third-party licenses are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
