# Downloads Wintun 0.14.1 (WireGuard LLC), checks the published SHA-256 and puts
# wintun.dll (amd64) and its LICENSE.txt into .\wintun for embedding. Used by build.bat.
$ErrorActionPreference = 'Stop'
$sha = '07C256185D6EE3652E09FA55C0B673E2624B565E02C4B9091C79CA7D2F24EF51'
$zip = Join-Path $env:TEMP 'wintun-0.14.1.zip'
$out = Join-Path $env:TEMP 'wintun-0.14.1'
Invoke-WebRequest -UseBasicParsing 'https://www.wintun.net/builds/wintun-0.14.1.zip' -OutFile $zip
if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne $sha) { throw 'wintun-0.14.1.zip: SHA-256 mismatch' }
Expand-Archive $zip $out -Force
New-Item -ItemType Directory -Force (Join-Path $PSScriptRoot 'wintun') | Out-Null
Copy-Item (Join-Path $out 'wintun\bin\amd64\wintun.dll') (Join-Path $PSScriptRoot 'wintun')
Copy-Item (Join-Path $out 'wintun\LICENSE.txt') (Join-Path $PSScriptRoot 'wintun')
Write-Host 'Wintun ready'
