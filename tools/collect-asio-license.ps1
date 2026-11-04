# Collects the Steinberg ASIO SDK licence text(s) shipped inside the SDK into one file for the installer.
param([Parameter(Mandatory)][string]$Out)
$sdk = Join-Path $PSScriptRoot '..\external\asiosdk\sdk'
$files = Get-ChildItem -Path $sdk -Recurse -File | Where-Object { $_.Name -match '(?i)licen[cs]e' -or $_.Name -match '(?i)^gpl' }
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine('Steinberg ASIO SDK licence text(s), copied from the SDK used to build this program.')
[void]$sb.AppendLine('ASIO is a trademark and software of Steinberg Media Technologies GmbH.')
[void]$sb.AppendLine('This program uses the SDK under its GPLv3 option (see LICENSE.txt).')
[void]$sb.AppendLine('')
if (-not $files) { throw 'No licence file found inside the ASIO SDK folder; refusing to ship without it.' }
foreach ($f in $files) {
  [void]$sb.AppendLine("===== $($f.FullName.Substring($sdk.Length).TrimStart('\')) =====")
  [void]$sb.AppendLine((Get-Content -Raw $f.FullName))
}
[IO.File]::WriteAllText($Out, $sb.ToString())
Write-Host "wrote $Out from $($files.Count) file(s)"
