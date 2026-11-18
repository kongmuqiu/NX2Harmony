# 更新记录 · Changelog

中文 · [English](#english)

版本号格式：主版本.次版本.修订。每个版本注明它运行的游戏版本。

## ng-fence4（2026-11-17，versionName 仍为 0.5.0 / versionCode 100004）· 游戏 1.4.3 · 两帧并行、界面贴边、新包名、新界面

0.5.0 之后的全部改动，按构建名记录；版本号在下一次发布时再提高。

真机验证：ng-fence4 在一台手机上玩了约 8 分钟，24.5–30 fps，没有问题；一次自动的两帧读档测试平均 29.7 fps。手机上装的是用同一份代码、由另一个打包工具以别的包名构建的包，本仓库自己的 `build_hap.ps1` → `TotZ.NativeGames.harmonyos` → `push_v143.ps1` 这一整条流程还没在真机上从头走过。下面标“（真机）”的在手机上跑过或测过，标“（电脑）”的在电脑上跑过，标“（只编译）”的只编译或写好、还没实际用过。

**从 0.5.0 升级**

包名从 `com.macnativegames.harmonyprobe` 改成了 `TotZ.NativeGames.harmonyos`，新包装上是另一个应用：旧应用里约 20 GB 的游戏数据和存档不会带过来，`hdc install -r` 也不会替换旧应用。

1. 先在电脑上备份旧应用的存档（0.5.0 的“导出”建文件夹时会出错，可能导不出来）：
   ```
   hdc file recv -b com.macnativegames.harmonyprobe /data/storage/el2/base/haps/entry/files/android/user-data <电脑上的文件夹>
   ```
2. 更新仓库后运行 `python native-harmonyos/prepare_v143.py --only libs` 重编原生库，再用 `build_hap.ps1` 换个标签打包、装到手机。其他准备步骤不用重做：着色器种子和以前逐字节相同，代码改写和导出没有变。要界面贴边，再运行一次 `--only patches --aspect <宽>x<高>`（见“画面”）。
3. 按[安装说明](docs/INSTALL.md)第 5 节让新应用启动一次，把数据全部重新送一遍：`push_v143.ps1 -RomFS -GameData -Seed -Patches`。
4. 把备份的存档文件夹在电脑上打成 zip、传到手机，在新应用的“存档”页导入（只编译）。
5. 确认新应用能玩之后，再 `hdc uninstall com.macnativegames.harmonyprobe`（旧应用的数据和存档会一起删掉）。两个应用都在时，手机要放得下两份约 20 GB。

**运行和性能**
- 默认两帧并行（`TOTK_OHOS_SYNC_LAG=1`，真机）：游戏帧结尾的等待改为等前一帧，`TOTK_OHOS_INTERVAL_MAX=2` 让游戏的帧率控制不再落到 20 fps 档，呈现时钟在上限 30 时为 60 Hz、上限 60 时为 90 Hz。不锁频、玩家自己的设置：上限 30 走动 29.6–29.9 fps（连续 10 分钟 29.8），单帧时 25.4–27.8；海拉鲁平原站立 30 fps，单帧时 19–20；上限 60 约 39–40 fps，GPU 90–92% 忙。
- 两帧时的世界停住（真机；已缓解，没有确认修好）：两帧时，空岛上一个位置出现过两次世界停住（画面还在动，游戏的读取圈一直转，时间不走），单帧时那里从没停过。现在加了一致性检查（`TOTK_OHOS_SYNC_CONSIST`）：游戏已经被告知完成的帧，之后某次检查如果还看到没完成，先等它真正完成，最多 2 秒。默认只查同步轮询 / 等待、事件读取和缓冲区失效，正常游玩里这几类从不用等（29.9 fps，0 次等待）；查全部种类（`all`）时帧率从 30 掉到 19。停住之后，当晚 7 次两帧读档（包括那个位置）和 ng-fence4 的 8 分钟游玩都没再停住；但没开这个检查的一轮也没停住，所以卡住世界的是哪一种检查、这个检查是不是起了作用，都还不知道。
- 退回单帧（`TOTK_OHOS_SYNC_LAG=0`）：世界再停住时用，上限 30 时呈现时钟为 90 Hz，帧率回到上面单帧的数。设置页没有这个开关，也不要写进 `env.txt`（设置页每次保存都会整份重写它）：在电脑上建一个只有这一行的 `env-dev.txt` 送到手机，下次开始游戏生效：
  ```
  hdc file send -b TotZ.NativeGames.harmonyos env-dev.txt /data/storage/el2/base/haps/entry/files/android/env-dev.txt
  ```
  以后要再改，用新文件名送（`env-dev-<任意>.txt`，按名字顺序读，后读的为准；应用看不到它读过的文件被改写）；`TOTK_OHOS_SYNC_LAG=`（等号后为空）回到默认的两帧。
- 帧率上限只剩 30 / 60，以前存的 90 / 120 按 60 读（真机）。
- 不锁频、在手机上交替对比后改的默认值（真机，2026-11-15，单帧）：
  - 描述符池不因压力重建（`TOTK_OHOS_STOCK_PRESSURE_GENS=0`）：站立 +0.7、走动 +1.1 / +2.5 fps，多用约 100 MiB 内存；
  - 只在读档时变过的纹理，150 帧不变后回到抽查（`TOTK_OHOS_TEX_VOL_DECAY=150`）：录制线程每帧 4.3 → 1.6 ms；
  - 上限 30 时场景通道每 1024 个绘制提交一次（`TOTK_VK_SPLIT_PASS_DRAWS=1024`），GPU 不再空等录制：21.9 / 23.4 → 26.3 / 25.4 fps；
  - 分段时不再做只为高通 LRZ 准备的深度读写（`TOTK_VK_SPLIT_LRZ_SAFE=0`，Maleoon 没有 LRZ）；
  - 每次提交先送 32 个绘制，大场景通道之后每块 150 个（`TOTK_OHOS_CHUNK_FIRST`、`TOTK_OHOS_CHUNK_TAIL`）；
  - 向系统游戏性能服务报“游戏中”场景（7，以前是 1）。

**画面**
- 界面贴边（真机上看过，当时布局是另一个工具送的；用 `push_v143.ps1 -Patches` 送布局还没在真机上走过）：`prepare_v143.py --aspect <宽>x<高>`（横屏像素，大的在前，例如 `2848x1320`）给这块屏幕生成一份界面布局 `romfs-patches-<宽>x<高>`，`push_v143.ps1 -Patches` 把它送到手机。血条贴屏幕左边、小地图贴右边，读取画面、过场的淡入淡出和菜单背景铺满全屏。设置“画面 › 界面位置”：贴边（默认）/ 居中 16:9，只在“画面比例”为全面屏时显示；手机上没有这块屏幕的布局时，界面仍在中间 16:9。手机上放布局的文件夹由应用在全面屏下开始游戏时建好，所以要先开始一次游戏，`-Patches` 才送得进去。
- 中文、韩文要用的字体（真机）：选中文时在标题画面退出的问题修好了。主机字体现在放在 `game-v143/fonts`，开始游戏（运行时启动）时建好这个文件夹、在日志里记下找到几个字体文件，`push_v143.ps1 -GameData` 把它们一起送过去；以前的位置既没人建也没人送。
- 所有页面横屏、沉浸显示（真机）。

**应用**
- 新启动页（真机）：图标和游戏名，游戏数据 / 着色器 / 存档三个状态，开始游戏，以及“设置 / 存档 / 关于”三页。设置改了就保存，下次开始游戏生效。
- 读取画面（真机）：开始游戏到出现画面之间，全屏显示分六步的进度（准备运行环境、校验游戏数据、挂载游戏数据、初始化游戏模块、游戏启动中、进入画面）和提示。
- 游戏菜单（真机）：点屏幕顶部中间的小标签或用返回手势打开，有继续游戏、游戏设置（应用后重启）、触屏按键（自动 / 一直显示 / 隐藏）、按键布局、视角操作（划屏 / 双摇杆）、回到启动页，取代了游戏里左上角的齿轮。
- 触屏按键（真机）：换成图标和彩色圈，游戏定制布局里的按键挪开了贴边的界面；新增“标准手柄”布局（Switch 原样的 A B X Y、十字键、肩键和 + −），在“操作 › 触屏按键布局”或游戏菜单里切换。
- 关于页（只编译）：应用版本、包名、游戏版本、游戏数据和运行日志的位置、开源许可。
- 只有手柄会让屏幕按键隐藏：以前连着蓝牙手表这类设备时，屏幕按键也会消失（问题在真机上见过，修复只编译）。
- 存档（只编译）：修好 0.5.0 建文件夹时报 “File exists”，让备份、导入、恢复、导出在同一文件夹的第二个文件就失败的问题；存档页改版。

**电脑端工具**
- 着色器种子改由 `build_seed.py` 生成（电脑）：先看这台电脑的 Docker 有多少 CPU 和内存，再决定开几个容器，任务切成块、做完一块再领下一块。在测试的电脑上从零生成 131 秒（原来约 55 分钟），`seed.vkpack` 与 `build_seed.sh`（留作参照）逐字节相同。可用 `TOTK_SEED_CONTAINERS`、`TOTK_SEED_SERVICES` 指定。
- `fill_shaders_ohos.py` 从种子包里读变体列表（新种子没有散文件）（只编译）。
- `prepare_v143.py --aspect <宽>x<高>`（可写多个）；`push_v143.ps1 -GameData` 也送字体，`-Patches` 也送每一份按屏幕的布局（先送清单）（只编译）。
- 脚本（`ohos_env.ps1`、`ohos_env.py`、`push_v143.ps1`、`fill_shaders_ohos.py`）读环境变量 `TOTK_OHOS_BUNDLE`，没设时用 `TotZ.NativeGames.harmonyos`；应用运行时读自己安装的包名（游戏性能服务、关于页、崩溃诊断），用别的包名构建的也能用（应用读包名：真机；脚本：只编译）。
- `tools/ui_icons.py` 生成应用里的图标（电脑）。

**诊断（默认关闭）**
- `files/android/env-dev.txt` 和 `env-dev-*.txt`：在 `env.txt` 之后按名字顺序读，设置页从不改写它们；`KEY=`（等号后为空）清掉这个变量。诊断开关放在这里（真机）。
- `TOTK_OHOS_TOLD_LOG=1`：每 300 帧记录一致性检查的统计；`TOTK_OHOS_FRAME_REC`：在应用里录帧（都在真机上用过）；另有几个实验开关。

**已知问题**
- 两帧并行时世界停住的原因还没确定（见上，可以退回单帧）。
- 宽屏上过场视频横向拉伸约 21%。
- 跟着物体走的界面标记还没挪到宽屏的位置。
- 启动页在没有游戏数据时的提示说的是另一个电脑工具；用本仓库时，数据用 `push_v143.ps1` 送。“着色器”状态在手机上没有种子时显示“首次生成”，其实是还没送：种子在电脑上生成，用 `push_v143.ps1 -Seed` 送。
- 游戏里打开设置、改了又取消，之后再点游戏菜单的“按键布局”，取消的改动会被一起保存，下次开始游戏生效。
- 新区域要在电脑上补一次着色器。

## 0.5.0（versionCode 100004）· 游戏 1.4.3 · 手柄、震动、体感、键盘、存档管理

这一版的新功能已经编译打包，还没在真机上验证。

- **实体手柄**：蓝牙或有线手柄通过系统的游戏控制器接口（GameControllerKit，API 21）读取，和屏幕按键同时可用；连上后屏幕按键自动隐藏（设置“触屏按键”可改为一直显示）。按键布局可选按标签（任天堂）或按位置（Xbox）。系统没有 Select 键的监听，这个键由游戏页的按键事件转成 −。旧系统没有这个接口时只用屏幕按键。
- **震动**：游戏的震动用手机马达表现，按强度换成每 50 ms 里的振动时长，强度可选弱 / 中 / 强 / 关。系统接口不支持手柄马达。
- **体感瞄准**：手机陀螺仪和加速度计按 200 Hz 读取，换算到游戏的坐标，只在游戏画面在前台时打开；方向反了可以选“开（方向反了选这个）”。
- **游戏内键盘**：游戏要输入文字时弹出对话框和系统键盘，以前是直接取消。
- **存档管理**：启动页新增“存档”页，可以导出 zip、从 zip 导入、查看和恢复备份。每次开始游戏前（存档有变化时）、导入和恢复前自动备份，保留 10 份。
- 返回手势和映射成返回键的手柄键不会再退出游戏画面。
- 新增权限：振动、加速度计、陀螺仪（都是系统直接授予的普通权限）。

## 0.4.0（versionCode 100003）· 游戏 1.4.3 · 第一个能玩的版本

**运行**
- 游戏 1.4.3 在鸿蒙手机上原生运行：游戏代码提前改写后直接执行，NVN 图形走 Vulkan（Maleoon GPU）。运行时、Vulkan 后端、代码改写器和着色器翻译器取自 totk-android 的固定提交，鸿蒙改动在 `native-harmonyos/overlay/`，前端在 `native-harmonyos/ohos/`。
- 1.4.3 的代码补丁在电脑上导出代码时打好；阴影尺寸、级联数、点光阴影、远景距离、FXAA 这些画面选项在启动时从数据页读取，可以在设置里切换。
- 全面屏比例、输出宽度、SGSR 放大，以及系统的 XEngine 空域超分。
- Maleoon 不支持 BC 纹理：上传时在辅助线程上实时转成 ETC2，GPU 内存从 2.5 GB 降到 1.85 GB，离系统约 3.1 GB 的上限有足够余量。
- 默认使用窄通道屏障和 uniform 重命名：每帧通道结束次数 266 → 152，GPU 时间 28.8 → 26.7 ms，超过 50 ms 的帧减半（初始空岛起点实测）。
- 游戏帧控制器使用硬件 GPU 时间；描述符复用；三个上传辅助线程；关键线程放在大核 / 超大核并设置 QoS；向系统游戏性能服务声明场景和关键线程。
- 呈现线程有了自己的名字（`totk-present`），调度器会把它放在大核上。
- 种子里缺少的着色器变体会被记下，`fill_shaders_ohos.py` 在电脑上补齐后送回手机。

**应用**
- 启动页即设置页，分页显示，带四个预设（流畅 / 均衡 / 画质 / 高帧率）和每项的说明；预设都保留游戏自带 FXAA 和中等阴影，不再缩小贴图。
- 触屏按键；右侧滑动转视角，跟手。

**电脑端工具**
- `prepare_v143.py`：一条命令从你的游戏文件准备好一切，每步都在容器里做、按哈希核对、可以中断后继续。
- `build_hap.ps1`：用你自己的签名打包，并检查包里的每个文件。
- `build_opus_ohos.ps1`、`build_vpx_ohos.sh`：音频和视频解码库。
- 本机设置（DevEco Studio 位置、设备号）放在不进 git 的 `native-harmonyos/local.ohos.ps1`，PowerShell 脚本和 Python 工具都读它。
- 给 AI 助手的安装 skill（`.claude/skills/install-totk-harmonyos/`）和 `AGENTS.md`。

**诊断（默认关闭）**
- `TOTK_OHOS_FLICKER=<阈值>`：画面闪烁检测，记录并保存前后帧。
- 日志里每 300 帧的帧率、通道结束原因、屏障、纹理对象统计。

**已知问题**
- 开放世界 25–30 fps，手机发热后更低。
- 偶尔一帧近处地面的阴影变暗。
- 新区域要在电脑上补一次着色器。
- `TOTK_OHOS_BC_SHARE=1`（复用重复创建的纹理）第一次测试时读档卡住，默认关闭，已按推测的原因修改，待重测。

---

<a id="english"></a>

[中文](#更新记录--changelog) · English

Versions are major.minor.patch. Each entry names the game version it runs.

## ng-fence4 (2026-11-17, versionName still 0.5.0 / versionCode 100004) · game 1.4.3 · two frames in flight, HUD at the screen edges, a new bundle name, a new app shell

Everything since 0.5.0, recorded under the build's name; the version number is raised at the next release.

Tested on the phone: ng-fence4 was played on one phone for about 8 minutes at 24.5–30 fps without problems, and an
automated two-frame load test averaged 29.7 fps. The phone ran a package built from this same code by another packaging
tool under another bundle name; this repository's own route, `build_hap.ps1` → `TotZ.NativeGames.harmonyos` →
`push_v143.ps1`, has not yet been run end to end on a phone. Items marked (phone) were run or measured on the phone,
(PC) were run on the PC, and (built only) are built or written but not yet used.

**Upgrading from 0.5.0**

The bundle name changed from `com.macnativegames.harmonyprobe` to `TotZ.NativeGames.harmonyos`, so the new package
installs as a separate app: the old app's game data (about 20 GB) and saves do not come along, and `hdc install -r` does
not replace the old app.

1. First back up the old app's saves on the PC (0.5.0's export goes wrong when it makes folders and may not finish):
   ```
   hdc file recv -b com.macnativegames.harmonyprobe /data/storage/el2/base/haps/entry/files/android/user-data <folder on the PC>
   ```
2. After updating the repository, rebuild the native libraries with
   `python native-harmonyos/prepare_v143.py --only libs`, then build the package with `build_hap.ps1` under a new tag
   and install it. No other preparation step needs redoing: the shader seed is byte for byte the same as before, and the
   code rewrite and export have not changed. For the HUD at the screen edges, also run `--only patches --aspect <W>x<H>`
   (see Picture).
3. Start the new app once as in section 5 of the [install guide](docs/INSTALL.en.md) and send all the data again:
   `push_v143.ps1 -RomFS -GameData -Seed -Patches`.
4. Zip the backed-up save folder on the PC, copy the zip to the phone and import it on the new app's 存档 (Saves) page
   (built only).
5. Once the new app plays, run `hdc uninstall com.macnativegames.harmonyprobe` (the old app's data and saves go with
   it). While both apps are installed the phone needs room for two copies of about 20 GB.

**Running and performance**
- Two frames in flight by default (`TOTK_OHOS_SYNC_LAG=1`, phone): the game's frame-end wait now waits for the frame
  before, `TOTK_OHOS_INTERVAL_MAX=2` keeps the game's frame controller off its 20 fps tier, and the present clock is
  60 Hz at cap 30 and 90 Hz at cap 60. Unlocked clocks, a player's own settings: cap 30 walking 29.6–29.9 fps (29.8 over
  10 minutes) against 25.4–27.8 with one frame; standing in Hyrule Field 30 fps against 19–20; cap 60 about 39–40 fps
  with the GPU 90–92% busy.
