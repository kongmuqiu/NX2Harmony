# From Link's house in Kokiri Forest (the save's start): turn the camera to the gate, RUN to it (the left stick held up),
# press A to open it, and sample the process's GPU memory all the way (hidumper's GL and Graph columns: what the system's
# GPU_THRESHOLD_KILLER counts). Lines "<time> <step> GL <MB> Graph <MB> Total <MB>" go to artifacts\runs\<Tag>-gate.txt;
# screenshots <Tag>-g0 (start), -g1 (turned), -r<n> (after each run leg), -g3 (end). Each line also has the process's
# swapped memory and the phone's /proc/meminfo (MiB): the kills of oot-a81..a83 were the phone running out of memory
# (hilog memmgrservice "LowerAvailBuffer availBuf=1450 <= LowestValue=1450", then signal 9 with killId -1), not the
# GPU-memory line.
#   native-harmonyos\oot\gate_run.ps1 -Tag oot-a82 [-Legs 6] [-Leg 20] [-After 120] [-NoTurn]
# The stick is held with "uinput -T -m <centre> <up> -k <ms>" (a touch that moves up and stays): a swipe let go at once,
# and at 2-4 frames a second the game saw a few frames of it (oot-a79..a81: Link hardly moved). The game steps one
# 1/30 s a frame, so a leg of 20 s at 3 fps is two seconds of running; A after each leg rolls on the way and opens the gate
# once Link stands at it.
param([Parameter(Mandatory = $true)][string]$Tag, [int]$Legs = 6, [int]$Leg = 20, [int]$After = 120, [switch]$NoTurn)
. "$PSScriptRoot\..\ohos_env.ps1"
Set-Location $port
$out = Join-Path $runs "$Tag-gate.txt"
"" | Set-Content $out -Encoding ascii
function Mem([string]$step) {
    $p = "$(& $hdc @d shell "pidof $bundle")".Trim()
    if (-not $p) { "$(Get-Date -Format HH:mm:ss) $step GONE" | Add-Content $out -Encoding ascii; return $false }
    $m = & $hdc @d shell "hidumper --mem $p 2>/dev/null"
    $v = @{}
    foreach ($l in $m) { if ($l -match '^\s+(GL|Graph|Total)\s+(\d+)') { if (-not $v.ContainsKey($Matches[1])) { $v[$Matches[1]] = [int]([double]$Matches[2] / 1024) } } }
    # the whole phone (the system kills the foreground app when its available buffer reaches the floor, 1450 MB): MiB
    $s = @{}
    foreach ($l in (& $hdc @d shell "cat /proc/meminfo")) { if ($l -match '^(MemAvailable|MemFree|Cached|SwapFree|ZramUsed|AnonPages|Shmem|GpuTotalUsed|IonTotalUsed|IonTotalCache|RsvTotalUsed|PageTables|Slab):\s+(\d+)') { $s[$Matches[1]] = [int]([double]$Matches[2] / 1024) } }
    $swap = 0; foreach ($l in $m) { if ($l -match '^\s+Total\s+(\d+)\s+\d+\s+\d+\s+\d+\s+\d+\s+(\d+)\s+(\d+)') { $swap = [int]([double]$Matches[2] / 1024); break } }
    "$(Get-Date -Format HH:mm:ss) $step GL $($v['GL']) Graph $($v['Graph']) Total $($v['Total']) Swap $swap | phone: avail $($s['MemAvailable']) free $($s['MemFree']) cached $($s['Cached']) swapfree $($s['SwapFree']) zram $($s['ZramUsed']) anon $($s['AnonPages']) shmem $($s['Shmem']) gpu $($s['GpuTotalUsed']) ion $($s['IonTotalUsed']) ioncache $($s['IonTotalCache']) rsv $($s['RsvTotalUsed']) pt $($s['PageTables']) slab $($s['Slab'])" | Add-Content $out -Encoding ascii
    return $true
}
function Shot([string]$n) { $null = & $hdc @d shell "snapshot_display -f /data/local/tmp/$n.jpeg"; $null = & $hdc @d file recv "/data/local/tmp/$n.jpeg" (Join-Path $runs "$n.jpeg") }
if (-not (Mem 'start')) { exit 1 }
Shot "$Tag-g0"
if (-not $NoTurn) {
    foreach ($i in 1..6) { $null = & $hdc @d shell "uitest uiInput swipe 1988 1210 2138 1210 100"; Start-Sleep -Seconds 2 }   # the camera to the gate
    Start-Sleep -Seconds 6
    if (-not (Mem 'turned')) { exit 1 }
    Shot "$Tag-g1"
}
# (-Legs 0: no run at all. "1..0" counts DOWN in PowerShell: it ran two legs, back to the house door, until 2026-11-13,
#  so every "platform view, standing" figure before that date is the view of the house door after two legs.)
foreach ($i in @(1..([Math]::Max($Legs, 1)) | Where-Object { $_ -le $Legs })) {   # run: the left stick (centre 426,1039) held up for a leg, memory sampled halfway and at its end
    $hold = Start-Process -FilePath $hdc -ArgumentList (@($d) + @('shell', "uinput -T -m 426 1039 426 860 -k $($Leg * 1000) 250")) -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds ([int]($Leg / 2))
    if (-not (Mem "run$i-half")) { exit 1 }
    $null = $hold.WaitForExit(($Leg + 10) * 1000)
    if (-not (Mem "run$i")) { exit 1 }
    Shot "$Tag-r$i"
    & "$PSScriptRoot\keys.ps1" -Seq 'A' -Hold 600 | Out-Null   # roll on, or open the gate
}
for ($t = 0; $t -lt $After; $t += 10) { Start-Sleep -Seconds 8; if (-not (Mem "after+$($t + 10)s")) { exit 1 } }
Shot "$Tag-g3"
