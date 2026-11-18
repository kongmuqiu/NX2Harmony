# Send diagnostics environment lines to the device for the next start, as a new files/android/env-dev-<time>.txt (the app
# reads every env-dev-*.txt in name order, a later one winning; a file hdc rewrites is not seen by the app at once).
# Clear a variable by sending it empty: KEY=
#   native-harmonyos\za\devenv.ps1 'TOTK_VK_PASS_DUMP=2600,0' 'ZA_UBO_DIAG='
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Lines)
. "$PSScriptRoot\..\ohos_env.ps1"
$name = "env-dev-$(Get-Date -Format 'yyyyMMddHHmmss').txt"
$tmp = Join-Path $env:TEMP $name
[IO.File]::WriteAllText($tmp, (($Lines -join "`n") + "`n"))
& $hdc @d file send -b $bundle $tmp "/data/storage/el2/base/haps/entry/files/android/$name" | Select-Object -Last 1
Remove-Item $tmp