- The world stopping with two frames (phone; mitigated, not confirmed fixed): with two frames the world stopped twice at
  one spot on a sky island (the picture still moved, the game's loading ring kept turning, the clock stood still); with
  one frame it never stopped there. There is now a consistency check (`TOTK_OHOS_SYNC_CONSIST`): when the game, already
  told a frame is complete, later checks on it and still finds it unfinished, the check first waits for that frame to
  finish, at most 2 s. By default it covers sync polls and waits, event reads and buffer invalidates, which never have to wait in
  normal play (29.9 fps, 0 waits); covering every kind (`all`) dropped 30 fps to 19. After the freeze, 7 two-frame
  loads that night (that spot included) and ng-fence4's 8 minutes of play did not stop; but a run without the check did
  not stop either, so which check held the world, and whether this check is what helps, are not known yet.
- Back to one frame (`TOTK_OHOS_SYNC_LAG=0`): for when the world stops again. The present clock is then 90 Hz at cap
  30, and the frame rate goes back to the one-frame figures above. The settings page has no switch for it, and it does
  not go in `env.txt` (the settings page rewrites that whole file on every save): make an `env-dev.txt` on the PC holding
  just that line and send it; it applies at the next start:
  ```
  hdc file send -b TotZ.NativeGames.harmonyos env-dev.txt /data/storage/el2/base/haps/entry/files/android/env-dev.txt
  ```
  To change it later, send a file with a new name (`env-dev-<anything>.txt`, read in name order, the later one wins;
  the app does not see a file it already read being rewritten); `TOTK_OHOS_SYNC_LAG=` with nothing after the `=` goes
  back to the default two frames.
- Frame-rate caps are 30 and 60 only; a stored 90 or 120 reads as 60 (phone).
- Defaults changed after alternating comparisons on the phone with unlocked clocks (phone, 2026-11-15, one frame):
  - no descriptor-pool rebuild on pressure (`TOTK_OHOS_STOCK_PRESSURE_GENS=0`): +0.7 fps standing, +1.1 / +2.5 moving,
    about 100 MiB more memory;
  - textures that changed only while a save loaded go back to sampled checks after 150 unchanged frames
    (`TOTK_OHOS_TEX_VOL_DECAY=150`): the recording thread 4.3 → 1.6 ms a frame;
  - at cap 30 the scene pass is submitted every 1024 draws (`TOTK_VK_SPLIT_PASS_DRAWS=1024`), so the GPU no longer
    waits for recording: 21.9 / 23.4 → 26.3 / 25.4 fps;
  - splits skip the depth read and write that exist only for Qualcomm's LRZ (`TOTK_VK_SPLIT_LRZ_SAFE=0`; Maleoon has
    no LRZ);
  - each submission sends its first 32 draws first, and chunks of 150 after the big scene pass (`TOTK_OHOS_CHUNK_FIRST`,
    `TOTK_OHOS_CHUNK_TAIL`);
  - the system's game performance service is told the "gaming" scene (7, it was 1).

