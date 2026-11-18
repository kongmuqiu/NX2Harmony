# One device iteration for The Legend of Zelda: Ocarina of Time: (optionally) build the libraries and the HAP, install it, start the game
# from the launcher (its Start button), wait, then pull the runtime log to artifacts\runs\<Tag>.log and print its tail.
#   native-harmonyos\oot\iterate_oot.ps1 -Tag oot-a1 [-Build] [-Seconds 40] [-Grep <regex>]
param([Parameter(Mandatory = $true)][string]$Tag, [switch]$Build, [int]$Seconds = 40, [string]$Grep = '')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\..\ohos_env.ps1"
Set-Location $port
New-Item -ItemType Directory -Force $runs | Out-Null
if ($Build) {
    $r = & python -I native-harmonyos\build_ohos.py --gpu vk --version oot131072 --guest prepared\oot\embed 2>&1 | Where-Object { $_ -notmatch '^compile' }
    $r | Select-Object -Last 4
    if ($LASTEXITCODE) { throw 'native build failed' }
    & "$PSScriptRoot\..\build_hap.ps1" -Tag $Tag | Select-Object -Last 1
    $hap = "artifacts\release\$Tag\totk-harmonyos-$Tag-signed.hap"
    $null = & $hdc @d shell "aa force-stop $bundle"
    (& $hdc @d install -r $hap | Out-String).Trim().Split("`n")[0]
}
$null = & $hdc @d shell "aa force-stop $bundle"
$null = & $hdc @d shell "power-shell wakeup; power-shell timeout -o 2147483647"   # screen on, and kept on (power-shell timeout -r restores)
# The system's game mark (game_mode.ps1; the mark a performance-monitor app's game mode sets): set when the system has
# neither recognised the app as a game nor has the developer mark. $env:TOTK_NO_GAME_MARK = 1 leaves it alone (to
# measure without it after "game_mode.ps1 -Action off").
if (-not $env:TOTK_NO_GAME_MARK) {
    $gm = (& $hdc @d shell "hidumper -s 66006 -a '-t $bundle'" | Out-String)
    if ($gm -notmatch "1\s+$([regex]::Escape($bundle))") { & "$PSScriptRoot\game_mode.ps1" -Action on | Out-Null }
}
$t0 = [long]"$(& $hdc @d shell 'date +%s')".Trim()
# hilog's time stamp format: only this run's lines. No quotes or spaces in the argument: Windows PowerShell 5.1 drops
# inner double quotes when it passes an argument to hdc, and the phone then ran a broken "date" (fixed oot-a113)
$ts0 = "$(& $hdc @d shell 'date +%m-%d_%H:%M:%S')".Trim() -replace '_', ' '
$applog = "/data/app/el2/100/base/$bundle/haps/entry/files/android/totk-native.log"
# A process that dies (a crash, or exit() at library load) is reported from hilog: the app's fatal lines and the
# appspawn exit line, from this run only.
function Show-Death([string]$why) {
    "== $why =="
    & $hdc @d shell "hilog -x 2>/dev/null | grep -E '$bundle.*(Unexpected call|exit with signal|JsError|Error message)|DfxSignalHandler.*processName\($bundle|$bundle/.* F ' | tail -40" | Where-Object { $_.Length -ge 14 -and $_.Substring(0, 14) -ge $ts0 } | Select-Object -Last 12 | ForEach-Object { $_.Substring(0, [Math]::Min(240, $_.Length)) }
}
$started = (& $hdc @d shell "aa start -b $bundle -a EntryAbility" | Out-String).Trim()
if ($started -notmatch 'success') { "aa start: $started" }
# Tap the launcher's Start button only once the launcher is on screen and its process stays up (a tap on the home
# screen opens whatever is there)
$front = $false
for ($i = 0; $i -lt 15 -and -not $front; $i++) {
    Start-Sleep -Seconds 1
    $up = "$(& $hdc @d shell "pidof $bundle")".Trim()
    $win = "$(& $hdc @d shell "uitest dumpLayout -p /data/local/tmp/oot-front.json >/dev/null; grep -c 'bundleName.:.$bundle' /data/local/tmp/oot-front.json")".Trim()
    $front = $up -and $win -match '^[1-9]'
}
if (-not $front) { Show-Death 'the launcher did not come to the front within 15 s (nothing tapped)'; exit 1 }
Start-Sleep -Seconds 1
# The launcher's Start button, found by its text in the layout dump (the launcher turns with the phone: portrait and
# landscape put it in different places); the portrait position when it cannot be found
$tap = '1054 2549'
$layout = Join-Path $env:TEMP 'oot-front.json'
$startText = -join [char[]](0x5F00, 0x59CB, 0x6E38, 0x620F)   # the button's text, without a non-ASCII literal (Windows PowerShell reads this file as ANSI)
$null = & $hdc @d shell "uitest dumpLayout -p /data/local/tmp/oot-front.json >/dev/null"
$null = & $hdc @d file recv /data/local/tmp/oot-front.json $layout
if (Test-Path $layout) {
    $m = [regex]::Match((Get-Content $layout -Raw -Encoding utf8), ('"bounds"\s*:\s*"\[(\d+),(\d+)\]\[(\d+),(\d+)\]"[^{}]*?"text"\s*:\s*"' + $startText + '"'))
    if (-not $m.Success) { $m = [regex]::Match((Get-Content $layout -Raw -Encoding utf8), ('"text"\s*:\s*"' + $startText + '"[^{}]*?"bounds"\s*:\s*"\[(\d+),(\d+)\]\[(\d+),(\d+)\]"')) }
    if ($m.Success) { $tap = "$([int](([int]$m.Groups[1].Value + [int]$m.Groups[3].Value) / 2)) $([int](([int]$m.Groups[2].Value + [int]$m.Groups[4].Value) / 2))" }
    else { "Start button not found in the layout: tapping the portrait position" }
    Remove-Item $layout
}
$null = & $hdc @d shell "uitest uiInput click $tap"
Start-Sleep -Seconds $Seconds
$log = Join-Path $runs "$Tag.log"
$mt = [long]"$(& $hdc @d shell "stat -c %Y $applog 2>/dev/null || echo 0")".Trim()
if ($mt -lt $t0) { "== the native log was not written this run (last written $([DateTimeOffset]::FromUnixTimeSeconds($mt).LocalDateTime)) ==" }
$null = & $hdc @d file recv -b $bundle /data/storage/el2/base/haps/entry/files/android/totk-native.log $log
$null = & $hdc @d shell "snapshot_display -f /data/local/tmp/$Tag.jpeg"
$null = & $hdc @d file recv "/data/local/tmp/$Tag.jpeg" (Join-Path $runs "$Tag.jpeg")
$pid2 = "$(& $hdc @d shell "pidof $bundle")".Trim()
"pid: '$pid2'   log: $log"
if (-not $pid2) { Show-Death 'the process is gone' }
if ($Grep) { Select-String -Path $log -Pattern $Grep | Select-Object -Last 30 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(240, $_.Line.Length)) } }
else { Get-Content $log | Select-Object -Last 30 | ForEach-Object { $_.Substring(0, [Math]::Min(240, $_.Length)) } }
