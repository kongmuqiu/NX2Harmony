# Installation

[中文](INSTALL.md) · English

> This is the copy NX2Harmony carries. Inside NX2Harmony, installation goes through NX2Harmony: see
> [NX2Harmony's README](../../../README.md) and its [install guide](../../../docs/install.md) (an AI assistant follows
> NX2Harmony's install skill, `/install-nx2harmony`). The standalone NativeGames-harmonyos project's own AI install skill
> is not carried here. Below are this backend's own manual steps, for doing it yourself.

Getting from your own game files to playing on the phone takes three stages: preparation on the PC (about half an
hour the first time, mostly merging the game data and rewriting the code), building and signing the package, then installing it and sending the data.

## 1. What you need

**PC** (Windows 10/11 x64, the tested setup)

- Docker (Docker Desktop or Docker Engine), able to pull `python:3.14` and `mcr.microsoft.com/dotnet/sdk:10.0-noble-aot`
- Python 3.12 or newer, Git (only to fetch the totk-android checkout) and PowerShell 7
- DevEco Studio with the HarmonyOS API 26 SDK (it brings `hdc` and the packaging tools)
- A Huawei developer account (free) to sign the app
- About 80 GB of free space

**Phone**

- HarmonyOS 7 with developer mode and USB debugging on, connected by cable so that `hdc list targets` shows it
- About 20 GB of free space

**Your own game files**, in folders (one folder may hold all three, as long as it has only one `.xci` and one `.nsz`):

| The folder holds | Notes |
| --- | --- |
| the base game 1.0.0 `.xci` (only this file) | dumped from your own cartridge |
| the 1.4.3 update `.nsz` (only this file) | dumped from the update on your own console |
| `prod.keys` | dumped from your own console; read only inside the preparation containers, never copied or printed |

Every file is checked against the known 1.4.3 hashes; a wrong version or a wrong file is refused.

## 2. Local settings

In the repository root:

```
copy native-harmonyos\local.ohos.example.ps1 native-harmonyos\local.ohos.ps1
```

Edit `native-harmonyos\local.ohos.ps1`: set where DevEco Studio is (hdc and the SDK are found inside it), and, when more
than one device is connected, your phone's device key as `hdc list targets` shows it. Both the PowerShell scripts and the
Python tools read this file; it stays on your PC and is ignored by git.

The upstream totk-android sources are cloned automatically next to the repository (`../totk-android`) and held at the
commit in `native-harmonyos/UPSTREAM_PIN` (set `TOTK_UPSTREAM` in your local settings to use another location).

## 3. One preparation command

```
python native-harmonyos/prepare_v143.py --base <folder with the .xci> --update <folder with the .nsz> --keys <folder with prod.keys>
```

It runs these steps in order; a finished step is skipped, so after an interruption run the same command again:

1. unpack the update and check the 1.4.3 code modules
2. merge the base game and the update into the complete 1.4.3 game data (17 GB)
3. write the few data files the runtime replaces
4. take the console fonts from the cartridge's system-update partition
5. build the shader translator
6. rewrite the three code modules and compare them byte for byte with upstream's verified result
7. export the patched code, the runtime tables and the game data index
8. generate the shader seed (a few minutes)
9. build the Opus audio and VP9 video decoders
10. compile the two native libraries that run on the phone

`--dry-run` only prints the commands; `--only seed,libs` runs just those steps.

Step 8 (`native-harmonyos/build_seed.py`) first asks Docker how many CPUs and how much free memory it has on this PC,
then picks how many containers to run and how many translators each gets (8 containers x 4 with 32 CPUs and 16 GB).
The translations are cut into chunks, largest first; each container takes the next chunk when it finishes one, so a
faster one does more. The intermediate files stay in a Docker volume; only `seed.vkpack`, `sb-align.txt`,
`seed-report.json` (the counts and the skipped variants) and `spirv-cross` are written to the PC.
`TOTK_SEED_CONTAINERS` / `TOTK_SEED_SERVICES` override the choice. The `seed.vkpack` is byte for byte the one the
earlier single-container `build_seed.sh` makes (from scratch on 2026-11-16 on a PC with 32 threads and 16 GB of memory
for Docker: 131 seconds instead of 55 minutes).

When the phone's screen is wider than 16:9 (most phones), add `--aspect <width>x<height>` with its landscape pixels,
e.g. `--aspect 2848x1320`. Its third step then also writes the game's UI laid out for that screen,
`extracted/v143/romfs-patches-<width>x<height>`: the HUD at the screen's left and right edges, loading screens, cutscenes
and menu backgrounds across the whole screen. To find the size, run `hdc shell hidumper -s RenderService -a screen` and
read `physical resolution=1320x2848` on the `screen[0]` line; the larger number comes first. After a finished
preparation, make just this one with `python native-harmonyos/prepare_v143.py --only patches --aspect 2848x1320`.

If installing packages in the container fails with `Hash Sum mismatch`, it is usually the network: build the
container image once through another Debian mirror, then run the command again:

```
docker build --build-arg APT_MIRROR=<mirror host> -t nativegames-tools:1 -f tools/docker/d2r.Dockerfile tools/docker
```

## 4. Sign and package

1. Open this repository in DevEco Studio with the phone connected.
2. File → Project Structure → Signing Configs: sign in with your Huawei account and tick automatic signing. This writes
   `build-profile.json5` in the repository root (ignored by git). The signature only works on phones registered to it.
3. Build:

   ```
   native-harmonyos\build_hap.ps1 -Tag mine
   ```

   This produces `artifacts\release\mine\totk-harmonyos-mine-signed.hap` and checks that it contains only the app, the
   two native libraries and the license texts. **The package contains your game's code: use it only on your own phone.**

## 5. Install on the phone

```
hdc install -r artifacts\release\mine\totk-harmonyos-mine-signed.hap
```

The data folders are made by the app's runtime (hdc cannot make them, and opening the launcher alone does not), so the
first line starts the runtime once; then send the game data (about 20 GB) and close the app:

```
hdc shell aa start -a EntryAbility -b TotZ.NativeGames.harmonyos --ps command start
native-harmonyos\v143\push_v143.ps1 -RomFS -GameData -Seed -Patches
hdc shell aa force-stop TotZ.NativeGames.harmonyos
```

On this first start the app also writes its settings file for this screen and makes the folder for the layouts made
for your screen, `romfs-patches-<width>x<height>`. `-GameData` also sends the console fonts into `game-v143/fonts`,
which the game needs to show Chinese and Korean text. The layouts `-Patches` sends have subfolders, which the app makes
only when it finds the layouts' list (`manifest.tsv`) at start: on the first push the files in subfolders fail to
send while the rest goes on; then start the game once on the phone and run `push_v143.ps1 -Patches` again, and the HUD
is at the edges from the next start.

Then open the app on the phone, choose your settings and tap 开始游戏 (start).

When the app on the phone has another bundle name than `TotZ.NativeGames.harmonyos` (for example after changing
`AppScope/app.json5`), add the line `$env:TOTK_OHOS_BUNDLE = '<bundle name>'` to `native-harmonyos\local.ohos.ps1`:
`push_v143.ps1` and `fill_shaders_ohos.py` then use it. Use that name in the hdc commands of this guide too.

**Later updates**: `git pull`, `python native-harmonyos/upstream.py --update`,
`python native-harmonyos/prepare_v143.py --only libs`, then build the package as in section 4 and `hdc install -r` it.
While the bundle name and the signature stay the same, saves and game data stay; [CHANGELOG](../CHANGELOG.md) says
what to prepare or send again.

**Coming from 0.5.0**: 0.5.0's bundle name was `com.macnativegames.harmonyprobe`; it is now `TotZ.NativeGames.harmonyos`.
The new package installs as a separate app, and the old app's game data and saves do not come along. In this order:

1. Back up the old app's saves: tap 导出存档 (export) on the old app's 存档 (Saves) tab. 0.5.0's export may fail with
   `File exists`; then fetch them from the PC with
   `hdc file recv -b com.macnativegames.harmonyprobe /data/storage/el2/base/haps/entry/files/android/user-data <folder on the PC>`,
   zip the `user-data` folder you received and copy the zip to the phone.
2. Update the code and build the native libraries as in "Later updates". The bundle name changed and the signature
   made for 0.5.0 covers only the old one: redo section 4 step 2 in DevEco Studio (automatic signing), then build the
   package; the rest of the preparation on
   the PC needs no redo (for the HUD at the edges, run section 3's `--only patches --aspect` once). Install the new app
   and send all the data as above. While both apps are installed, the phone needs about 20 GB more.
3. On the new app's 存档 (Saves) tab, tap 导入存档 (import) and pick that zip (it finds the folder that holds the saves
   inside the zip).
4. Once a save loads and plays in the new app, uninstall the old one: `hdc uninstall com.macnativegames.harmonyprobe`.
   That deletes the old app's saves and its 20 GB of data.

## 6. Filling shaders

The first time you reach a place, shaders missing from the seed are skipped (some things are not drawn for a moment) and
the phone records which ones were missing. After playing for a while, with the phone connected, run on the PC:

```
python native-harmonyos/fill_shaders_ohos.py --version v143
```

It fetches the records, translates the shaders on the PC and sends them back; the next start has them.

## 7. Settings

Changes on the launcher's 设置 (Settings) tab are saved as you make them and apply at the next start. In game, tap the
small tab at the top centre (or use the back gesture) for the game menu: 继续游戏 (continue), 游戏设置 (settings; apply
them with 应用并重启, apply and restart), 触屏按键 (on-screen keys: auto / always shown / hidden), 按键布局 (key layout:
the game's own / standard pad), 视角操作 (camera: swipe / two sticks) and 回到启动页 (back to the launcher).

| Preset | What it sets | Good for |
| --- | --- | --- |
| 流畅 Smooth (recommended) | 540p rendering, the game's FXAA, medium shadows / 2 cascades, 1920 output, SGSR upscaling | everyday play, the steadiest |
| 均衡 Balanced | the game's own render resolution | a finer picture, slightly lower frame rate |
| 画质 Quality | docked mode, FSR, SMAA | scenery |
| 高帧率 High frame rate | 60 fps cap, otherwise like Smooth | 50+ fps in open areas, still in the 20s in busy scenes |

界面位置 (HUD position) under 画面 (Display), shown only while 画面比例 (aspect) is 全面屏 (full screen): **贴边** (edges,
the default) uses the layouts made for your screen, with the hearts at the left edge and the minimap at the right;
without layouts for this screen on the phone (prepared without `--aspect`) the HUD and menus stay in the middle 16:9.
**居中 16:9** (centred) always keeps them there.

Controls: the on-screen buttons are labelled with this game's actions by default (游戏定制, the game's own); 触屏按键布局
(on-screen key layout) under 操作 (Controls) switches them to 标准手柄 (standard pad: the Switch's A B X Y, D-pad,
shoulder buttons and + −). Swipe on the empty right side to turn the camera.
A Bluetooth or wired controller works too, and the on-screen buttons hide while it is connected. The 操作 (Controls)
tab sets the controller's button layout (by label or by position), rumble strength and gyro aiming. When the game asks
for text (naming a horse, for example), the system keyboard opens.

## 8. Saves

Saves live in the app's own folder: **uninstalling the app deletes them.**

The launcher's 存档 (Saves) tab exports the saves as a zip to any place on the phone, and imports a zip (one exported by
this app, or a complete save set exported by a save manager). Before every start, every import and every restore the
current saves are backed up; the newest 10 backups are kept and can be restored on the same tab.

