Prebuilt third-party libraries for aarch64-linux-ohos (HarmonyOS), linked into every game's native library.

How a backend uses them
  nx2h/backends/common.py (BaseBackend._prebuilt) copies prebuilt-ohos/native-opus and prebuilt-ohos/native-vpx into a
  backend's artifacts/ folder when that backend has no libopus.a / libvpx.a of its own, so no PC has to build them (that
  needs PowerShell 7, Docker and the network). The backend's native-harmonyos/build_ohos.py then compiles the game's
  native library with -I artifacts/native-opus/source/opus-1.6.1/include and -I artifacts/native-vpx/ohos/include and
  links artifacts/native-opus/build-ohos/opus/libopus.a and artifacts/native-vpx/ohos/lib/libvpx.a into it.

native-opus: libopus 1.6.1, fixed point ("libopus 1.6.1-fixed")
  licence  BSD-3-Clause: native-opus/source/opus-1.6.1/COPYING
  source   the official release archive opus-1.6.1.tar.gz (https://downloads.xiph.org/releases/opus/),
           SHA-256 6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1; include/*.h and COPYING are
           unchanged from it
  build    opus's own CMakeLists, static library: OPUS_FIXED_POINT=ON, OPUS_ENABLE_FLOAT_API=ON,
           OPUS_DISABLE_INTRINSICS=ON, hardening on, assertions off, no programs or tests. Compiled with DevEco Studio's
           OpenHarmony clang 15.0.4 (llvm-project 329916b9) for aarch64-linux-ohos with
             -O2 -DNDEBUG -std=gnu99 -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections -funwind-tables
             -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fno-addrsig -ffixed-x18 -D__MUSL__
             -ffile-prefix-map=<source folder>/=opus-1.6.1/
           No debug information, and thanks to the prefix map no build folder is inside: the assertion messages name
           the sources opus-1.6.1/celt/..., opus-1.6.1/silk/..., opus-1.6.1/src/...
  archive  build-ohos/opus/libopus.a: 132 objects, 808076 bytes,
           SHA-256 af44cebcbda25fc7d6c30d26b96411a970e34730205af119725bacf6a4f438a3

native-vpx: libvpx v1.15.0 (commit 9f9b7e9ba2eb9d01640a9e69a3d655866265cf7f), VP9 decoder only
  licence  BSD-3-Clause with the WebM Project's patent grant: native-vpx/src/libvpx/LICENSE and PATENTS
  build    native-harmonyos/build_vpx_ohos.sh in an Ubuntu container with clang 18.1.3: configured with --enable-pic
           --disable-vp8 --disable-vp9-encoder --enable-vp9-decoder --enable-runtime-cpu-detect (no examples, tools,
           docs, unit tests, webm-io or libyuv), compiled for aarch64-linux-ohos against the HarmonyOS SDK sysroot with
           -ffile-prefix-map for its source and build folders. Its build-configuration string (vpx_codec_build_config)
           keeps the configure line, with the container's install prefix.
  archive  ohos/lib/libvpx.a: 74 objects, 575748 bytes,
           SHA-256 f2369631791e656009510083dda0c510e95079f1a3fdb682e2a5d3b652d0d41f;
           ohos/include/vpx/*.h are unchanged from that commit

Without this folder a backend uses another backend's build where it can, else builds them itself with its
native-harmonyos/build_opus_ohos.ps1 (PowerShell 7) and native-harmonyos/build_vpx_ohos.sh (in its Docker image).
With the same clang, build_opus_ohos.ps1 gives this libopus.a byte for byte.
