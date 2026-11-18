# NX2Harmony 设计（nxnative 的鸿蒙版）

**目标**：照 nxnative 的做法做一个鸿蒙版：一套通用主机，加上每个游戏一份档案（profile）。玩家用自己的游戏文件和密钥，在 **Windows** 上一条命令构建出该游戏的签名 HAP，装到鸿蒙手机。TOTK 和 Z-A 是头两个游戏，之后加游戏主要是加档案。

**结论**：可行。分两段走：

- **A 段**：先做统一入口。"已适配"以**我们本地做完的为准**：TOTK 1.4.3 和 Z-A 2.0.2。先做一个统一的 Windows 构建入口，导入游戏文件，一条命令出可玩的 HAP。这两个游戏就用现在已经跑通的两条流水线做"游戏后端"，鸿蒙上的成果一点不丢。
- **B 段**：通用内核。移植 nxnative 本身（跟随上游），只换掉苹果平台层：Metal 换成 Vulkan，Darwin 换成鸿蒙，Mac 构建换成 Windows 构建。新游戏走这个内核：用上游档案，加 AI 辅助适配。TOTK 和 Z-A 在新内核上达到同样效果后，再切换过去。

A 段里的入口、版本识别、签名、安装、推送、验证表，B 段原样沿用；B 段只替换"改写 + 主机 + 图形"这一块。

本文起草于 2026-11-09，A 段的进展记在第 8 节。游戏文件、密钥和构建产物照旧只留在本机，不进仓库。

---

## 1. 两边现状对照

| | nxnative（Mac / iPhone） | 我们的鸿蒙栈（TOTK、Z-A） |
|---|---|---|
| 血缘 | `dm/` = D2R Mobile 演化出的通用版 | totk-android（同样源自 d2r-mobile）+ 鸿蒙 overlay |
| 游戏 | 158 份档案，含 BotW、TOTK（`0100F2C0115B6000`）、Z-A（`0100F43008C44000`） | 2 个，每个游戏一套仓库 / 工程 |
| 改写器 | `dm/tools/patch_nso.py`：按 Darwin 改写（x18 和 `tpidr_el0` 会被苹果内核改动，线程槽经 `tpidrro_el0` 取） | `d2r_ohos.py`：已按鸿蒙改好（musl，线程指针 `tpidr_el0`，TLS 从 TP+0 起） |
| 主机（Horizon 服务） | `dm/host` 约 3.9 万行 C；Darwin 接口集中在 19 个文件、约 130 处（mach 时间、线程 QoS、`os_unfair_lock`、dispatch、`MAP_JIT`），另有 4 个 `.m` | totk-android `runtime/`（118 个文件，只为 TOTK），Z-A 靠 overlay 往上补 |
| 图形 | NVN 先记录，再回放成 Metal（`nvn_mtl.m`）；记录格式 59 种操作 | NVN 先记录，再回放成 Vulkan（`android/gfx`，约 6 万行）；记录格式 45 种，是 nxnative 的子集；外加 Maleoon 修正（TF 移到片元阶段、生成几何着色器做分层、存储缓冲 256 对齐、原生缓冲内存池等） |
| 着色器 | 离线：Ryujinx 出 SPIR-V，再转 MSL、编成 metallib；每个着色器只翻一次（答案来自 `shader_facts.tsv`），运行时由 `tex_fit` 适配绑定 | 离线：Ryujinx 出 SPIR-V，再做 Vulkan 反射；现在也是每个着色器只翻一次（`TOTK_VK_DEPS=queried`） |
| 前端 | Swift / ObjC：启动器（由 `settings.tsv` 生成）、触屏手柄、手柄、体感 | ArkTS：启动页、设置、存档、触屏手柄，XComponent + Vulkan，OHAudio，GameController |
| 构建 | 只能在 macOS 上：Xcode、`xcrun metal`、xcodegen | Windows：DevEco（OHOS clang、hvigor、hdc），Linux 工具放 Docker |
| 许可 | MIT | totk-android MIT、d2r MIT、我们自己的代码 |

关键事实：
- 两边的 NVN 记录格式同源，所以现有的 Vulkan 回放能改成直接吃 nxnative 的记录流。只需补 14 种操作：TF、多重绘制、push constant、等待事件 / 同步、视口 swizzle、DLSS 等，其中 TF、光栅化丢弃我们为 Z-A 已经做过。
- nxnative 的着色器链、档案表、改写器核心都是 Python 或 C#，能在 Windows 或 Docker 里跑。离不开 Mac 的只有 Metal 编译和 Xcode 打包，这两步鸿蒙版本来就不需要。

## 2. 分层：哪些复用，哪些换掉

