# From the title screen into the save: press A until the replay draws a 3D scene (the title, the save list and the Load
# dialog each take one A and draw under 100 draws a frame; the world draws several hundred), then wait for the scene.
#   native-harmonyos\oot\enter_world.ps1 [-Tag oot-enter] [-Settle 60]
param([string]$Tag = 'oot-enter', [int]$Settle = 60)
. "$PSScriptRoot\..\ohos_env.ps1"
Set-Location $port
$log = Join-Path $runs "$Tag.log"
function Draws {
    $null = & $hdc @d file recv -b $bundle /data/storage/el2/base/haps/entry/files/android/totk-native.log $log
    $l = (Select-String -Path $log -Pattern 'walker cpu frame \d+:.* draws ([\d.]+)/frame' | Select-Object -Last 1)
    if ($l) { [double]$l.Matches[0].Groups[1].Value } else { 0 }
}
for ($i = 0; $i -lt 10; $i++) {
    $n = Draws
    if ($n -ge 300) { "in the world ($n draws a frame) after $i presses"; break }
    & "$PSScriptRoot\keys.ps1" -Seq 'A' | Out-Null
    Start-Sleep -Seconds 14
    if (-not "$(& $hdc @d shell "pidof $bundle")".Trim()) { "the process is gone"; exit 1 }
}
Start-Sleep -Seconds $Settle
"pid: $("$(& $hdc @d shell "pidof $bundle")".Trim()), $(Draws) draws a frame"