**Picture**
- HUD at the screen edges (seen on the phone, with the layouts put there by another tool; sending them with
  `push_v143.ps1 -Patches` has not yet been run on a phone): `prepare_v143.py --aspect <W>x<H>` (landscape pixels, the
  larger number first, e.g. `2848x1320`) makes the game's UI laid out for that screen, `romfs-patches-<W>x<H>`, and
  `push_v143.ps1 -Patches` sends it to the phone. The hearts sit at the left edge and the minimap at the right; loading
  screens, cutscene fades and menu backgrounds cover the whole screen. The setting 画面 › 界面位置 (Display › HUD
  position): 贴边 (edges, the default) or 居中 16:9 (centred), shown only while 画面比例 (aspect) is 全面屏 (full
  screen); without layouts for this screen on the phone the HUD stays in the middle 16:9. The app makes the folder for
  the layouts when the game starts with 全面屏, so start the game once before `-Patches` can send into it.
- Fonts for Chinese and Korean (phone): choosing Chinese no longer closes the game at the title. The console's shared
  fonts now live in `game-v143/fonts`, which the runtime creates when the game starts (the log says how many font files it found), and
  `push_v143.ps1 -GameData` sends them; the earlier place was neither created nor filled by anything.
- Every page is landscape and full screen (phone).