```
nxnative 上游（固定提交，尽量不改）         鸿蒙平台层（新写或从现有鸿蒙栈移植）
───────────────────────────────            ─────────────────────────────────────
dm/games/<id>/  档案、补丁、着色器表        platform/ohos/os_compat   Darwin → musl / 鸿蒙
dm/tools/       改写器核心、nso 工具、       platform/ohos/rewrite     线程槽改走 tpidr_el0，输出 ELF .so
                shader_split、dmsh_facts    platform/ohos/gfx/nvn_vk  记录流 → Vulkan 回放（Maleoon）
dm/host/*.c     Horizon 服务（HLE）          platform/ohos/shader      dmsh_build 只做到 SPIR-V + Vulkan 反射
dm/gfx/nvn/hle_nvn.c, nvn_obj.h  接口层     platform/ohos/app         ArkTS 前端（启动器 / 设置 / 手柄 / 音频）
dm/gfx/tex      块线性解交错                platform/ohos/build       build_hap_from_nsp.py（Windows）
```

1. **OS 兼容层**：用一个头文件加少量 C，接住那约 130 处 Darwin 调用。
   - `mach_absolute_time` 换成 `clock_gettime`。
   - 线程 QoS 换成 `setpriority` 和亲和性（沿用现有鸿蒙栈的调度）。
   - `os_unfair_lock` 换成 futex 或 pthread 锁，dispatch 换成 pthread。
   - 需要 JIT 的游戏（`hle_jit.c`，目前 1 个）在鸿蒙上不支持：没有可写可执行内存。
2. **改写器后端**：`patch_nso.py` 的分析部分（跳转表、自旋等待、函数匹配补丁）原样用，只换"线程槽怎么取"和输出格式。
   - 线程槽改走 `tpidr_el0`，规则搬自 `d2r_ohos.py`。
   - x18 的改写保留；鸿蒙不会动 x18，保留它也无害。
   - 输出改为由 OHOS clang 链接的 ELF `.so`，放进签名 HAP（同 iOS 把代码嵌进可执行文件的思路）。
3. **图形回放 `nvn_vk`**：从现有鸿蒙 Vulkan 后端移植。
   - 接口层用 nxnative 的 `hle_nvn.c`，回放层改成吃它的记录格式。
   - 再逐项补上 nxnative 回放里有、我们没有的功能：`tex_fit` 式视图适配（2D / 数组 / cube 互转，零 cube / 零 3D 占位）、模板视图、驱动段 base instance（`c0+0x644`）、图像原子、间接多重绘制、遮挡查询、条件渲染。
   - 设备差异集中在一个"设备事实"表里：无顶点阶段存储、无 `shaderOutputLayer`、R16G16_SNORM 不可渲染等。
4. **着色器链**：`dmsh_build.py` 加一个 Vulkan 目标：翻译器出 SPIR-V 后做 Vulkan 反射，打包成 seed.vkpack，不再走 MSL / metallib。
   - 翻译器用 nxnative 的 `dmsh_xlate`（它能从指令推断纹理类型），在 Docker 里构建 linux-x64 版，或直接用 Windows 版 .NET。
   - `shader_facts.tsv`、`shader_pairs.tsv` 直接用上游的。
5. **前端**：从 NativeGames-harmonyos / LZA 搬 ArkTS 应用。启动器和设置页改为由 nxnative 的 `settings.tsv` 生成，与 Mac / iOS 同一份定义。
6. **以 Vulkan / GL 渲染的游戏**（`dm/gfx/vk`、`gl`，如 Hades）：在鸿蒙上，客体 Vulkan 几乎能直通宿主 Vulkan。排在后面做。
7. **构建驱动 `build_hap_from_nsp.py`（Windows）**：
   - 解包：`nx_extract`，Python。
   - 档案：上游的 survey / profile。
   - 改写：Python 加 capstone。
   - 编译主机和游戏 `.so`：DevEco 的 `clang.exe`。
   - 着色器：Docker，或 Windows 版 .NET。
   - 准备 RomFS。
   - hvigor 打包，用玩家自己的签名。
   - hdc 安装，RomFS 推进沙箱，大游戏不放进 HAP（同现在）。

## 3. 和上游怎么相处

- 新建私有仓库（暂定 `nxnative-harmonyos`），用固定提交引入 nxnative（submodule 或 subtree），平台层全部放在 `platform/ohos/`。（2026-11-18 注，0.1.2：时之笛后端用到的 nxnative 没有做成 submodule，它的固定提交记在 `backends/SOURCES.tsv`，用时检出到 `backends/nxnative` 或本仓库旁边。）
- 对上游文件的少量必要修改做成补丁，或用编译期 overlay，与现在 totk-android overlay 的做法一样。这样跟进上游是"升级固定提交 → 重打补丁 → 跑回归"，不是手工合并。
- 档案、补丁表、着色器表一律读上游的，不复制。新游戏先看上游有没有档案，有就直接构建。

