# 工作原理

[← 返回 README](../README.md) · [English](how-it-works.md)

NX2Harmony 是玩家直接运行的那一部分。它自己不移植游戏：每个已适配的游戏版本都有一个后端来做这件事（把游戏代码改写成能在鸿蒙上运行、提供游戏要调用的系统服务、图形、着色器、外面那层鸿蒙应用）。每个后端在自己的工程里开发；本仓库在 `backends\` 里放着每个后端的一份副本（见 [backends/README.md](../backends/README.md)）。NX2Harmony 负责检查电脑、找到玩家的游戏，再一步步驱动对应的后端，从玩家自己的文件一直走到游戏在手机上运行。安装和使用见 [install.zh-CN.md](install.zh-CN.md)。总体设计和来龙去脉见 [DESIGN.md](DESIGN.md)。

目录：[组成部分](#组成部分) · [哪些游戏能构建](#哪些游戏能构建) · [识别游戏](#识别游戏) · [一次构建](#一次构建) · [进度事件](#进度事件) · [包名和签名](#包名和签名) · [安装和推送](#安装和推送) · [图标和字体](#图标和字体) · [完整包](#完整包) · [什么留在电脑上](#什么留在电脑上)

## 组成部分

```
NX2Harmony 窗口（nx2h/app.py）          命令行：python nx2h.py / nx2h.exe（nx2h/cli.py）
   以子进程运行 "nx2h build ... --progress json"，把它的事件画出来
                │
nx2h/       scan · doctor · device · bundle · signing · agc · icon · fonts · events · config
                │
nx2h/backends/  common.py：HAP、包名和签名的替换、图标、安装、推送
                totk_v143.py · za_202.py · oot_v131072.py：各游戏的输入和步骤
                │  一步一步运行后端自己的脚本
                ▼
backends/          NativeGames-harmonyos（王国之泪）· LZA（Z-A）· OOT（时之笛）
用到               固定提交的 totk-android · nxnative（时之笛的着色器工具）
                   Docker 容器（Linux 工具）· DevEco Studio（clang、hvigor、hdc）