**App**
- New launcher (phone): an icon and the game's title, the state of game data, shaders and saves, the start button, and the
  设置 / 存档 / 关于 (Settings / Saves / About) pages. Settings are saved as they change and apply at the next start.
- Loading screen (phone): from tapping start until the game shows its picture, a full-screen progress bar in six steps
  (preparing the runtime, verifying the game data, mounting it, initialising the game modules, the game starting,
  first frames) with tips.
- Game menu (phone): the small tab at the top centre or the back gesture opens it: continue, game settings (apply and
  restart), on-screen keys (auto / always shown / hidden), key layout, camera control (swipe / two sticks) and back to the
  launcher. It replaces the gear at the top left in game.
- On-screen keys (phone): icons and coloured rings; the keys of the game's own layout moved off the HUD at the edges.
  A new 标准手柄 (standard pad) layout has the Switch's own A B X Y, D-pad, shoulder buttons and + −; switch it under
  操作 › 触屏按键布局 (Controls › on-screen layout) or in the game menu.
- About page (built only): app version, bundle name, game version, where the game data and the runtime log are, and the
  open source licences.
- Only gamepads hide the on-screen keys: a connected device such as a Bluetooth watch used to hide them too (the problem
  was seen on the phone; the fix is built only).
- Saves (built only): fixed 0.5.0's "File exists" when making folders, which stopped backup, import, restore and export
  at the second file in a folder; the Saves page is redesigned.

