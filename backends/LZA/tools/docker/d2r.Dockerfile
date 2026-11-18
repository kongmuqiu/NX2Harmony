# Tools for the preparation steps (native-harmonyos/prepare_v143.py): the d2r code rewriter
# (capstone / lz4 pinned like games/totk/requirements.txt of the Android port), its structural checker (llvm-objdump /
# llvm-objcopy), spirv-val / spirv-opt / glslangValidator for the shader seed (validation, SSA, mesh kernels), gcc for
# the export_text host tool, make / git for fetched sources, and what libvpx's configure needs (an
# aarch64 Linux cross gcc for its link tests; the library itself is compiled for aarch64-linux-ohos with the HarmonyOS
# SDK's clang, native-harmonyos/build_vpx_ohos.sh). Self-contained: no private base image.
#   docker build -t nativegames-tools:1 -f tools/docker/d2r.Dockerfile tools/docker   [--build-arg APT_MIRROR=<mirror>]
#     [--build-arg PIP_MIRROR=<package index>]
# python:3.14 (Debian) is the image the other preparation steps already use, so no second base image is pulled.
FROM python:3.14
ENV DEBIAN_FRONTEND=noninteractive
# Where deb.debian.org is slow or unreliable: --build-arg APT_MIRROR=https://<Debian mirror> takes the place of
# http://deb.debian.org (HTTPS: a network that changes plain-HTTP downloads on the way breaks the packages' hashes;
# python:3.14 has ca-certificates), https://deb.debian.org for the default archive over HTTPS; a bare host
# (mirrors.tuna.tsinghua.edu.cn) replaces only the host name, over plain HTTP as before.
# --build-arg PIP_MIRROR=<index URL> for the two pip packages (the default: PyPI). NX2Harmony measures the sources
# on the PC first and passes the fastest (nx2h/mirrors.py).
ARG APT_MIRROR=
ARG PIP_MIRROR=
RUN case "$APT_MIRROR" in \
      "") ;; \
      *://*) sed -i "s|http://deb.debian.org|$APT_MIRROR|g" /etc/apt/sources.list.d/debian.sources ;; \
      *) sed -i "s|deb.debian.org|$APT_MIRROR|g" /etc/apt/sources.list.d/debian.sources ;; \
    esac \
 && printf 'Acquire::Retries "3";\nAcquire::http::Pipeline-Depth "0";\nAcquire::http::No-Cache "true";\nAcquire::BrokenProxy "true";\n' > /etc/apt/apt.conf.d/90-reliable \
 && apt-get update && apt-get install -y --no-install-recommends \
      make git gcc g++ libc6-dev clang llvm gcc-aarch64-linux-gnu libc6-dev-arm64-cross ca-certificates xz-utils \
      spirv-tools glslang-tools \
 && rm -rf /var/lib/apt/lists/* \
 && pip3 install --no-cache-dir ${PIP_MIRROR:+--index-url "$PIP_MIRROR"} capstone==5.0.9 lz4==4.4.5
