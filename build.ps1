<#
.SYNOPSIS
  Squizzy Studio build wrapper: loads MSVC (vcvars64), configures with CMake + Ninja, builds, runs CTest.

.EXAMPLE
  .\build.ps1                                    # windows-debug -> build\windows-debug, build + test
  .\build.ps1 -Preset windows-release
  .\build.ps1 -BuildDir build\agent-engine       # private dir for a parallel agent (no clobbering)
  .\build.ps1 -BuildDir build\agent-ui -Target squi -NoTest
  .\build.ps1 -TestFilter "settings|xml"         # ctest -R
  .\build.ps1 -ExcludeLabel gui                 # ctest -LE (skip the window smoke test)
  .\build.ps1 -Clean                             # wipe the build dir first
  .\build.ps1 -CMakeArgs "-DSQ_WARNINGS_AS_ERRORS=ON"
#>
[CmdletBinding()]
param(
  [ValidateSet('windows-debug', 'windows-release')]
  [string]$Preset = 'windows-debug',
  [string]$BuildDir = '',
  [string]$Target = '',
  [string]$TestFilter = '',
  [string]$ExcludeLabel = '',
  [string[]]$CMakeArgs = @(),
  [switch]$NoTest,
  [switch]$Clean,
  [switch]$ConfigureOnly,
  [int]$Jobs = 0
)

$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot

function Find-VsRoot {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (Test-Path $vswhere) {
    $p = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if ($p -and (Test-Path $p)) { return $p.Trim() }
  }
  foreach ($c in @(
      "$env:ProgramFiles\Microsoft Visual Studio\2022\Community",
      "$env:ProgramFiles\Microsoft Visual Studio\2022\Professional",
      "$env:ProgramFiles\Microsoft Visual Studio\2022\Enterprise",
      "$env:ProgramFiles\Microsoft Visual Studio\2022\BuildTools")) {
    if (Test-Path "$c\VC\Auxiliary\Build\vcvars64.bat") { return $c }
  }
  throw 'Visual Studio 2022 with C++ tools not found.'
}

function Import-VcVars([string]$vsRoot) {
  if ($env:VSCMD_ARG_TGT_ARCH -eq 'x64' -and (Get-Command cl.exe -ErrorAction SilentlyContinue)) { return }
  $bat = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
  $lines = & cmd.exe /d /s /c "`"$bat`" >nul 2>&1 && set"
  if ($LASTEXITCODE -ne 0) { throw "vcvars64.bat failed ($LASTEXITCODE)" }
  foreach ($l in $lines) {
    $i = $l.IndexOf('=')
    if ($i -gt 0) { [Environment]::SetEnvironmentVariable($l.Substring(0, $i), $l.Substring($i + 1), 'Process') }
  }
  # Bundled CMake + Ninja (vcvars usually adds them, but make sure).
  $cm = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake'
  $env:PATH = "$cm\CMake\bin;$cm\Ninja;$env:PATH"
}

$vs = Find-VsRoot
Import-VcVars $vs

$cmake = (Get-Command cmake.exe -ErrorAction Stop).Source
$ctest = Join-Path (Split-Path $cmake) 'ctest.exe'

if (-not $BuildDir) { $BuildDir = Join-Path 'build' $Preset }
if (-not [System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $Root $BuildDir }
$buildType = if ($Preset -eq 'windows-release') { 'RelWithDebInfo' } else { 'Debug' }

if ($Clean -and (Test-Path $BuildDir)) {
  Write-Host "== clean $BuildDir"
  Remove-Item -Recurse -Force $BuildDir
}

Write-Host "== configure ($Preset, $buildType) -> $BuildDir"
$cfg = @('-S', $Root, '-B', $BuildDir, '-G', 'Ninja', "-DCMAKE_BUILD_TYPE=$buildType",
  '-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl')
# `powershell -File build.ps1 -CMakeArgs "-DA=1","-DB=2"` arrives as ONE comma-joined string, and
# a single quoted "-DA=1 -DB=2" arrives as one string too. Split both forms back into separate
# arguments at a comma/space that starts a new -D/-U/-G/-W option (values keep their ';' lists).
foreach ($a in $CMakeArgs) {
  foreach ($p in ($a -split '[,\s]+(?=-[DUGW])')) { if ($p.Trim()) { $cfg += $p.Trim() } }
}
& $cmake @cfg
if ($LASTEXITCODE -ne 0) { Write-Host '== CONFIGURE FAILED'; exit $LASTEXITCODE }
if ($ConfigureOnly) { exit 0 }

Write-Host '== build'
$b = @('--build', $BuildDir)
if ($Target) { $b += @('--target', $Target) }
if ($Jobs -gt 0) { $b += @('--parallel', "$Jobs") }
& $cmake @b
if ($LASTEXITCODE -ne 0) { Write-Host '== BUILD FAILED'; exit $LASTEXITCODE }

if ($NoTest) { Write-Host '== done (tests skipped)'; exit 0 }

Write-Host '== test'
$t = @('--test-dir', $BuildDir, '--output-on-failure', '--timeout', '300')
if ($TestFilter) { $t += @('-R', $TestFilter) }
if ($ExcludeLabel) { $t += @('-LE', $ExcludeLabel) }
& $ctest @t
$rc = $LASTEXITCODE
if ($rc -ne 0) { Write-Host '== TESTS FAILED' } else { Write-Host '== ALL GREEN' }
exit $rc