**PC tools**
- The shader seed is now made by `build_seed.py` (PC): it asks this PC's Docker how many CPUs and how much memory it has,
  picks how many containers to run, cuts the work into chunks and has each container take the next chunk when it
  finishes one. From scratch on the test PC: 131 seconds (about 55 minutes before), and `seed.vkpack` byte for byte the
  same as `build_seed.sh`'s (kept as the reference). `TOTK_SEED_CONTAINERS` and `TOTK_SEED_SERVICES` override the choice.
- `fill_shaders_ohos.py` reads the variant lists from the seed pack (the new seed has no loose files) (built only).
- `prepare_v143.py --aspect <W>x<H>` (can be given more than once); `push_v143.ps1 -GameData` also sends the fonts and
  `-Patches` also sends each per-screen layout folder (its manifest first) (built only).
- The scripts (`ohos_env.ps1`, `ohos_env.py`, `push_v143.ps1`, `fill_shaders_ohos.py`) read the environment variable
  `TOTK_OHOS_BUNDLE`, `TotZ.NativeGames.harmonyos` when it is not set; the app reads its own installed bundle name at run
  time (game performance service, About page, crash diagnostics), so a copy built under another name works too (the
  app reading its name: phone; the scripts: built only).
- `tools/ui_icons.py` generates the app's icons (PC).

