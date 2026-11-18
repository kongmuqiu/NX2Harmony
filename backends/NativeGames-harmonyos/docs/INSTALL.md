# 安装说明

中文 · [English](INSTALL.en.md)

> 这是 NX2Harmony 带的一份拷贝。在 NX2Harmony 里，安装走 NX2Harmony：见 [NX2Harmony 的 README](../../../README.md) 和它的[安装说明](../../../docs/install.zh-CN.md)（AI 助手照 NX2Harmony 的安装 skill `/install-nx2harmony` 来做）。独立的 NativeGames-harmonyos 工程自带的 AI 安装 skill 没有带进来。下面是这个后端自己的手动步骤，写给自己动手的人。

从你自己的游戏文件到手机上能玩，分三段：电脑上准备（第一次约半小时，主要是合并游戏数据和改写代码）、打包签名、装到手机并送数据。

## 1. 准备环境

**电脑**（Windows 10/11 x64，验证过的环境）

- Docker（Docker Desktop 或 Docker Engine），能拉取 `python:3.14` 和 `mcr.microsoft.com/dotnet/sdk:10.0-noble-aot` 两个镜像
- Python 3.12 或更新、Git（只用来取 totk-android 参考仓库）、PowerShell 7
- DevEco Studio（带 HarmonyOS API 26 SDK），它自带 `hdc` 和打包工具
- 一个华为开发者账号（免费），用来给应用签名
- 约 80 GB 空闲空间

**手机**

- HarmonyOS 7，打开开发者模式和 USB 调试，用数据线连上电脑，`hdc list targets` 能看到它
- 约 20 GB 空闲空间

**你自己的游戏文件**，放进文件夹（可以是同一个，只要里面只有一个 `.xci` 和一个 `.nsz`）：

| 文件夹里放 | 说明 |
| --- | --- |
| 基础游戏 1.0.0 的 `.xci`（只放这一个） | 你自己卡带导出的 |
| 1.4.3 更新的 `.nsz`（只放这一个） | 你自己主机上下载的更新导出的 |
| `prod.keys` | 你自己主机上导出的密钥，只在准备时由容器读取，不会被复制或打印 |

每个文件都会按 1.4.3 的已知哈希逐一核对，版本不对或文件不对都会被拒绝。

## 2. 本机设置

在仓库根目录：

```
copy native-harmonyos\local.ohos.example.ps1 native-harmonyos\local.ohos.ps1
```

打开 `native-harmonyos\local.ohos.ps1`，填 DevEco Studio 的位置（hdc 和 SDK 会从里面找）；电脑连着不止一台设备时，再填手机的设备号（`hdc list targets` 显示的那串）。PowerShell 脚本和 Python 工具都读这个文件，它只在你电脑上，不会进 git。

totk-android 的源码会被自动克隆到仓库旁边的 `../totk-android`，并固定在 `native-harmonyos/UPSTREAM_PIN` 里的提交上（想放别处就在本机设置里写 `TOTK_UPSTREAM`）。

## 3. 一条命令准备

```
python native-harmonyos/prepare_v143.py --base <放 .xci 的文件夹> --update <放 .nsz 的文件夹> --keys <放 prod.keys 的文件夹>
```

它按顺序做这些事，每一步做完就会被跳过，中途中断了重新运行同一条命令即可：

1. 解开更新包，取出并核对 1.4.3 的代码模块
2. 把基础游戏和更新合并成完整的 1.4.3 游戏数据（17 GB）
3. 生成运行时需要替换的几个数据文件
4. 从卡带的系统更新分区取出主机字体
5. 构建着色器翻译器
6. 改写三个代码模块，并与 totk-android 验证过的结果逐字节核对
7. 导出打好补丁的代码、运行时表和游戏数据索引
8. 生成着色器种子（几分钟）
9. 构建 Opus 音频和 VP9 视频解码库
10. 编译手机上运行的两个原生库

