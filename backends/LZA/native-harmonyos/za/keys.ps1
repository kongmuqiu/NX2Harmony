# Press the on-screen pad keys of the running game (landscape 2848x1320, native-harmonyos ... PhoneControls.ets layout).
#   native-harmonyos\za\keys.ps1 -Seq 'R,D,D,D,A' [-Gap 1.2] [-Hold 120]
# Keys: A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT (U D Lt Rt for short) L3 R3; SU SD SL SR push the left
# stick that way (a drag from its centre); WAIT<s> pauses. A key is held for -Hold ms
# (a 200 px/s swipe over Hold/5 px inside the key), long enough for the game's 30 fps input poll.
param([Parameter(Mandatory = $true)][string]$Seq, [double]$Gap = 1.2, [int]$Hold = 150)
. "$PSScriptRoot\..\ohos_env.ps1"
$at = @{
    'A' = @(2683, 684); 'B' = @(2442, 894); 'X' = @(2442, 474); 'Y' = @(2200, 684)
    'ZL' = @(156, 158); 'L' = @(397, 158); 'ZR' = @(2442, 224); 'R' = @(2683, 224)
    'MINUS' = @(1221, 91); 'PLUS' = @(1504, 91)
    'UP' = @(368, 394); 'DOWN' = @(368, 709); 'LEFT' = @(170, 551); 'RIGHT' = @(567, 551)
    'U' = @(368, 394); 'D' = @(368, 709); 'LT' = @(170, 551); 'RT' = @(567, 551)
    'L3' = @(824, 1210); 'R3' = @(1988, 1210)
}
foreach ($k in $Seq.Split(',')) {
    $k = $k.Trim().ToUpper(); if (-not $k) { continue }
    if ($k -match '^WAIT(\d+(\.\d+)?)$') { Start-Sleep -Seconds ([double]$Matches[1]); continue }
    $stick = @{ 'SU' = @(0, -1); 'SD' = @(0, 1); 'SL' = @(-1, 0); 'SR' = @(1, 0) }[$k]
    if ($stick) {   # the left stick's centre (landscape): 426,1039; a 150 px push at 200 px/s
        $null = & $hdc @d shell "uitest uiInput swipe 426 1039 $(426 + 150 * $stick[0]) $(1039 + 150 * $stick[1]) 200"
        "$k"; Start-Sleep -Seconds $Gap; continue
    }
    $p = $at[$k]; if (-not $p) { throw "unknown key $k" }
    $null = & $hdc @d shell "uitest uiInput swipe $($p[0]) $($p[1]) $($p[0] + [int]($Hold / 5)) $($p[1]) 200"
    "$k"
    Start-Sleep -Seconds $Gap
}
