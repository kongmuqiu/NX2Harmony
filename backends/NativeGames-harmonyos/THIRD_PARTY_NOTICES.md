# 第三方代码与许可 · Third-party notices

中文 · [English](#english)

本项目自己的代码以 MIT 发布（[LICENSE](LICENSE)）。下面列出项目里包含、编进应用、或构建时在电脑上用到的别人的代码，以及它们的许可。这些都不是游戏内容。

编进应用原生库（`libtotk_ohos.so`）的每个第三方部分，许可原文都在 `entry/src/main/resources/rawfile/licenses/`，应用的“关于 → 开源许可”逐个列出。

## 包含在仓库里

| 内容 | 来源 | 许可 |
| --- | --- | --- |
| `native-harmonyos/overlay/`：在 totk-android 文件基础上修改的鸿蒙版本（编进应用） | [totk-android](https://github.com/AurelioB/totk-android)，`native-harmonyos/UPSTREAM_PIN` 中的提交 | MIT，版权 2026 MacNativeGames contributors；原文见 `native-harmonyos/overlay/LICENSE.totk-android`，应用内附 `licenses/totk-android-MIT.txt` |
| `native-harmonyos/overlay/third_party/d2r_gfx/`：其中 NVN 层文件的修改版（编进应用） | [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)，经 totk-android | MIT，版权 2026 jhaizhou；原文见 `native-harmonyos/overlay/third_party/d2r_gfx/LICENSE`，应用内附 `licenses/d2r-mobile-MIT.txt` |
| `native-harmonyos/v143/ultrawide_layout.py`：在 totk-android 的 `games/totk/scripts/ultrawide_layout.py` 基础上修改的版本（宽屏界面布局） | [totk-android](https://github.com/AurelioB/totk-android)，`native-harmonyos/UPSTREAM_PIN` 中的提交 | MIT，版权 2026 MacNativeGames contributors；原文见 `native-harmonyos/overlay/LICENSE.totk-android` |
| `native-harmonyos/third_party/etcpak/`：ETC1 / ETC2 编码（编进应用） | [etcpak](https://github.com/wolfpld/etcpak)，提交 `3d716e1550023dc5ff8b602636af8b76584e0b66` | BSD 3-Clause，版权 2013–2026 Bartosz Taudul；原文见同目录 `LICENSE.txt`，应用内附 `licenses/etcpak-BSD-3-Clause.txt` |

## 构建时取用、编进应用（不在仓库里）

`build_ohos.py` 编译 totk-android 的 `games/totk`（`runtime/`、`third_party/d2r_gfx/`、`android/gfx/`、`android/compat/`，上面盖 `native-harmonyos/overlay/`），再链接下面的库。totk-android 里每个文件的来源见它自己的 `THIRD_PARTY_NOTICES.md`。

| 内容 | 在应用里的用处 | 许可（应用内附的原文） |
| --- | --- | --- |
| totk-android（固定提交，`upstream.py` 自动克隆） | 运行时、NVN 层、Vulkan 后端 | MIT，版权 2026 MacNativeGames contributors（`totk-android-MIT.txt`；它所出自的 macOS 项目 [MacNativeGames](https://github.com/ly05010419/MacNativeGames) 的许可是同一段文字） |
| [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)（在 totk-android 里） | NVN 层、纹理工具和主机头文件（`third_party/d2r_gfx`），线程代码（`runtime/threads.c`、`gate.S`、`thread_enter.S`） | MIT，版权 2026 jhaizhou（`d2r-mobile-MIT.txt`） |
| h1k421 的 NVN 接口头文件 `nvn.h`（在 totk-android 里） | NVN 层的类型定义 | MIT，版权 2020 h1k421（`NVN-header-MIT.txt`，即头文件自带的声明） |
| deko3d（在 totk-android 里） | `nvn_tex.c` 里的纹理排布尺寸 | zlib，版权 2018–2020 fincs（`deko3d-zlib.txt`） |
| [bcdec](https://github.com/iOrange/bcdec) 0.985（在 totk-android 里） | BC 纹理解码 | MIT 或公有领域（`bcdec-LICENSE.txt`） |
| [SMAA](https://github.com/iryoku/smaa)（在 totk-android 里） | `vk_post_spv.h`、`vk_post_lut.h` 里的抗锯齿着色器和查找表 | MIT（`SMAA-MIT.txt`） |
| AMD FidelityFX FSR 1.0.2（在 totk-android 里） | `vk_post_spv.h` 里的超分着色器 | MIT，版权 2021 Advanced Micro Devices, Inc.（`FidelityFX-FSR-MIT.txt`） |
| Snapdragon Game Super Resolution 1（在 totk-android 里） | `vk_post_spv.h` 里的超分着色器 | BSD 3-Clause，版权 2023 Qualcomm Innovation Center, Inc.（`Snapdragon-GSR-BSD-3-Clause.txt`） |
| Ryujinx.Audio（移植进 totk-android） | 音效和混音：`runtime/audio_effects.c`、`audio_reverb.c`、`audio_mixer.c` | MIT，版权 Ryujinx Team and Contributors（`Ryujinx-Audio-MIT.txt`） |
| [TOTKRecompAndroid](https://github.com/mamcjr/TOTKRecompAndroid)（经 totk-android） | `runtime/game_patches_v143.c` 的淡入淡出和过场处理，`hle_nvn.c` 的 NVN 节拍适配 | MIT，版权 2026 Peiikai（`TOTKRecompAndroid-MIT.txt`） |
| [LASRecomp-Android-Plus](https://github.com/Nycolazs/LASRecomp-Android-Plus)（经 totk-android） | 着色器缺失时的后台翻译（`vk_shader_xlate.c`）和帧节拍（`vk_record.c`、`fps_auto_block.h`、`hle_nvn.c`）的做法 | MIT，版权 2026 Peiikai（`LASRecomp-Android-Plus-MIT.txt`） |
| [nxnative](https://github.com/seedds/nxnative)、nx2android（经 totk-android） | `runtime/threads.c`、`audio_mixer.c`、`hle_nvn.c`、`nvn_obj.h`、`nvn_tex.c` 里的修正；`runtime/android_perf.c` 的性能提示查找照 nx2android 的做法 | MIT，版权 2026 seedds（`nxnative-MIT.txt`；nx2android 的许可是同一段文字） |
| libopus 1.6.1（官方发布包，按 SHA-256 核对） | 游戏音频 | BSD 3-Clause（`libopus-BSD.txt`） |
| libvpx v1.15.0 | 过场动画（VP9） | BSD 3-Clause（`libvpx-BSD.txt`）；专利授权见 libvpx 的 `PATENTS` |
| 鸿蒙 SDK 的 libc++ / libc++abi（LLVM），静态链接 | etcpak 编码器的 C++ 运行库 | Apache-2.0 with LLVM Exceptions：这一例外免除了编进程序的部分附带许可的要求，所以应用内不附；原文见 SDK 的 `llvm/NOTICE` |

## 只在电脑上用（不进应用）

| 内容 | 用途 | 许可 |
| --- | --- | --- |
| totk-android 的 d2r 代码改写器（`games/totk/third_party/d2r`，出自 d2r-mobile） | 电脑上改写游戏代码 | MIT |
| Ryujinx 着色器编译器（totk-android 的 `vendor/ryujinx`，经它的 `totk_xlate`，在 .NET SDK 容器里构建） | 电脑上翻译着色器 | MIT |
| SPIRV-Cross | 电脑上生成着色器种子 | Apache-2.0 |
| SPIRV-Tools（spirv-val、spirv-opt）、glslang（glslangValidator），装在工具容器里 | 电脑上检查、优化着色器，编译本项目自己的着色器 | Apache-2.0；glslang 为多种宽松许可（见它的 `LICENSE.txt`） |
| hactool、nsz 5.0.0 | 电脑上解开和合并游戏文件，作为独立工具运行 | ISC；nsz 为 GPL-3.0（仅作为独立程序调用，不链接、不分发） |
| capstone 5.0.9、lz4 4.4.5、cryptography 50.0.2 | 电脑上容器里的改写和解包工具 | BSD 3-Clause、BSD 3-Clause、Apache-2.0 / BSD |

---

<a id="english"></a>

[中文](#第三方代码与许可--third-party-notices) · English

This project's own code is released under MIT ([LICENSE](LICENSE)). Below is the code by others that the project
contains, compiles into the app, or uses on the PC at build time, with its license. None of it is game content.

Every third-party part compiled into the app's native library (`libtotk_ohos.so`) has its license text in
`entry/src/main/resources/rawfile/licenses/`; the app lists them under 关于 → 开源许可 (About → open-source licenses).

## In the repository

| What | Source | License |
| --- | --- | --- |
| `native-harmonyos/overlay/`: HarmonyOS versions of totk-android files (compiled into the app) | [totk-android](https://github.com/AurelioB/totk-android), the commit in `native-harmonyos/UPSTREAM_PIN` | MIT, copyright 2026 MacNativeGames contributors; text in `native-harmonyos/overlay/LICENSE.totk-android`, shipped in the app as `licenses/totk-android-MIT.txt` |
| `native-harmonyos/overlay/third_party/d2r_gfx/`: changed files of its NVN layer (compiled into the app) | [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile), through totk-android | MIT, copyright 2026 jhaizhou; text in `native-harmonyos/overlay/third_party/d2r_gfx/LICENSE`, shipped as `licenses/d2r-mobile-MIT.txt` |
| `native-harmonyos/v143/ultrawide_layout.py`: a modified copy of totk-android's `games/totk/scripts/ultrawide_layout.py` (UI layouts for wide screens) | [totk-android](https://github.com/AurelioB/totk-android), the commit in `native-harmonyos/UPSTREAM_PIN` | MIT, copyright 2026 MacNativeGames contributors; text in `native-harmonyos/overlay/LICENSE.totk-android` |
| `native-harmonyos/third_party/etcpak/`: ETC1 / ETC2 encoding (compiled into the app) | [etcpak](https://github.com/wolfpld/etcpak), commit `3d716e1550023dc5ff8b602636af8b76584e0b66` | BSD 3-Clause, copyright 2013–2026 Bartosz Taudul; text in `LICENSE.txt` in that folder, shipped as `licenses/etcpak-BSD-3-Clause.txt` |

## Taken at build time and compiled into the app (not in the repository)

`build_ohos.py` compiles totk-android's `games/totk` (`runtime/`, `third_party/d2r_gfx/`, `android/gfx/`,
`android/compat/`, with `native-harmonyos/overlay/` on top) and links the libraries below. totk-android's own
`THIRD_PARTY_NOTICES.md` says where each of its files comes from.

| What | Used for in the app | License (text shipped in the app) |
| --- | --- | --- |
| totk-android (pinned commit, cloned by `upstream.py`) | The runtime, the NVN layer and the Vulkan backend | MIT, copyright 2026 MacNativeGames contributors (`totk-android-MIT.txt`; [MacNativeGames](https://github.com/ly05010419/MacNativeGames), the macOS project it comes from, has the same text) |
| [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) (inside totk-android) | The NVN layer, texture tools and host headers (`third_party/d2r_gfx`); thread code (`runtime/threads.c`, `gate.S`, `thread_enter.S`) | MIT, copyright 2026 jhaizhou (`d2r-mobile-MIT.txt`) |
| The NVN API header `nvn.h` by h1k421 (inside totk-android) | The NVN layer's type definitions | MIT, copyright 2020 h1k421 (`NVN-header-MIT.txt`, the header's own notice) |
| deko3d (inside totk-android) | Texture layout sizes in `nvn_tex.c` | zlib, copyright 2018–2020 fincs (`deko3d-zlib.txt`) |
| [bcdec](https://github.com/iOrange/bcdec) 0.985 (inside totk-android) | BC texture decoding | MIT or public domain (`bcdec-LICENSE.txt`) |
| [SMAA](https://github.com/iryoku/smaa) (inside totk-android) | Anti-aliasing shaders and lookup tables in `vk_post_spv.h`, `vk_post_lut.h` | MIT (`SMAA-MIT.txt`) |
| AMD FidelityFX FSR 1.0.2 (inside totk-android) | Upscaling shaders in `vk_post_spv.h` | MIT, copyright 2021 Advanced Micro Devices, Inc. (`FidelityFX-FSR-MIT.txt`) |
| Snapdragon Game Super Resolution 1 (inside totk-android) | Upscaling shader in `vk_post_spv.h` | BSD 3-Clause, copyright 2023 Qualcomm Innovation Center, Inc. (`Snapdragon-GSR-BSD-3-Clause.txt`) |
| Ryujinx.Audio (ported into totk-android) | Audio effects and mixer: `runtime/audio_effects.c`, `audio_reverb.c`, `audio_mixer.c` | MIT, copyright Ryujinx Team and Contributors (`Ryujinx-Audio-MIT.txt`) |
| [TOTKRecompAndroid](https://github.com/mamcjr/TOTKRecompAndroid) (through totk-android) | Fade and cutscene handling in `runtime/game_patches_v143.c`, the NVN pacing adapter in `hle_nvn.c` | MIT, copyright 2026 Peiikai (`TOTKRecompAndroid-MIT.txt`) |
| [LASRecomp-Android-Plus](https://github.com/Nycolazs/LASRecomp-Android-Plus) (through totk-android) | The design of the shader-miss worker (`vk_shader_xlate.c`) and of frame pacing (`vk_record.c`, `fps_auto_block.h`, `hle_nvn.c`) | MIT, copyright 2026 Peiikai (`LASRecomp-Android-Plus-MIT.txt`) |
| [nxnative](https://github.com/seedds/nxnative), nx2android (through totk-android) | Fixes in `runtime/threads.c`, `audio_mixer.c`, `hle_nvn.c`, `nvn_obj.h`, `nvn_tex.c`; the performance-hint lookup in `runtime/android_perf.c` follows nx2android | MIT, copyright 2026 seedds (`nxnative-MIT.txt`; nx2android's notice is the same text) |
| libopus 1.6.1 (official release archive, SHA-256 checked) | Game audio | BSD 3-Clause (`libopus-BSD.txt`) |
| libvpx v1.15.0 | Cutscenes (VP9) | BSD 3-Clause (`libvpx-BSD.txt`); its patent grant is libvpx's `PATENTS` |
| libc++ / libc++abi of the HarmonyOS SDK (LLVM), linked statically | The C++ runtime of the etcpak encoder | Apache-2.0 with LLVM Exceptions, whose exception lifts the notice requirement for what is compiled into a program, so no text is shipped; the text is in the SDK's `llvm/NOTICE` |

## Used only on the PC (not in the app)

| What | Used for | License |
| --- | --- | --- |
| totk-android's d2r code rewriter (`games/totk/third_party/d2r`, from d2r-mobile) | Rewriting the game code on the PC | MIT |
| Ryujinx shader compiler (totk-android's `vendor/ryujinx`, through its `totk_xlate`, built in the .NET SDK container) | Shader translation on the PC | MIT |
| SPIRV-Cross | Shader seed generation on the PC | Apache-2.0 |
| SPIRV-Tools (spirv-val, spirv-opt) and glslang (glslangValidator), in the tools container | Checking and optimizing shaders, compiling this project's own shaders | Apache-2.0; glslang: several permissive licenses (its `LICENSE.txt`) |
| hactool, nsz 5.0.0 | Unpacking and merging the game files on the PC, run as separate tools | ISC; nsz is GPL-3.0 (run only as a separate program, not linked or distributed) |
| capstone 5.0.9, lz4 4.4.5, cryptography 50.0.2 | Rewrite and unpacking tools in the PC containers | BSD 3-Clause, BSD 3-Clause, Apache-2.0 / BSD |
