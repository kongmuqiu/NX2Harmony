# libopus for the HarmonyOS runtime (the game's Opus audio): the official 1.6.1 release archive, checked against its
# published SHA-256, built as a static library with the HarmonyOS SDK's CMake toolchain (fixed point, no intrinsics,
# no programs or tests). build_ohos.py links artifacts\native-opus\build-ohos\opus\libopus.a and includes
# artifacts\native-opus\source\opus-1.6.1\include.
#   native-harmonyos\build_opus_ohos.ps1 [-DevEcoRoot <DevEco Studio>]
param([string]$DevEcoRoot = '')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\ohos_env.ps1"
if (-not $DevEcoRoot) { $DevEcoRoot = if ($env:TOTK_DEVECO) { $env:TOTK_DEVECO } else { 'C:\Program Files\Huawei\DevEco Studio' } }
$sdk = Join-Path $DevEcoRoot 'sdk\default\openharmony\native'
$cmake = Join-Path $sdk 'build-tools\cmake\bin\cmake.exe'; $ninja = Join-Path $sdk 'build-tools\cmake\bin\ninja.exe'
$toolchain = (Join-Path $sdk 'build\cmake\ohos.toolchain.cmake').Replace('\', '/')
foreach ($tool in $cmake, $ninja) { if (-not (Test-Path $tool)) { throw "missing $tool" } }
$root = Join-Path $port 'artifacts\native-opus'; $src = Join-Path $root 'source'; $build = Join-Path $root 'build-ohos\opus'
New-Item -ItemType Directory -Force $src | Out-Null
# the release archive and its mirrors (downloads.xiph.org drops connections now and then)
$urls = @('https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz',
          'https://ftp.osuosl.org/pub/xiph/releases/opus/opus-1.6.1.tar.gz',
          'https://github.com/xiph/opus/releases/download/v1.6.1/opus-1.6.1.tar.gz')
$sha = '6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1'
$archive = Join-Path $root 'opus-1.6.1.tar.gz'
if (-not (Test-Path (Join-Path $src 'opus-1.6.1\include\opus.h'))) {
    if (-not (Test-Path $archive)) {
        # three rounds over the mirrors; each download goes to a .part file that becomes the archive only with the
        # published SHA-256. TLS 1.2 and no progress bar for Windows PowerShell 5.1 (older default protocols; its
        # progress bar slows the download down); PowerShell 7 needs neither and ignores them.
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        $ProgressPreference = 'SilentlyContinue'
        $part = "$archive.part"
        $why = @()
        foreach ($round in 1..3) {
            foreach ($url in $urls) {
                try {
                    Invoke-WebRequest -Uri $url -OutFile $part -UseBasicParsing
                    $got = (Get-FileHash -Algorithm SHA256 $part).Hash.ToLower()
                    if ($got -eq $sha) { Move-Item -Force $part $archive; break }
                    $why += "$url : SHA-256 $got"
                } catch {
                    $why += "$url : $($_.Exception.Message)"
                }
                Remove-Item -Force -ErrorAction SilentlyContinue $part
            }
            if (Test-Path $archive) { break }
            if ($round -lt 3) { Start-Sleep -Seconds (5 * $round) }
        }
        if (-not (Test-Path $archive)) { throw "opus archive not downloaded (expected SHA-256 $sha):`n$($why -join "`n")" }
    }
    $got = (Get-FileHash -Algorithm SHA256 $archive).Hash.ToLower()
    if ($got -ne $sha) { Remove-Item $archive; throw "opus archive SHA-256 $got, expected $sha" }
    tar -xzf $archive -C $src
}
# __FILE__ (celt_assert's message) names the sources opus-1.6.1/..., not this PC's folder: the prefix map is a compile
# option, which CMake quotes for a folder with spaces (CMAKE_C_FLAGS would be split at them)
$prefixMap = Join-Path $root 'file-prefix-map.cmake'
Set-Content -LiteralPath $prefixMap -Encoding ascii -Value 'add_compile_options("-ffile-prefix-map=${PROJECT_SOURCE_DIR}/=opus-1.6.1/")'
& $cmake -S (Join-Path $src 'opus-1.6.1') -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$($ninja.Replace('\', '/'))" "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
    "-DCMAKE_PROJECT_INCLUDE=$($prefixMap.Replace('\', '/'))" -DCMAKE_C_VISIBILITY_PRESET=hidden `
    -DOHOS_ARCH=arm64-v8a -DOHOS_SDK_NATIVE_PLATFORM=ohos-26 -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DOPUS_BUILD_SHARED_LIBRARY=OFF `
    -DOPUS_BUILD_TESTING=OFF -DBUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_DISABLE_INTRINSICS=ON -DOPUS_FIXED_POINT=ON -DOPUS_ENABLE_FLOAT_API=ON
if ($LASTEXITCODE) { throw 'libopus configuration failed' }
& $cmake --build $build --target opus
if ($LASTEXITCODE) { throw 'libopus build failed' }
"libopus: $(Join-Path $build 'libopus.a')"