## 4. 鸿蒙上的约束与风险

| 项 | 影响 | 对策 |
|---|---|---|
| 代码签名、无 JIT | 运行时不能生成代码；需要 JIT 的游戏不支持 | 一个游戏一个签名 HAP，所有代码离线生成 |
| GPU（Maleoon） | nxnative 为 Metal 做的功能，每项都要有 Vulkan 版；Maleoon 有若干缺口 | 设备事实表，加上每项功能先在 lavapipe 上验证（Z-A 已用过这套方法） |
| 性能 | Z-A 现在受 CPU 限制：主线程每帧约 39 ms 花在提交和出图上 | 回放层保持精简：记录流只扫一遍，描述符和管线都缓存；性能单独立项 |
| 内存 | 部分游戏要 5 GB 地址布局 | 沿用现有鸿蒙内存池；低内存手机标为不支持 |
| 上游变化快 | nxnative 天天在改 | 固定提交 + 补丁 + 回归（截图、pass dump、日志对比） |
| 构建平台 | 上游脚本假定 macOS | 鸿蒙驱动单独写，只调用上游的 Python 和 C# 工具 |

## 5. 分阶段（每阶段先离线验证）

**A 段：统一入口，覆盖本地已适配的两个游戏**

| 阶段 | 内容 | 验证 |
|---|---|---|
| A1 入口和验证表 | `build_hap_from_nsp.py`：扫描文件夹里的 `.nsp/.nsz/.xci/.xcz/.nup`，识别 title、版本和 build ID（NUP 不需要密钥），查鸿蒙验证表；已适配的交给对应后端，否则生成调查报告并停下 | 两个游戏各用现有输入跑一遍，产物与现在的手工流程逐字节一致（改写模块、runtime tables、种子） |
| A2 输入格式统一 | 解包统一用 nxnative 的 `nx_extract`（LibHac）和 `nup_extract.py`；后端只接已解开、已核对哈希的 ExeFS / RomFS，不再关心是 xci 还是 nsz | TOTK 用 nsp 或 xcz 替代原来的 xci 加 nsz，校验哈希与 `expected-v1.4.3.json` 一致 |
| A3 Z-A 后端补齐 | Z-A 也做成一条命令的准备流程（照 `prepare_v143.py`）；种子要先在手机上用游戏自带的 Oodle 解包，这一步自动化：先装包，用 `ZA_SHADER_DUMP` 启动，拉回 `bnsh.pack`，再执行 `za_seed.py`、`za_reseed.py`，最后推送 | 从空的 `prepared/` 出发，一条命令到能进游戏 |
| A4 图形前端 | NX2Harmony：引导（电脑、手机、签名、密钥、游戏文件夹）、游戏库、构建页和进度、完成后在手机上打开（第 7 节）；构建脚本加 `--progress json`，事件格式与 nxnative 相同 | 从全新 Windows 环境点完引导，一次构建装上 Z-A，进游戏 |
| A5 两个端口合并 | 两个端口合成一棵源码树：鸿蒙平台层只有一份，游戏差异留在 `#ifdef TOTK_GAME_ZA` 和各自的游戏目录；包名、启动器按游戏区分 | TOTK 回归：现有路线、截图、帧率都不比 0.5.0 差 |

**B 段：nxnative 通用内核**

| 阶段 | 内容 | 验证 |
|---|---|---|
| P0 骨架 | 新仓库，引入上游；Windows 构建驱动跑通"解包 → 改写 → 编出 `.so`"（先不运行） | 上游的 Python 单元测试在 Windows 上通过；同一 NSO 的改写结果与 Mac 输出比对，只有线程槽相关指令不同 |
| P1 主机启动 | OS 兼容层加最小前端，NVN 只记录不回放，让一个游戏跑到 `nnMain` 和第一次提交 | 日志里的 HLE 调用序列与 nxnative 在 Mac 上的同一启动段对照 |
| P2 Vulkan 回放 | `nvn_vk` 吃 nxnative 记录流；先拿 TOTK 和 Z-A 回归 | 与现有两个鸿蒙端口同场景比截图和 pass dump；不出现新的 RED 行 |
| P3 着色器链 | `dmsh_build.py` 的 Vulkan 目标，seed 打包 | 与现有 Z-A 种子逐字节比对（同翻译器时） |
| P4 前端与替换 | 设置、存档、手柄、音频齐全后，新 HAP 取代两个旧端口 | 两个游戏的现有路线全部走通 |
| P5 第三个游戏 | 选一个在 nxnative 上功能需求少的游戏（如 Metroid Dread、Mario Wonder），检验"加游戏 ≈ 加档案" | 不改平台层代码就能进游戏，或者需要改的地方都可以写成通用功能 |

