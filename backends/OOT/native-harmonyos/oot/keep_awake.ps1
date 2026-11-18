# Keeps the phone's screen on while tests run: wakes it and overrides the screen timeout, every $Period seconds.
# The device comes from ohos_env.ps1 (TOTK_HDC_SERVER / TOTK_HDC_TARGET: local.ohos.ps1; over Wi-Fi the target is
# <ip>:<port>, connected with "hdc -s <server> tconn <ip>:<port>"). Run it in the background; stopping it leaves the
# override on the phone, which "hdc ... shell power-shell timeout -r" restores. Nothing else on the phone is changed.
# A Wi-Fi target that dropped is connected again (tconn) on the next round.
param([int]$Period = 20)
. "$PSScriptRoot\..\ohos_env.ps1"
$tgt = $env:TOTK_HDC_TARGET
Write-Output "keeping the screen of $tgt on (every $Period s)"
while ($true) {
    if ($tgt -match '^\d+\.\d+\.\d+\.\d+:\d+$') {
        $st = (& $hdc -s $env:TOTK_HDC_SERVER list targets 2>$null | Out-String)
        if ($st -notmatch [regex]::Escape($tgt)) { $null = & $hdc -s $env:TOTK_HDC_SERVER tconn $tgt 2>$null }
    }
    $null = & $hdc @d shell "power-shell wakeup; power-shell timeout -o 2147483647" 2>$null
    Start-Sleep -Seconds $Period
}
