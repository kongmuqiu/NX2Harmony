# Run one shell command on the device (hdc shell), or take a screenshot to artifacts\runs\<Shot>.jpeg.
#   native-harmonyos\oot\sh.ps1 -Cmd 'ls /data/local/tmp'      native-harmonyos\oot\sh.ps1 -Shot now
param([string]$Cmd = '', [string]$Shot = '')
. "$PSScriptRoot\..\ohos_env.ps1"
Set-Location $port
if ($Cmd) { & $hdc @d shell $Cmd }
if ($Shot) {
    $null = & $hdc @d shell "snapshot_display -f /data/local/tmp/$Shot.jpeg"
    $null = & $hdc @d file recv "/data/local/tmp/$Shot.jpeg" (Join-Path $runs "$Shot.jpeg")
    Join-Path $runs "$Shot.jpeg"
}
