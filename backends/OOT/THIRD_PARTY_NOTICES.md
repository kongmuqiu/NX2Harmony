# 第三方代码与许可 · Third-party notices

中文 · [English](#english)

本项目（OOT：鸿蒙上的塞尔达传说 时之笛）自己的代码以 MIT 发布（[LICENSE](LICENSE)）。下面列出项目里包含、编进应用、或构建时在电脑上用到的别人的代码，以及它们的许可。这些都不是游戏内容。

编进应用原生库（`libtotk_ohos.so`）的第三方部分，许可原文在 `entry/src/main/resources/rawfile/licenses/`，应用的“关于 → 开源许可”逐个列出（pokopia-native 除外，见下表）。

## 包含在仓库里

| 内容 | 来源 | 许可 |
| --- | --- | --- |
| 应用（`entry/`）和 `native-harmonyos/` 构建脚本的大部分 | NativeGames-harmonyos（鸿蒙上的王国之泪），本项目由它复制而来 | MIT，版权 2026 NativeGames-harmonyos contributors，条款与 [LICENSE](LICENSE) 相同 |
| `native-harmonyos/overlay/` 中在 totk-android 有同名文件的：那些文件的鸿蒙修改版（编进应用） | [totk-android](https://github.com/AurelioB/totk-android)，`native-harmonyos/UPSTREAM_PIN` 中的提交 | MIT，版权 2026 MacNativeGames contributors；原文见 `native-harmonyos/overlay/LICENSE.totk-android`，应用内附 `licenses/totk-android-MIT.txt` |
| `native-harmonyos/overlay/third_party/d2r_gfx/`：其中 NVN 层文件的修改版（编进应用） | [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)，经 totk-android | MIT，版权 2026 jhaizhou；原文见 `native-harmonyos/overlay/third_party/d2r_gfx/LICENSE`，应用内附 `licenses/d2r-mobile-MIT.txt` |
| `native-harmonyos/overlay/runtime/ns2_host.c`、`ns2_codec.c`、`ns2_opus_ms.c`、`offline_host.c`（Switch 2 主机和离线服务）、`audio_out_ohos.c`（AudioOut 服务，改编自 `games/xcx/runtime/audio_out.c`），都编进应用；`overlay/third_party/d2r_gfx/gfx/nvn/nvn_ctl.h` | [MacNativeGames](https://github.com/ly05010419/MacNativeGames) 的 `games/totk` 等 | MIT，版权 2026 MacNativeGames contributors；与 `LICENSE.totk-android` 是同一段文字 |
| 从 nxnative 移植的 Switch 2 主机代码：`overlay/runtime/ns2_av1.c`、`ns2_av1.h`、`ns2_audio.c`、`ns2_sdk22.c`，以及 `ns2_host.c`、`ns2_codec.c`、`offline_host.c` 和其他 overlay 文件里注明出自 nxnative 的部分（编进应用） | [nxnative](https://github.com/seedds/nxnative)，见各文件开头的注释 | MIT，版权 2026 seedds；应用内附 `licenses/nxnative-MIT.txt` |
| `overlay/runtime/ns2_host.c` 里宝可梦 Pokopia 用的 Switch 2 SDK 桩函数（编进应用） | [pokopia-native](https://github.com/xFractals/pokopia-native) 给 nxnative 的补丁 | MIT（据该文件的注释）；它的许可原文这里没有，应用内暂未附 |
| `native-harmonyos/third_party/dav1d/`：AV1 解码器，由 `build_dav1d_ohos.sh` 编成 `libdav1d.a` 链进应用 | [dav1d](https://code.videolan.org/videolan/dav1d) 1.5.4（`54706fc`），即 nxnative 的 `dm/third_party/dav1d` 的 arm64 子集；`config-ohos/` 是本项目的鸿蒙配置 | BSD 2-Clause，版权 2018–2025 VideoLAN and dav1d authors；原文见同目录 `COPYING`，应用内附 `licenses/dav1d-BSD-2-Clause.txt` |
| `native-harmonyos/oot/d2r/`：代码改写器及其检查工具，加了需要的改动（只在电脑上用） | [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) 提交 `9f0c25768ac4b231c6f4cb0a9f182ded68d0e7cf`，经 totk-android 的 `games/totk/third_party/d2r`（同目录 `README.md`）；`patch_nso.py` 的 `site_sp` 和符号计数出自 nxnative | MIT，版权 2026 jhaizhou，原文见同目录 `LICENSE`；nxnative 部分为 MIT，版权 2026 seedds |
| `native-harmonyos/third_party/etcpak/`：ETC1 / ETC2 编码（编进应用） | [etcpak](https://github.com/wolfpld/etcpak)，提交 `3d716e1550023dc5ff8b602636af8b76584e0b66` | BSD 3-Clause，版权 2013–2026 Bartosz Taudul；原文见同目录 `LICENSE.txt`，应用内附 `licenses/etcpak-BSD-3-Clause.txt` |

## 构建时取用、编进应用（不在仓库里）

`build_ohos.py` 编译 totk-android 的 `games/totk`（`runtime/`、`third_party/d2r_gfx/`、`android/gfx/`、`android/compat/`，上面盖 `native-harmonyos/overlay/`），再链接下面的库和上面的 dav1d。totk-android 里每个文件的来源见它自己的 `THIRD_PARTY_NOTICES.md`。

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
| libopus 1.6.1（官方发布包，按 SHA-256 核对） | 游戏音频（含 Opus 多流） | BSD 3-Clause（`libopus-BSD.txt`） |
| libvpx v1.15.0 | 视频（VP9） | BSD 3-Clause（`libvpx-BSD.txt`）；专利授权见 libvpx 的 `PATENTS` |
| 鸿蒙 SDK 的 libc++ / libc++abi（LLVM），静态链接 | etcpak 编码器的 C++ 运行库 | Apache-2.0 with LLVM Exceptions：这一例外免除了编进程序的部分附带许可的要求，所以应用内不附；原文见 SDK 的 `llvm/NOTICE` |

## 只在电脑上用（不进应用）

| 内容 | 用途 | 许可 |
| --- | --- | --- |
| Ryujinx 着色器编译器：nxnative 的 `dm/third_party/ryujinx`（Ryujinx 1.3.2 加 nxnative 的修正和 Ampere 前端），配 totk-android 的 `totk_xlate` 服务和补丁，由 `xlate/prepare_xlate_ns2.py` 准备、在 .NET SDK 容器里构建；`NXNATIVE` 指向 Swap2-Native（mamcjr/Swap2-Native）时用它的 | 电脑上翻译 Switch 2 着色器 | MIT |
| nxnative 的着色器工具（`dm/gfx/tools`：`shader_split.py`、`dmsh_meshstage.py` 等，和它们导入的部分），由 `oot/oot_seed.py`、`oot/oot_mesh.py` 调用 | 电脑上拆分着色器、准备网格着色器 | MIT，版权 2026 seedds |
| SPIRV-Cross | 电脑上生成着色器种子 | Apache-2.0 |
| SPIRV-Tools（spirv-val、spirv-opt）、glslang（glslangValidator），装在工具容器里 | 电脑上检查、优化着色器，编译网格着色器包装和本项目自己的着色器 | Apache-2.0；glslang 为多种宽松许可（见它的 `LICENSE.txt`） |
| Mesa 的 lavapipe（`tools/mesh_selftest` 用的容器） | 只用于离线测试 | Mesa 自己的许可（以 MIT 为主） |
| capstone 5.0.9、lz4 4.4.5（工具容器）、zstandard（`tools/nup_extract.py`） | 电脑上的改写和解包工具 | BSD 3-Clause |
| zbic（`oot/nso_unzbic.py`） | 电脑上解开 Switch 2 模块的 ZBIC 段 | 以该包自己的许可为准（此处未记录） |
| unicorn（`oot/d2r/equiv.py`，只用于等价检查，工具容器里不装） | 电脑上核对改写结果 | GPL-2.0（仅作为独立工具使用，不链接进应用、不随应用分发） |

---

<a id="english"></a>

[中文](#第三方代码与许可--third-party-notices) · English

This project's own code (OOT: The Legend of Zelda: Ocarina of Time on HarmonyOS) is released under MIT
([LICENSE](LICENSE)). Below is the code by others that the project contains, compiles into the app, or uses on the PC
at build time, with its license. None of it is game content.

The third-party parts compiled into the app's native library (`libtotk_ohos.so`) have their license texts in
`entry/src/main/resources/rawfile/licenses/`; the app lists them under 关于 → 开源许可 (About → open-source licenses).
pokopia-native is the exception (see the table).

## In the repository

| What | Source | License |
| --- | --- | --- |
| Most of the app (`entry/`) and of the build scripts in `native-harmonyos/` | NativeGames-harmonyos (Tears of the Kingdom on HarmonyOS), which this project was made from | MIT, copyright 2026 NativeGames-harmonyos contributors, the same terms as [LICENSE](LICENSE) |
| The files in `native-harmonyos/overlay/` that totk-android also has: HarmonyOS versions of them (compiled into the app) | [totk-android](https://github.com/AurelioB/totk-android), the commit in `native-harmonyos/UPSTREAM_PIN` | MIT, copyright 2026 MacNativeGames contributors; text in `native-harmonyos/overlay/LICENSE.totk-android`, shipped in the app as `licenses/totk-android-MIT.txt` |
| `native-harmonyos/overlay/third_party/d2r_gfx/`: changed files of its NVN layer (compiled into the app) | [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile), through totk-android | MIT, copyright 2026 jhaizhou; text in `native-harmonyos/overlay/third_party/d2r_gfx/LICENSE`, shipped as `licenses/d2r-mobile-MIT.txt` |
| `native-harmonyos/overlay/runtime/ns2_host.c`, `ns2_codec.c`, `ns2_opus_ms.c`, `offline_host.c` (the Switch 2 host and the offline services), `audio_out_ohos.c` (the AudioOut server, adapted from `games/xcx/runtime/audio_out.c`), all compiled into the app; `overlay/third_party/d2r_gfx/gfx/nvn/nvn_ctl.h` | [MacNativeGames](https://github.com/ly05010419/MacNativeGames)' `games/totk` and others | MIT, copyright 2026 MacNativeGames contributors; the same text as `LICENSE.totk-android` |
| Switch 2 host code ported from nxnative: `overlay/runtime/ns2_av1.c`, `ns2_av1.h`, `ns2_audio.c`, `ns2_sdk22.c`, and the parts of `ns2_host.c`, `ns2_codec.c`, `offline_host.c` and other overlay files that name nxnative (compiled into the app) | [nxnative](https://github.com/seedds/nxnative), as the files' first comments say | MIT, copyright 2026 seedds; shipped as `licenses/nxnative-MIT.txt` |
| The Switch 2 SDK stubs for Pokémon Pokopia in `overlay/runtime/ns2_host.c` (compiled into the app) | [pokopia-native](https://github.com/xFractals/pokopia-native)'s nxnative patch | MIT (as that file's comment says); its license text is not here and not yet shipped in the app |
| `native-harmonyos/third_party/dav1d/`: the AV1 decoder, built into `libdav1d.a` by `build_dav1d_ohos.sh` and linked into the app | [dav1d](https://code.videolan.org/videolan/dav1d) 1.5.4 (`54706fc`), the arm64 subset of nxnative's `dm/third_party/dav1d`; `config-ohos/` is this project's HarmonyOS configuration | BSD 2-Clause, copyright 2018–2025 VideoLAN and dav1d authors; text in `COPYING` in that folder, shipped as `licenses/dav1d-BSD-2-Clause.txt` |
| `native-harmonyos/oot/d2r/`: the code rewriter and its checkers, with the changes needed (PC only) | [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) commit `9f0c25768ac4b231c6f4cb0a9f182ded68d0e7cf`, through totk-android's `games/totk/third_party/d2r` (`README.md` in that folder); `patch_nso.py`'s `site_sp` and its symbol count from nxnative | MIT, copyright 2026 jhaizhou, text in `LICENSE` in that folder; the nxnative parts: MIT, copyright 2026 seedds |
| `native-harmonyos/third_party/etcpak/`: ETC1 / ETC2 encoding (compiled into the app) | [etcpak](https://github.com/wolfpld/etcpak), commit `3d716e1550023dc5ff8b602636af8b76584e0b66` | BSD 3-Clause, copyright 2013–2026 Bartosz Taudul; text in `LICENSE.txt` in that folder, shipped as `licenses/etcpak-BSD-3-Clause.txt` |

## Taken at build time and compiled into the app (not in the repository)

`build_ohos.py` compiles totk-android's `games/totk` (`runtime/`, `third_party/d2r_gfx/`, `android/gfx/`,
`android/compat/`, with `native-harmonyos/overlay/` on top) and links the libraries below and dav1d (above).
totk-android's own `THIRD_PARTY_NOTICES.md` says where each of its files comes from.

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
| libopus 1.6.1 (official release archive, SHA-256 checked) | Game audio (multistream included) | BSD 3-Clause (`libopus-BSD.txt`) |
| libvpx v1.15.0 | Video (VP9) | BSD 3-Clause (`libvpx-BSD.txt`); its patent grant is libvpx's `PATENTS` |
| libc++ / libc++abi of the HarmonyOS SDK (LLVM), linked statically | The C++ runtime of the etcpak encoder | Apache-2.0 with LLVM Exceptions, whose exception lifts the notice requirement for what is compiled into a program, so no text is shipped; the text is in the SDK's `llvm/NOTICE` |

## Used only on the PC (not in the app)

| What | Used for | License |
| --- | --- | --- |
| Ryujinx shader compiler: nxnative's `dm/third_party/ryujinx` (Ryujinx 1.3.2 with nxnative's fixes and Ampere frontend), with totk-android's `totk_xlate` service and patches, prepared by `xlate/prepare_xlate_ns2.py` and built in the .NET SDK container; Swap2-Native's (mamcjr/Swap2-Native) when `NXNATIVE` points at it | Translating the Switch 2 shaders on the PC | MIT |
| nxnative's shader tools (`dm/gfx/tools`: `shader_split.py`, `dmsh_meshstage.py` and others, with what they import), run by `oot/oot_seed.py` and `oot/oot_mesh.py` | Splitting the shaders and preparing the mesh shaders on the PC | MIT, copyright 2026 seedds |
| SPIRV-Cross | Shader seed generation on the PC | Apache-2.0 |
| SPIRV-Tools (spirv-val, spirv-opt) and glslang (glslangValidator), in the tools container | Checking and optimizing shaders, compiling the mesh shader wrappers and this project's own shaders | Apache-2.0; glslang: several permissive licenses (its `LICENSE.txt`) |
| Mesa's lavapipe (the container of `tools/mesh_selftest`) | Offline tests only | Mesa's own licenses (mostly MIT) |
| capstone 5.0.9, lz4 4.4.5 (tools container), zstandard (`tools/nup_extract.py`) | Rewrite and unpacking tools on the PC | BSD 3-Clause |
| zbic (`oot/nso_unzbic.py`) | Unpacking the ZBIC segments of Switch 2 modules on the PC | The package's own license (not recorded here) |
| unicorn (`oot/d2r/equiv.py`, equivalence checks only; not installed in the tools container) | Checking the rewrite on the PC | GPL-2.0 (used only as a separate tool, not linked into or shipped with the app) |
