<#
.SYNOPSIS
  Stages the release build the way CI packages it and builds the Squizzy Studio x64 MSI with WiX v5.

.DESCRIPTION
  1. Reads SQ_VERSION from the repo VERSION file (unless -Version is given).
  2. Stages <OutDir>/stage/SquizzyStudio with exactly what the app needs, mirroring the
     "Package (Windows ...)" step of .github/workflows/ci.yml: the app, the command-line tools,
     sqplatform.dll, services/, fonts/, themes/, scripts/, THIRD_PARTY_NOTICES.txt, README.md and the
     app-local VC++ runtime from the newest Visual Studio redist folder. Debug artefacts (*.pdb, *.ilk),
     sqtest_*.exe and the test-only folders (testmodules/, testplugins*) are never staged.
  3. Runs "wix build" on installer/SquizzyStudio.wxs and writes <OutDir>/SquizzyStudio-<version>-x64.msi.

  Every failure is terminating: a missing input, a missing redist, a non-zero wix exit code or a missing
  output all stop the script with a non-zero exit code. Nothing is installed.

.PARAMETER BuildDir
  Build directory whose bin/ holds the release binaries. Default build/release (relative to the repo root).
.PARAMETER OutDir
  Where the stage folder and the MSI go. Default dist (relative to the repo root).
.PARAMETER Version
  Product version a.b.c. Default: SQ_VERSION from VERSION.
.PARAMETER Wix
  Path of the wix executable. Default: "wix" on PATH, else %USERPROFILE%\.dotnet\tools\wix.exe.
.PARAMETER NoVcRuntime
  Skip the app-local VC++ runtime (only for machines without Visual Studio; the MSI then needs the
  redistributable to be installed separately).

.EXAMPLE
  .\installer\build-installer.ps1
  .\installer\build-installer.ps1 -BuildDir build\windows-release -OutDir out\dist
