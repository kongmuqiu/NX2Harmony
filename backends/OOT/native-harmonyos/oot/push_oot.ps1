# Send The Legend of Zelda: Ocarina of Time's prepared material to the phone (it stays in ignored folders on the PC and in
# the app's sandbox):
#   -GameData  prepared/oot/game-data/{runtime-tables.bin,romfs-config.txt,shared-fonts.txt} -> files/game-oot
#   -Fonts     prepared/oot/shared-fonts/*.bfttf                                            -> files/game-oot/fonts
#   -RomFS     prepared/oot/romfs/{runtime-index.bin,romfs.bin}                             -> files/game-oot
#   -Seed      prepared/oot/seed-build/seed.vkpack (oot_seed.py --pack) and
#              prepared/oot/seed-build/shaders/oot131072/sb-align.txt                       -> files/android/shaders/oot131072
#   -Save      prepared/oot/<backup>/user-data (account.uid + save/<id>/...)                -> files/android/user-data
# The folders are created by libtotk_ohos.so at start (totk_napi.c), so start the app once before the first push.
param([switch]$GameData, [switch]$Fonts, [switch]$RomFS, [switch]$Seed, [string]$Save = '')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\..\ohos_env.ps1"
$files = '/data/storage/el2/base/haps/entry/files'
$prep = Join-Path $port 'prepared\oot'
function Send([string]$local, [string]$remote) {
    $t0 = Get-Date
    $r = (& $hdc @d file send -b $bundle $local $remote | Out-String).Trim()
    "{0} -> {1}: {2} ({3:N0} s)" -f (Split-Path $local -Leaf), $remote, ($r -split "`n")[-1], ((Get-Date) - $t0).TotalSeconds
}
if ($GameData) { foreach ($f in 'runtime-tables.bin', 'romfs-config.txt', 'shared-fonts.txt') { Send "$prep\game-data\$f" "$files/game-oot/$f" } }
if ($Fonts) { Get-ChildItem "$prep\shared-fonts\*.bfttf" | ForEach-Object { Send $_.FullName "$files/game-oot/fonts/$($_.Name)" } }
if ($RomFS) { foreach ($f in 'runtime-index.bin', 'romfs.bin') { Send "$prep\romfs\$f" "$files/game-oot/$f" } }
if ($Seed) {
    $null = & $hdc @d shell "aa force-stop $bundle"   # the app maps the pack it opened
    Send "$prep\seed-build\shaders\oot131072\sb-align.txt" "$files/android/shaders/oot131072/sb-align.txt"
    Send "$prep\seed-build\seed.vkpack" "$files/android/shaders/oot131072/seed.vkpack"
}
if ($Save) {
    $src = Join-Path $prep "$Save\user-data"
    if (-not (Test-Path (Join-Path $src 'account.uid'))) { throw "no user-data backup at $src" }
    $null = & $hdc @d shell "aa force-stop $bundle"
    Send (Join-Path $src 'account.uid') "$files/android/user-data/account.uid"
    Get-ChildItem (Join-Path $src 'save') -Directory | ForEach-Object {
        $id = $_.Name
        Get-ChildItem $_.FullName -File | ForEach-Object { Send $_.FullName "$files/android/user-data/save/$id/$($_.Name)" }
    }
}
