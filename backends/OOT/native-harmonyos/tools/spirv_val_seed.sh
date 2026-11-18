#!/bin/sh
# Validate every SPIR-V module of a seed folder with spirv-val and move the ones that fail, with their .vk, into
# <seed>/../invalid-<folder name>/: a driver given an invalid module may crash, while a variant that is not in the seed
# only skips its draws (vk_shader.c "SPIR-V or reflection missing"). The target env follows the module's version: Vulkan
# 1.1 for SPIR-V 1.3 (what the translator emits), Vulkan 1.3 for 1.4 and up (global memory through uvec2 pointers,
# prepare_xlate_ns2.py). The failures go to <seed>/../spirv-val.txt (variant, first error line). Runs in
# nativegames-tools:1 (spirv-val). With a list file (one .spv name a line) only those modules are checked.
# SPIRV_VAL_JOBS modules are checked at a time (default: the container's CPUs): one after another, Ocarina of Time's
# 42,328 modules took over an hour through the PC's folder mount (each module starts od, tr and spirv-val).
#   sh spirv_val_seed.sh <seed shaders/<version> dir> [list file]
set -eu
if [ "${1:-}" = "--one" ]; then   # xargs: some modules; one line each, "ok" or "bad<TAB>name<TAB>first error line"
  bad=$2; shift 2
  for s in "$@"; do
    ver=$(od -An -tx4 -j4 -N4 "$s" | tr -d ' \n')
    env=vulkan1.1; [ "$ver" \> "000103ff" ] && env=vulkan1.3
    if e=$(spirv-val --target-env $env "$s" 2>&1); then
      echo ok
    else
      printf 'bad\t%s\t%s\n' "$(basename "$s")" "$(printf '%s' "$e" | tr '\n' ' ' | cut -c1-240)"
      mv "$s" "$bad/"
      v="${s%.spv}.vk"; if [ -f "$v" ]; then mv "$v" "$bad/"; fi
    fi
  done
  exit 0
fi
seed=$1
list=${2:-}
jobs=${SPIRV_VAL_JOBS:-$(nproc 2>/dev/null || echo 4)}
out=$(dirname "$seed")/spirv-val.txt
bad=$(dirname "$seed")/invalid-$(basename "$seed")
todo=$(dirname "$seed")/spirv-val.todo
res=$(dirname "$seed")/spirv-val.results
mkdir -p "$bad"
if [ -n "$list" ]; then
  tr -d '\r' < "$list" | while IFS= read -r name; do if [ -f "$seed/$name" ]; then printf '%s\n' "$seed/$name"; fi; done > "$todo"
else
  ls "$seed" | grep '\.spv$' | sed "s|^|$seed/|" > "$todo" || true
fi
total=$(wc -l < "$todo")
# through cat: each worker line is one write into a pipe, so lines of parallel workers never mix
xargs -P "$jobs" -n 32 sh "$0" --one "$bad" < "$todo" | cat > "$res"
grep '^bad' "$res" | cut -f2- > "$out" || true
n=$(wc -l < "$res"); f=$(grep -c '^bad' "$res" || true)
rm -f "$todo" "$res"
if [ "$n" -ne "$total" ]; then echo "spirv-val: only $n of $total modules were checked" >&2; exit 1; fi
echo "$n modules, $f invalid (moved to $bad, listed in $out)"
