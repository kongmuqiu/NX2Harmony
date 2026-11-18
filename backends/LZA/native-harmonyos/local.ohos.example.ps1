# Copy to native-harmonyos\local.ohos.ps1 (ignored by git) and fill in this machine's values. The PowerShell scripts
# and the Python tools both read this file. Leave a line out (or empty) to use the default.
$env:TOTK_DEVECO = 'C:\Program Files\Huawei\DevEco Studio'   # DevEco Studio itself; hdc and the SDK are found inside it
$env:TOTK_HDC_TARGET = ''                                      # the phone's key from `hdc list targets` (needed with more than one device)
$env:TOTK_HDC_SERVER = ''                                      # a private hdc server such as 127.0.0.1:18710; empty for the default one
# $env:TOTK_UPSTREAM = 'X:\src\totk-android'                   # the totk-android checkout (default: ..\totk-android next to this repository)
# $env:TOTK_PWSH = 'C:\Program Files\PowerShell\7\pwsh.exe'    # PowerShell 7 when pwsh is not on PATH
