# A/B of two seed packs on the phone by swapping the default seed.vkpack (the app cannot be given a second seed folder:
# neither hdc shell nor a bundle file send may create a directory in its sandbox). Order: push B, warm-up run of B
# (discarded: its changed kernels compile), then B, A, B, A, each pushed before its run (~90 s a push over USB).
# Every run: sched_matrix with the given extra switches, -Legs 0 -After 30 -Rec <n>. The default pack is A again at the end.
#   native-harmonyos\oot\seed_ab.ps1 -Tag a122c -PackA prepared\oot\seed-build\seed-a121.vkpack -PackB prepared\oot\seed-build\seed-a122C.vkpack
param([Parameter(Mandatory = $true)][string]$Tag, [Parameter(Mandatory = $true)][string]$PackA, [Parameter(Mandatory = $true)][string]$PackB,
      [string]$Extra = 'TOTK_VK_PASS_PROFILE=20', [int]$Rec = 300, [switch]$NoWarm)
. "$PSScriptRoot\..\ohos_env.ps1"
Set-Location $port
$files = '/data/storage/el2/base/haps/entry/files/android'
function Push-Pack([string]$p) {
    $null = & $hdc @d shell "aa force-stop $bundle"
    $t0 = Get-Date
    $r = (& $hdc @d file send -b $bundle (Resolve-Path $p) "$files/shaders/oot131072/seed.vkpack" | Out-String).Trim()
    "{0} pushed in {1:N0} s: {2}" -f (Split-Path $p -Leaf), ((Get-Date) - $t0).TotalSeconds, ($r -split "`n")[-1]
}
$steps = @()
if (-not $NoWarm) { $steps += , @('B', 'warmB') }
$steps += , @('B', 'b1'); $steps += , @('A', 'a1'); $steps += , @('B', 'b2'); $steps += , @('A', 'a2')
foreach ($s in $steps) {
    Push-Pack ($(if ($s[0] -eq 'A') { $PackA } else { $PackB }))
    & "$PSScriptRoot\sched_matrix.ps1" -Tag "$Tag-$($s[1])" -Rec $Rec -Legs 0 -After 30 -Configs @("$($s[1])=$Extra") | Select-Object -Last 1
}
"done: the default seed is $(Split-Path $PackA -Leaf) again"