**Diagnostics (off by default)**
- `files/android/env-dev.txt` and `env-dev-*.txt`: read after `env.txt` in name order and never rewritten by the
  settings page; `KEY=` with nothing after the `=` clears that variable. Diagnostic switches go here (phone).
- `TOTK_OHOS_TOLD_LOG=1`: consistency-check counts every 300 frames; `TOTK_OHOS_FRAME_REC`: in-app frame recording (both
  used on the phone); and a few experimental switches.

**Known issues**
- Why the world stops with two frames in flight is not known yet (see above; one frame is the fallback).
- Cutscene videos are stretched sideways by about 21% on a wide screen.
- HUD markers that follow objects in the world are not yet moved for the wide view.
- The launcher's message when there is no game data names a different PC tool; with this repository the data goes over
  with `push_v143.ps1`. The 着色器 (shaders) state shows 首次生成 ("first build") when the phone has no seed, which
  means it has not been sent: the seed is made on the PC and sent with `push_v143.ps1 -Seed`.
- If the settings are opened in game, changed and then cancelled, tapping 按键布局 (key layout) in the game menu
  afterwards saves the cancelled changes too, and they apply at the next start.
- New areas need one shader fill on the PC.

## 0.5.0 (versionCode 100004) · game 1.4.3 · controllers, rumble, motion, keyboard, save management