P0 到 P1 主要是搬运和适配，风险低。P2 工作量最大。现有两个端口在 P4 完成前保持不动。

## 6. 玩家视角：导入游戏文件，一条命令出包

最终用法与 nxnative 的 `build_ipa_from_nsp.py` 相同，只是在 Windows 上运行，产物是 HAP：

```
python platform/ohos/build/build_hap_from_nsp.py D:\Games\ZA -k D:\keys\prod.keys
```

（B 段的设想，尚未实现；A 段现在的入口是 `nx2h.py build`，见 README。）

- **输入**：游戏文件夹（本体、更新、DLC；`.nsp` / `.nsz` / `.xci` / `.xcz`）和自己的 `prod.keys`。解包用上游的 `nx_extract`（.NET + LibHac，Windows 上能直接编译），nsz / xcz 的解压用 Python 的 `nsz`。
- **流程**：
  1. 识别游戏和版本（build ID）并选档案；
  2. 改写，编译主机和游戏 `.so`；
  3. 翻译着色器，打包 seed.vkpack；
  4. 准备 RomFS；
  5. hvigor 打包，用玩家自己在 DevEco 配好的签名（脚本只读不打印）；
  6. 用 hdc 安装到已连接的手机，并推送 RomFS 和种子。

  首次构建以着色器翻译为主（Z-A 约 1 小时），推送 RomFS 按约 28 MB/s 算（Z-A 5.6 GB 约 3–4 分钟）；输入不变时再构建只需重新打包。
- **按版本分三档**（与 nxnative 相同）：

| 档 | 条件 | 结果 |
|---|---|---|
| 已适配 | 鸿蒙验证表里有这个（游戏、build ID），也就是我们本地做完的版本。目前有三个：TOTK 1.4.3（模块哈希见 `expected-v1.4.3.json`），时之笛 v131072，Z-A 2.0.2（`main` `b1f12fd9…`、`sdk` `df7a3960…`） | 一条命令出可玩的包，结果与现在两个端口相同 |
| 有档案、版本未登记 | 例如另一个更新版本 | 由上游档案派生，补丁靠函数指纹匹配（`MATCH_CUTS`），尽力而为；脚本用红字列出没套上的补丁（`<module>.match.tsv`） |
| 没有档案 | 新游戏 | 生成调查报告（survey）和临时档案，能出包但多半进不去；之后按 nxnative 的 `docs/agents` 流程，由 AI 辅助逐步适配（解包 → 调查 → 主机启动 → 图形 → 调试） |

- **鸿蒙验证表 `games.tsv`**：nxnative 在 Mac / iPhone 上测过，不等于鸿蒙上能玩。原因有三：Vulkan 回放要覆盖该游戏用到的功能，Maleoon 有缺口，内存和性能也不同。所以"已适配"只认这张表：按（游戏、build ID）记录后端（A 段是哪条现有流水线，B 段是通用内核）和状态（已验证：玩通过 / 优化中：能构建能玩、还在调 / 适配中：不能构建）。现有王国之泪 1.4.3（已验证）、时之笛 v131072 和 Z-A 2.0.2（优化中）三行。构建开始时脚本先报出这一项。以后每适配完一个版本就加一行。
- **与苹果的差别**：
  - 签名必须是玩家自己的鸿蒙签名，脚本没法代办：首次在 DevEco 里配一次，用自动签名或自己的证书。
  - 手机要开开发者模式并连上电脑。
  - RomFS 不放进 HAP，推到应用沙箱，相当于 nxnative 的 `--romfs separate`。
  - 以后可以加"在手机上从文件管理器导入游戏文件"，像 totk-android 那样在设备上准备，作为可选路径。

## 7. 图形前端（对标 NX2Apple）

NX2Apple 是社区项目，建在 nxnative 之上：它调用 `build_ipa_from_nsp.py --progress json`，读取逐行输出的 JSON 事件（`stage` / `step` / `progress` / `stage_end` / `game` / `say`，约定见 nxnative `docs/frontends.md`），画成游戏库、引导和构建进度。鸿蒙版做法一样：**构建脚本输出同一套事件**，Windows 上的图形前端（暂名 NX2Harmony）只负责画界面和调用脚本，不直接碰构建细节。

