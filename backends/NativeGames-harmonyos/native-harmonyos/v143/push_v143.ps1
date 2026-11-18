# Send the 1.4.3 game material to the phone (all of it stays in ignored folders on the PC and in the app's sandbox):
#   -RomFS     prepared/totk-v143/extracted/v143/romfs/{romfs.bin,runtime-index.bin} -> files/game-v143 (17 GB)
#   -GameData  prepared/harmonyos-v143/game-data/{runtime-tables.bin,romfs-config.txt,shared-fonts.txt} -> files/game-v143
#              and prepared/totk-v143/shared-fonts/*.bfttf -> files/game-v143/fonts (totk-ohos)
#   -Seed      prepared/harmonyos-v143/seed-build-sb256/shaders/v143/{seed.vkpack,sb-align.txt} -> files/android/shaders/v143
#   -Patches   prepared/totk-v143/extracted/v143/romfs-patches/** -> files/android/extracted/v143/romfs-patches
#              and each per-screen overlay romfs-patches-<W>x<H> (prepare_v143.py --aspect) beside it (totk-ohos)
# The folders are created by libtotk_ohos.so at start (totk_napi.c), so start the 1.4.3 build once before the first push.
param([switch]$RomFS, [switch]$GameData, [switch]$Seed, [switch]$Patches)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\..\ohos_env.ps1"
$bundle = if ($env:TOTK_OHOS_BUNDLE) { $env:TOTK_OHOS_BUNDLE } else { 'TotZ.NativeGames.harmonyos' }
$files = '/data/storage/el2/base/haps/entry/files'
$port = Join-Path $port 'prepared'   # the prepared/ folder
function Send([string]$local, [string]$remote) {
    $t0 = Get-Date
    $r = (& $hdc @d file send -b $bundle $local $remote | Out-String).Trim()
    "{0} -> {1}: {2} ({3:N0} s)" -f (Split-Path $local -Leaf), $remote, ($r -split "`n")[-1], ((Get-Date) - $t0).TotalSeconds
}
if ($GameData) {
    foreach ($f in 'runtime-tables.bin', 'romfs-config.txt', 'shared-fonts.txt') { Send "$port\harmonyos-v143\game-data\$f" "$files/game-v143/$f" }
    # totk-ohos: the console's shared fonts (prepare_v143.py fonts step) into game-v143/fonts, which the app creates
    # (totk_napi.c TOTK_SHARED_FONTS): Chinese / Korean text reads them, and a missing one stops the game at the title
    $fonts = "$port\totk-v143\shared-fonts"
    if (Test-Path "$fonts\*.bfttf") { Get-ChildItem "$fonts\*.bfttf" | ForEach-Object { Send $_.FullName "$files/game-v143/fonts/$($_.Name)" } }
    else { Write-Warning "$fonts has no .bfttf (prepare_v143.py --only fonts, or nx2h work/shared-fonts): Chinese / Korean text will stop the game" }
}
if ($RomFS) {
    foreach ($f in 'runtime-index.bin', 'romfs.bin') { Send "$port\totk-v143\extracted\v143\romfs\$f" "$files/game-v143/$f" }
}
if ($Seed) {
    foreach ($f in 'sb-align.txt', 'seed.vkpack') { Send "$port\harmonyos-v143\seed-build-sb256\shaders\v143\$f" "$files/android/shaders/v143/$f" }
}
if ($Patches) {
    # totk-ohos: the per-screen overlays too (the app serves the one that matches its screen, totk_napi.c), each manifest first
    $v143 = "$port\totk-v143\extracted\v143"
    $names = @('romfs-patches') + @(Get-ChildItem -Directory $v143 | Where-Object { $_.Name -match '^romfs-patches-\d+x\d+$' } | ForEach-Object { $_.Name })
    foreach ($name in $names) {
        $root = "$v143\$name"
        Get-ChildItem -Recurse -File $root | Sort-Object { $_.FullName -ne "$root\manifest.tsv" }, FullName | ForEach-Object {
            $rel = $_.FullName.Substring($root.Length + 1).Replace('\', '/')
            $dir = [System.IO.Path]::GetDirectoryName($rel)
            if ($dir) {   # the sandbox folders below romfs-patches: hdc cannot create them, mkdir through the app's shell is not possible either
                $null = & $hdc @d shell "mkdir -p /data/app/el2/100/base/$bundle/haps/entry/files/android/extracted/v143/$name/$($dir.Replace('\', '/'))"
            }
            Send $_.FullName "$files/android/extracted/v143/$name/$rel"
        }
    }
}
