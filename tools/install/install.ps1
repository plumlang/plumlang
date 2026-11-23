# SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
#
# Install Plumlang on Windows:
#
#   irm https://plum.elixpo.com/install.ps1 | iex
#
# Settings, as environment variables:
#   PLUM_VERSION   the version to install, e.g. 1.2.3 (default: the newest release)
#   PLUM_INSTALL   where to install (default: %LOCALAPPDATA%\Plumlang); the binary goes in bin\
#
# The archive is checked against the release's SHA256SUMS before anything is
# installed. The only other change is adding bin\ to the user's PATH.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$Repo = 'plumlang/plumlang'
$Base = if ($env:PLUM_DOWNLOAD_BASE) { $env:PLUM_DOWNLOAD_BASE } else { "https://github.com/$Repo/releases/download" }
$Api = if ($env:PLUM_API) { $env:PLUM_API } else { "https://api.github.com/repos/$Repo/releases?per_page=1" }
$Dest = if ($env:PLUM_INSTALL) { $env:PLUM_INSTALL } else { Join-Path $env:LOCALAPPDATA 'Plumlang' }

function Fail($msg) { Write-Error "plum install: $msg"; exit 1 }

$arch = $env:PROCESSOR_ARCHITECTURE
if ($env:PROCESSOR_ARCHITEW6432) { $arch = $env:PROCESSOR_ARCHITEW6432 }
switch ($arch) {
    'AMD64' { $target = 'windows-x86_64' }
    'ARM64' { $target = 'windows-arm64' }
    default { Fail "no build for Windows on $arch; build from source: https://github.com/$Repo" }
}

$version = "$env:PLUM_VERSION".TrimStart('v')
if (-not $version -or $version -eq 'latest') {
    try { $releases = Invoke-RestMethod -Uri $Api -Headers @{ 'User-Agent' = 'plum-install' } }
    catch { Fail 'could not reach GitHub to find the newest release' }
    if (-not $releases) { Fail 'no release found; set PLUM_VERSION' }
    $version = "$(@($releases)[0].tag_name)".TrimStart('v')
}

$archive = "plum-$version-$target.zip"
$tmp = Join-Path ([IO.Path]::GetTempPath()) ([Guid]::NewGuid())
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    Write-Host "Installing Plumlang $version ($target) into $Dest"
    try {
        Invoke-WebRequest -Uri "$Base/v$version/$archive" -OutFile (Join-Path $tmp $archive) -UseBasicParsing
        Invoke-WebRequest -Uri "$Base/v$version/SHA256SUMS" -OutFile (Join-Path $tmp 'SHA256SUMS') -UseBasicParsing
    } catch { Fail "could not download $archive" }

    $want = $null
    foreach ($line in Get-Content (Join-Path $tmp 'SHA256SUMS')) {
        $parts = $line -split '\s+'
        if ($parts.Count -ge 2 -and $parts[1].TrimStart('*') -eq $archive) { $want = $parts[0] }
    }
    if (-not $want) { Fail "SHA256SUMS has no entry for $archive" }
    $got = (Get-FileHash -Algorithm SHA256 (Join-Path $tmp $archive)).Hash
    if ($got -ne $want.ToUpper()) { Fail "checksum mismatch for $archive; nothing was installed" }

    Expand-Archive -Path (Join-Path $tmp $archive) -DestinationPath $tmp
    $bin = Join-Path $Dest 'bin'
    New-Item -ItemType Directory -Force -Path $bin | Out-Null
    Copy-Item -Force (Join-Path $tmp "plum-$version-$target\plum.exe") (Join-Path $bin 'plum.exe')

    $exe = Join-Path $bin 'plum.exe'
    $installed = & $exe version
    if ($LASTEXITCODE -ne 0) { Fail "installed, but $exe does not run" }
    Write-Host "Installed: $installed"

    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    if (-not (($userPath -split ';') -contains $bin)) {
        [Environment]::SetEnvironmentVariable('Path', "$bin;$userPath", 'User')
        Write-Host "Added $bin to your PATH. Open a new terminal to use plum."
    }
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