`--dry-run` 只打印要执行的命令；`--only seed,libs` 只做其中几步。

第 8 步（`native-harmonyos/build_seed.py`）先看这台电脑的 Docker 实际有多少 CPU 和空闲内存，再决定开几个容器、每个容器几个翻译进程（Docker 有 32 个 CPU、16 GB 内存时是 8 个容器 × 4 个）；翻译任务切成由大到小的块，每个容器做完一块再领下一块，快的多做。中间文件都留在 Docker 卷里，电脑上只写回 `seed.vkpack`、`sb-align.txt`、`seed-report.json`（数量和跳过的变体）和 `spirv-cross`。想自己指定可设环境变量 `TOTK_SEED_CONTAINERS`、`TOTK_SEED_SERVICES`。生成的 `seed.vkpack` 与原来单容器的 `build_seed.sh` 逐字节相同（2026-11-16 在一台 32 线程、Docker 分到 16 GB 内存的电脑上从零生成：131 秒，原来 55 分钟）。

手机屏幕比 16:9 宽（多数手机是这样）时，再加上 `--aspect <宽>x<高>`，填横屏的像素数，例如 `--aspect 2848x1320`。准备的第 3 步会多生成一份按这块屏幕排好的界面布局 `extracted/v143/romfs-patches-<宽>x<高>`：HUD 贴到屏幕左右两边，加载画面、过场和菜单的背景铺满全屏。分辨率这样查：`hdc shell hidumper -s RenderService -a screen`，看 `screen[0]` 那行的 `physical resolution=1320x2848`，大的数写在前面。已经准备过的，只补这一份：`python native-harmonyos/prepare_v143.py --only patches --aspect 2848x1320`。

如果容器里装软件包时报 `Hash Sum mismatch`，多半是网络问题，换个软件源先把容器镜像建好再运行：

```
docker build --build-arg APT_MIRROR=mirrors.tuna.tsinghua.edu.cn -t nativegames-tools:1 -f tools/docker/d2r.Dockerfile tools/docker
```

## 4. 签名并打包

1. 用 DevEco Studio 打开这个仓库，手机连着电脑。
2. File → Project Structure → Signing Configs，登录华为账号，勾选自动生成签名。它会写好根目录的 `build-profile.json5`（不会进 git）。签名只对登记过的手机有效。
3. 打包：

   ```
   native-harmonyos\build_hap.ps1 -Tag mine
   ```

   得到 `artifacts\release\mine\totk-harmonyos-mine-signed.hap`。脚本会检查包里只有应用本身、两个原生库和许可证文本。**这个包含有你的游戏代码，只给自己的手机用。**

## 5. 装到手机

```
hdc install -r artifacts\release\mine\totk-harmonyos-mine-signed.hap
```

数据文件夹要由应用的运行时来建（hdc 建不了，只打开启动页也不会建），所以先用第一行让运行时启动一次，再把游戏数据送过去（大约 20 GB），送完关掉应用：

```
hdc shell aa start -a EntryAbility -b TotZ.NativeGames.harmonyos --ps command start
native-harmonyos\v143\push_v143.ps1 -RomFS -GameData -Seed -Patches
hdc shell aa force-stop TotZ.NativeGames.harmonyos
```

第一次这样启动时，应用还会按这块屏幕写好设置文件，并建好按屏幕生成的布局要用的文件夹 `romfs-patches-<宽>x<高>`。`-GameData` 也会把主机字体送进 `game-v143/fonts`，游戏显示中文、韩文要用。`-Patches` 送的布局里有子文件夹，应用要在启动时读到布局的清单（`manifest.tsv`）才会建它们，所以第一次送时，子文件夹里的文件会报发送失败，其余照常；这时在手机上开始一次游戏，再运行一次 `push_v143.ps1 -Patches` 就补上了，下次启动界面贴边。

然后在手机上打开应用，选好设置点“开始游戏”。

