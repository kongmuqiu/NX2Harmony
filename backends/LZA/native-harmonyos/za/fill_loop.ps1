# Shader fill rounds for Pokémon Legends: Z-A: start the game (no rebuild), play the new-game sequence (newgame.ps1) until
# the opening, let it run, then translate on the PC the shader requests it left and send them back
# (fill_shaders_ohos.py). Stops early when a round brings no new requests.
#   native-harmonyos\za\fill_loop.ps1 [-Rounds 3] [-Play 150]
param([int]$Rounds = 3, [int]$Play = 150)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\..\ohos_env.ps1"
Set-Location $port
for ($r = 1; $r -le $Rounds; $r++) {
    $tag = "za-fill-$(Get-Date -Format 'HHmmss')-r$r"
    "== round $r ($tag) =="
    & "$PSScriptRoot\iterate_za.ps1" -Tag $tag -Seconds 75 -Grep 'signal=' | Select-Object -Last 2
    & "$PSScriptRoot\newgame.ps1" | Out-Null
    & "$PSScriptRoot\keys.ps1" -Seq "WAIT$Play" | Out-Null
    & "$PSScriptRoot\sh.ps1" -Shot "$tag-end" | Out-Null
    & "$PSScriptRoot\pull_log.ps1" -Tag "$tag-end" -Lines 3 -Grep 'fps over the last|signal='
    $out = & python "$PSScriptRoot\..\fill_shaders_ohos.py" --version za202 2>&1 | Out-String
    $out.Trim().Split("`n") | Select-Object -Last 3
    if ($out -match '(?m)no shader requests|^0 new requests') { "no new requests: done"; break }
}