| NX2Apple 的步骤（视频 3:00 起） | 鸿蒙对应 |
|---|---|
| 游戏库：封面、"构建游戏"、你的游戏（已构建 / 可用于 iPhone） | 同样的游戏库；状态取自鸿蒙验证表（已适配 / 未适配），再加"已装到手机"（hdc 查询） |
| 引导 1"准备开始游戏"：Apple 芯片的 Mac、游戏文件和 prod.keys、磁盘空间 | Windows 10/11 x64、游戏文件和 prod.keys、磁盘空间（TOTK 约 80 GB）、一台鸿蒙手机 |
| 引导 2"你想在哪里玩"：Mac / iPhone / 两者 | 只有鸿蒙手机（以后可加平板），这一步可省 |
| 引导 3"准备好你的 Mac"：Xcode 检查，Python、.NET、构建工具一键装 | "准备好你的电脑"：检测 DevEco Studio 和 SDK（需玩家自己下载登录，前端给出入口）；Docker Desktop、Python、Git、PowerShell 7 用 winget 一键装；Docker 镜像在后台拉取 |
| —— | 新增"连接手机"：`hdc list targets` 找到手机，引导打开开发者模式和 USB 调试，显示型号、系统版本、剩余空间 |
| —— | 新增"签名"：检测是否已配签名；没配就引导在 DevEco 里自动签名一次（华为账号由玩家自己登录）；前端只判断有没有，不显示内容 |
| 引导 4"添加你的密钥"：自动找到 `~/.switch/prod.keys` | 自动找 `%USERPROFILE%\.switch\prod.keys`，或拖进来；只核对键名是否齐全，不显示内容 |
| 构建页：游戏文件夹、本体 / 更新 / DLC 自动识别、保存到（兼作构建缓存）、在哪里玩、安装到设备 | 同样一页；保存位置兼作缓存（现有流水线本来就能断点续跑）；"安装到手机"默认打开，也可以"只导出 HAP" |
| 构建进度：build nx_extract → extract → rewrite → shader split → iOS shaders → app build → IPA pack → 打包 romfs | 解包 → 合并 RomFS → 补丁 → 字体 → 翻译器 → 改写 → 客体 → 着色器种子（Z-A 中途自动在手机上解包一次）→ 原生库 → HAP 打包签名审计 → 安装 → 推送游戏数据（按字节显示进度） |
| 完成后三步手动装：AltStore 装 IPA、用"文件"把 romfs 拷进 Documents、启动 | **自动完成**：hdc 安装，再把数据推进应用沙箱，完成后直接"在手机上打开"。只导出 HAP 时，给出"推送数据"按钮 |
| 手机上的启动页：Play、恢复测试过的默认设置、云存档、内存上限 | 已有 ArkTS 启动页（设置、存档）；改为由 `launcher.json` 式设置表生成，加"恢复测试过的默认设置" |

鸿蒙比 iPhone 方便的地方：装包和推数据全自动，不用侧载工具，也不用手动拷文件。

麻烦的地方：
- 签名要玩家在 DevEco 里配一次；
- Windows 上要 Docker；
- 首次构建慢：TOTK 的种子约 2–3 分钟（子项目的 build_seed.py：按这台电脑 Docker 的 CPU 和空闲内存开多个容器，中间文件留在 Docker 卷里；旧的 build_seed.sh 单容器、约 19 万个文件经绑定挂载写出，要 55 分钟，seed.vkpack 逐字节相同），Z-A 约 1 小时加手机端解包 1 分钟。

**要装的东西**（按两边仓库实际检查的项）：

| | nxnative（Mac） | 鸿蒙现状（Windows） | 鸿蒙可精简到 |
|---|---|---|---|
| 大件开发工具 | Xcode 27.2（指定版本），另需下载 Metal 工具链；出 iPhone 包还要装 iOS 平台组件 | DevEco Studio + HarmonyOS SDK（API 26） | 不变 |
| 包管理 | Homebrew | winget（系统自带） | 不变 |
| Python | 3.12+，必须 arm64 版（Intel 版会让翻译器跑错架构）；pip 装 lz4、capstone 5.x、fonttools、nsz、zbic（3.14 以下另加 backports.zstd） | 3.12+ | 3.12+，pip 装同一组包 |
| .NET | .NET SDK 10（翻译器和 nx_extract 要用） | 装在 Docker 镜像里 | Windows 版 .NET SDK 10 |
| 其他工具 | xcodegen（仅 iPhone 包）、zstd、glslang（OpenGL 游戏才用，但每次都检查）、make（随 Xcode） | Docker Desktop（WSL 2）；时之笛另要 zstd（2026-11-16 起不再要 Git Bash；PowerShell 7 只在没有预编译库、要自己编 libopus 时用） | 去掉：容器里的步骤改成 Windows 原生（Python、.NET，再加 DevEco 自带的 LLVM 工具），bash 和 ps1 脚本改写成 Python |
| 磁盘 | 游戏大小 × 2 + 10 GB | TOTK 约 80 GB，外加 Docker 磁盘镜像 | 少了 Docker 磁盘镜像 |