#>
[CmdletBinding()]
param(
  [string]$BuildDir = 'build/release',
  [string]$OutDir = 'dist',
  [string]$Version,
  [string]$Wix,
  [switch]$NoVcRuntime
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $PSScriptRoot
function Resolve-RepoPath([string]$p) {
  if ([System.IO.Path]::IsPathRooted($p)) { return [System.IO.Path]::GetFullPath($p) }
  return [System.IO.Path]::GetFullPath((Join-Path $repoRoot $p))
}
function Fail([string]$msg) { Write-Error $msg; exit 1 }

$buildDirFull = Resolve-RepoPath $BuildDir
$outDirFull = Resolve-RepoPath $OutDir
$bin = Join-Path $buildDirFull 'bin'
$wxs = Join-Path $PSScriptRoot 'SquizzyStudio.wxs'
$licenseRtf = Join-Path $PSScriptRoot 'License.rtf'

if (!(Test-Path $bin)) { Fail "build output folder not found: $bin (build the windows-release preset first)" }
foreach ($f in $wxs, $licenseRtf) { if (!(Test-Path $f)) { Fail "installer source missing: $f" } }

# --- version ---------------------------------------------------------------------------------------------
if (-not $Version) {
  $versionFile = Join-Path $repoRoot 'VERSION'
  $m = Select-String -Path $versionFile -Pattern '^SQ_VERSION=(.+)$' | Select-Object -First 1
  if (-not $m) { Fail "SQ_VERSION not found in $versionFile" }
  $Version = $m.Matches[0].Groups[1].Value.Trim()
}
# MSI ProductVersion must be numeric major.minor.build; drop any pre-release suffix such as "-dev".
$msiVersion = ($Version -split '[-+]')[0]
if ($msiVersion -notmatch '^\d{1,3}\.\d{1,3}\.\d{1,5}(\.\d+)?$') {
  Fail "version '$Version' is not an MSI-compatible a.b.c version"
}

# --- stage -----------------------------------------------------------------------------------------------
$stage = Join-Path $outDirFull 'stage\SquizzyStudio'
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null

# Required files: the shell, the plugin scanner and the notices. Missing any of them is a broken build.
foreach ($name in 'SquizzyStudio.exe', 'sq-pluginscan.exe', 'THIRD_PARTY_NOTICES.txt') {
  $src = Join-Path $bin $name
  if (!(Test-Path $src)) { Fail "required file missing from $bin : $name" }
  Copy-Item $src $stage
}
# Optional command-line tools (present in a full build, absent in an SQ_ONLY build).
foreach ($name in 'sq-pack.exe', 'sq-skinc.exe') {
  $src = Join-Path $bin $name
  if (Test-Path $src) { Copy-Item $src $stage }
}
# Top-level DLLs (sqplatform.dll and anything the app links against directly).
Get-ChildItem (Join-Path $bin '*.dll') | Copy-Item -Destination $stage
# Resource and module folders. testmodules/ and testplugins*/ are deliberately not in this list.
foreach ($d in 'services', 'fonts', 'themes', 'scripts') {
  $src = Join-Path $bin $d
  if (Test-Path $src) { Copy-Item -Recurse $src (Join-Path $stage $d) }
}
# Strip debug artefacts and any test binary that slipped through.
Get-ChildItem -Recurse $stage -Include '*.pdb', '*.ilk', '*.exp', '*.lib', 'sqtest_*.exe' -File | Remove-Item -Force

# App-local VC++ runtime so the installed app runs on a clean Windows install.
if (-not $NoVcRuntime) {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (!(Test-Path $vswhere)) { Fail "vswhere.exe not found ($vswhere); install Visual Studio or pass -NoVcRuntime" }
  $vs = & $vswhere -latest -property installationPath
  if (-not $vs) { Fail 'vswhere found no Visual Studio installation' }
  $crt = Get-ChildItem "$vs/VC/Redist/MSVC/*/x64/Microsoft.VC*.CRT" -Directory -ErrorAction SilentlyContinue |
    Sort-Object FullName | Select-Object -Last 1
  if (-not $crt) { Fail "no x64 VC++ redist folder under $vs\VC\Redist\MSVC" }
  Get-ChildItem (Join-Path $crt.FullName '*.dll') | Copy-Item -Destination $stage
  Write-Host "VC++ runtime: $($crt.FullName)"
}

Copy-Item (Join-Path $repoRoot 'README.md') $stage

$staged = Get-ChildItem -Recurse -File $stage | Measure-Object -Property Length -Sum
Write-Host ("staged: {0} files, {1} MB -> {2}" -f $staged.Count, [math]::Round($staged.Sum / 1MB, 1), $stage)

# --- wix -------------------------------------------------------------------------------------------------
if (-not $Wix) {
  $cmd = Get-Command wix -ErrorAction SilentlyContinue
  if ($cmd) { $Wix = $cmd.Source }
  else { $Wix = Join-Path $env:USERPROFILE '.dotnet\tools\wix.exe' }
}
if (!(Test-Path $Wix)) { Fail "wix not found ($Wix). Install with: dotnet tool install --global wix" }

New-Item -ItemType Directory -Force $outDirFull | Out-Null
$msi = Join-Path $outDirFull "SquizzyStudio-$Version-x64.msi"
if (Test-Path $msi) { Remove-Item -Force $msi }

$wixArgs = @(
  'build', $wxs,
  '-arch', 'x64',
  '-d', "StageDir=$stage",
  '-d', "Version=$msiVersion",
  '-d', "LicenseRtf=$licenseRtf",
  '-ext', 'WixToolset.UI.wixext',
  '-pdbtype', 'none',
  '-o', $msi
)
Write-Host "$Wix $($wixArgs -join ' ')"
# wix reports errors on stderr; merge and echo every line so a failure is readable however the script is invoked.
& $Wix @wixArgs 2>&1 | ForEach-Object { Write-Host "$_" }
if ($LASTEXITCODE -ne 0) { Fail "wix build failed with exit code $LASTEXITCODE" }
if (!(Test-Path $msi)) { Fail "wix reported success but $msi does not exist" }

$size = (Get-Item $msi).Length
Write-Host ("MSI: {0} ({1} MB, {2} bytes)" -f $msi, [math]::Round($size / 1MB, 1), $size)