From the PC:

```
hdc file recv -b TotZ.NativeGames.harmonyos /data/storage/el2/base/haps/entry/files/android/user-data <folder on the PC>
```

Saves from 1.0.0 load as they are.

## 9. When something goes wrong

- Runtime log: `hdc file recv -b TotZ.NativeGames.harmonyos /data/storage/el2/base/haps/entry/files/android/totk-native.log .`
  Every 300 frames it lists the frame rate, GPU and CPU time, texture uploads and more.
- The system closes the app without a message: usually GPU memory went over the per-app limit (about 3.1 GB); the
  last `memory Vulkan bytes` line in the log shows the usage.
- Diagnostic switches do not go in `env.txt`: the launcher rewrites it whole every time it saves the settings. Put them
  in `files/android/env-dev.txt`, which is read after `env.txt` and never written by the settings page. Make the file on
  the PC (one `NAME=value` per line) and send it; it applies at the next start:

  ```
  Set-Content -Encoding ascii env-dev.txt 'TOTK_OHOS_FLICKER=20'
  hdc file send -b TotZ.NativeGames.harmonyos env-dev.txt /data/storage/el2/base/haps/entry/files/android/env-dev.txt
  ```

  To change it later, send a file with a new name, e.g. `env-dev-2.txt`: the app may not see a file it has already read
  being rewritten, and the `env-dev-*.txt` files are read after `env-dev.txt` in name order, the later one winning.
  `NAME=` with nothing after the `=` clears that switch.
