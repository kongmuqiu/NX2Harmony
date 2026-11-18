# Machine-specific values for the HarmonyOS scripts (dot-source: . "$PSScriptRoot\ohos_env.ps1"). Nothing here is
# committed per machine: set the TOTK_* variables in the environment, or put them in native-harmonyos\local.ohos.ps1
# (ignored by git; see local.ohos.example.ps1). The Python tools read the same file (ohos_env.py).
#   TOTK_DEVECO       DevEco Studio folder (default C:\Program Files\Huawei\DevEco Studio); hdc and the SDK are found inside it
#   TOTK_HDC          hdc executable (default: hdc on PATH, else the one inside DevEco Studio)
#   TOTK_HDC_SERVER   hdc server to use with -s (optional, e.g. 127.0.0.1:18710 for a private server)
#   TOTK_HDC_TARGET   device key for -t (optional when one device is connected)
#   TOTK_PWSH         PowerShell 7 executable (default: pwsh on PATH)
$local = Join-Path $PSScriptRoot 'local.ohos.ps1'
if (Test-Path $local) { . $local }
$port = (Resolve-Path (Join-Path $PSScriptRoot '..')).ProviderPath  # the project root (ProviderPath: no provider prefix on a network path)
$runs = Join-Path $port 'artifacts\runs'
$bundle = if ($env:TOTK_OHOS_BUNDLE) { $env:TOTK_OHOS_BUNDLE } else { 'nx2h.u000000.oot' }
$deveco = if ($env:TOTK_DEVECO) { $env:TOTK_DEVECO } else { 'C:\Program Files\Huawei\DevEco Studio' }
$hdc = $env:TOTK_HDC
if (-not $hdc) {
    $onPath = Get-Command hdc -ErrorAction SilentlyContinue
    if ($onPath) { $hdc = $onPath.Source }
    else { $hdc = Join-Path $deveco 'sdk\default\openharmony\toolchains\hdc.exe' }
}
$d = @()
if ($env:TOTK_HDC_SERVER) { $d += @('-s', $env:TOTK_HDC_SERVER) }
if ($env:TOTK_HDC_TARGET) { $d += @('-t', $env:TOTK_HDC_TARGET) }
$pwsh = if ($env:TOTK_PWSH) { $env:TOTK_PWSH } else { 'pwsh' }
New-Item -ItemType Directory -Force $runs | Out-Null