两边一样多：都是"一个大开发工具 + Python + .NET + 几个小工具"。NX2Apple 的"一键安装"只是把这些检查和安装做成了按钮，Xcode 仍要玩家自己装（Apple 账号），对应我们这边玩家自己装 DevEco（华为账号）。精简到最后一列，可以作为 A2 的一部分做。去掉 Docker 后，引导里就只剩"DevEco + Python + .NET"三项。

前端技术：PySide6（Qt）桌面程序，用 PyInstaller 打包成便携文件夹，不另装 Node 或 Electron；构建脚本照旧是 Python。

## 8. 软件里要放的东西（对比 Mac）

nxnative 一个仓库就包含所有游戏需要的东西：档案、改写器、主机、图形层、着色器链、打包脚本。玩家只需要准备 Xcode 和游戏文件。NX2Harmony 要做到同样完整，按环节对照如下（"现在"指当前状态）：

| 环节 | Mac（nxnative + NX2Apple） | NX2Harmony 现在 | 还要放进软件的 |
|---|---|---|---|
| 识别游戏文件 | NSP / NSZ / XCI / XCZ / NUP / 导出文件夹；nx_extract（LibHac）解包 | NSP / NSZ / XCI / XCZ / NUP；不认导出文件夹 | 导出文件夹（`base\` / `updates\<版本>\` / `dlc\`，title ID 从 main.npdm 读） |
| 每个游戏的适配 | `dm/games/<id>/`：档案、补丁、着色器表 | `games.tsv` 只记状态；三个后端（王国之泪：NativeGames-harmonyos；Z-A：LZA；时之笛：OOT）各在自己的工程里 | 两个游戏的后端收进本仓库（A5），软件自带：TOTK 的 `prepare_v143` 流水线、Z-A 的整套脚本（2026-11-18 注，0.1.2：三个后端的源码已作为导入的副本收进 `backends/`；A5 说的合成一棵源码树还没做） |
| 改写游戏代码 | `patch_nso.py`，在 Mac 上运行 | TOTK：`rewrite_ohos.py`；Z-A：`rewrite_za.py`（都在 Docker 里运行） | 同上，随后端一起收进来；Docker 镜像由软件自动拉取或构建 |
| 系统字体 | 由主机提供 | TOTK 从本体卡带取；**Z-A 的 NUP 不带字体**，现在是拷 TOTK 取出的那份 | 字体单独作为一项："从卡带 / 固件取一次，所有游戏共用"，放在 `data\`；Z-A 构建时没有字体就提示去哪里取 |
| 着色器 | 每个游戏离线翻译成着色器库，随包带走；缺了在设备上现场编译 | TOTK：完整种子（首次约 3 分钟）；Z-A：要先在手机上用游戏自带的 Oodle 解包一次，再在 PC 上翻译，缺的边玩边补 | Z-A 解包自动化（装包 → 解包模式启动 → 拉回 → 翻译 → 推送）；构建页加"补着色器"：拉回手机上的缺失请求，翻译后推回 |
| 打包 | Xcode 出 IPA / Mac App，图标来自游戏文件 | hvigor 出 HAP（玩家自己的签名），图标来自游戏文件或 eShop（已做） | — |
| 包名 | 每个游戏一个 bundle id | 每个游戏自动生成，见下文"包名和签名"（已做） | — |
| 安装和游戏数据 | 侧载 IPA，RomFS 打进包里或手动拷进去 | hdc 自动安装、推送数据，手机上已有的同一份会跳过（三个游戏都已接上，真机验证过） | — |
| 游戏设置 | 由 `settings.tsv` 生成 `launcher.json`，App 内设置页，"恢复测试过的默认设置" | 手机端 ArkTS 设置页（TOTK 那套，Z-A 是复制过来的） | 按游戏区分的设置表；电脑端可以不管，由手机 App 读 |
| 存档 | 存在 App 里，iCloud 云存档，覆盖前留备份 | 存在 App 沙箱里；换签名重装会清掉 | "备份 / 恢复存档"（hdc 拉回电脑，按游戏、时间存放），装包前提示先备份 |
| 出问题时 | `bug_report.py` 收集日志 | 日志只在电脑端 `data\logs` | "导出诊断"：拉回手机上的运行日志和崩溃记录，和电脑端日志一起打包 |
| 环境 | 引导：Xcode、Python、.NET、构建工具一键装 | "环境"页：Python、Git、Docker、DevEco、签名、手机、密钥、磁盘（已做） | Z-A 额外依赖（nup 读取已内置，不需要额外工具） |
| 新游戏 | 没有档案的游戏先生成调查报告和临时档案，再由 AI 辅助适配 | 显示"未适配" | B 段：nxnative 通用内核 |

### 包名和签名（2026-11-09 已做）

- **规则**（DevEco SDK `configcheck/configSchema_rich.json` 的 `app.bundleName`）：7–127 字节，反向域名，每段以字母开头，只含字母、数字、下划线。
- **生成**（`nx2h/bundle.py`）：`nx2h.<本机>.<游戏简称>`，例如 `nx2h.uk3f9a2.totk`。游戏简称是 `games.tsv` 的 slug 列；"本机"是 `u` 加 Windows MachineGuid 哈希的 6 位 36 进制，源码运行和打包版得到同一个名字，不同玩家也不会撞名（华为账号签名时会登记包名）。生成后记在 `nx2h.local.json` 的 `bundles` 里，之后不再变：手机上的 App、存档、签名都绑在这个名字上。两个游戏简称相同时，第二个加数字。
- **不沿用**后端仓库里写死的包名（TotZ / LZA），每个游戏都用生成的名字。
- **签名**（`nx2h/signing.py`）：DevEco 自动签名生成的 `.p7b` 只对一个包名有效，`bundle-info.bundle-name` 写着它。软件扫描后端工程和签名工程的 `build-profile.json5`，找出签这个包名、还没过期的那份配置。配置只在进程里读，不打印，也不拷出本机。
- **后台签名**（`nx2h/agc.py`，默认方式，不用打开 DevEco）：做法照 DevEco Studio 26.0.0.621 的自动签名（`login-*.jar`、`hos-project-mgmt-*.jar` 和 `messages/SignatureMgmt.properties`，服务地址取自它的 GRS 缓存 `caches/grs.json`）：
  1. 登录：本机在 `127.0.0.1:<端口>/callback` 等回调，浏览器打开 `https://cn.devecostudio.huawei.com/console/DevEcoIDE/apply?port=…&appid=1007&code=…`，玩家登录华为账号。随后 `temptoken/check` 换到 JWT，`jwToken/check`（请求头带 jwtToken）换到 accessToken，再从 `user-team-list` 取团队。
  2. 证书：用 DevEco 自带 JDK 的 keytool 生成 EC P-256 密钥和 CSR，`cert/add` 申请调试证书。这台电脑的所有游戏共用一把密钥、一张证书。
  3. 设备：手机连着时，`bm get --udid` 取 UDID，不在账号里就 `device/add`。
  4. Profile：`ide/test/provision/add` 为这个包名申请调试 Profile，带上账号里的全部设备，不带 ACL 权限（与 DevEco 签出来的一致）。下载地址由 `objects/url/reapply` 给出；换新的时删掉这个包名的旧 Profile。
  5. 给 hvigor 用：`build-profile.json5` 的 material 指向这里的 .p12 / .cer / .p7b。两个密码按 hvigor-ohos-plugin `utils/decipher-util.js` 的格式加密（AES-128-GCM，工作密钥由旁边 `material/` 的根密钥封存），`material/` 是软件自己生成的，不用 DevEco 的。已用 hvigor 自己的解密代码验证能解开。
  6. 存放：都在 `%USERPROFILE%\.nx2h`。`account.dat` 和 `keys\key.dat` 用 Windows DPAPI 加密，只有这个 Windows 用户能打开。
  - 构建时，包名没有签名、或签名里没有连着的手机，就自动走这一步。失败时退回下面的 DevEco 方式。