- Flicker: put `TOTK_OHOS_FLICKER=20` in an env-dev file (the example above); the log then records flickering frames
  and saves pictures of them.
- The world stops (the picture still refreshes, but the game's clock does not move and the game's loading ring keeps
  turning): the default of two frames in flight can do this now and then; the cause is still being looked into. Put
  `TOTK_OHOS_SYNC_LAG=0` in an env-dev file for one frame in flight: it has not stopped like this so far, but runs
  slower (measured about 20–27 fps, about 30 with two frames). Keep the runtime log of that session for a report. To go back to
  two frames, send another file with a new name holding `TOTK_OHOS_SYNC_LAG=1`.
- The game closes at the title after choosing Chinese or Korean, and the log has `[totk] FAIL: open shared font`: the
  console fonts are not on the phone (0.5.0 did not send them). Run `push_v143.ps1 -GameData` again; when it warns that
  `shared-fonts` has no .bfttf, first run
  `python native-harmonyos/prepare_v143.py --only fonts --base <folder with the .xci> --keys <folder with prod.keys>`.
- 贴边 (edges) is chosen but the HUD sits in the middle 16:9: the log's `HUD in the middle 16:9` line gives the reason.
  The app looks for the layouts of the screen size in its settings file, `TOTK_ASPECT=<width>:<height>` in
  `files/android/env.txt` (fetch it with `hdc file recv`). When that differs from the `--aspect` you prepared (for
  example when the phone's screen resolution setting is below the panel's own), run `--only patches --aspect
  <width>x<height>` once with that size and send `-Patches` again.
