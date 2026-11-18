# Pull the running game's log (no restart) to artifacts\runs\<Tag>.log and print its tail without the periodic lines.
#   native-harmonyos\za\pull_log.ps1 -Tag za-a2b [-Wait 50] [-Lines 40] [-Grep <regex>]
param([Parameter(Mandatory = $true)][string]$Tag, [int]$Wait = 0, [int]$Lines = 40, [string]$Grep = '')
. "$PSScriptRoot\..\ohos_env.ps1"
Set-Location $port
New-Item -ItemType Directory -Force $runs | Out-Null
if ($Wait) { Start-Sleep -Seconds $Wait }
$log = Join-Path $runs "$Tag.log"
$null = & $hdc @d file recv -b $bundle /data/storage/el2/base/haps/entry/files/android/totk-native.log $log
"pid: '" + "$(& $hdc @d shell "pidof $bundle")".Trim() + "'"
$text = Get-Content $log | Where-Object { $_ -notmatch '^\[ohos-pin\]|Verifying game data|^\[gyro\]' }
if ($Grep) { $text = $text | Where-Object { $_ -match $Grep } }
$text | Select-Object -Last $Lines | ForEach-Object { $_.Substring(0, [Math]::Min(240, $_.Length)) }