- **DevEco 方式（备用）**：软件生成一个签名工程，放在 `%USERPROFILE%\.nx2h\signing\NX2H_<简称>`，并自动用 DevEco Studio 打开。工程里是后端的 AppScope 和 entry 配置，用同一份 module.json5，所以签出来的名带同样的权限；不含游戏代码和数据。玩家登录华为账号，在 Project Structure → Signing Configs 里勾选自动签名，之后的构建就能找到这份签名。
- **构建时替换**：打 HAP 前，把后端的 `AppScope/app.json5` 包名和 `build-profile.json5` 签名换成这个游戏的，打完换回。原件先挪到后端已忽略的 `prepared/nx2h-stage/signing/`；如果构建中途被停掉，下次构建开始前也会先换回。已验证：用另一个工程的签名配置打 TOTK 的 HAP，hvigor 正常签名，`module.json` 里是换上的包名，换回后后端两个文件逐字节相同。
- **后端跟着包名走**：两个后端都改成运行时读自己的包名。ArkTS 的 `CrashDiagnostics.ets` 用 `bundleManager.getBundleInfoForSelfSync`；原生的 `totk_napi.c` 用 `OH_NativeBundle_GetCurrentApplicationInfo`，链接 `libbundle_ndk.z.so`。脚本先读环境变量 `TOTK_OHOS_BUNDLE`，软件运行后端脚本时会设置它。

