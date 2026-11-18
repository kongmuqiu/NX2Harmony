# Scheduling experiments on the phone: each configuration (a name and its environment lines) runs the route's first legs
# from a fresh start; the frame rate, the walker's CPU time and the GPU time of the last 300-frame windows, the cores the
# critical threads ran on and the frequencies SP_daemon reads at the end go to artifacts\runs\<Tag>-matrix.txt.
#   native-harmonyos\oot\sched_matrix.ps1 -Tag a99 -Configs 'spin0=TOTK_OHOS_SPIN=0', 'prime=TOTK_OHOS_MAIN_CORE=12;TOTK_OHOS_WALK_CORE=13'
# Every variable a configuration names is cleared again (sent empty) before the next one. -Build: build and install first.
# -Legs 0 -After 60: no running, the camera turned to the platform view and held for 60 s (the heaviest view of the route).
# The system's device state lines (every 5 s: temperature level, GPU load by stage, GPU kHz) of the WHOLE run are summarised:
# a comparison between runs is only fair at the same temperature level and GPU frequency.
# -Cool <s>: before every configuration but the first, the game is stopped and the phone left idle that long (the phone
# reaches temperature level 3 within 15-20 minutes of this load and loses about 15% of its frame rate: oot-a106c).
# -Rec <n>: record n presented frames at the end of each run (TOTK_OHOS_FRAME_REC, oot-a118) into <run>-rec.raw and
# measure its flashes with oot\frame_rec.py (animated <run>-rec.webp, <run>-rec-flashes.png).
param([Parameter(Mandatory = $true)][string]$Tag, [string[]]$Configs = @('default='), [int]$Legs = 3, [int]$After = 5, [switch]$Build, [int]$Cool = 0, [int]$Rec = 0)
. "$PSScriptRoot\..\ohos_env.ps1"
Set-Location $port
$out = Join-Path $runs "$Tag-matrix.txt"
"" | Set-Content $out -Encoding ascii
function Say([string]$l) { $l | Add-Content $out -Encoding ascii; $l }
$first = $true
$used = @{}
$ci = 0
foreach ($c in $Configs) {
    if ($Cool -gt 0 -and $ci++ -gt 0) { $null = & $hdc @d shell "aa force-stop $bundle"; Start-Sleep -Seconds $Cool }
    $name, $rest = $c -split '=', 2
    $lines = @(); if ($rest) { $lines = @($rest -split ';' | Where-Object { $_ }) }
    if ($Rec -gt 0) { $lines += "TOTK_OHOS_FRAME_REC=${Rec}:4" }
    $null = & $hdc @d shell "aa force-stop $bundle"
    # clear what earlier configurations set, then this one's lines
    $send = @(); foreach ($k in $used.Keys) { $send += "$k=" }
    foreach ($l in $lines) { $send += $l; $used[($l -split '=', 2)[0]] = 1 }
    if ($send.Count) { Start-Sleep -Seconds 1; & "$PSScriptRoot\devenv.ps1" @send | Out-Null }
    $t = "$Tag-$name"
    # hilog's time stamp format: only this run's device state lines (no quotes or spaces: PowerShell 5.1 drops inner
    # double quotes on the way to hdc; before oot-a113 this was an error text and every buffered line counted)
    $since = "$(& $hdc @d shell 'date +%m-%d_%H:%M:%S')".Trim() -replace '_', ' '
    if ($Build -and $first) { & "$PSScriptRoot\iterate_oot.ps1" -Tag $t -Build -Seconds 40 -Grep 'zzz' | Out-Null }
    else { & "$PSScriptRoot\iterate_oot.ps1" -Tag $t -Seconds 40 -Grep 'zzz' | Out-Null }
    $first = $false
    & "$PSScriptRoot\enter_world.ps1" -Tag "$t-enter" -Settle 30 | Out-Null
    $files = '/data/storage/el2/base/haps/entry/files/android'
    $go = Join-Path $env:TEMP 'frame-rec-go'; [IO.File]::WriteAllText($go, "go`n")
    if ($Rec -gt 0 -and $Legs -gt 0) {   # walking: the recording covers the walk itself (from its start)
        $null = & $hdc @d shell "rm -f /data/app/el2/100/base/$bundle/haps/entry/files/android/frame-rec.raw"
        $null = & $hdc @d file send -b $bundle $go "$files/frame-rec-go"
    }
    & "$PSScriptRoot\gate_run.ps1" -Tag $t -Legs $Legs -Leg 20 -After $After | Out-Null
    if ($Rec -gt 0) {   # the game still runs here: record the next $Rec presented frames (standing), pull them, measure the flashes
        if ($Legs -eq 0) {
            $null = & $hdc @d shell "rm -f /data/app/el2/100/base/$bundle/haps/entry/files/android/frame-rec.raw"
            $null = & $hdc @d file send -b $bundle $go "$files/frame-rec-go"
            Start-Sleep -Seconds ([int][Math]::Ceiling($Rec / 10.0) + 6)
        } else { Start-Sleep -Seconds 8 }
        $raw = Join-Path $runs "$t-rec.raw"
        $null = & $hdc @d file recv -b $bundle "$files/frame-rec.raw" $raw
        # with -Legs 0 the view is the standing one: compare against the reference recording of it (ref-standing-rec.raw)
        $refArgs = @(); $ref = Join-Path $runs 'ref-standing-rec.raw'; if ($Legs -eq 0 -and (Test-Path $ref)) { $refArgs = @('--ref', $ref) } else {
            $refArgs = @('--sheet', '30'); $rw = Join-Path $runs 'ref-walk-rec.raw'; $refArgs += @('--hud', $(if (Test-Path $rw) { $rw } else { '-' })) }
        $refArgs += '--blob'   # isolated white foliage frames (the treehouse camera cut on the walking route)
        $recLine = if (Test-Path $raw) { "  rec: " + ((& python -I "$PSScriptRoot\frame_rec.py" $raw --label "$t-rec" --webp @refArgs 2>&1 | Where-Object { $_ -notmatch 'animation:|flash frames [0-9, ]+:' }) -join ' | ') } else { "  rec: no recording pulled" }
    }
    # SmartPerf takes one client: when the live page (oot\perfdash, port 8766) holds a session, a second SP_daemon would end
    # it, so the frequencies are left to the page then
    # (8765: an external SmartPerf page, which holds the SmartPerf session while it runs; 8766: this port's)
    $pageUp = $false
    foreach ($port in 8765, 8766) { try { $tc = New-Object Net.Sockets.TcpClient; $tc.Connect('127.0.0.1', $port); if ($tc.Connected) { $pageUp = $true }; $tc.Close() } catch { } }
    $sp = if ($pageUp) { '' } else { (& $hdc @d shell "SP_daemon -N 3 -c -g" | Out-String) }
    $log = Join-Path $runs "$t.log"
    $null = & $hdc @d file recv -b $bundle /data/storage/el2/base/haps/entry/files/android/totk-native.log $log
    Say "== $name  [$($lines -join ' ')]"
    if ($Rec -gt 0) { Say $recLine; $recLine = '' }
    Select-String -Path $log -Pattern 'fps over the last' | Select-Object -Last 3 | ForEach-Object { Say ("  " + ($_.Line -replace '^\[nvn-vk\] ', '' -replace ', \d+ draws.*', '')) }
    Select-String -Path $log -Pattern 'per frame: recording' | Select-Object -Last 2 | ForEach-Object { Say ("  " + ($_.Line -replace '^\[nvn-vk\] ', '' -replace '; descriptor.*', '')) }
    Select-String -Path $log -Pattern 'walker cpu' | Select-Object -Last 2 | ForEach-Object { Say ("  " + ($_.Line -replace '^\[nvn-vk\] ', '')) }
    Select-String -Path $log -Pattern 'ohos-pin\] totk-walk|ohos-spin\] (frequency|the last)' | Select-Object -Last 4 | ForEach-Object { Say ("  " + $_.Line.Substring(0, [Math]::Min(210, $_.Line.Length))) }
    $f = @{}; foreach ($m in [regex]::Matches($sp, '(cpu\d+Frequency|gpuFrequency|gpuLoad)=([\d.]+)')) { $k = $m.Groups[1].Value; if (-not $f.ContainsKey($k)) { $f[$k] = @() }; $f[$k] += [double]$m.Groups[2].Value }
    $line = '  MHz:'; foreach ($k in 'cpu0Frequency', 'cpu4Frequency', 'cpu9Frequency', 'cpu12Frequency') { if ($f.ContainsKey($k)) { $line += " $($k -replace 'Frequency', '') " + (($f[$k] | ForEach-Object { [int]($_ / 1000) }) -join '/') } }
    if ($f.ContainsKey('gpuFrequency')) { $line += '  gpu ' + (($f['gpuFrequency'] | ForEach-Object { [int]($_ / 1000000) }) -join '/') + ' load ' + (($f['gpuLoad'] | ForEach-Object { [int]$_ }) -join '/') }
    Say $line
    # the system's view (Game Service Kit, entry's Game.ets): temperature level, GPU load by stage, GPU frequency
    $hl = @(& $hdc @d shell "hilog -x | grep 'device state'" | Where-Object { $_.Length -gt 14 -and $_.Substring(0, 14) -ge $since })
    $khz = @(); $temp = @(); $bw = @(); $frag = @()
    foreach ($l in $hl) { if ($l -match 'temp level (-?\d+), GPU load \d+ \(vertex \d+, fragment (\d+), texture \d+, bandwidth (\d+)\), GPU (\d+) kHz') {
        if ([int]$Matches[1] -gt 0) { $temp += [int]$Matches[1] }; $frag += [int]$Matches[2]; $bw += [int]$Matches[3]; $khz += [int]$Matches[4] } }
    if ($khz.Count) {
        $m = $khz | Measure-Object -Minimum -Maximum -Average
        Say ("  device state, {0} samples of this run: GPU {1:N0} - {2:N0} MHz (average {3:N0}), temperature level {4} - {5}, bandwidth level average {6:N1}, fragment {7:N1}" -f $khz.Count,
            ($m.Minimum / 1000), ($m.Maximum / 1000), ($m.Average / 1000), ($temp | Measure-Object -Minimum).Minimum, ($temp | Measure-Object -Maximum).Maximum,
            ($bw | Measure-Object -Average).Average, ($frag | Measure-Object -Average).Average)
    }
    # the same over the last 60 s only (the standing phase at the end of the run, not boot and menus); oot-a112
    $dnow = "$(& $hdc @d shell 'date +%m-%d_%H:%M:%S')".Trim() -replace '_', ' '   # the device's clock; 60 s back computed here
    $cut = ([datetime]::ParseExact($dnow, 'MM-dd HH:mm:ss', $null)).AddSeconds(-60).ToString('MM-dd HH:mm:ss')
    $k60 = @(); foreach ($l in $hl) { if ($l.Substring(0, 14) -ge $cut -and $l -match 'GPU (\d+) kHz') { $k60 += [int]$Matches[1] / 1000 } }
    if ($k60.Count) {
        $m = $k60 | Measure-Object -Minimum -Maximum -Average
        Say ("  device state, last 60 s ({0} samples): GPU {1:N0} - {2:N0} MHz, average {3:N0}, at 900+ MHz {4}" -f $k60.Count, $m.Minimum, $m.Maximum, $m.Average, @($k60 | Where-Object { $_ -ge 900 }).Count)
    }
    $hl | Select-Object -Last 2 | ForEach-Object { if ($_ -match '(device state: .*)$') { Say ("  " + $Matches[1]) } }
    Select-String -Path $log -Pattern 'mesh draws in 5 s' | Select-Object -Last 1 | ForEach-Object { Say ("  " + ($_.Line -replace '^\[nvn-vk\] ', '' -replace ', render pass split.*', '')) }
    Select-String -Path $log -Pattern 'ohos-fault\]' | Select-Object -Last 3 | ForEach-Object { Say ("  " + $_.Line.Substring(0, [Math]::Min(230, $_.Line.Length))) }
    $mem = Get-Content (Join-Path $runs "$t-gate.txt") | Select-Object -Last 1
    Say ("  " + ($mem -replace '\| phone: (avail \d+).*', '| $1'))
}
# leave the phone with defaults
$null = & $hdc @d shell "aa force-stop $bundle"
$send = @(); foreach ($k in $used.Keys) { $send += "$k=" }
if ($send.Count) { Start-Sleep -Seconds 1; & "$PSScriptRoot\devenv.ps1" @send | Out-Null }
Say "done"
