# Build the app package (HAP) for your own phone: the ArkTS app (entry/) with the two native libraries built by
# build_ohos.py, signed with your own DevEco signing configuration. The package contains libtotk_guest.so, the game's
# rewritten code, so it is private to the owner of the game files: build it for yourself, never share it.
#   native-harmonyos\build_hap.ps1 -Tag <name>
#   -> artifacts\release\<Tag>\totk-harmonyos-<Tag>-signed.hap, plus audit.txt (every entry of the package)
# Signing: open the project in DevEco Studio once, File > Project Structure > Signing Configs, sign in and let it
# generate the configuration; it is written to build-profile.json5 at the project root (ignored by git).
param([Parameter(Mandatory = $true)][string]$Tag, [string]$DevEcoRoot = '')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\ohos_env.ps1"
Set-Location $port
if (-not $DevEcoRoot) { $DevEcoRoot = if ($env:TOTK_DEVECO) { $env:TOTK_DEVECO } else { 'C:\Program Files\Huawei\DevEco Studio' } }
$node = Join-Path $DevEcoRoot 'tools\node\node.exe'
$ohpm = Join-Path $DevEcoRoot 'tools\ohpm\bin\pm-cli.js'
$hvigor = Join-Path $DevEcoRoot 'tools\hvigor\bin\hvigorw.js'
foreach ($tool in $node, $ohpm, $hvigor) { if (-not (Test-Path $tool)) { throw "missing $tool (set TOTK_DEVECO to your DevEco Studio folder)" } }
if (-not (Test-Path 'build-profile.json5')) {
    Copy-Item 'build-profile.template.json5' 'build-profile.json5'
    Write-Warning 'build-profile.json5 created from the template without signing: configure signing in DevEco Studio, then build again'
}
$libDir = "artifacts\harmonyos-native\lib-vk"
foreach ($so in 'libtotk_ohos.so', 'libtotk_guest.so') { if (-not (Test-Path "$libDir\$so")) { throw "$libDir\$so missing: run prepare_v143.py (step libs) first" } }
foreach ($stale in 'entry\build', 'entry\libs') { if (Test-Path $stale) { Remove-Item -Recurse -Force $stale } }
New-Item -ItemType Directory -Force entry\libs\arm64-v8a | Out-Null
Copy-Item "$libDir\libtotk_ohos.so", "$libDir\libtotk_guest.so" entry\libs\arm64-v8a\ -Force

$env:DEVECO_SDK_HOME = Join-Path $DevEcoRoot 'sdk'
$env:NODE_HOME = Join-Path $DevEcoRoot 'tools\node'
$env:PATH = "$env:NODE_HOME;$(Join-Path $DevEcoRoot 'tools\ohpm\bin');$env:PATH"
# hvigor starts java (spawn java ENOENT on a PC without Java on PATH): DevEco Studio's own runtime then
if (-not (Get-Command java -ErrorAction SilentlyContinue)) {
    $jbr = Join-Path $DevEcoRoot 'jbr'
    if (-not (Test-Path (Join-Path $jbr 'bin\java.exe'))) { throw "java not found: neither on PATH nor $jbr\bin\java.exe (set TOTK_DEVECO to your DevEco Studio folder)" }
    $env:JAVA_HOME = $jbr
    $env:PATH = "$(Join-Path $jbr 'bin');$env:PATH"
}
$out = "artifacts\release\$Tag"; New-Item -ItemType Directory -Force $out | Out-Null
$log = "$out\build.log"
# the tools write progress to stderr: collect everything in the log without treating it as an error
$ErrorActionPreference = 'Continue'
& $node $ohpm install 2>&1 | Out-File -Encoding utf8 $log
$code = $LASTEXITCODE
if ($code -eq 0) { & $node $hvigor --no-daemon assembleHap --mode module -p module=entry@default -p product=default 2>&1 | Out-File -Append -Encoding utf8 $log; $code = $LASTEXITCODE }
$ErrorActionPreference = 'Stop'
if ($code -ne 0) { throw "the HAP build failed ($code), see $log" }
$signed = 'entry\build\default\outputs\default\entry-default-signed.hap'
if (-not (Test-Path $signed)) { throw "no signed package (entry-default-signed.hap): configure signing in DevEco Studio" }
$hap = "$out\totk-harmonyos-$Tag-signed.hap"; Copy-Item $signed $hap -Force

# Every entry must be one of these; anything else (game data, stray libraries, captures) fails the build.
$allow = @('^module\.json$', '^pack\.info$', '^pkgContextInfo\.json$', '^resources\.index$', '^\.pages\.info$', '^ets/', '^resources/',
           '^libs/arm64-v8a/libtotk_ohos\.so$', '^libs/arm64-v8a/libtotk_guest\.so$')
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead((Resolve-Path $hap).ProviderPath)
$lines = @(); $bad = @()
try {
    foreach ($e in $zip.Entries) {
        if (-not $e.FullName -or $e.FullName.EndsWith('/')) { continue }
        $ok = $false; foreach ($p in $allow) { if ($e.FullName -match $p) { $ok = $true; break } }
        if (-not $ok) { $bad += $e.FullName }
        $lines += ('{0,12} {1}' -f $e.Length, $e.FullName)
    }
} finally { $zip.Dispose() }
$lines | Set-Content -Encoding utf8 "$out\audit.txt"
if ($bad.Count) { throw ("package audit: unexpected entries: " + ($bad -join ', ')) }
"hap: $hap ($([math]::Round((Get-Item $hap).Length / 1MB)) MB); audit passed, $($lines.Count) entries"
"install: hdc install -r `"$hap`""
