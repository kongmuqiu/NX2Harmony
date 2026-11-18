# 第三方代码与许可 · Third-party notices

中文 · [English](#english)

本项目（LZA：鸿蒙上的宝可梦传说 Z-A）自己的代码以 MIT 发布（[LICENSE](LICENSE)）。下面列出项目里包含、编进应用、或构建时在电脑上用到的别人的代码，以及它们的许可。这些都不是游戏内容。

编进应用原生库（`libtotk_ohos.so`）的第三方部分，许可原文都在 `entry/src/main/resources/rawfile/licenses/`，应用的“关于 → 开源许可”逐个列出。例外是从 Khronos Vulkan 注册表生成的枚举名表 `vk_profile_gen.inc`（Apache-2.0 或 MIT，见下表）：它的许可原文应用内暂未附。

## 包含在仓库里

| 内容 | 来源 | 许可 |
| --- | --- | --- |
| 应用（`entry/`）和 `native-harmonyos/` 构建脚本的大部分 | NativeGames-harmonyos（鸿蒙上的王国之泪），本项目由它复制而来 | MIT，版权 2026 NativeGames-harmonyos contributors，条款与 [LICENSE](LICENSE) 相同 |
| `native-harmonyos/overlay/` 中在 totk-android 有同名文件的：那些文件的鸿蒙修改版（编进应用） | [totk-android](https://github.com/AurelioB/totk-android)，`native-harmonyos/UPSTREAM_PIN` 中的提交 | MIT，版权 2026 MacNativeGames contributors；原文见 `native-harmonyos/overlay/LICENSE.totk-android`，应用内附 `licenses/totk-android-MIT.txt` |
| `native-harmonyos/overlay/third_party/d2r_gfx/`：其中 NVN 层文件的修改版（编进应用） | [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)，经 totk-android | MIT，版权 2026 jhaizhou；原文见 `native-harmonyos/overlay/third_party/d2r_gfx/LICENSE`，应用内附 `licenses/d2r-mobile-MIT.txt` |
| `native-harmonyos/overlay/runtime/audio_out_ohos.c`：AudioOut 服务（编进应用） | 改编自 [MacNativeGames](https://github.com/ly05010419/MacNativeGames) 的 `games/xcx/runtime/audio_out.c` | MIT，版权 2026 MacNativeGames contributors；与 `LICENSE.totk-android` 是同一段文字 |
| `native-harmonyos/overlay/runtime/za_hle.c`、`za_shader_dump.c`：照 nxnative 的做法接管 SDK 函数、导出着色器（编进应用） | [nxnative](https://github.com/seedds/nxnative)，见各文件开头的注释 | MIT，版权 2026 seedds；应用内附 `licenses/nxnative-MIT.txt` |
| `native-harmonyos/overlay/android/gfx/vk_profile_gen.inc`：`native-harmonyos/tools/gen_vk_profile.py` 生成的枚举名表（编进应用） | Khronos 的 Vulkan 注册表（Vulkan-Headers 的 `registry/vk.xml`） | Apache-2.0 或 MIT（注册表自带的声明）；许可原文应用内暂未附 |
| `native-harmonyos/za/d2r/`：代码改写器及其检查工具，加了 Z-A 需要的改动（只在电脑上用） | [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) 提交 `9f0c25768ac4b231c6f4cb0a9f182ded68d0e7cf`，经 totk-android 的 `games/totk/third_party/d2r`（同目录 `README.md`）；`patch_nso.py` 的 `site_sp` 出自 nxnative | MIT，版权 2026 jhaizhou，原文见同目录 `LICENSE`；nxnative 部分为 MIT，版权 2026 seedds |
| `native-harmonyos/third_party/etcpak/`：ETC1 / ETC2 编码（编进应用） | [etcpak](https://github.com/wolfpld/etcpak)，提交 `3d716e1550023dc5ff8b602636af8b76584e0b66` | BSD 3-Clause，版权 2013–2026 Bartosz Taudul；原文见同目录 `LICENSE.txt`，应用内附 `licenses/etcpak-BSD-3-Clause.txt` |

## 构建时取用、编进应用（不在仓库里）

`build_ohos.py` 编译 totk-android 的 `games/totk`（`runtime/`、`third_party/d2r_gfx/`、`android/gfx/`、`android/compat/`，上面盖 `native-harmonyos/overlay/`），再链接下面的库。totk-android 里每个文件的来源见它自己的 `THIRD_PARTY_NOTICES.md`。

| 内容 | 在应用里的用处 | 许可（应用内附的原文） |
| --- | --- | --- |
| totk-android（固定提交，`upstream.py` 自动克隆） | 运行时、NVN 层、Vulkan 后端 | MIT，版权 2026 MacNativeGames contributors（`totk-android-MIT.txt`；它所出自的 macOS 项目 MacNativeGames 的许可是同一段文字） |
| [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)（在 totk-android 里） | NVN 层、纹理工具和主机头文件（`third_party/d2r_gfx`），线程代码（`runtime/threads.c`、`gate.S`、`thread_enter.S`） | MIT，版权 2026 jhaizhou（`d2r-mobile-MIT.txt`） |
| h1k421 的 NVN 接口头文件 `nvn.h`（在 totk-android 里） | NVN 层的类型定义 | MIT，版权 2020 h1k421（`NVN-header-MIT.txt`，即头文件自带的声明） |
| deko3d（在 totk-android 里） | `nvn_tex.c` 里的纹理排布尺寸 | zlib，版权 2018–2020 fincs（`deko3d-zlib.txt`） |
| [bcdec](https://github.com/iOrange/bcdec) 0.985（在 totk-android 里） | BC 纹理解码 | MIT 或公有领域（`bcdec-LICENSE.txt`） |
| [SMAA](https://github.com/iryoku/smaa)（在 totk-android 里） | `vk_post_spv.h`、`vk_post_lut.h` 里的抗锯齿着色器和查找表 | MIT（`SMAA-MIT.txt`） |
| AMD FidelityFX FSR 1.0.2（在 totk-android 里） | `vk_post_spv.h` 里的超分着色器 | MIT，版权 2021 Advanced Micro Devices, Inc.（`FidelityFX-FSR-MIT.txt`） |
| Snapdragon Game Super Resolution 1（在 totk-android 里） | `vk_post_spv.h` 里的超分着色器 | BSD 3-Clause，版权 2023 Qualcomm Innovation Center, Inc.（`Snapdragon-GSR-BSD-3-Clause.txt`） |
| Ryujinx.Audio（移植进 totk-android） | 音效和混音：`runtime/audio_effects.c`、`audio_reverb.c`、`audio_mixer.c` | MIT，版权 Ryujinx Team and Contributors（`Ryujinx-Audio-MIT.txt`） |
| [TOTKRecompAndroid](https://github.com/mamcjr/TOTKRecompAndroid)（经 totk-android） | `hle_nvn.c` 的 NVN 节拍适配 | MIT，版权 2026 Peiikai（`TOTKRecompAndroid-MIT.txt`） |
| [LASRecomp-Android-Plus](https://github.com/Nycolazs/LASRecomp-Android-Plus)（经 totk-android） | 着色器缺失时的后台翻译（`vk_shader_xlate.c`）和帧节拍（`vk_record.c`、`fps_auto_block.h`、`hle_nvn.c`）的做法 | MIT，版权 2026 Peiikai（`LASRecomp-Android-Plus-MIT.txt`） |
| [nxnative](https://github.com/seedds/nxnative)、nx2android（经 totk-android） | `runtime/threads.c`、`audio_mixer.c`、`hle_nvn.c`、`nvn_obj.h`、`nvn_tex.c` 里的修正；`runtime/android_perf.c` 的性能提示查找照 nx2android 的做法 | MIT，版权 2026 seedds（`nxnative-MIT.txt`；nx2android 的许可是同一段文字） |
| libopus 1.6.1（官方发布包，按 SHA-256 核对） | 游戏音频 | BSD 3-Clause（`libopus-BSD.txt`） |
| libvpx v1.15.0 | 过场动画（VP9） | BSD 3-Clause（`libvpx-BSD.txt`）；专利授权见 libvpx 的 `PATENTS` |
| 鸿蒙 SDK 的 libc++ / libc++abi（LLVM），静态链接 | etcpak 编码器的 C++ 运行库 | Apache-2.0 with LLVM Exceptions：这一例外免除了编进程序的部分附带许可的要求，所以应用内不附；原文见 SDK 的 `llvm/NOTICE` |

## 只在电脑上用（不进应用）

| 内容 | 用途 | 许可 |
| --- | --- | --- |
| Ryujinx 着色器编译器（totk-android 的 `vendor/ryujinx`，经它的 `totk_xlate`，在 .NET SDK 容器里构建） | 电脑上翻译着色器 | MIT |
| nxnative 的 `dm/host`（`native-harmonyos/za/hle_candidates.py` 读它的接管列表来对照） | 开发时查找还没接管的 SDK 函数 | MIT |
| SPIRV-Cross | 电脑上生成着色器种子 | Apache-2.0 |
| SPIRV-Tools（spirv-val、spirv-opt）、glslang（glslangValidator），装在工具容器里 | 电脑上检查、优化着色器，编译本项目自己的着色器 | Apache-2.0；glslang 为多种宽松许可（见它的 `LICENSE.txt`） |
| Khronos `vk.xml`（Vulkan-Headers 的注册表） | `tools/gen_vk_profile.py` 的输入 | Apache-2.0 或 MIT |
| capstone 5.0.9、lz4 4.4.5（工具容器）、zstandard（`tools/nup_extract.py`） | 电脑上的改写和解包工具 | BSD 3-Clause |
| unicorn（`za/d2r/equiv.py`，只用于等价检查，工具容器里不装） | 电脑上核对改写结果 | GPL-2.0（仅作为独立工具使用，不链接进应用、不随应用分发） |

---

<a id="english"></a>

[中文](#第三方代码与许可--third-party-notices) · English

This project's own code (LZA: Pokémon Legends: Z-A on HarmonyOS) is released under MIT ([LICENSE](LICENSE)). Below is
the code by others that the project contains, compiles into the app, or uses on the PC at build time, with its license.
None of it is game content.

The third-party parts compiled into the app's native library (`libtotk_ohos.so`) have their license texts in
`entry/src/main/resources/rawfile/licenses/`; the app lists them under 关于 → 开源许可 (About → open-source licenses).
The exception is `vk_profile_gen.inc`, the enum name table generated from the Khronos Vulkan registry (Apache-2.0 OR
MIT, see the table): its license text is not yet shipped in the app.

## In the repository

| What | Source | License |
| --- | --- | --- |
| Most of the app (`entry/`) and of the build scripts in `native-harmonyos/` | NativeGames-harmonyos (Tears of the Kingdom on HarmonyOS), which this project was made from | MIT, copyright 2026 NativeGames-harmonyos contributors, the same terms as [LICENSE](LICENSE) |
| The files in `native-harmonyos/overlay/` that totk-android also has: HarmonyOS versions of them (compiled into the app) | [totk-android](https://github.com/AurelioB/totk-android), the commit in `native-harmonyos/UPSTREAM_PIN` | MIT, copyright 2026 MacNativeGames contributors; text in `native-harmonyos/overlay/LICENSE.totk-android`, shipped in the app as `licenses/totk-android-MIT.txt` |
| `native-harmonyos/overlay/third_party/d2r_gfx/`: changed files of its NVN layer (compiled into the app) | [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile), through totk-android | MIT, copyright 2026 jhaizhou; text in `native-harmonyos/overlay/third_party/d2r_gfx/LICENSE`, shipped as `licenses/d2r-mobile-MIT.txt` |
| `native-harmonyos/overlay/runtime/audio_out_ohos.c`: the AudioOut server (compiled into the app) | Adapted from [MacNativeGames](https://github.com/ly05010419/MacNativeGames)' `games/xcx/runtime/audio_out.c` | MIT, copyright 2026 MacNativeGames contributors; the same text as `LICENSE.totk-android` |
| `native-harmonyos/overlay/runtime/za_hle.c`, `za_shader_dump.c`: SDK takeovers and the shader dump, following nxnative (compiled into the app) | [nxnative](https://github.com/seedds/nxnative), as the files' first comments say | MIT, copyright 2026 seedds; shipped as `licenses/nxnative-MIT.txt` |
| `native-harmonyos/overlay/android/gfx/vk_profile_gen.inc`: name tables generated by `native-harmonyos/tools/gen_vk_profile.py` (compiled into the app) | The Khronos Vulkan registry (`registry/vk.xml` of Vulkan-Headers) | Apache-2.0 OR MIT (the registry's own notice); its license text is not yet shipped in the app |
| `native-harmonyos/za/d2r/`: the code rewriter and its checkers, with the changes Z-A needs (PC only) | [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) commit `9f0c25768ac4b231c6f4cb0a9f182ded68d0e7cf`, through totk-android's `games/totk/third_party/d2r` (`README.md` in that folder); `patch_nso.py`'s `site_sp` from nxnative | MIT, copyright 2026 jhaizhou, text in `LICENSE` in that folder; the nxnative part: MIT, copyright 2026 seedds |
| `native-harmonyos/third_party/etcpak/`: ETC1 / ETC2 encoding (compiled into the app) | [etcpak](https://github.com/wolfpld/etcpak), commit `3d716e1550023dc5ff8b602636af8b76584e0b66` | BSD 3-Clause, copyright 2013–2026 Bartosz Taudul; text in `LICENSE.txt` in that folder, shipped as `licenses/etcpak-BSD-3-Clause.txt` |

## Taken at build time and compiled into the app (not in the repository)

`build_ohos.py` compiles totk-android's `games/totk` (`runtime/`, `third_party/d2r_gfx/`, `android/gfx/`,
`android/compat/`, with `native-harmonyos/overlay/` on top) and links the libraries below. totk-android's own
`THIRD_PARTY_NOTICES.md` says where each of its files comes from.

| What | Used for in the app | License (text shipped in the app) |
| --- | --- | --- |
| totk-android (pinned commit, cloned by `upstream.py`) | The runtime, the NVN layer and the Vulkan backend | MIT, copyright 2026 MacNativeGames contributors (`totk-android-MIT.txt`; MacNativeGames, the macOS project it comes from, has the same text) |
| [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) (inside totk-android) | The NVN layer, texture tools and host headers (`third_party/d2r_gfx`); thread code (`runtime/threads.c`, `gate.S`, `thread_enter.S`) | MIT, copyright 2026 jhaizhou (`d2r-mobile-MIT.txt`) |
| The NVN API header `nvn.h` by h1k421 (inside totk-android) | The NVN layer's type definitions | MIT, copyright 2020 h1k421 (`NVN-header-MIT.txt`, the header's own notice) |
| deko3d (inside totk-android) | Texture layout sizes in `nvn_tex.c` | zlib, copyright 2018–2020 fincs (`deko3d-zlib.txt`) |
| [bcdec](https://github.com/iOrange/bcdec) 0.985 (inside totk-android) | BC texture decoding | MIT or public domain (`bcdec-LICENSE.txt`) |
| [SMAA](https://github.com/iryoku/smaa) (inside totk-android) | Anti-aliasing shaders and lookup tables in `vk_post_spv.h`, `vk_post_lut.h` | MIT (`SMAA-MIT.txt`) |
| AMD FidelityFX FSR 1.0.2 (inside totk-android) | Upscaling shaders in `vk_post_spv.h` | MIT, copyright 2021 Advanced Micro Devices, Inc. (`FidelityFX-FSR-MIT.txt`) |
| Snapdragon Game Super Resolution 1 (inside totk-android) | Upscaling shader in `vk_post_spv.h` | BSD 3-Clause, copyright 2023 Qualcomm Innovation Center, Inc. (`Snapdragon-GSR-BSD-3-Clause.txt`) |
| Ryujinx.Audio (ported into totk-android) | Audio effects and mixer: `runtime/audio_effects.c`, `audio_reverb.c`, `audio_mixer.c` | MIT, copyright Ryujinx Team and Contributors (`Ryujinx-Audio-MIT.txt`) |
| [TOTKRecompAndroid](https://github.com/mamcjr/TOTKRecompAndroid) (through totk-android) | The NVN pacing adapter in `hle_nvn.c` | MIT, copyright 2026 Peiikai (`TOTKRecompAndroid-MIT.txt`) |
| [LASRecomp-Android-Plus](https://github.com/Nycolazs/LASRecomp-Android-Plus) (through totk-android) | The design of the shader-miss worker (`vk_shader_xlate.c`) and of frame pacing (`vk_record.c`, `fps_auto_block.h`, `hle_nvn.c`) | MIT, copyright 2026 Peiikai (`LASRecomp-Android-Plus-MIT.txt`) |
| [nxnative](https://github.com/seedds/nxnative), nx2android (through totk-android) | Fixes in `runtime/threads.c`, `audio_mixer.c`, `hle_nvn.c`, `nvn_obj.h`, `nvn_tex.c`; the performance-hint lookup in `runtime/android_perf.c` follows nx2android | MIT, copyright 2026 seedds (`nxnative-MIT.txt`; nx2android's notice is the same text) |
| libopus 1.6.1 (official release archive, SHA-256 checked) | Game audio | BSD 3-Clause (`libopus-BSD.txt`) |
| libvpx v1.15.0 | Cutscenes (VP9) | BSD 3-Clause (`libvpx-BSD.txt`); its patent grant is libvpx's `PATENTS` |
| libc++ / libc++abi of the HarmonyOS SDK (LLVM), linked statically | The C++ runtime of the etcpak encoder | Apache-2.0 with LLVM Exceptions, whose exception lifts the notice requirement for what is compiled into a program, so no text is shipped; the text is in the SDK's `llvm/NOTICE` |

## Used only on the PC (not in the app)

| What | Used for | License |
| --- | --- | --- |
| Ryujinx shader compiler (totk-android's `vendor/ryujinx`, through its `totk_xlate`, built in the .NET SDK container) | Shader translation on the PC | MIT |
| nxnative's `dm/host` (`native-harmonyos/za/hle_candidates.py` compares against its takeovers) | Finding SDK functions not taken over yet, during development | MIT |
| SPIRV-Cross | Shader seed generation on the PC | Apache-2.0 |
| SPIRV-Tools (spirv-val, spirv-opt) and glslang (glslangValidator), in the tools container | Checking and optimizing shaders, compiling this project's own shaders | Apache-2.0; glslang: several permissive licenses (its `LICENSE.txt`) |
| Khronos `vk.xml` (the Vulkan-Headers registry) | Input of `tools/gen_vk_profile.py` | Apache-2.0 OR MIT |
| capstone 5.0.9, lz4 4.4.5 (tools container), zstandard (`tools/nup_extract.py`) | Rewrite and unpacking tools on the PC | BSD 3-Clause |
| unicorn (`za/d2r/equiv.py`, equivalence checks only; not installed in the tools container) | Checking the rewrite on the PC | GPL-2.0 (used only as a separate tool, not linked into or shipped with the app) |
