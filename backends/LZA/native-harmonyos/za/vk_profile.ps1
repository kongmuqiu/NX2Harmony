# Export the phone's Vulkan driver as a Vulkan Profiles JSON file (the format `vulkaninfo --json` writes) and pull it to
# artifacts\runs\vk-profile-<date>.json. HarmonyOS has no vulkaninfo and hdc shell does not run self-built programs, so
# the app writes it at its start (TOTK_VK_PROFILE, overlay/android/gfx/vk_profile.inc): this starts the runtime without
# the game page (EntryAbility's command=start); no game data is needed, and the game itself is stopped again.
# The app comes to the front for a few seconds: run it when nothing else is using the phone.
#   native-harmonyos\za\vk_profile.ps1 [-Install <hap>]
param([string]$Install = '')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\..\ohos_env.ps1"
$files = '/data/storage/el2/base/haps/entry/files'
$app = "/data/app/el2/100/base/$bundle/haps/entry/files/android"
function Shell([string]$c) { (& $hdc @d shell $c | Out-String).Trim() }
function StartHeadless {
    $r = Shell "aa start -b $bundle -a EntryAbility --ps command start"
    if ($r -notmatch 'success') { throw "aa start: $r" }
}
if ($Install) { (& $hdc @d install -r $Install | Out-String).Trim().Split("`n")[0] }
$null = Shell "aa force-stop $bundle"
if ((Shell "ls -d $app 2>/dev/null") -eq '') {
    # the work folder is made by the runtime's start (hdc cannot make folders in the sandbox): start once without the flag
    StartHeadless; Start-Sleep 4; $null = Shell "aa force-stop $bundle"
}
& "$PSScriptRoot\devenv.ps1" 'TOTK_VK_PROFILE=1' | Out-Null
try {
    StartHeadless
    # totk-native.log starts empty at every start (O_TRUNC): the line is this run's
    $log = ''
    for ($i = 0; $i -lt 20 -and -not $log; $i++) { Start-Sleep 1; $log = Shell "grep -a 'Vulkan profile' $app/totk-native.log | tail -1" }
    $null = Shell "aa force-stop $bundle"
    if ($log -notmatch 'written') { throw "no profile after $i s: $(if ($log) { $log } else { 'no [nvn-vk] Vulkan profile line in files/android/totk-native.log' })" }
    $log
    $out = Join-Path $runs ("vk-profile-{0}.json" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
    New-Item -ItemType Directory -Force $runs | Out-Null
    (& $hdc @d file recv -b $bundle "$files/android/vk-profile.json" $out | Out-String).Trim().Split("`n")[-1]
    $out
} finally {
    & "$PSScriptRoot\devenv.ps1" 'TOTK_VK_PROFILE=' | Out-Null   # the next start of the game exports nothing
    $null = Shell "aa force-stop $bundle"
}
