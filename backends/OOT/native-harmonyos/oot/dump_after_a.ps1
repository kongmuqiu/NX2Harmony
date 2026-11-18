# Pass dump of a frame after the title (debug): starts the game with TOTK_VK_PASS_DUMP=<Frame>,<MinWidth>, presses A twice
# once the frame counter passes -PressAt (the title), waits for the dump and pulls index.txt and the attachments into
# artifacts\runs\<Tag>\ (as PNGs at a quarter size by tools\raw2png.py when present).
#   native-harmonyos\oot\dump_after_a.ps1 -Tag pd-open -Frame 2600 [-PressAt 1400] [-MinWidth 640]
param([Parameter(Mandatory = $true)][string]$Tag, [int]$Frame = 2600, [int]$PressAt = 1400, [int]$MinWidth = 640)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\..\ohos_env.ps1"
Set-Location $port
$files = "/data/app/el2/100/base/$bundle/haps/entry/files/android"
# The frame goes to the device as a new env-dev-<time>.txt (devenv.ps1): the app reads every env-dev-*.txt in name order, a
# later one winning, and does not see a file hdc rewrites (an env-dev.txt sent here was never read: every dump of oot-a58 ..
# a61 was of frame 3900, the value an earlier file held). Sent only when the value in effect differs (the app reads at most
# 64 such files), with the app up (hdc file send -b reaches the sandbox only then). The runtime clears passdump/ itself.
$want = "TOTK_VK_PASS_DUMP=$Frame,$MinWidth"
$now = "$(& $hdc @d shell "cat $files/env-dev-*.txt 2>/dev/null | grep '^TOTK_VK_PASS_DUMP=' | tail -1")".Trim()
if ($now -ne $want) {
    $null = & $hdc @d shell "aa start -b $bundle -a EntryAbility"; Start-Sleep 4
    & "$PSScriptRoot\devenv.ps1" $want | Out-Null
    $now = "$(& $hdc @d shell "cat $files/env-dev-*.txt 2>/dev/null | grep '^TOTK_VK_PASS_DUMP=' | tail -1")".Trim()
    if ($now -ne $want) { throw "the device's env-dev files give '$now', not '$want'" }
}
& "$PSScriptRoot\iterate_oot.ps1" -Tag $Tag -Seconds 30 | Select-Object -Last 1
function FrameNow { $o = "$(& $hdc @d shell "grep -a -o 'frame [0-9]*' $files/totk-native.log | tail -1")"; if ($o -match '(\d+)') { [int]$Matches[1] } else { 0 } }
$t0 = Get-Date
while ((FrameNow) -lt $PressAt -and ((Get-Date) - $t0).TotalMinutes -lt 15) { Start-Sleep 5 }
"title at frame $(FrameNow): pressing A"
& "$PSScriptRoot\keys.ps1" -Seq 'A,WAIT15,A,WAIT10,A' | Out-Null   # title, the save slot, the Load confirm
$t0 = Get-Date
while (-not "$(& $hdc @d shell "grep -a 'pass dump of frame [0-9]*:' $files/totk-native.log")".Trim() -and ((Get-Date) - $t0).TotalMinutes -lt 20) { Start-Sleep 10 }
"$(& $hdc @d shell "grep -a 'pass dump of frame [0-9]*:' $files/totk-native.log")"
$out = Join-Path $port "artifacts\runs\$Tag"; New-Item -ItemType Directory -Force $out | Out-Null
$names = "$(& $hdc @d shell "ls $files/shots/passdump")" -split '\s+' | Where-Object { $_ -match '\.(txt|raw|bin)$' }
foreach ($n in $names) { $null = & $hdc @d file recv "$files/shots/passdump/$n" (Join-Path $out $n) }
# the dump frame stays set for the next start (clear it with devenv.ps1 'TOTK_VK_PASS_DUMP=')
"pulled $($names.Count) files into $out"