手机上的应用用的不是 `TotZ.NativeGames.harmonyos` 这个包名时（比如改过 `AppScope/app.json5`），在 `native-harmonyos\local.ohos.ps1` 里加一行 `$env:TOTK_OHOS_BUNDLE = '<包名>'`，`push_v143.ps1` 和 `fill_shaders_ohos.py` 就会用它；本文 hdc 命令里的包名也换成它。

**以后更新**：`git pull`、`python native-harmonyos/upstream.py --update`、`python native-harmonyos/prepare_v143.py --only libs`，再按第 4 节打包、`hdc install -r`。包名和签名不变时，存档和游戏数据都会保留；要重新准备或重新送什么，看 [CHANGELOG](../CHANGELOG.md)。

**从 0.5.0 升级**：0.5.0 的包名是 `com.macnativegames.harmonyprobe`，现在是 `TotZ.NativeGames.harmonyos`。新包装上是另一个应用，旧应用里的游戏数据和存档不会带过来，按这个顺序做：

1. 备份旧应用的存档：在旧应用的“存档”页点“导出存档”。0.5.0 的导出可能报 `File exists` 失败，那就用电脑取：`hdc file recv -b com.macnativegames.harmonyprobe /data/storage/el2/base/haps/entry/files/android/user-data <电脑上的文件夹>`，把收到的 `user-data` 文件夹打成 zip，传到手机上。
2. 按上面“以后更新”更新代码、编译原生库。包名变了，0.5.0 时生成的签名只对旧包名有效：先按第 4 节第 2 步在 DevEco Studio 里重新勾选自动生成签名，再打包；电脑上其余的准备不用重做（要界面贴边，补一次第 3 节的 `--only patches --aspect`）。装好新应用后按上面把数据全部送一遍。两个应用都在时，手机要再空出约 20 GB。
3. 在新应用的“存档”页点“导入存档”，选那个 zip（它会在 zip 里找到存档所在的文件夹）。
4. 在新应用里读档、确认能玩之后，再卸载旧应用：`hdc uninstall com.macnativegames.harmonyprobe`。卸载会删掉旧应用里的存档和约 20 GB 数据。

## 6. 补着色器

第一次到某个地方时，种子里没有的着色器会被跳过（那一帧有些东西画不出来），手机会记下缺了哪些。玩过一段后，连上手机在电脑上运行：

```
python native-harmonyos/fill_shaders_ohos.py --version v143
```

它取回这些记录、在电脑上翻译好再送回手机，下次启动就补齐了。

## 7. 设置

启动页“设置”页里的改动随改随存，下次开始游戏时生效。游戏里点屏幕顶部中间的小标签（或用返回手势）打开游戏菜单：继续游戏、游戏设置（改完点“应用并重启”）、触屏按键（自动 / 一直显示 / 隐藏）、按键布局（游戏定制 / 标准手柄）、视角操作（划屏 / 双摇杆）、回到启动页。

| 预设 | 内容 | 适合 |
| --- | --- | --- |
| 流畅（默认推荐） | 540p 渲染、游戏自带 FXAA、阴影中 / 2 级、输出 1920、SGSR 放大 | 日常玩，最稳 |
| 均衡 | 游戏自己的渲染分辨率 | 画面更细，帧率略低 |
| 画质 | 底座模式、FSR、SMAA | 看风景 |
| 高帧率 | 上限 60 帧，其余同流畅 | 开阔地带能到 50+，复杂场景仍在 20 多 |

“画面”里的“界面位置”（“画面比例”为全面屏时才显示）：**贴边**（默认）用按屏幕生成的布局，血条贴到屏幕左边、小地图贴到右边；手机上没有对应这块屏幕的布局（准备时没加 `--aspect`）时，界面和菜单在中间 16:9。**居中 16:9** 总是放在中间。

