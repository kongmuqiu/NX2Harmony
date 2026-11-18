# backends

The game backends NX2Harmony builds with, as the full package carries them: each game's HarmonyOS pipeline (scripts,
native code, the ArkTS app) and the prebuilt third-party libraries they link. The app finds them here from a checkout
and in `backends\` beside `NX2Harmony.exe` in the full package.

| Folder | Game | Comes from |
| --- | --- | --- |
| `NativeGames-harmonyos/` | Tears of the Kingdom 1.4.3 | the NativeGames-harmonyos project at the commit in `SOURCES.tsv`, with NX2's changes |
| `LZA/` | Pokémon Legends: Z-A 2.0.2 | the LZA project's working folder (not published on its own), with NX2's changes |
| `OOT/` | Ocarina of Time v131072 | the OOT project's working folder (not published on its own), with NX2's changes |
| `prebuilt-ohos/` | all three | libopus 1.6.1 and libvpx 1.15.0 built for aarch64-linux-ohos (`prebuilt-ohos/README.txt`) |
| `patches/` | | NX2's own changes to each backend, against the revision in `SOURCES.tsv`; on the maintainer's PC only (not committed: they quote the projects' original lines, which can name the PC they were written on) |

Two public projects are not copied here but pinned in `SOURCES.tsv` and fetched: totk-android (every backend builds on
it; the app's environment page clones it into `backends/totk-android`) and nxnative (Ocarina of Time's shader tools;
clone it into `backends/nxnative` or beside this repository). The full package carries both at their pins.

## Rules

- Game work (rewriting game code, game patches, shaders and the shader seed, picture and frame rate, a crash inside the
  game) is done in the backend's own project and then imported; a change made only here is a patch that has to be
  carried forward on every import. NX2's own changes (what the app, the clean-PC flow and the release need) are kept in
  `patches/` for the backend projects to take over; those patches stay on the maintainer's PC and go to the backend
  projects with their hand-off notes.
- Each folder holds only what the full package may carry: no game material, build output, machine settings, signing
  or official game art. Builds write `prepared/`, `artifacts/` and the like inside a backend folder; this repository
  ignores them. A build also changes two tracked things for its duration, the bundle name in `AppScope/app.json5` and
  the app's icons (the player's game icon), and puts them back at the end; a build that is stopped leaves them until
  the next one. Do not commit them: the pre-commit hook refuses a bundle name made on a PC and unlisted pictures.
- The standalone projects' own AI-install instructions and hooks (`.claude/`, `AGENTS.md`, `.githooks/`, `.github/`)
  stay out: NX2Harmony's apply here.

## Updating a backend

```
python packaging/backend_import.py <backend> <project folder> [--rev <commit>]   # take the project's files, apply patches/
python packaging/backend_import.py <backend> <project folder> --check            # 0 when backends/ is SOURCES.tsv's revision + patch
python packaging/backend_import.py <backend> <project folder> --save-patch       # after editing backends/<folder> here (against SOURCES.tsv's revision)
```

`<backend>` is `totk_v143`, `za_202` or `oot_v131072`. The import takes a git project's files at the commit (never its
working tree), else a working folder's project folders and root files; it refuses files that name a PC or a person,
and pictures that `packaging/bundle_pictures.tsv` does not list (a listed one already here stays). Review
`git diff -- backends`, build and play the game on a phone, then commit.

---

# backends（中文）

NX2Harmony 构建用的游戏后端，和完整包里带的一样：每个游戏的鸿蒙流水线（脚本、原生代码、ArkTS 应用），以及它们链接的预编译第三方库。
从源码运行时软件在这里找后端；完整包里在 `NX2Harmony.exe` 旁边的 `backends\`。

| 文件夹 | 游戏 | 来源 |
| --- | --- | --- |
| `NativeGames-harmonyos/` | 王国之泪 1.4.3 | NativeGames-harmonyos 工程在 `SOURCES.tsv` 记的提交，加 NX2 的改动 |
| `LZA/` | 宝可梦传说 Z-A 2.0.2 | LZA 工程的工作文件夹（没有单独发布），加 NX2 的改动 |
| `OOT/` | 时之笛 v131072 | OOT 工程的工作文件夹（没有单独发布），加 NX2 的改动 |
| `prebuilt-ohos/` | 三个游戏共用 | 为 aarch64-linux-ohos 编译的 libopus 1.6.1 和 libvpx 1.15.0（见 `prebuilt-ohos/README.txt`） |
| `patches/` | | NX2 对各后端的改动，基于 `SOURCES.tsv` 记的版本；只在维护者的电脑上（不进提交：补丁里引着原工程被改掉的行，那些行可能带着写它们的电脑的信息） |

两个公开项目不拷进来，只在 `SOURCES.tsv` 里固定提交、用时再取：totk-android（三个后端都建在它上面；软件的“环境”页把它克隆到
`backends/totk-android`）和 nxnative（时之笛的着色器工具；克隆到 `backends/nxnative` 或本仓库旁边）。完整包按固定提交带着这两个。

## 规则

- 游戏本身的工作（改写游戏代码、游戏补丁、着色器和着色器种子、画面和帧率、游戏里的崩溃）在后端自己的工程里做，再导入进来；只改在这里的，
  每次导入都要当补丁带着走。NX2 自己的改动（软件、新电脑流程和发布要用的）放在 `patches/`，交给各后端工程接手；这些补丁只留在维护者的电脑上，随交接说明交给各后端工程。
- 每个文件夹只放完整包可以带的东西：不放游戏内容、构建输出、本机设置、签名和游戏官方图。构建会在后端文件夹里写 `prepared/`、
  `artifacts/` 等，本仓库都忽略它们。构建期间还会临时改两处已提交的内容：`AppScope/app.json5` 里的包名和应用图标（换成玩家的游戏图标），构建结束时换回；
  中途停掉的构建会把它们留到下一次构建。不要提交它们：提交钩子会拒绝本机生成的包名和没登记的图片。
- 独立工程自己的 AI 安装说明和钩子（`.claude/`、`AGENTS.md`、`.githooks/`、`.github/`）不带进来：这里以 NX2Harmony 的为准。

## 更新后端

```
python packaging/backend_import.py <后端> <工程文件夹> [--rev <提交>]   # 取工程的文件，打上 patches/
python packaging/backend_import.py <后端> <工程文件夹> --check          # 返回 0：backends/ 就是 SOURCES.tsv 的版本加补丁
python packaging/backend_import.py <后端> <工程文件夹> --save-patch     # 在这里改了 backends/<文件夹> 之后（基于 SOURCES.tsv 记的版本）
```

`<后端>` 是 `totk_v143`、`za_202` 或 `oot_v131072`。git 工程按提交取文件（从不取工作区），工作文件夹取它的工程文件夹和根目录文件；
会拒绝带出电脑或个人信息的文件，以及 `packaging/bundle_pictures.tsv` 没登记的图片（这里已有的登记图片会保留）。看过
`git diff -- backends`、构建并在手机上玩过之后再提交。
