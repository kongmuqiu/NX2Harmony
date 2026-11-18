#!/bin/sh
# Promote the function variables of every SPIR-V module of a seed folder to SSA values with spirv-opt (local single-block /
# single-store / multi-store elimination, aggressive dead code elimination, block merging, instruction simplification),
# and keep the result when spirv-val passes and it is smaller. The translator (Ryujinx) declares every temporary as a
# function variable; Maleoon's compiler (libbishenggpucompiler) is very slow on such functions: Ocarina of Time's compute
# shader a22751e0034b3981 (685 KB, 8258 variables) held a build for minutes; after these passes it is 305 KB with 684.
# No inlining or unrolling (spirv-opt -O / -Os made the movie networks bigger). The originals go to <seed>/../pre-opt-<name>/.
# Runs in nativegames-tools:1 (spirv-opt, spirv-val):  sh spirv_opt_seed.sh <seed shaders/<version> dir> [jobs] [list file]
set -eu
if [ "$1" = "--one" ]; then   # xargs: one module
  keep=$2; s=$3
  t=/tmp/opt-$$.spv
  ver=$(od -An -tx4 -j4 -N4 "$s" | tr -d ' \n')
  env=vulkan1.1; [ "$ver" \> "000103ff" ] && env=vulkan1.3
  if spirv-opt --target-env=$env --eliminate-local-single-block --eliminate-local-single-store --eliminate-local-multi-store \
       --eliminate-dead-code-aggressive --merge-blocks --simplify-instructions --eliminate-dead-code-aggressive "$s" -o "$t" 2>/dev/null \
     && spirv-val --target-env $env "$t" 2>/dev/null && [ "$(stat -c %s "$t")" -lt "$(stat -c %s "$s")" ]; then
    cp "$s" "$keep/" && mv "$t" "$s" && echo "opt $(basename "$s")"
  else
    rm -f "$t"; echo "kept $(basename "$s")"
  fi
  exit 0
fi
seed=$1
jobs=${2:-8}
list=${3:-}
keep=$(dirname "$seed")/pre-opt-$(basename "$seed")
out=$(dirname "$seed")/spirv-opt.txt
mkdir -p "$keep"
if [ -n "$list" ]; then tr -d '\r' < "$list"; else ls "$seed" | grep '\.spv$'; fi | sed "s|^|$seed/|" | \
  xargs -P "$jobs" -n 1 sh "$0" --one "$keep" > "$out" 2>&1
echo "$(grep -c '^opt ' "$out") optimized, $(grep -c '^kept ' "$out") kept (originals of the optimized in $keep)"