### Z-A 后端（2026-11-09 已做，`nx2h/backends/za_202.py`）

按 LZA 当时手工跑的顺序串起来，每一步产出已在就跳过：
- 准备：解 NUP（只解 2.0.2 更新部分）→ Docker 里改写 main / sdk → 运行时表 → 打包 RomFS → 系统字体（`nx2h/fonts.py`，卡带或固件）→ 着色器翻译器 → 客体库 → 游戏数据 → libopus / libvpx（有王国之泪后端的就直接复制）。
- 宽屏（2026-11-09 同步）：客体库这一步跑 `prepare_guest.py za202`（2026-11-16 由 `prepare_guest.sh` 改写成 Python，电脑上不再需要 Git Bash）。先用 `export_text` 把 main 连同游戏补丁（`overlay/runtime/game_patches_za.c` 的宽屏相机，经 `aspect_patch.h` 的 `ldr q` 补丁点）导出到 `prepared/za/x0/main.x0.bin`，再生成客体库；客体库里记着导出代码的 SHA-256。以下任一情况会重新导出和生成：补丁源码比导出新，或导出比客体库新。已验证：经软件重跑的导出和客体库，与宽屏版逐字节相同。外壳"画面比例"（全面屏 / 16:9）用 `V_WIDE` 标记，对王国之泪 1.4.3 和宝可梦 2.0.2 都显示。
- 之后：原生库 → HAP（包名 `nx2h.<本机>.za`）→ 安装 → 推送。推送的文件与 `za/push_za.ps1` 相同。
- 着色器种子：第一次装到手机后自动生成。
  1. 写 `env-dev-*.txt`，让 App 带 `ZA_SHADER_DUMP` 启动，用游戏自带的 Oodle 解包着色器，写完自己退出。
  2. `za_seed.py --pull --send` 拉回电脑、翻译并推回种子。
  3. 再用 `ZA_SHADER_DUMP_REMOVE` 删掉手机上 7.5 GB 的解包文件。
  - SPIRV-Cross 按 `build_seed.sh` 钉的提交在容器里编一次。
- 两个后端共用 `nx2h/backends/common.py`（HAP、包名、签名、图标、安装、推送），每个游戏只写自己的输入和步骤。
- 离线已验证：这台电脑上全部准备步骤都跳过；原生库重新链接了 `libbundle_ndk`；以 `nx2h.<本机>.za` 打出的 HAP 通过 `verify-app`；王国之泪重构后也照常出包。
- 真机（2026-11-09，SGU-AL10）：两个游戏都以新包名装上手机并推完数据（Z-A 13 个文件约 7.8 GB，王国之泪 17.3 GB）。Z-A 已确认可玩。装机时修了三处：
  - App 正常启动只打开启动页，要带 `--ps command start` 才会启动原生运行时并建游戏文件夹（hdc 的 shell 用户没有权限在沙箱里建文件夹）；
  - 王国之泪的补丁子文件夹由运行时按 `manifest.tsv` 建，所以清单要先推，文件夹不存在时重启一次运行时再发；
  - Z-A 用的是分层图标（`$media:layered_image`），现在按 `app.json5` 和 `module.json5` 实际引用的图标资源来替换：分层图标的背景换成游戏图标、前景换成透明图，启动页图标也一起换。
- 未验证：手机上的着色器解包流程。这台电脑已经有种子，所以这次没有走到这一步。

优先级：
1. ~~Z-A 一键流程，含系统字体和自动解包（A3）~~ 已做，待真机验证。
2. 两个后端收进本仓库（A5），软件不再依赖另外两个仓库。（2026-11-18 注，0.1.2：部分做了，三个后端的源码作为导入的副本放在 `backends/`，用 `packaging/backend_import.py` 更新；合成一棵源码树还没做。）
3. 存档备份和恢复。
4. 补着色器、导出诊断。
5. 支持导出文件夹。
6. ~~真机验证安装和推送~~ 已做。

## 9. 需要先定的事

1. A 段就在本仓库里做：把 NativeGames-harmonyos 和 LZA 合进来，两个原仓库保留不动，作为对照。（2026-11-18 注，0.1.2：三个后端（连同 OOT）的源码已收进 `backends/`；原工程照旧保留，游戏本身的工作仍在那边做，再导入。）
2. 上游的引入方式：submodule 加补丁（推荐，跟进最省事），还是直接 fork 改。
3. B 段的 P1 / P2 先做哪个游戏：Z-A（离线验证工具链最新）还是 TOTK（鸿蒙上做得最久）。