操作：屏幕按键默认按这个游戏的动作标好（“游戏定制”），“操作”里的“触屏按键布局”可以换成“标准手柄”（Switch 原样的 A B X Y、十字键、肩键和 + −）；屏幕右侧空白处滑动转视角。也可以连蓝牙或有线手柄，连上后屏幕按键自动隐藏；“操作”页里能选手柄按键布局（按标签或按位置）、震动强度和体感瞄准。游戏里要输入文字（比如给马起名）时会弹出系统键盘。

## 8. 存档

存档在应用自己的目录里，**卸载应用会一起删除**。

启动页的“存档”页可以把存档导出成 zip 存到手机任意位置，也可以从 zip 导入（本应用导出的，或存档管理工具导出的整套存档）。每次开始游戏前、导入和恢复前都会自动备份，保留最近 10 份，在同一页里可以恢复。

也可以用电脑备份：

```
hdc file recv -b TotZ.NativeGames.harmonyos /data/storage/el2/base/haps/entry/files/android/user-data <电脑上的文件夹>
```

1.0.0 时期的存档也能直接读。

## 9. 出问题时

- 运行日志：`hdc file recv -b TotZ.NativeGames.harmonyos /data/storage/el2/base/haps/entry/files/android/totk-native.log .`。每 300 帧有一组帧率、GPU 和 CPU 用时、纹理上传等统计。
- 应用被系统直接关掉：多半是 GPU 内存超过了系统给单个应用的上限（约 3.1 GB），日志最后一条 `memory Vulkan bytes` 会显示用量。
- 诊断开关不要写进 `env.txt`：启动页每次保存设置都会整份重写它。写进 `files/android/env-dev.txt`，它在 `env.txt` 之后读，设置页从不改它。在电脑上建这个文件（每行一个 `名字=值`），送过去，下次开始游戏时生效：

  ```
  Set-Content -Encoding ascii env-dev.txt 'TOTK_OHOS_FLICKER=20'
  hdc file send -b TotZ.NativeGames.harmonyos env-dev.txt /data/storage/el2/base/haps/entry/files/android/env-dev.txt
  ```

  以后要改，换一个新文件名送，比如 `env-dev-2.txt`：应用不一定看得到已经读过的文件被改写，`env-dev-*.txt` 在 `env-dev.txt` 之后按文件名顺序读，后读的为准。`名字=`（等号后为空）清掉这个开关。
- 画面闪烁可以打开检测：在 env-dev 文件里写 `TOTK_OHOS_FLICKER=20`（上面的例子），日志会记录并保存闪烁那几帧的截图。
- 世界停住（画面还在刷新，但游戏里的时间不走，游戏的读取圈一直在转）：默认的两帧并行偶尔会这样，原因还在查。在 env-dev 文件里写 `TOTK_OHOS_SYNC_LAG=0` 退回单帧：单帧时还没出现过这种停住，只是帧率低一些（实测约 20–27 fps，两帧约 30）。把当时的运行日志留下来报告问题。要换回两帧，再送一个新名字的文件，写 `TOTK_OHOS_SYNC_LAG=1`。
- 选中文或韩文后游戏在标题画面退出，日志里有 `[totk] FAIL: open shared font`：主机字体没送到手机（0.5.0 没送）。重新运行 `push_v143.ps1 -GameData`；它提示 `shared-fonts` 里没有 .bfttf 时，先运行 `python native-harmonyos/prepare_v143.py --only fonts --base <放 .xci 的文件夹> --keys <放 prod.keys 的文件夹>`。
- 选了“贴边”，界面却在中间 16:9：日志里 `HUD in the middle 16:9` 那行写着原因。应用按设置文件里的屏幕大小找布局，文件夹名来自 `files/android/env.txt` 里的 `TOTK_ASPECT=<宽>:<高>`（用 `hdc file recv` 取回来看）；它和准备时 `--aspect` 填的不一样时（比如手机的屏幕分辨率设得比屏幕本身低），按它补一次 `--only patches --aspect <宽>x<高>`，再送 `-Patches`。
