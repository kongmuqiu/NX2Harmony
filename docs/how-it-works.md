# How it works

[← README](../README.md) · [中文](how-it-works.zh-CN.md)

NX2Harmony is the part the player runs. It does not port a game by itself: every adapted game version has a backend that does (rewriting the game's code to run on HarmonyOS, the system services the game calls, the graphics, the shaders, the HarmonyOS app around it). Each backend is developed in its own project; this repository carries a copy of each in `backends\` ([backends/README.md](../backends/README.md)). NX2Harmony checks the PC, finds the player's games, and drives the right backend step by step from the player's own files to the game running on the phone. Installing and using it: [install.md](install.md). The design and its history, in Chinese: [DESIGN.md](DESIGN.md).

Contents: [The parts](#the-parts) · [Which games build](#which-games-build) · [Finding the games](#finding-the-games) · [A build](#a-build) · [Progress events](#progress-events) · [Bundle names and signing](#bundle-names-and-signing) · [Install and push](#install-and-push) · [Icons and fonts](#icons-and-fonts) · [The full package](#the-full-package) · [What stays on the PC](#what-stays-on-the-pc)

## The parts

```
NX2Harmony window (nx2h/app.py)        command line: python nx2h.py / nx2h.exe (nx2h/cli.py)
   runs "nx2h build ... --progress json" as a child process and draws its events
                │
nx2h/       scan · doctor · device · bundle · signing · agc · icon · fonts · events · config
                │
nx2h/backends/  common.py: HAP, bundle name and signing swap, icon, install, push
                totk_v143.py · za_202.py · oot_v131072.py: each game's inputs and steps
                │  runs each backend's own scripts, one step at a time
                ▼
backends/          NativeGames-harmonyos (Tears of the Kingdom) · LZA (Z-A) · OOT (Ocarina of Time)
using              totk-android at a pinned commit · nxnative (Ocarina of Time's shader tools)
                   Docker containers (the Linux tools) · DevEco Studio (clang, hvigor, hdc)
```

| Part | What it does |
|---|---|
| `nx2h/cli.py` | `setup`, `scan` and `build`; the build's checks, signing and stages |
| `nx2h/app.py`, `nx2h/core.py` | the window (Qt / PySide6) and what it needs without drawing: the library, the checks, the folder choices |
| `nx2h/scan.py` | finds game files and says what they are, without keys |
| `nx2h/doctor.py` | the checks of the Environment (环境) page, and the installs with winget |
| `nx2h/device.py` | the phone through DevEco Studio's hdc; finds DevEco Studio and its Java |
| `nx2h/bundle.py`, `signing.py`, `agc.py` | each game's bundle name, and its signing (in the background, or with DevEco Studio) |
| `nx2h/icon.py`, `fonts.py` | game icons; the console's shared fonts |
| `nx2h/events.py` | progress events, as text for a person or as JSON lines for a front end |
| `nx2h/config.py` | where things are: the checkout, or `data\` beside the packed program; `nx2h.local.json` |
| `nx2h/mirrors.py` | choosing the fastest download sources (switched off, `ENABLED = False`) |
| `packaging/` | `build.py` packs the program with PyInstaller; `full_bundle.py` makes the full package |

The window never builds by itself: it starts the same command line with `--progress json` and draws what it reports, so the window and the command line always do the same thing.

Inside a backend, in short (the details are in [DESIGN.md](DESIGN.md) and in the backends themselves, `backends\`): the game's code modules are rewritten so they run as native libraries under HarmonyOS; a runtime from totk-android provides the console's system services; the game's NVN graphics commands are recorded and replayed with Vulkan; the shaders are translated on the PC into a seed (`seed.vkpack`); an ArkTS app is the shell on the phone. The game's RomFS is not packed into the HAP but sent into the app's own storage.

## Which games build

[games.tsv](../games.tsv) is the HarmonyOS verification table: one row per (game, version) with its title ID, slug, version, update, backend, status and the files it needs. A game builds in one click only when its row's status is 已验证 (Verified: played through on a phone) or 优化中 (Being optimized: playable, frame rate and a few pictures still being tuned) and its backend is present. A row with 适配中 (Being adapted) is shown but not built; a game with no row is shown as 未适配 (Not adapted). A version enters the table once this project has finished it and played it on a phone: a game running on a Mac or an iPhone says nothing about HarmonyOS, whose Vulkan replay, GPU, memory and speed differ.

| Game | Version | Backend module | Backend (folder in `backends\`) |
|---|---|---|---|
| Tears of the Kingdom | 1.4.3 (update v720896) | `totk_v143` | NativeGames-harmonyos |
| Ocarina of Time, Switch 2 | v131072 | `oot_v131072` | OOT (plus nxnative) |
| Pokémon Legends: Z-A, Switch 2 Edition | 2.0.2 (update v393216) | `za_202` | LZA |

Each backend is looked for in `nx2h.local.json` `"backends"` (a place remembered there counts while that folder exists), then in `backends\`: the checkout's own, or the one beside the packed program; then in the places used before 0.1.2, beside the checkout or a few folders above the packed program. A marker file shows that it is there (for example `native-harmonyos/prepare_v143.py`). The prebuilt libraries are looked for in `prebuilt-ohos` beside the backend folders.

## Finding the games

`scan.py` walks the chosen folders and all their subfolders (not hidden or system folders, not this program's own folders, not the backends' `prepared\` and `artifacts\`) for `.nsp`, `.nsz`, `.xci`, `.xcz` and `.nup` of at least 1 MB, and reads only what is not encrypted:
- **NSP / NSZ:** the PFS0 file table; a ticket's file name starts with the title ID. The version comes from the file name's `[v…]` tag.
- **XCI / XCZ:** the card header; the titles inside are encrypted, so the title ID and version come from the file name's `[…]` tags.
- **NUP** (a Switch 2 package): its table of contents lists title, version and kind of every part.

An update's title is the base's plus `0x800`, add-on content's the base's plus `0x1000` and more. A guess from a file name is safe: the backend checks the code modules' hashes before anything is built.

## A build

### Before the first step

`nx2h build` picks the one buildable game (or the one `--game` names), asks its backend which files it takes and whether its data still has to be prepared, and then checks, in this order: the files, free disk space where the backend is (only while data is still to be prepared), `prod.keys` (only when a step still needs it), the phone (unless `--no-install`), and a Python to run the backend's scripts. Then it keeps the phone's screen on, and signs when the game's bundle name has no signing on this PC or its signing does not cover the connected phone ([Bundle names and signing](#bundle-names-and-signing)).

### The stages

1. **准备游戏数据** (prepare the game data): the backend's preparation steps.
2. **编译原生库** (native libraries): the backend's `native-harmonyos/build_ohos.py` with DevEco Studio's clang.
3. **打包 HAP**: the backend's `native-harmonyos/build_hap.ps1` (hvigor), with this game's bundle name, signing and icon swapped in; it audits every entry of the package.
4. **安装到手机**, 5. **推送游戏数据** ([Install and push](#install-and-push)).
6. Stages a backend adds after the data is on the phone: Z-A's shader seed, the first time.

Then the game is started. With `--no-install` the build ends after stage 3.

Every backend first checks its reference, totk-android, at the commit the backends pin (`native-harmonyos/upstream.py --check`), so no step runs against a missing or different copy.

The preparation steps per game, each reported as a `step` event:

| Tears of the Kingdom (`prepare_v143.py --only <step>`) | Pokémon Legends: Z-A | Ocarina of Time |
|---|---|---|
| unpack the 1.4.3 update | unpack the NUP's update part (every file's SHA-256 checked) | unpack the NUP's update part (checked the same way) |
| merge the base and update RomFS, checked against 1.4.3 | rewrite `main` and `sdk` (against the expected hashes) | check the code modules (build IDs, SHA-256) |
| RomFS patch files; on a screen wider than 16:9 also `romfs-patches-<W>x<H>` | runtime tables | store `main`, `sdk`, `subsdk0` uncompressed |
| system fonts (`nx2h/fonts.py`) | pack the RomFS | rewrite the three modules |
| shader translator (built in the .NET SDK container) | system fonts | runtime tables |
| rewrite the game code | shader translator | pack the RomFS (about 12 minutes) |
| guest data (runtime tables, guest library sources, RomFS index) | guest library, with the wide-screen camera patch | system fonts |
| shader seed (about 3 minutes the first time) | game data | guest library, with the frame-step and dynamic-resolution patches |
| third-party libraries (libopus, libvpx) | third-party libraries | game data |
| | after the first install: the shader seed from the phone | shader translator (Ampere), then nine shader steps on the PC, from extracting every shader to packing the seed |
| | | libopus / libvpx, dav1d (the AV1 movies) |

Z-A's shader seed comes from the phone: its shaders sit in containers only the game's own Oodle decoder opens. After the first install the app is started with a setting that makes it unpack every shader (about 1 minute, 7.5 GB on the phone) and exit; the PC pulls them, translates every shader in containers (about 1 hour), sends the seed back, and one more start with another setting deletes the unpacked files. Ocarina of Time's RomFS uses zstd with dictionaries, so its shaders are extracted and translated on the PC with nxnative's tools. One of the nine shader steps translates the 146 variants that testing on the phone filled in (their hashes and answers only, no game data); with it, a seed built from nothing matches the seed tested on the phone in all 146 variants, byte for byte. It lacks only 182 fills that the runtime never picks, and has newer builds of four old modules.

**Steps resume.** A step whose output is already there is reported as skipped (已有，跳过) and not run; a step that runs but does not leave its expected output stops the build. A stopped build therefore goes on where it stopped, and a second build of the same game only packs and installs. The backend's `prepared\` is the cache: data prepared on another PC can be placed there, and then the steps it covers are skipped even without Docker; the guest library sources, which name their game images by absolute path, are made again for the new place.

**Logs.** Each step's output goes to its own log file, `<date>-<time>-<game>-<step>.log`; its newest line goes to the front end as a `detail` event. When a step fails, its last lines are printed with the log's path.

### Containers and downloads

The Linux tools run in Docker containers:
- `nativegames-tools:1`, built once on this PC from the backend's `tools/docker/d2r.Dockerfile` (code rewriting, the guest export, libvpx). For Ocarina of Time it is extended under the same tag with spirv-tools and glslang.
- `python:3.14` from Docker Hub (Tears of the Kingdom's preparation, the shader seeds, SPIRV-Cross).
- `mcr.microsoft.com/dotnet/sdk:10.0-noble-aot` (building the shader translator).
- the volume `totk-venv` holds the Python environment of Tears of the Kingdom's unpack step.

An image is built up to four times in all before the build gives up. `nx2h/mirrors.py` can measure Docker registries, Debian and Ubuntu archives, the Python package index and the NuGet feed, and use the fastest; it is switched off (`ENABLED = False`) until it has been tested on a fresh PC. While it is off nothing is measured: the published images are pulled by docker itself, no source is handed to pip or NuGet, and of the four image builds the first two use the Dockerfile's own Debian source and the third and fourth the Tsinghua and USTC Debian mirrors (`LEGACY_APT`).

## Progress events

`nx2h build --progress json` writes one JSON object per line on standard output, in the schema of nxnative's `build_ipa_from_nsp.py` (nxnative's `docs/frontends.md`), so a front end made for nxnative reads it the same way. Every object has `ev` and `t` (Unix time in seconds).

| `ev` | Fields | When |
|---|---|---|
| `game` | `title_id`, `name`, `version`, `bundle_id`, `out` (the HAP's path), `icon` | once, when the game is chosen |
| `stage` | `k`, `n`, `name`, `step` | a stage starts (`k` of `n`) |
| `step` | `k`, `name`, `step` | a step inside the stage starts |
| `progress` | `k`, `name`, `step`, `done`, `total`, `unit` (`"B"` for bytes), `est`, `note`, `frac`, `eta`, `elapsed` | while a step runs; `total` is `null` when unknown |
| `stage_end` | `k`, `name`, `ok`, `summary`, `elapsed` (and `warn`) | a stage ends |
| `say` | `msg` | a message; an error starts with `[RED] ` |

NX2Harmony adds three, which an nxnative front end may ignore:

| `ev` | Fields | When |
|---|---|---|
| `detail` | `text` | the running tool's newest output line, at most about two a second |
| `signing` | `bundle`, `project`, `opened` | background signing failed: the signing project for DevEco Studio, and whether it was opened |
| `awake` | `on`, `target`, `server`, `restore` | the phone's screen is held on (or no longer); a front end that kills the build runs `restore` on that phone with hdc |

`nx2h setup --progress json` reports each check as `{"ev": "check", "key", "label", "ok", "detail", "how", "fixable", "verb"}` (`ok` is `null` for a row that is only information; `verb` is what the automatic fix does: `安装` install or `启动` start).

```
{"ev": "game", "t": 1763370000.0, "title_id": "0100F2C0115B6000", "name": "塞尔达传说 王国之泪", "version": "1.4.3", "bundle_id": "nx2h.uk3f9a2.totk", "out": "…\\artifacts\\release\\nx2h-20261117-1015\\totk-harmonyos-nx2h-20261117-1015-signed.hap", "icon": null}
{"ev": "stage", "t": 1763370012.5, "k": 1, "n": 5, "name": "准备游戏数据", "step": "解包、改写、着色器"}
{"ev": "step", "t": 1763370012.6, "k": 1, "name": "准备游戏数据", "step": "解开 1.4.3 更新"}
{"ev": "stage_end", "t": 1763371200.0, "k": 1, "name": "准备游戏数据", "ok": true, "summary": "游戏数据就绪", "elapsed": 1187.5}
{"ev": "progress", "t": 1763371700.0, "k": 5, "name": "推送游戏数据", "step": "推送游戏数据", "done": 4294967296, "total": 18575523840, "unit": "B", "est": false, "note": "", "frac": 0.23, "eta": 412.0, "elapsed": 123.0}
```

Exit codes, as nxnative's: `0` done, `1` stopped on an error (the last `[RED]` line says why), `2` bad arguments, `130` interrupted. Without `--progress json` the same events are printed for a person (`[1/5] 准备游戏数据`, a progress line, `完成：…（12 秒）`), and `detail` goes only to the step's log.

## Bundle names and signing

**Bundle names** (`nx2h/bundle.py`). The SDK's rule for a bundle name: 7 to 127 bytes, a reverse domain name whose labels start with a letter and hold only letters, digits and underscores. NX2Harmony makes `nx2h.<this PC>.<slug>`, the slug from `games.tsv`, such as `nx2h.uk3f9a2.totk`. The PC part is `u` and six base-36 characters of a hash of Windows' MachineGuid, so the checkout and every copy of the packed program on this PC give the same names, and two players never ask Huawei for the same name (an account registers the names it signs); without a MachineGuid it is drawn at random once. A name, once made, is kept in `nx2h.local.json` `"bundles"` and never changes, because the app on the phone, its saves and its signing profile belong to it; a second game with the same slug gets a number. Each backend keeps a default bundle name in its `AppScope/app.json5`; for each HAP build the game's name is swapped in there (and its signing into `build-profile.json5`, see below) and also handed to the backend's scripts as `TOTK_OHOS_BUNDLE`.

**Which signing applies** (`nx2h/signing.py`). A debug profile (`.p7b`) names one bundle name and the phones it installs on. A name is signable on this PC when a profile for it has not expired: the one signed in the background first (renewed a day before it expires), then the configurations DevEco Studio wrote into the backends' and the signing projects' `build-profile.json5`. These files are only read inside the process: never printed, logged or copied off the PC.

**In the background** (`nx2h/agc.py`, the default), following what DevEco Studio's automatic signing does:
1. **Login:** a receiver on `127.0.0.1` waits while the browser shows Huawei's DevEco login page; the player logs in, the page hands a temporary token back, and it is exchanged for the account's access token and team.
2. **Certificate:** one EC P-256 key and certificate request, made with keytool (DevEco Studio's own Java first), and one debug certificate for this PC, shared by every game.
3. **Device:** the connected phone's UDID (`bm get --udid`), added to the account when it is not there.
4. **Profile:** a debug profile for the bundle name, with every phone of the account and no ACL permissions (as DevEco Studio signs it); the old profile of that name is replaced.
5. **For hvigor:** the passwords are encrypted the way hvigor decrypts them (AES-128-GCM with a work key sealed by a root key in `material\`, which NX2Harmony makes itself).
6. **Kept** in `%USERPROFILE%\.nx2h`; `account.dat` and `keys\key.dat` are sealed with Windows DPAPI, so only this Windows user can open them.

The service is the one DevEco Studio uses, not a documented public interface, so it may change.

**With DevEco Studio** (the fallback): `signing.project_for` copies the backend's app shell (`AppScope`, the entry module's `src/main` with the shell's own sources, resources and `module.json5`, the entry module's build files, the hvigor files; build outputs left out, so no game code, no built libraries and no game data) into `%USERPROFILE%\.nx2h\signing\NX2H_<slug>` with this bundle name and opens it in DevEco Studio, where the player turns on automatic signing once. The next build finds that configuration.

**During the HAP build** the backend's `AppScope/app.json5` gets this game's bundle name and `build-profile.json5` this name's signing; the originals are moved to the backend's ignored `prepared/nx2h-stage/signing` and put back right after, also at the start of the next build when one was stopped. The backend itself never keeps another player's name or signing.

## Install and push

- **Install:** `hdc install -r` with the signed HAP, after stopping a running copy of the app.
- **Marked as a game:** the system's game service (system ability 66006) is told to treat the bundle as a game, so it gets the system's game policy, the GPU held at a high frequency among others. A phone that does not confirm only gets a note; the build goes on.
- **Folders:** hdc cannot create folders inside an app's storage, and the shell may not create every one the game needs. So the app is started once without its game page (`aa start ... --ps command start`): its runtime makes the game's folders, and the build waits until the first one exists. Further folders are made with `mkdir -p` in the phone's shell; a send that fails for a missing folder starts the runtime once more and tries again.
- **Push:** the files the backend's own push script sends (`push_v143.ps1`, `push_za.ps1`, `push_oot.ps1`), to the same places: runtime tables, RomFS image and index, fonts, the shader seed, and for Tears of the Kingdom its patch files (each folder's `romfs-patches*/manifest.tsv` first; the app makes the patch subfolders from it). Before sending, each file is compared with the phone's copy: the same size and the same SHA-256, over the whole file up to 16 MiB, else over 16 one-MiB samples spread across it. Only files that differ or are missing are sent (`hdc file send -b <bundle>`); a big file reports its progress in bytes while it goes. The summary says how many were sent and how many skipped.
- **Screen:** from signing until the game opens, the phone's screen-off time is overridden to about five minutes and renewed every 20 seconds (`power-shell`); afterwards the phone's own setting is put back, also after an error. If the window stops the build, it puts it back itself (the `awake` event).
- **Which phone:** hdc comes from the DevEco Studio in use (the one with the newest SDK). With no phone on the hdc server in use, the other hdc servers running on this PC are asked (DevEco Studio may run its own), and the one that sees a phone is remembered.
- **Start:** `aa start -b <bundle> -a EntryAbility` opens the game.

## Icons and fonts

**Icons** (`nx2h/icon.py`): from the player's own files first. The control data of an XCI, NSP or NSZ holds the icon, decrypted with `prod.keys` inside the program by the readers in `nx2h/vendor/switchfs` (from totk-android, MIT); a NUP may carry one in its meta block, unencrypted. For a game whose files carry none, the eShop icon is looked up online by title ID (`fetch_online`): first on the image host tinfoil.media, then through the community titledb list's link to the official icon on Nintendo's CDN, and for a few Switch 2 games the picture on Nintendo's own site. This runs by itself when the library is scanned (`extract_later`) and again at a build, and from the 从网上取图标 button. Only the title ID goes out: the titledb lists are downloaded whole and searched on the PC, keeping a small index in `work\titledb-index.json`. Icons are kept in `work\icons`; the project and the program carry no game pictures. For the HAP, the backend's icon images (the ones `app.json5` and `module.json5` name, both halves of a layered icon, the shell's own game picture) are moved aside to `prepared/nx2h-stage/app_icon`, replaced by the game's icon for this build, and put back right after.

**Fonts** (`nx2h/fonts.py`): every game asks the console for its six shared fonts (BFTTF). A card image (`.xci`) carries them in its system update; a NUP, an eShop package or an `.xcz` is not read for them. They are read once, with the player's `prod.keys`, from one of the player's own `.xci` card images in the library (not necessarily of this game) or from firmware dumped from the player's own console, unpacked, near `prod.keys` or named as `"firmware"` in `nx2h.local.json`. They are verified (the NCA's file-system header, the six hash levels, each font's header) and kept in `work\shared-fonts` for every game.

## The full package

`python packaging/full_bundle.py --out <folder> [--zip]` makes a folder that runs on a clean PC with nothing but the player's game files, keys, DevEco Studio and a phone:
- the packed program (`dist\NX2Harmony`, run `packaging/build.py` first) with an empty `data\`, and `THIRD_PARTY_NOTICES.md` beside it;
- in `backends\`: this repository's `backends/` as the current commit holds it (`git archive`: each backend's sources; prebuilt libopus and libvpx with their licenses in `prebuilt-ohos\`; its `README.md` and `backends/SOURCES.tsv`), with `backends/NX2HARMONY-COMMIT.txt` naming that commit; the whole totk-android tree at the pinned commit, with a marker file (`totk-android/NX2HARMONY-UPSTREAM.txt`, naming its URL and commit) that `upstream.py` takes for a checkout at the pin, so no clone is needed; the part of nxnative that Ocarina of Time runs, at the commit `backends/SOURCES.tsv` pins (`nxnative/NX2HARMONY-COMMIT.txt`). Every `.sh` goes in with LF line ends (they run in Linux containers).

Never put in: anything under `prepared\` or `artifacts\` (game material, built packages), build outputs and caches, signing configurations (`build-profile.json5` at a project's root), machine settings (`local.ohos.env` / `.ps1` and their copies, `local.properties`), keys, certificates and profiles, game files, pictures outside an app's resources, backup copies. `packaging/backend_import.py` already keeps these out of `backends/`; the package checks the committed files again. A bundle name made on some PC that a backend keeps as its default becomes `nx2h.u000000.<slug>`.

Refused before anything is copied: changes under `backends/` that are not committed (the package must match a commit); a backend folder in that commit without its marker file, or holding a file the package never carries; backends that pin different totk-android commits, or no totk-android checkout that holds the pin (`backends\totk-android`, the one a backend uses, else beside this repository); a backend file that would tell who packed it or where (for example the packing user's folder, their git e-mail address, or a local network address with a port); a picture in a backend's app that is not listed, by SHA-256, in `packaging/bundle_pictures.tsv` (each listed one has been looked at: neutral default art, no game's or company's artwork); `prebuilt-ohos` without libopus or libvpx. Refused while packing, which leaves the folder unfinished: a backend without `build-profile.template.json5`, no nxnative checkout that holds its pinned commit (`backends\nxnative`, beside this repository, or where the Ocarina of Time backend finds it), and anything from `prepared\` or `artifacts\` in the result. Paths of the packing PC's own folders in a backend file are only listed, not refused (`packaging/backend_import.py` refuses them before they reach the repository). The package carries no game content: the player's own game icon goes into each HAP at build time.

## What stays on the PC

Game files, keys, firmware, everything made from them (`prepared\`, `artifacts\`, `work\`, `logs\`, HAPs) and all signing material stay on the player's PC. `.gitignore` keeps them out of the repository, and the release standard is source only: no HAP, game data or signing material is ever published. Nothing the build makes is uploaded anywhere; the only things sent out are the signing requests to the player's Huawei account, the icon lookup by title ID ([Icons and fonts](#icons-and-fonts)), and the downloads of tools and container images.
