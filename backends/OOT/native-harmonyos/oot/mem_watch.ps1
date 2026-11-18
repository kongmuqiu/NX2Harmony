# Memory of the running game every -Every seconds (hidumper --mem: PSS total, GL, Graph = dma-buf), until the process is gone,
# into artifacts\runs\<Tag>-mem.txt: what the system's leak detector (hiview FaultLeakDetector, "StabilityCheckKill") saw last.
#   native-harmonyos\oot\mem_watch.ps1 -Tag oot-a24 [-Every 10]
param([Parameter(Mandatory = $true)][string]$Tag, [int]$Every = 10)
. "$PSScriptRoot\..\ohos_env.ps1"
$out = Join-Path $port "artifacts\runs\$Tag-mem.txt"
$p = "$(& $hdc @d shell "pidof $bundle")".Trim()
if (-not $p) { "no process"; exit 1 }
"pid $p" | Set-Content $out -Encoding ascii
while ($true) {
    $m = & $hdc @d shell "hidumper --mem $p 2>/dev/null"
    if (-not ($m -match '^\s+Total\s')) { "$(Get-Date -Format HH:mm:ss) gone" | Add-Content $out -Encoding ascii; break }
    $gl = ($m -match '^\s+GL\s') -replace '\s+', ' '; $gr = ($m -match '^\s+Graph\s') -replace '\s+', ' '; $t = ($m -match '^\s+Total\s') -replace '\s+', ' '
    # the kernel's RSS split (RSS_THRESHOLD_KILLER): anonymous, file pages (the seed package's mapping), shared (guest memory)
    $st = (& $hdc @d shell "grep -E 'VmRSS|RssAnon|RssFile|RssShmem' /proc/$p/status 2>/dev/null") -replace '\s+', ' ' -replace ' kB', '' -join ' '
    "$(Get-Date -Format HH:mm:ss) |$gl |$gr |$t | $st" | Add-Content $out -Encoding ascii
    Start-Sleep $Every
}
