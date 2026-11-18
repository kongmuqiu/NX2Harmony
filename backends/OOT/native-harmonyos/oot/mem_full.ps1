# The whole "hidumper --mem <pid>" table of the running game every -Every seconds, with the kernel's RSS split, appended to
# artifacts\runs\<Tag>-memfull.txt until the process is gone: which kind of memory grows (mem_watch.ps1 keeps three lines).
#   native-harmonyos\oot\mem_full.ps1 -Tag oot-a71c [-Every 5]
param([Parameter(Mandatory = $true)][string]$Tag, [int]$Every = 5)
. "$PSScriptRoot\..\ohos_env.ps1"
$out = Join-Path $port "artifacts\runs\$Tag-memfull.txt"
$p = "$(& $hdc @d shell "pidof $bundle")".Trim()
if (-not $p) { "no process"; exit 1 }
"pid $p" | Set-Content $out -Encoding ascii
while ($true) {
    $m = & $hdc @d shell "hidumper --mem $p 2>/dev/null"
    if (-not ($m -match '^\s+Total\s')) { "== $(Get-Date -Format HH:mm:ss) gone" | Add-Content $out -Encoding ascii; break }
    $st = (& $hdc @d shell "grep -E 'VmRSS|RssAnon|RssFile|RssShmem|VmSwap' /proc/$p/status 2>/dev/null") -replace '\s+', ' ' -replace ' kB', '' -join ' '
    "== $(Get-Date -Format HH:mm:ss) $st" | Add-Content $out -Encoding ascii
    $m | Where-Object { $_ -match '\S' } | Add-Content $out -Encoding ascii
    Start-Sleep $Every
}
