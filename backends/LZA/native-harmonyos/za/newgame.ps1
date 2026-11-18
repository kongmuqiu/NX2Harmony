# From the language screen of a fresh start to the opening: 简体中文, the last look, the name "Test", "以这样开始!".
# Waits (up to -Timeout s) until the screen shows LANGUAGE SELECTION with English highlighted, then plays the keys.
# Screens: artifacts\runs\ng-*.jpeg
param([string]$Name = 'Test', [int]$Timeout = 180)
$k = "$PSScriptRoot\keys.ps1"; $sh = "$PSScriptRoot\sh.ps1"
$check = @'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
bright = lambda p: sum(im.getpixel(p)) > 700
print('yes' if sum(im.getpixel((1022, 467))) > 740 and sum(im.getpixel((1022, 600))) > 550 and sum(im.getpixel((300, 1250))) < 300 else 'no')
'@
$py = Join-Path $env:TEMP 'za_lang_check.py'; [IO.File]::WriteAllText($py, $check)
$t0 = Get-Date; $seen = $false
while (((Get-Date) - $t0).TotalSeconds -lt $Timeout) {
    $shot = (& $sh -Shot ng-wait | Select-Object -Last 1)
    if ((& python $py $shot) -eq 'yes') { $seen = $true; break }
    & $k -Seq 'WAIT3' | Out-Null
}
if (-not $seen) { "the language screen did not appear within $Timeout s"; exit 1 }
& $k -Seq 'WAIT2,RT,D,D,D,A,WAIT4' -Gap 1 | Out-Null    # language: Simplified Chinese
& $k -Seq 'SR,A,WAIT4' -Gap 1.5 | Out-Null              # looks: the stick picks one, A takes it
& $sh -Cmd "uitest uiInput inputText 1420 615 $Name" | Out-Null   # the name dialog's field (the system keyboard opens)
& $k -Seq 'WAIT1.5' | Out-Null
& $sh -Cmd 'uitest uiInput click 2336 1160' | Out-Null  # the keyboard's 完成 submits the dialog
& $k -Seq 'WAIT3,D,D,A' -Gap 1 | Out-Null               # profile: 以这样开始!
& $sh -Shot ng-start
