# Time one mesh kernel variant on the phone (prepared/oot/mesh-build/exp-kernel/build_one.py makes the variants):
# its .spv / .vk go next to the seed pack as loose files (they win over the pack's), the game runs the route's first legs
# with TOTK_VK_PASS_PROFILE, and the kernel's "dispatches of ..." lines are printed. -Restore removes the loose files.
#   native-harmonyos\oot\kernel_variant.ps1 -Key eb021183bf9b0092 -Variant noraw
#   native-harmonyos\oot\kernel_variant.ps1 -Key eb021183bf9b0092 -Restore
param([string]$Key, [string]$Variant = '', [int]$Legs = 4, [switch]$Restore)
. "$PSScriptRoot\..\ohos_env.ps1"
$files = '/data/storage/el2/base/haps/entry/files/android/shaders/oot131072'
$null = & $hdc @d shell "aa force-stop $bundle"
$src = Join-Path $port "prepared\oot\mesh-build\exp-kernel\out\$Variant"
if ($Restore) {
    $dir = "/data/app/el2/100/base/$bundle/haps/entry/files/android/shaders/oot131072"
    & $hdc @d shell "rm -f $dir/$Key*.spv $dir/$Key*.vk; ls $dir | grep -c $Key"
    return
}
Get-ChildItem $src -File | Where-Object { $_.Name -like "$Key*" -and ($_.Extension -eq '.spv' -or $_.Extension -eq '.vk') } | ForEach-Object {
    $r = (& $hdc @d file send -b $bundle $_.FullName "$files/$($_.Name)" | Out-String).Trim()
    "{0}: {1}" -f ($_.Name.Substring(0, 16) + $_.Extension), ($r -split "`n")[-1]
}
$tag = "kv-$Variant"
& "$PSScriptRoot\iterate_oot.ps1" -Tag "$tag-start" -Seconds 40 -Grep 'zzz' | Select-Object -Last 1
& "$PSScriptRoot\enter_world.ps1" -Tag "$tag-enter" -Settle 30 | Select-Object -Last 1
& "$PSScriptRoot\gate_run.ps1" -Tag $tag -Legs $Legs -Leg 20 -After 5 | Out-Null
$log = Join-Path $port "artifacts\runs\$tag.log"
$null = & $hdc @d file recv -b $bundle /data/storage/el2/base/haps/entry/files/android/totk-native.log $log
$lines = Select-String -Path $log -Pattern "dispatches of $Key" | ForEach-Object { $_.Line }
"{0}: {1} profiled frames with the kernel" -f $Variant, $lines.Count
$lines | Select-Object -Last 6 | ForEach-Object { $_ -replace '.*pass profile frame (\d+): dispatches of \S+ \(.*?\): ', 'frame $1: ' }
