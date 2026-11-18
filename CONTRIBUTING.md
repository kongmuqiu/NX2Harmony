# Contributing

Help is welcome. Below: how the pieces of NX2Harmony fit together and what a pull request has to include. You can help without writing code: when a build stops, open an issue with the "Build problem" form. Say which game and version, the Windows version, which phone and which HarmonyOS version, and paste the last red line and the step's log, with your user name, the phone's serial, your Huawei account and team name, e-mail addresses and the middle part of the bundle name (nx2h.<this part>.totk) replaced by `***`.

[中文说明在下面](#参与贡献)

## Legal: nothing from any game

- Keep everything that comes from a game, or was made from one, out of commits: keys and firmware, saves, translated shaders and built libraries, the game's code and RomFS files, its text, audio, artwork and icons. The same goes for what a build produces (HAPs, the full package, `prepared/`, `artifacts/`, `work/`, `logs/`, `dist/`), for signing files (`.p12`, `.cer`, `.p7b`, a project's `build-profile.json5` with its signing configuration, anything from `%USERPROFILE%\.nx2h`) and for this PC's settings (`nx2h.local.json`).
- Run `git config core.hooksPath .githooks` in every new clone. `.githooks/pre-commit` then refuses a commit that stages what it can recognize: by name (game files, extracted `romfs/` / `exefs/` folders and other game data, saves, keys, built packages, libraries and archives, signing files, this PC's settings, anything under `prepared/`, `artifacts/`, `signing/`, `work/`, `logs/`, `dist/` or `build/`), by size (any file over 20 MB) or by content (console key lines, as in `prod.keys`). It lets through two kinds of file under `backends/`: a backend's module build file `entry/build-profile.json5` while it holds no signing, and the prebuilt third-party libraries in `backends/prebuilt-ohos`. It runs with `python` or `python3`, whichever starts. Anything it cannot tell, such as a game picture saved under an ordinary name, is up to you. Never skip it with `--no-verify`.
- No details of your own machine in code, docs, comments, logs, screenshots or examples: user names, user folders and other absolute paths of your PC, IP addresses, phone serials, Huawei account or team names, e-mail addresses, bundle names made on your PC. Use generic examples such as `C:\Games`, `D:\keys` and `nx2h.uk3f9a2.totk`.
- This project gives no help in getting games, keys or firmware and links to no place that offers them. Write about game files only as the player's own dumps from their own console.
- Code you contribute must be your own and is accepted under MIT. Someone else's code is used under its own license: say in `THIRD_PARTY_NOTICES.md` where it came from and which license it carries, and when the packed app ships it, have `packaging/build.py` put its license file in `licenses\` beside the exe (`COMMITTED` and `packaging/licenses/` for a text kept here, `pairs` or `PACKAGES` for one taken from the build).

## NX2Harmony only brings the backends together

- NX2Harmony is the front door: it recognizes files, checks the PC and the phone, makes bundle names, signs, packs, installs, pushes the data and drives each game backend's own pipeline. Work on a game itself (rewriting its code, game patches, shaders and the shader seed, picture and frame rate, a crash inside the game) belongs to that game's backend project (NativeGames-harmonyos, LZA, OOT, or upstream in nxnative / totk-android) and is fixed there, then imported into `backends/` with `packaging/backend_import.py`; this repository picks up the result. A game fix made only in `backends/` is a patch that has to be carried forward on every import; NX2's own changes to a backend (what the app, the clean-PC flow and the release need) are kept as patches in `backends/patches/` on the maintainer's PC (not committed) for the backend projects to take over ([backends/README.md](backends/README.md)).
- What stays here: the app and the command line (`nx2h/`), one thin adapter per game version (`nx2h/backends/`: which inputs, which steps in which order, what goes to the phone), the imported copies of the backends (`backends/`), the packaging (`packaging/`) and the documentation.
- A new game or version is first made playable on a phone in its backend project and imported into `backends/`. Then it gets a row in `games.tsv` and an adapter in `nx2h/backends/`, registered in `nx2h/backends/__init__.py` (`BACKENDS`, `MARKERS` and an explicit import in `load()`, so that PyInstaller packs it) and in `DEFAULT_BACKENDS` in `nx2h/config.py`. Its status says only what was seen on a phone: 已验证 Verified (played through), 优化中 Being optimized (plays, still being tuned), 适配中 Being adapted (shown, not built).
- A route that has not been tested on a fresh PC or a phone stays switched off until it has been; `nx2h/mirrors.py` (`ENABLED = False`) is the example.

## Checks stay as strict as they are

- The backends check your files against known hashes, and the HAP builder audits every entry of the package. `packaging/full_bundle.py` refuses files that would identify the PC it runs on and pictures not listed in `packaging/bundle_pictures.tsv`. None of these (a hash, the audit, that gate, `.gitignore`, the hook, the picture list) is ever edited to make an error go away. When a hash does not match, the dump is damaged or comes from a different release of the game.
- A picture goes into `packaging/bundle_pictures.tsv` only after someone has looked at it and found neutral default art, never a game's or a company's.
- Keys and signing files are only looked for, never opened for their values, printed, logged or copied off the PC. Keep it that way.

## Repository layout

- `nx2h/`: the desktop app (`app.py`, Qt; `core.py`, its logic without drawing), the command line (`cli.py`), recognizing files (`scan.py`), the environment checks (`doctor.py`), the phone (`device.py`), bundle names and signing (`bundle.py`, `signing.py`, `agc.py`), icons and system fonts (`icon.py`, `fonts.py`), settings and paths (`config.py`), progress events (`events.py`), starting other programs (`proc.py`).
- `nx2h/backends/`: one adapter per adapted version; `common.py` holds what all of them do the same way.
- `nx2h/vendor/`: code from other projects (totk-android's game-file readers in `nx2h/vendor/switchfs`), changed as little as possible, each with its license.
- `backends/`: the game backends' sources, imported from their own projects, with prebuilt libopus / libvpx (`prebuilt-ohos`) and `backends/SOURCES.tsv` (where each folder comes from; the pins of totk-android and nxnative, which are fetched, not copied). `backends/README.md` says how to update one.
- `packaging/`: `build.py` and `packaging/NX2Harmony.spec` pack the app; `full_bundle.py` makes the full package from the committed `backends/`; `backend_import.py` imports or updates a backend; `bundle_pictures.tsv` lists the pictures the package may carry; `check_no_windows.py` checks that the packed app opens no console window.
- `tests/`: checks that need no game files, keys, phone or network.
- `docs/`: `docs/install.md`, `docs/how-it-works.md` and their Chinese versions `docs/install.zh-CN.md`, `docs/how-it-works.zh-CN.md`; `docs/DESIGN.md` is the design history, in Chinese.
- `games.tsv`: the HarmonyOS verification table (Verified and Being optimized versions). `nx2h.py`, `NX2Harmony.cmd`, `一键安装环境.cmd` and `一键构建.cmd` at the root are the entries.

## Code

- Keep Python 3.9-compatible: no `match`, no `X | Y` types evaluated at run time (in annotations they are fine when the file starts with `from __future__ import annotations`), no backslashes inside f-string expressions. The tests parse every file with Python 3.9's grammar, which does not catch all of this; run with a 3.9 interpreter when you can.
- Start external programs through `nx2h/proc.py` (or, as `app.py`'s build runner does, with `CREATE_NO_WINDOW`), so no console window flashes up.
- `nx2h/` uses the standard library plus PySide6 (the window) and cryptography (signing). Add a dependency only when there is no other way, in `requirements.txt` and `packaging/requirements-build.txt`.
- What the player reads (the window, the command line's messages) is in Chinese; code, comments and commit messages are in English.
- The `.cmd` files keep CRLF line ends (`.gitattributes`).

## Pull requests

- **Keep a pull request to one subject.** Leave out debugging leftovers, mass reformatting and anything unrelated.
- **Run the tests** from the repository root: `python -m unittest discover -s tests`. The GitHub Actions workflow runs this command too.
- **Testing notes:** state how far you got (the HAP built, installed on a phone, or the game played), with the game and its version, the phone model and its HarmonyOS version. A change to building, signing, installing or pushing also needs one real build and install, not only the tests.
- **A change players will notice** gets a line under "What's new" in `README.md`, in English and in Chinese, and in `CHANGELOG.md`.
- **Commit messages** take the form `<type>: <description>`; the types are feat, fix, refactor, docs, test, chore, perf and ci.

---

# 参与贡献

欢迎参与。下面说明 NX2Harmony 各部分怎么分工，以及 pull request 需要满足什么。不写代码也能帮忙：构建停下时，用“构建问题”表单开一个 issue，写明游戏和版本、Windows 版本、手机型号和 HarmonyOS 版本，贴上最后一行红字和那一步的日志；先把里面的用户名、手机序列号、华为账号和团队名、邮箱，以及包名中间那段（nx2h.<这一段>.totk）换成 `***`。

## 法律：不收任何游戏的东西

- 来自游戏或由游戏做出来的东西一律不进提交：密钥和固件、存档、翻译好的着色器和编出的库、游戏代码和 RomFS 文件、游戏的文字、音频、美术和图标。构建产生的东西（HAP、完整包、`prepared/`、`artifacts/`、`work/`、`logs/`、`dist/`）、签名文件（`.p12`、`.cer`、`.p7b`、工程根目录下带签名配置的 `build-profile.json5`、`%USERPROFILE%\.nx2h` 里的任何东西）和本机设置（`nx2h.local.json`）同样不进。
- 每次新克隆后执行 `git config core.hooksPath .githooks`，之后 `.githooks/pre-commit` 会拒绝暂存了它能认出来的东西的提交：按名字认（游戏文件、解出的 `romfs/` / `exefs/` 文件夹和其他游戏数据、存档、密钥、打好的包、库和压缩包、签名文件、本机设置，以及 `prepared/`、`artifacts/`、`signing/`、`work/`、`logs/`、`dist/`、`build/` 下的一切），按大小认（超过 20 MB 的文件），按内容认（像 `prod.keys` 里那样的主机密钥行）。`backends/` 下有两类文件它会放行：后端的模块构建文件 `entry/build-profile.json5`（里面没有签名时），以及 `backends/prebuilt-ohos` 里预编译的第三方库。它用 `python` 或 `python3` 运行，哪个能启动就用哪个。它认不出的（比如换了普通名字的游戏图片）要你自己把关。不要用 `--no-verify` 跳过它。
- 代码、文档、注释、日志、截图和示例里都不写你自己机器的信息：用户名、用户文件夹和本机的其他绝对路径、IP 地址、手机序列号、华为账号或团队名、邮箱、在你电脑上生成的包名。示例用通用的写法，比如 `C:\Games`、`D:\keys`、`nx2h.uk3f9a2.totk`。
- 本项目不提供任何获取游戏、密钥或固件的帮助，也不链接提供它们的地方。提到游戏文件时，只说玩家从自己主机导出的文件。
- 提交的代码须是你自己写的，按 MIT 许可接收；别人的代码按它原来的许可用：在 `THIRD_PARTY_NOTICES.md` 写明它从哪来、是什么许可；打包好的软件也带上它时，还要让 `packaging/build.py` 把它的许可文件放进 exe 旁边的 `licenses\`（放在本仓库的文本登记在 `COMMITTED` 并放进 `packaging/licenses/`，从构建环境取的登记在 `pairs` 或 `PACKAGES`）。

## 本项目只做汇总

- NX2Harmony 是入口：识别文件、检查电脑和手机、生成包名、签名、打包、安装、推送数据，并驱动各游戏后端自己的流水线。游戏本身的工作（改写游戏代码、游戏补丁、着色器和着色器种子、画面和帧率、游戏里的崩溃）属于那个游戏的后端工程（NativeGames-harmonyos、LZA、OOT，或者上游的 nxnative / totk-android），在那边修，再用 `packaging/backend_import.py` 导入 `backends/`；本仓库只接它的结果。只改在 `backends/` 里的游戏修复是一个补丁，每次导入都要带着走；NX2 自己对后端的改动（软件、新电脑流程和发布要用的）记成补丁放在维护者电脑上的 `backends/patches/`（不进提交），交给各后端工程接手（见 [backends/README.md](backends/README.md)）。
- 留在这里的是：软件和命令行（`nx2h/`）、每个游戏版本一个薄适配层（`nx2h/backends/`：收哪些输入、按什么顺序跑哪些步骤、往手机送什么）、导入的各后端副本（`backends/`）、打包（`packaging/`）和文档。
- 新游戏或新版本，先在它的后端工程里做到能在手机上玩，导入 `backends/`，然后在 `games.tsv` 加一行、在 `nx2h/backends/` 加一个适配层，并在 `nx2h/backends/__init__.py`（`BACKENDS`、`MARKERS`，以及 `load()` 里的显式 import，这样 PyInstaller 才会把它打进去）和 `nx2h/config.py` 的 `DEFAULT_BACKENDS` 里登记。状态只写在手机上实际看到的：“已验证”（玩通过）、“优化中”（能玩，还在调）、“适配中”（只显示，不能构建）。
- 没在新电脑或手机上测过的路线，测过之前一直关着；`nx2h/mirrors.py`（`ENABLED = False`）就是例子。

## 检查只能保持原样

- 后端会按已知哈希核对你的文件，HAP 构建会审计包里的每一项。`packaging/full_bundle.py` 会拒收能看出是哪台电脑打的包的文件，以及 `packaging/bundle_pictures.tsv` 里没登记的图片。哈希、审计、这道关口、`.gitignore`、提交钩子和图片清单，都不能为了让报错消失而改动。哈希对不上时，要么导出的文件坏了，要么不是这个游戏版本。
- 图片要有人看过、确认是中性的默认图（不是任何游戏或公司的美术）之后，才能加进 `packaging/bundle_pictures.tsv`。
- 密钥和签名文件只查找、不读取里面的值，也不打印、不写日志、不拷出这台电脑。保持这样。

## 仓库结构

- `nx2h/`：桌面软件（`app.py`，Qt；`core.py`，它的与绘制无关的逻辑）、命令行（`cli.py`）、识别文件（`scan.py`）、环境检查（`doctor.py`）、手机（`device.py`）、包名和签名（`bundle.py`、`signing.py`、`agc.py`）、图标和系统字体（`icon.py`、`fonts.py`）、设置和路径（`config.py`）、进度事件（`events.py`）、启动其他程序（`proc.py`）。
- `nx2h/backends/`：每个已适配版本一个适配层；`common.py` 是它们共同的做法。
- `nx2h/vendor/`：来自其他项目的代码（`nx2h/vendor/switchfs` 里是 totk-android 读取游戏文件的代码），尽量少改，各带自己的许可。
- `backends/`：各游戏后端的源码，从它们自己的工程导入，另有预编译的 libopus / libvpx（`prebuilt-ohos`）和 `backends/SOURCES.tsv`（每个文件夹从哪来；totk-android 和 nxnative 的固定提交，这两个用时再取，不拷进来）。怎么更新见 `backends/README.md`。
- `packaging/`：`build.py` 和 `packaging/NX2Harmony.spec` 打包软件；`full_bundle.py` 用已提交的 `backends/` 做完整包；`backend_import.py` 导入或更新后端；`bundle_pictures.tsv` 列出完整包可以带的图片；`check_no_windows.py` 检查打包好的软件不弹控制台窗口。
- `tests/`：不需要游戏文件、密钥、手机和网络的检查。
- `docs/`：`docs/install.md`、`docs/how-it-works.md` 和它们的中文版 `docs/install.zh-CN.md`、`docs/how-it-works.zh-CN.md`；`docs/DESIGN.md` 是中文的设计历程。
- `games.tsv`：鸿蒙验证表（已验证和优化中的版本）。根目录的 `nx2h.py`、`NX2Harmony.cmd`、`一键安装环境.cmd` 和 `一键构建.cmd` 是入口。

## 代码

- 保持 Python 3.9 可用：不用 `match`，运行时不求值 `X | Y` 类型（写在注解里可以，前提是文件开头有 `from __future__ import annotations`），f-string 的表达式里不放反斜杠。测试会用 Python 3.9 的语法解析每个文件，但抓不全这些；条件允许时用 3.9 的解释器跑一遍。
- 启动外部程序要经过 `nx2h/proc.py`（或像 `app.py` 的构建进程那样带 `CREATE_NO_WINDOW`），这样不会弹出控制台窗口。
- `nx2h/` 只用标准库，加上 PySide6（窗口）和 cryptography（签名）。除非没有别的办法，不要加依赖；要加时写进 `requirements.txt` 和 `packaging/requirements-build.txt`。
- 玩家看到的内容（窗口、命令行的提示）用中文；代码、注释和提交信息用英文。
- `.cmd` 文件保持 CRLF 换行（`.gitattributes`）。

## Pull request

- **每个 PR 只围绕一个主题。** 调试残留、大面积格式化和无关改动都不要带进来。
- **跑测试：** 在仓库根目录运行 `python -m unittest discover -s tests`；GitHub Actions 的工作流也跑这条命令。
- **测试说明：** 写明做到了哪一步（HAP 构建出来、装上了手机，还是实际玩了），以及游戏和版本、手机型号和 HarmonyOS 版本。改到构建、签名、安装或推送的，除了跑测试，还要真的构建、安装一次。
- **玩家能感觉到的变化**，在 `README.md` 的“更新说明”/“What's new”里用中英文各加一行，并写进 `CHANGELOG.md`。
- **提交信息**写成 `<type>: <description>`，类型可用 feat、fix、refactor、docs、test、chore、perf、ci。