```

| 部分 | 做什么 |
|---|---|
| `nx2h/cli.py` | `setup`、`scan`、`build`；构建前的检查、签名和各段 |
| `nx2h/app.py`、`nx2h/core.py` | 窗口（Qt / PySide6），以及它要用、但与画界面无关的逻辑：游戏库、检查、文件夹选择 |
| `nx2h/scan.py` | 找到游戏文件并识别，不需要密钥 |
| `nx2h/doctor.py` | “环境”页的各项检查，以及用 winget 安装 |
| `nx2h/device.py` | 通过 DevEco Studio 的 hdc 操作手机；找 DevEco Studio 和它的 Java |
| `nx2h/bundle.py`、`signing.py`、`agc.py` | 每个游戏的包名和签名（后台申请，或用 DevEco Studio） |
| `nx2h/icon.py`、`fonts.py` | 游戏图标；主机的共享字体 |
| `nx2h/events.py` | 进度事件：给人看的文字，或给前端读的逐行 JSON |
| `nx2h/config.py` | 东西放在哪：仓库目录，或打包版旁边的 `data\`；`nx2h.local.json` |
| `nx2h/mirrors.py` | 选最快的下载源（已关闭，`ENABLED = False`） |
| `packaging/` | `build.py` 用 PyInstaller 打包软件；`full_bundle.py` 打完整包 |

窗口自己从不构建：它带 `--progress json` 启动同一个命令行，把汇报的内容画出来，所以窗口和命令行做的永远是同一件事。

后端里面做什么，简单说（细节见 [DESIGN.md](DESIGN.md) 和 `backends\` 里的各后端）：游戏的代码模块被改写成鸿蒙下的原生库；totk-android 的运行时提供主机的系统服务；游戏的 NVN 图形命令先记录，再用 Vulkan 回放；着色器在电脑上翻译成种子（`seed.vkpack`）；手机上外面那层是 ArkTS 应用。游戏的 RomFS 不打进 HAP，而是推进应用自己的存储空间。

## 哪些游戏能构建

[games.tsv](../games.tsv) 是鸿蒙验证表：每个（游戏、版本）一行，记着 title ID、简称、版本、更新、后端、状态和需要的文件。只有这一行的状态是“已验证”（在手机上玩通过）或“优化中”（能玩，帧率和个别画面还在调），并且后端在，这个游戏才能一键构建。状态是“适配中”的只显示、不构建；表里没有的游戏显示为“未适配”。一个版本要在本项目里做完、在手机上玩过才进表：在 Mac 或 iPhone 上能玩，说明不了鸿蒙上的情况，Vulkan 回放、GPU、内存和速度都不一样。

| 游戏 | 版本 | 后端模块 | 后端（`backends\` 里的文件夹） |
|---|---|---|---|
| 王国之泪 | 1.4.3（更新 v720896） | `totk_v143` | NativeGames-harmonyos |
| 时之笛，Switch 2 版 | v131072 | `oot_v131072` | OOT（另需 nxnative） |
| 宝可梦传说 Z-A，Switch 2 Edition | 2.0.2（更新 v393216） | `za_202` | LZA |

后端按这个顺序找：`nx2h.local.json` 的 `"backends"`（那里记下的位置，只要文件夹还在就用），然后是 `backends\`：仓库自己的，或打包版旁边的；再往后是 0.1.2 以前用的位置：本仓库旁边，或打包版往上几层的文件夹。用一个标志文件判断它在不在（比如 `native-harmonyos/prepare_v143.py`）。预编译库在后端文件夹旁边的 `prebuilt-ohos` 里找。

## 识别游戏

`scan.py` 遍历选中的文件夹和所有子文件夹（跳过隐藏和系统文件夹、软件自己的文件夹、后端的 `prepared\` 和 `artifacts\`），找至少 1 MB 的 `.nsp`、`.nsz`、`.xci`、`.xcz`、`.nup`，只读没有加密的部分：
- **NSP / NSZ：** PFS0 文件表；票据文件名的开头就是 title ID。版本取自文件名里的 `[v…]` 标签。
- **XCI / XCZ：** 卡带头；里面的 title 是加密的，所以 title ID 和版本取自文件名里的 `[…]` 标签。
- **NUP**（Switch 2 的包）：目录表里列着每一部分的 title、版本和类型。

更新的 title 是本体加 `0x800`，追加内容是本体加 `0x1000` 以上。凭文件名猜错也不要紧：后端在构建任何东西之前会核对代码模块的哈希。

## 一次构建

### 第一步之前

`nx2h build` 选出唯一可构建的游戏（或 `--game` 指定的那个），问它的后端要哪些文件、数据还要不要准备，然后依次检查：文件；后端所在盘的剩余空间（只在还要准备数据时）；`prod.keys`（只在还有步骤要用时）；手机（除非 `--no-install`）；运行后端脚本用的 Python。之后让手机保持亮屏；这个游戏的包名在这台电脑上还没有签名、或签名里没有连着的手机时，先签名（见[包名和签名](#包名和签名)）。

### 各段

1. **准备游戏数据**：后端的准备步骤。
2. **编译原生库**：后端的 `native-harmonyos/build_ohos.py`，用 DevEco Studio 的 clang。
3. **打包 HAP**：后端的 `native-harmonyos/build_hap.ps1`（hvigor），换上这个游戏的包名、签名和图标；它会逐项审计包里的内容。
4. **安装到手机**，5. **推送游戏数据**（见[安装和推送](#安装和推送)）。
6. 后端在数据推完后追加的段：Z-A 第一次时的着色器种子。

最后启动游戏。用 `--no-install` 时，第 3 段完成就结束。

每个后端先检查参考仓库 totk-android 是否在各后端固定的提交（`native-harmonyos/upstream.py --check`），不会对着缺失或不同的副本运行任何步骤。

各游戏的准备步骤，每一步发一个 `step` 事件：

| 王国之泪（`prepare_v143.py --only <步骤>`） | 宝可梦传说 Z-A | 时之笛 |
|---|---|---|
| 解开 1.4.3 更新 | 解开 NUP 的更新部分（逐个文件核对 SHA-256） | 解开 NUP 的更新部分（同样核对） |
| 合并本体和更新的 RomFS，按 1.4.3 核对 | 改写 `main` 和 `sdk`（对照预期哈希） | 核对代码模块（build ID、SHA-256） |
| RomFS 补丁文件；屏幕比 16:9 宽时另做 `romfs-patches-<宽>x<高>` | 运行时表 | `main`、`sdk`、`subsdk0` 存成未压缩 |
| 系统字体（`nx2h/fonts.py`） | 打包 RomFS | 改写这三个模块 |
| 着色器翻译器（在 .NET SDK 容器里编译） | 系统字体 | 运行时表 |
| 改写游戏代码 | 着色器翻译器 | 打包 RomFS（约 12 分钟） |
| 客体数据（运行时表、客体库源码、RomFS 索引） | 客体库，含宽屏相机补丁 | 系统字体 |
| 着色器种子（首次约 3 分钟） | 游戏数据 | 客体库，含帧步长和动态分辨率补丁 |
| 第三方静态库（libopus、libvpx） | 第三方静态库 | 游戏数据 |
| | 第一次安装后：从手机生成着色器种子 | 着色器翻译器（Ampere），然后在电脑上的九步着色器，从抽出全部着色器到打包种子 |
| | | libopus / libvpx、dav1d（AV1 过场动画） |

Z-A 的着色器种子来自手机：它的着色器放在只有游戏自带 Oodle 解码器能打开的容器里。第一次安装后，带一个设置启动应用，让它解包全部着色器（约 1 分钟，手机上约 7.5 GB）后自己退出；电脑拉回、在容器里翻译全部着色器（约 1 小时）、推回种子，再带另一个设置启动一次，删掉解包文件。时之笛的 RomFS 是带字典的 zstd，所以着色器在电脑上用 nxnative 的工具抽出和翻译。九步着色器里有一步专门翻译手机测试时补出来的 146 个变体（只有哈希和答案，不含游戏数据）；有了这一步，从头构建的种子在这 146 个变体上和手机上测试过的那份逐字节一致，只缺 182 个运行时不会用到的补翻，另有 4 个旧模块是更新的构建。

**断点续跑。** 产出已经在的步骤报告为“已有，跳过”，不再运行；运行结束却没留下预期产出的步骤会让构建停下。所以停下的构建会从停下的地方接着走，同一个游戏第二次构建只重新打包和安装。后端的 `prepared\` 就是缓存：别的电脑上准备好的数据可以放进去，它覆盖的步骤即使没有 Docker 也会跳过；客体库源码里按绝对路径写着游戏镜像的位置，换了位置会重新生成。

**日志。** 每一步的输出写进自己的日志文件 `<日期>-<时间>-<游戏>-<步骤>.log`，最新一行作为 `detail` 事件发给前端。某一步失败时，打印它的最后几行和日志路径。

### 容器和下载

Linux 工具在 Docker 容器里运行：
- `nativegames-tools:1`：在这台电脑上用后端的 `tools/docker/d2r.Dockerfile` 建一次（改写代码、客体导出、libvpx）。时之笛在同一个标签下给它加上 spirv-tools 和 glslang。
- `python:3.14`，来自 Docker Hub（王国之泪的准备、着色器种子、SPIRV-Cross）。
- `mcr.microsoft.com/dotnet/sdk:10.0-noble-aot`（编译着色器翻译器）。
- 卷 `totk-venv`：放王国之泪解包那一步的 Python 环境。

一个镜像总共最多建四次，都不成功构建才停下。`nx2h/mirrors.py` 能对 Docker 镜像仓库、Debian 和 Ubuntu 软件源、Python 包索引和 NuGet 源测速，用最快的；在全新电脑上实测之前它是关着的（`ENABLED = False`）。关着时什么都不测：发布的镜像由 docker 自己拉，不给 pip 和 NuGet 指定任何源；四次建镜像里，前两次用 Dockerfile 自己的 Debian 源，第三、四次换用清华和中科大的 Debian 镜像站（`LEGACY_APT`）。

## 进度事件

`nx2h build --progress json` 在标准输出上每行写一个 JSON 对象，格式与 nxnative 的 `build_ipa_from_nsp.py` 相同（见 nxnative 的 `docs/frontends.md`），为 nxnative 做的前端可以照样读。每个对象都有 `ev` 和 `t`（Unix 时间，秒）。

| `ev` | 字段 | 什么时候 |
|---|---|---|
| `game` | `title_id`、`name`、`version`、`bundle_id`、`out`（HAP 路径）、`icon` | 选定游戏时一次 |
| `stage` | `k`、`n`、`name`、`step` | 一段开始（第 `k` 段，共 `n` 段） |
| `step` | `k`、`name`、`step` | 段里的一步开始 |
| `progress` | `k`、`name`、`step`、`done`、`total`、`unit`（字节为 `"B"`）、`est`、`note`、`frac`、`eta`、`elapsed` | 步骤运行中；不知道总量时 `total` 为 `null` |
| `stage_end` | `k`、`name`、`ok`、`summary`、`elapsed`（还可能有 `warn`） | 一段结束 |
| `say` | `msg` | 一条消息；错误以 `[RED] ` 开头 |

NX2Harmony 多了三种，nxnative 的前端可以忽略：

| `ev` | 字段 | 什么时候 |
|---|---|---|
| `detail` | `text` | 正在运行的工具的最新一行输出，大约每秒最多两次 |
| `signing` | `bundle`、`project`、`opened` | 后台签名失败：给 DevEco Studio 用的签名工程，以及是否已经打开 |
| `awake` | `on`、`target`、`server`、`restore` | 手机开始（或不再）保持亮屏；前端如果中途杀掉构建，要用 hdc 在那台手机上运行 `restore` |

`nx2h setup --progress json` 把每项检查报告为 `{"ev": "check", "key", "label", "ok", "detail", "how", "fixable", "verb"}`（只是信息的行，`ok` 为 `null`；`verb` 是自动修复做的事：`安装` 或 `启动`）。

```
{"ev": "game", "t": 1763370000.0, "title_id": "0100F2C0115B6000", "name": "塞尔达传说 王国之泪", "version": "1.4.3", "bundle_id": "nx2h.uk3f9a2.totk", "out": "…\\artifacts\\release\\nx2h-20261117-1015\\totk-harmonyos-nx2h-20261117-1015-signed.hap", "icon": null}
{"ev": "stage", "t": 1763370012.5, "k": 1, "n": 5, "name": "准备游戏数据", "step": "解包、改写、着色器"}
{"ev": "step", "t": 1763370012.6, "k": 1, "name": "准备游戏数据", "step": "解开 1.4.3 更新"}
{"ev": "stage_end", "t": 1763371200.0, "k": 1, "name": "准备游戏数据", "ok": true, "summary": "游戏数据就绪", "elapsed": 1187.5}
{"ev": "progress", "t": 1763371700.0, "k": 5, "name": "推送游戏数据", "step": "推送游戏数据", "done": 4294967296, "total": 18575523840, "unit": "B", "est": false, "note": "", "frac": 0.23, "eta": 412.0, "elapsed": 123.0}
```

退出码与 nxnative 相同：`0` 完成，`1` 因错误停下（最后一行 `[RED]` 说明原因），`2` 参数不对，`130` 被中断。不带 `--progress json` 时，同样的事件以给人看的文字打印（`[1/5] 准备游戏数据`、进度行、`完成：…（12 秒）`），`detail` 只写进那一步的日志。

## 包名和签名

**包名**（`nx2h/bundle.py`）。SDK 对包名的规定：7 到 127 字节，反向域名，每一段以字母开头，只含字母、数字和下划线。NX2Harmony 生成 `nx2h.<本机>.<简称>`，简称取自 `games.tsv`，比如 `nx2h.uk3f9a2.totk`。“本机”那一段是 `u` 加 Windows MachineGuid 哈希的 6 位 36 进制字符，所以这台电脑上的仓库目录和打包版的每一份拷贝得到同样的名字，两个玩家也不会向华为申请同一个名字（账号会登记它签过的包名）；没有 MachineGuid 时随机生成一次。名字一旦生成就记在 `nx2h.local.json` 的 `"bundles"` 里，不再改变，因为手机上的应用、它的存档和签名 Profile 都绑在这个名字上；两个游戏简称相同时，第二个加数字。每个后端在自己的 `AppScope/app.json5` 里有一个默认包名；每次打 HAP 时那里换成这个游戏的包名（签名换进 `build-profile.json5`，见下文），同时通过 `TOTK_OHOS_BUNDLE` 交给后端的脚本。

**用哪份签名**（`nx2h/signing.py`）。调试 Profile（`.p7b`）写着一个包名和能安装的手机。一个包名在这台电脑上能签，是指有一份没过期的、写着它的 Profile：先找后台申请的那份（到期前一天就重新申请），再找 DevEco Studio 写进后端和签名工程 `build-profile.json5` 里的配置。这些文件只在进程里读：不打印、不写日志、不拷出这台电脑。

**后台签名**（`nx2h/agc.py`，默认），照 DevEco Studio 自动签名的做法：
1. **登录：** 在 `127.0.0.1` 上开一个接收端，浏览器打开华为的 DevEco 登录页；玩家登录后，页面把临时令牌交回，再换成账号的访问令牌和团队。
2. **证书：** 用 keytool（优先 DevEco Studio 自带的 Java）生成一把 EC P-256 密钥和证书请求，为这台电脑申请一张调试证书，所有游戏共用。
3. **设备：** 读连着的手机的 UDID（`bm get --udid`），账号里没有就加进去。
4. **Profile：** 为这个包名申请调试 Profile，带上账号里的全部手机，不带 ACL 权限（和 DevEco Studio 签出来的一样）；同一包名的旧 Profile 换掉。
5. **给 hvigor 用：** 密码按 hvigor 解密的格式加密（AES-128-GCM，工作密钥由 `material\` 里的根密钥封存，`material\` 是 NX2Harmony 自己生成的）。
6. **存放：** 都在 `%USERPROFILE%\.nx2h`；`account.dat` 和 `keys\key.dat` 用 Windows DPAPI 加密，只有这个 Windows 用户能打开。

这个服务就是 DevEco Studio 用的那个，不是公开文档里的接口，所以可能会变。

**用 DevEco Studio（备用）：** `signing.project_for` 把后端的应用外壳（`AppScope`、entry 模块的 `src/main`，即外壳自己的源码、资源和 `module.json5`，entry 模块的构建文件，hvigor 文件；构建产物不拷，所以不含游戏代码、编译出的库和游戏数据）拷进 `%USERPROFILE%\.nx2h\signing\NX2H_<简称>`，换成这个包名，再用 DevEco Studio 打开，玩家在里面打开一次自动签名。下次构建就能找到这份配置。

**打 HAP 期间**，后端的 `AppScope/app.json5` 换成这个游戏的包名，`build-profile.json5` 换成这个包名的签名；原件挪到后端已忽略的 `prepared/nx2h-stage/signing`，打完立刻换回，构建中途被停掉时，下次构建开始前也会先换回。后端本身从不留下别的玩家的包名或签名。

## 安装和推送

- **安装：** 先停掉正在运行的应用，再用 `hdc install -r` 装签名的 HAP。
- **标记为游戏：** 告诉系统的游戏服务（系统能力 66006）把这个包当游戏，于是它得到系统的游戏策略，其中包括让 GPU 保持高频。手机没确认时只记一行提示，构建照常继续。
- **文件夹：** hdc 不能在应用的存储空间里建文件夹，shell 也不一定能建游戏要的每一个。所以先不带游戏页启动一次应用（`aa start ... --ps command start`）：它的运行时会建好游戏的文件夹，构建等到第一个文件夹出现为止。其余文件夹用手机 shell 的 `mkdir -p` 建；因为文件夹不在而发送失败时，再启动一次运行时，然后重发。
- **推送：** 推的文件和后端自己的推送脚本相同（`push_v143.ps1`、`push_za.ps1`、`push_oot.ps1`），位置也相同：运行时表、RomFS 镜像和索引、字体、着色器种子，王国之泪还有补丁文件（每个文件夹先推 `romfs-patches*/manifest.tsv`，应用按它建补丁的子文件夹）。发送前，每个文件和手机上的那份比较：大小相同、SHA-256 相同，16 MiB 以内比整个文件，更大的比均匀分布的 16 段、每段 1 MiB。只发送不同或缺少的文件（`hdc file send -b <包名>`）；大文件边发边按字节报告进度。最后汇总发了几个、跳过几个。
- **亮屏：** 从签名到游戏打开，手机的息屏时间被改成约 5 分钟，每 20 秒续一次（`power-shell`）；结束后恢复手机自己的设置，出错时也一样。窗口中途停止构建时，由窗口自己恢复（靠 `awake` 事件）。
- **用哪台手机：** hdc 来自正在用的 DevEco Studio（SDK 最新的那个）。正在用的 hdc 服务看不到手机时，会问这台电脑上运行着的其他 hdc 服务（DevEco Studio 可能开着自己的），看得到手机的那个会被记住。
- **启动：** `aa start -b <包名> -a EntryAbility` 打开游戏。

## 图标和字体

**图标**（`nx2h/icon.py`）：先从玩家自己的文件里取。XCI、NSP、NSZ 的 control 数据里有图标，由 `nx2h/vendor/switchfs` 里的读取代码（取自 totk-android，MIT）用 `prod.keys` 在软件进程内解密；NUP 的 meta 块里可能有一个，不加密。文件里没有图标的游戏，按 title ID 联网查 eShop 图标（`fetch_online`）：先查图片服务 tinfoil.media，查不到再用社区 titledb 列表里指向任天堂 CDN 官方图标的链接，少数 Switch 2 游戏用任天堂官网上的图。扫描游戏库时自动进行（`extract_later`），构建时也会再查一次，「从网上取图标」按钮也能触发。发出去的只有 title ID：titledb 列表是整份下载后在电脑上查的，只留一个小索引 `work\titledb-index.json`。图标存在 `work\icons`；项目和软件包里都不带游戏图片。打 HAP 时，后端的图标图片（`app.json5` 和 `module.json5` 引用的那些、分层图标的前后两层、外壳自己的游戏图）先挪到 `prepared/nx2h-stage/app_icon`，这次构建换成游戏图标，打完立刻换回。

**字体**（`nx2h/fonts.py`）：每个游戏都向主机要 6 个共享字体（BFTTF）。卡带镜像（`.xci`）的系统更新里带着；NUP、eShop 版和 `.xcz` 不用来取字体。软件用玩家的 `prod.keys`，从游戏库里玩家自己的一张 `.xci` 卡带镜像（不必是这个游戏的），或玩家从自己主机导出并解开的固件（放在 `prod.keys` 附近，或在 `nx2h.local.json` 里写成 `"firmware"`）里读一次，校验（NCA 文件系统头、6 级哈希、每个字体的文件头）后存在 `work\shared-fonts`，所有游戏共用。

## 完整包

`python packaging/full_bundle.py --out <文件夹> [--zip]` 生成一个文件夹，在全新电脑上只要再有玩家自己的游戏文件、密钥、DevEco Studio 和手机就能用：
- 打包好的软件（`dist\NX2Harmony`，先运行 `packaging/build.py`），`data\` 是空的，旁边放一份 `THIRD_PARTY_NOTICES.md`；
- `backends\` 里：本仓库当前提交里的 `backends/`（`git archive`：各后端的源码；`prebuilt-ohos\` 里预编译的 libopus 和 libvpx 及其许可；它的 `README.md` 和 `backends/SOURCES.tsv`），并用 `backends/NX2HARMONY-COMMIT.txt` 记下这个提交；固定提交的完整 totk-android，带一个标志文件（`totk-android/NX2HARMONY-UPSTREAM.txt`，写着它的地址和提交），`upstream.py` 把它当作固定提交的检出，不需要再克隆；时之笛要用的那部分 nxnative，在 `backends/SOURCES.tsv` 固定的提交（`nxnative/NX2HARMONY-COMMIT.txt`）。所有 `.sh` 都以 LF 换行放进去（它们在 Linux 容器里运行）。

从不放进去的：`prepared\` 或 `artifacts\` 下的任何东西（游戏材料、构建出的包）、构建输出和缓存、签名配置（工程根目录的 `build-profile.json5`）、本机设置（`local.ohos.env` / `.ps1` 及其副本、`local.properties`）、密钥、证书和 Profile、游戏文件、应用资源以外的图片、备份副本。`packaging/backend_import.py` 导入时就不让这些进 `backends/`，打包时再按提交里的文件查一遍。后端里留作默认值、在某台电脑上生成的包名会换成 `nx2h.u000000.<简称>`。

拷贝任何东西之前就会拒绝：`backends/` 下有没提交的改动（完整包必须对得上一个提交）；那个提交里的后端文件夹缺标志文件，或带着完整包从不放的文件；几个后端固定了不同的 totk-android 提交，或者找不到含这个提交的 totk-android 检出（`backends\totk-android`、后端自己用的那份，再就是本仓库旁边）；后端里会暴露打包人或打包地点的文件（比如打包用户的文件夹、他的 git 邮箱、带端口的局域网地址）；后端应用里没有按 SHA-256 登记在 `packaging/bundle_pictures.tsv` 的图片（登记过的每一张都有人看过：中性的默认图，不含任何游戏或公司的美术）；`prebuilt-ohos` 里缺 libopus 或 libvpx。打包过程中会拒绝、留下一个没打完的文件夹的：缺 `build-profile.template.json5` 的后端、找不到含固定提交的 nxnative 检出（`backends\nxnative`、本仓库旁边，或时之笛后端自己找到的那份）、结果里混进了 `prepared\` 或 `artifacts\` 的东西。后端文件里写到的打包电脑自己的文件夹路径只列出来，不拒收（`packaging/backend_import.py` 在它们进仓库之前就会拒收）。完整包里没有游戏内容：玩家自己的游戏图标在构建时才放进各自的 HAP。

## 什么留在电脑上

游戏文件、密钥、固件，用它们做出的一切（`prepared\`、`artifacts\`、`work\`、`logs\`、HAP），以及所有签名材料，都留在玩家自己的电脑上。`.gitignore` 把它们挡在仓库外，发布标准是只发源码：从不发布 HAP、游戏数据或签名材料。构建做出的东西不上传到任何地方；往外发的只有向玩家华为账号申请签名的请求、按 title ID 查图标（见[图标和字体](#图标和字体)），以及下载工具和容器镜像。