The new features in this version are built and packaged but not yet tested on a phone.

- **Physical controllers**: Bluetooth and wired controllers are read through the system's game controller interface
  (GameControllerKit, API 21), together with the on-screen buttons, which hide while a controller is connected (the
  触屏按键 setting keeps them). Face buttons by label (Nintendo) or by position (Xbox). The system has no monitor for
  Select, so the game page turns that key into Minus. Without the interface (older systems) only the on-screen buttons work.
- **Rumble** through the phone's motor: the strength becomes the share of every 50 ms it vibrates; weak, medium, strong
  or off. The system interface cannot drive a controller's motors.
- **Gyro aiming** with the phone's gyroscope and accelerometer at 200 Hz, turned into the game's frame and only on while
  the game is in the foreground; a "flipped" choice covers a landscape the other way round.
- **In-game keyboard**: when the game asks for text, a dialog with the system keyboard opens (it used to be cancelled).
- **Save management**: a new 存档 (Saves) tab on the launcher exports a zip, imports one, and lists and restores
  backups. Saves are backed up before every start (when they changed), every import and every restore; 10 are kept.
- The back gesture, and a controller key mapped to Back, no longer leave the game page.
- New permissions: vibration, accelerometer and gyroscope (normal permissions the system grants on install).

## 0.4.0 (versionCode 100003) · game 1.4.3 · the first playable version

