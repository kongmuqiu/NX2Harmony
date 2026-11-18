# The system's "game" mark for this app, as a performance-monitor app's game mode sets it over hdc (its add to / remove
# from the game list): the game service (system ability 66006) keeps a
# developer list of bundles it treats as games; a bundle on it gets the system's game policy (the GPU held at its top
# frequency among other things). Whether the GPU then has work all the time is this port's own business.
#   native-harmonyos\oot\game_mode.ps1 [-Action status|on|off] [-Bundle <name>]
# on:  cem publish -e game.assistant.custom.deviceTemplate ; hidumper -s 66006 -a "-se <bundle>"   (isGame(dev) = 1)
# off: hidumper -s 66006 -a "-sd <bundle>" ; hidumper -s 66006 -a "-sc <bundle>"                   (the record is removed)
# The mark is kept by the system, not by the app: it stays until "off" (or until a monitor app removes it from the list).
param([ValidateSet('status', 'on', 'off')][string]$Action = 'status', [string]$Bundle = '')
. "$PSScriptRoot\..\ohos_env.ps1"
if (-not $Bundle) { $Bundle = $bundle }
if ($Bundle -notmatch '^[A-Za-z0-9._-]+$') { throw "bundle name '$Bundle'" }
function Sh([string]$c) { (& $hdc @d shell $c | Out-String).Trim() }
function Status {
    $t = Sh "hidumper -s 66006 -a '-t $Bundle'"
    $line = ($t -split "`n" | Where-Object { $_ -match [regex]::Escape($Bundle) } | Select-Object -First 1)
    $head = ($t -split "`n" | Where-Object { $_ -match 'isGame\(live\)' } | Select-Object -First 1)
    if ($head) { "  $($head.Trim())" }
    if ($line) { "  $($line.Trim())" } else { "  no record for $Bundle" }
}
switch ($Action) {
    'on' {
        $null = Sh 'cem publish -e game.assistant.custom.deviceTemplate'
        Start-Sleep -Milliseconds 500
        $r = Sh "hidumper -s 66006 -a '-se $Bundle'"
        if ($r -notmatch [regex]::Escape("set bundleName=$Bundle isGame=1 success.")) { "the system did not confirm: $r" } else { "game mark on: $Bundle" }
    }
    'off' {
        $null = Sh 'cem publish -e game.assistant.custom.deviceTemplate'
        Start-Sleep -Milliseconds 500
        $r1 = Sh "hidumper -s 66006 -a '-sd $Bundle'"
        $r2 = Sh "hidumper -s 66006 -a '-sc $Bundle'"
        if ($r1 -match 'isGame=0 success' -and $r2 -match 'cancel game recognition') { "game mark off: $Bundle" } else { "the system did not confirm: $r1 | $r2" }
    }
}
Status
