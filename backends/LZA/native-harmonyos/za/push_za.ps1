# Send Pokémon Legends: Z-A's prepared material to the phone (it stays in ignored folders on the PC and in the app's sandbox):
#   -GameData  prepared/za/game-data/{runtime-tables.bin,romfs-config.txt,shared-fonts.txt} -> files/game-za
#   -Fonts     prepared/za/shared-fonts/*.bfttf                                           -> files/game-za/fonts
#   -RomFS     prepared/za/romfs/{runtime-index.bin,romfs.bin} (6 GB)                      -> files/game-za
#   -Seed      prepared/za/seed-build/seed.vkpack (1.8 GB, za_seed.py / za_reseed.py) and
#              prepared/za/seed-build/shaders/za202/sb-align.txt                           -> files/android/shaders/za202
#   -Save      prepared/za/<backup>/user-data (account.uid + save/<id>/main, e.g. phone-backup-20261109) -> files/android/user-data
# The folders are created by libtotk_ohos.so at start (totk_napi.c), so start the app once before the first push; for
# -Save start it once to the title (the runtime makes user-data/save/<id>), then send: it replaces that save.
param([switch]$GameData, [switch]$Fonts, [switch]$RomFS, [switch]$Seed, [string]$Save = '')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\..\ohos_env.ps1"
$files = '/data/storage/el2/base/haps/entry/files'
$prep = Join-Path $port 'prepared\za'
function Send([string]$local, [string]$remote) {
    $t0 = Get-Date
    $r = (& $hdc @d file send -b $bundle $local $remote | Out-String).Trim()
    "{0} -> {1}: {2} ({3:N0} s)" -f (Split-Path $local -Leaf), $remote, ($r -split "`n")[-1], ((Get-Date) - $t0).TotalSeconds
}
if ($GameData) { foreach ($f in 'runtime-tables.bin', 'romfs-config.txt', 'shared-fonts.txt') { Send "$prep\game-data\$f" "$files/game-za/$f" } }
if ($Fonts) { Get-ChildItem "$prep\shared-fonts\*.bfttf" | ForEach-Object { Send $_.FullName "$files/game-za/fonts/$($_.Name)" } }
if ($RomFS) { foreach ($f in 'runtime-index.bin', 'romfs.bin') { Send "$prep\romfs\$f" "$files/game-za/$f" } }
if ($Seed) {
    # the pack the device indexes (an earlier prepared/za/seed held only sb-align.txt: nothing was drawn)
    $null = & $hdc @d shell "aa force-stop $bundle"   # the app maps the pack it opened
    Send "$prep\seed-build\shaders\za202\sb-align.txt" "$files/android/shaders/za202/sb-align.txt"
    Send "$prep\seed-build\seed.vkpack" "$files/android/shaders/za202/seed.vkpack"
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