**Runtime**
- Game 1.4.3 runs natively on HarmonyOS phones: the game's code is rewritten ahead of time and runs directly, and its NVN
  graphics go to Vulkan (Maleoon GPU). The runtime, Vulkan backend, code rewriter and shader translator come from a pinned
  totk-android commit; the HarmonyOS changes are in `native-harmonyos/overlay/`, the front end in `native-harmonyos/ohos/`.
- The 1.4.3 code patches are applied when the code is exported on the PC. Shadow size, cascade count, point-light
  shadows, draw distance and FXAA are read from the data page at start and can be switched in the settings.
- Full-screen aspect, output width, SGSR upscaling, and the system's XEngine spatial upscaling.
- The Maleoon GPU has no BC textures: they are transcoded to ETC2 on helper threads as they upload, which lowers GPU
  memory from 2.5 GB to 1.85 GB, well under the system's limit of about 3.1 GB.
- Narrow pass barriers and uniform renaming are on by default: pass ends per frame 266 → 152, GPU time 28.8 → 26.7 ms,
  half as many frames over 50 ms (measured at the first Sky Island start).
- The game's frame controller uses hardware GPU time; descriptors are reused; three upload helper threads; the key
  threads run on the big and prime cores with QoS set; the scene and key threads are declared to the system's game
  performance service.
- The presentation thread has its own name (`totk-present`) so the scheduler keeps it on a big core.
- Shader variants missing from the seed are recorded; `fill_shaders_ohos.py` fills them on the PC and sends them back.

**App**
- The launcher is the settings page, in tabs, with four presets (Smooth / Balanced / Quality / High frame rate) and a
  description of every option. All presets keep the game's own FXAA and medium shadows and no longer shrink textures.
- Touch buttons; swipe on the right side to turn the camera, following the finger.

**PC tools**
- `prepare_v143.py`: one command prepares everything from your game files; every step runs in a container, checks hashes
  and resumes after an interruption.
- `build_hap.ps1`: builds the package with your own signing and checks every file in it.
- `build_opus_ohos.ps1`, `build_vpx_ohos.sh`: the audio and video decoders.
- Machine settings (DevEco Studio location, device key) live in `native-harmonyos/local.ohos.ps1`, ignored by git and
  read by both the PowerShell scripts and the Python tools.
- An install skill for AI assistants (`.claude/skills/install-totk-harmonyos/`) and `AGENTS.md`.

**Diagnostics (off by default)**
- `TOTK_OHOS_FLICKER=<threshold>`: flicker detection, which logs and saves the frames around it.
- Every 300 frames the log lists the frame rate, pass-end reasons, barriers and texture object counts.

**Known issues**
- 25–30 fps in the open world, lower once the phone is hot.
- Now and then one frame with darker shadows on the ground nearby.
- New areas need one shader fill on the PC.
- `TOTK_OHOS_BC_SHARE=1` (reusing textures that are created again and again) stalled loading on its first test. It is
  off by default; it was changed for the suspected cause and waits for a new test.
