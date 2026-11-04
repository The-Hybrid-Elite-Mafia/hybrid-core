<#
  fetch-asio-sdk.ps1 — downloads the official Steinberg ASIO SDK 2.3.4 into
  external\asiosdk\sdk, verifies its SHA-512, and applies the one-line
  delete/delete[] fix in host\pc\asiolist.cpp (same fix vcpkg ships).

  The SDK is Steinberg's code (dual-licensed: GPLv3 or Steinberg's proprietary
  licence). It is NEVER committed to the repo (.gitignore blocks it). Running this
  script means you accept Steinberg's licence; the licence files are kept next
  to the SDK for reference.

  Usage (PowerShell, from the repo folder):
      powershell -ExecutionPolicy Bypass -File tools\fetch-asio-sdk.ps1
#>
param(
    [string]$Destination = (Join-Path $PSScriptRoot '..\external\asiosdk\sdk')
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'   # Invoke-WebRequest is slow with the progress bar on

$Url    = 'https://download.steinberg.net/sdk_downloads/ASIO-SDK_2.3.4_2025-10-15.zip'
$Sha512 = '57de2c0cd0df0783275987e08255abfa49da12982f9d462ac40b7f57300c36e024dcb65d100b799fb3c96a9c7c5ee86e61ceb0e68d2839324206c1629d3905ed'

$work    = Join-Path ([IO.Path]::GetTempPath()) ('hc-asiosdk-' + [Guid]::NewGuid().ToString('N'))
$zip     = Join-Path $work 'asiosdk.zip'
$extract = Join-Path $work 'x'
New-Item -ItemType Directory -Force -Path $work, $extract | Out-Null

try {
    Write-Host "Downloading $Url"
    Invoke-WebRequest -Uri $Url -OutFile $zip -UseBasicParsing

    $actual = (Get-FileHash -Path $zip -Algorithm SHA512).Hash.ToLowerInvariant()
    if ($actual -ne $Sha512) {
        throw "SHA-512 mismatch. Expected $Sha512 but got $actual. Not installing."
    }
    Write-Host 'Checksum OK.'

    Expand-Archive -Path $zip -DestinationPath $extract -Force

    $asioH = Get-ChildItem -Path $extract -Recurse -Filter 'asio.h' |
             Where-Object { $_.Directory.Name -eq 'common' } |
             Select-Object -First 1
    if (-not $asioH) { throw 'common\asio.h not found in the archive. SDK layout changed.' }
    $sdkRoot = $asioH.Directory.Parent.FullName

    if (Test-Path $Destination) { Remove-Item -Recurse -Force $Destination }
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    Copy-Item -Path (Join-Path $sdkRoot '*') -Destination $Destination -Recurse -Force

    # Byte-preserving patch: mismatched new[]/delete in deleteDrvStruct().
    $list   = Join-Path $Destination 'host\pc\asiolist.cpp'
    $latin1 = [Text.Encoding]::GetEncoding(28591)
    $text   = [IO.File]::ReadAllText($list, $latin1)
    if ($text.Contains('delete lpdrv;')) {
        [IO.File]::WriteAllText($list, $text.Replace('delete lpdrv;', 'delete[] lpdrv;'), $latin1)
        Write-Host 'Patched host\pc\asiolist.cpp (delete -> delete[]).'
    }

    foreach ($req in 'common\asio.h','common\asio.cpp','host\asiodrivers.cpp','host\pc\asiolist.cpp') {
        if (-not (Test-Path (Join-Path $Destination $req))) { throw "Missing $req after install." }
    }
    Write-Host "ASIO SDK installed to $((Resolve-Path $Destination).Path)"
    Write-Host 'Re-run cmake with -DHC_WITH_ASIO=ON to build the ASIO version.'
}
finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
