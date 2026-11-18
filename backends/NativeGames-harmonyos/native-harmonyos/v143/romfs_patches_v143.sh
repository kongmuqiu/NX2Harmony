#!/bin/sh
# 1.4.3 for the HarmonyOS port, step 3: the RomFS overlay files the runtime serves in place of the originals
# (runtime/romfs.c, TOTK_ROMFS_PATCHES, default extracted/v143/romfs-patches under the working folder), made by the
# reference's own scripts/make_romfs_patches.py --version v143 (the quick-menu capture keys at 60 FPS, verified against
# the merged image). The script finds its input as <games/totk>/extracted/v143/romfs, so it runs from a copy of the
# reference's scripts folder next to a link to the merged RomFS. No output-size heap overlays: HarmonyOS keeps the
# game's 1920x1080 output (game_patches_v143.c, the iOS rule), whose heaps fit.
# totk-ohos: with <W>x<H> (a landscape screen wider than 16:9, prepare_v143.py --aspect) it makes the per-screen overlay
# extracted/v143/romfs-patches-<W>x<H> instead: the same files, with the UI layouts laid out for that screen by
# v143/ultrawide_layout.py, copied over the reference's scripts/ultrawide_layout.py for this run only (the reference's
# edits plus more full-screen panes; --aspect <W>x<H> --hud corner: layouts narrowed to 16:9, backgrounds and fades as
# wide as the screen, the HUD at the screen's edges), and v143/title_layouts_v143.py gives the Title archive's copies of
# those layouts the same edits (one more file in that overlay). totk_napi.c serves it instead of romfs-patches when
# TOTK_ASPECT matches it and TOTK_HUD is not center, and turns the runtime's UI squeeze off (TOTK_UI_SQUEEZE=0).
# Container: python:3.14 with /android (reference, read-only), /work (prepared/totk-v143), /venv.
#   sh romfs_patches_v143.sh [<W>x<H>]
set -eu
A=${1:-}
case "$A" in
    "") set -- ;;
    *[!0-9x]* | 0* | x* | *x | *x0* | *x*x*) echo "romfs_patches_v143.sh: expected <width>x<height>, got '$A'" >&2; exit 2 ;;
    *x*) set -- --aspect "$A" --hud corner ;;
    *) echo "romfs_patches_v143.sh: expected <width>x<height>, got '$A'" >&2; exit 2 ;;
esac
OUT=/work/extracted/v143/romfs-patches${A:+-$A}
R=/tmp/totk
rm -rf "$R"; mkdir -p "$R/extracted/v143"
cp -r /android/games/totk/scripts "$R/scripts"
[ -z "$A" ] || cp "$(dirname "$0")/ultrawide_layout.py" "$R/scripts/ultrawide_layout.py"
ln -s /work/extracted/v143/romfs "$R/extracted/v143/romfs"
rm -rf "$OUT"
cd "$R"
/venv/bin/python scripts/make_romfs_patches.py --version v143 --output-dir "$OUT" "$@"
# totk-ohos: the Title archive's copies of those layouts (the title screen, its menus, the first loading screen) edited the same way
[ -z "$A" ] || /venv/bin/python "$(dirname "$0")/title_layouts_v143.py" /work/extracted/v143/romfs "$OUT" "$A"
ls -la "$OUT"; cat "$OUT/manifest.tsv"
