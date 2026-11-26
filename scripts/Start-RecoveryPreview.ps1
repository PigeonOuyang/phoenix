# SPDX-License-Identifier: Unlicense
[CmdletBinding()]
param(
    [ValidateSet('Build', 'Accept', 'Run', 'Snapshot', 'All')]
    [string] $Action = 'All',
    [string] $Flows,
    [string] $FontDirectory,
    [string] $FontImage,
    [string] $OutputDirectory,
    [ValidateSet('host', 'headset')]
    [string] $Mode = 'host',
    [string] $Scenario = 'observed',
    [string] $At = '-',
    [string] $Snapshot,
    [ValidateSet('en', 'zh-CN')]
    [string] $Locale = 'en',
    [switch] $ReducedMotion,
    [switch] $Play,
    [switch] $MirrorDeps
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../../..')).Path
$root = (Resolve-Path -LiteralPath (Join-Path $repo '../..')).Path
$linux = Join-Path $root 'tools/invoke_rea_linux.ps1'
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repo 'out/recovery-preview'
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$fixture = Join-Path $OutputDirectory 'flows.json'
$atlas = Join-Path $PSScriptRoot '../src/recovery-ui/breeze/atlas.png'
$testExecutable = Join-Path $OutputDirectory 'accept-recovery-preview'
$windowExecutable = Join-Path $OutputDirectory 'recovery-preview'

function Invoke-Linux([string] $Tool, [string[]] $Arguments) {
    & $linux -Tool $Tool -ToolArguments $Arguments -WorkingDirectory $repo
    if ($LASTEXITCODE -ne 0) { throw "Recovery preview command failed: $Tool ($LASTEXITCODE)" }
}

if ($Action -notin @('Run', 'Snapshot')) {
    if (-not $Flows) {
        $Flows = Join-Path $root 'pico4-recovery-progress-design/assets/flows.js'
    }
    $Flows = (Resolve-Path -LiteralPath $Flows).Path
    New-Item -Path $OutputDirectory -ItemType Directory -Force | Out-Null
    & node (Join-Path $PSScriptRoot '../tooling/recovery-preview/export-flows.cjs') $Flows > $fixture
    if ($LASTEXITCODE -ne 0) { throw 'Could not export approved recovery scenarios.' }

    $headers = ''
    $mirror = Join-Path $OutputDirectory 'deps'
    if ($MirrorDeps) {
        Invoke-Linux bash @((Join-Path $PSScriptRoot 'prepare-recovery-preview-deps.sh'), $mirror)
    }
    $mirroredHeaders = Join-Path $mirror 'headers'
    if (Test-Path -LiteralPath $mirroredHeaders -PathType Container) {
        $headers = $mirroredHeaders
    }
    $buildMode = if ($Action -eq 'Accept') { '--accept-only' } else { '--all' }
    # A dash keeps the positional argument intact when calling wsl.exe.
    if (-not $headers) { $headers = '-' }
    Invoke-Linux bash @((Join-Path $PSScriptRoot 'build-recovery-preview.sh'), $OutputDirectory, $headers, $buildMode)
    Invoke-Linux env @($testExecutable, $fixture)
    if ($Action -eq 'Accept') { exit 0 }
}

if (-not $FontDirectory) {
    $FontDirectory = Join-Path $OutputDirectory 'fonts'
}
$FontDirectory = [IO.Path]::GetFullPath($FontDirectory)
if ($FontImage) {
    $FontImage = (Resolve-Path -LiteralPath $FontImage).Path
    Invoke-Linux env @('PYTHONPATH=.', 'PYTHONDONTWRITEBYTECODE=1', 'python3',
        (Join-Path $PSScriptRoot '../tooling/recovery-preview/extract-fonts.py'), $FontImage, $FontDirectory)
}
$fontNames = @('NotoSans-Regular.ttf', 'NotoSans-Bold.ttf', 'NotoSansCJK-SC-Regular.otf', 'NotoSansCJK-SC-Bold.otf')
foreach ($name in $fontNames) {
    if (-not (Test-Path -LiteralPath (Join-Path $FontDirectory $name) -PathType Leaf)) {
        throw "Missing $name. Supply -FontDirectory or -FontImage pointing to an existing local Recovery image."
    }
}
if ($Action -ne 'Run') {
    $renderArgs = @($windowExecutable, '--render-check', $fixture, $FontDirectory, $atlas)
    if ($Action -eq 'Snapshot' -and -not $Snapshot) {
        $Snapshot = Join-Path $OutputDirectory 'headset-build-en.png'
    }
    if ($Snapshot) { $renderArgs += [IO.Path]::GetFullPath($Snapshot) }
    Invoke-Linux env $renderArgs
}
if ($Action -in @('Build', 'Snapshot')) { exit 0 }
foreach ($path in @($windowExecutable, $fixture, $atlas)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing preview input: $path"
    }
}
$motion = if ($ReducedMotion) { '1' } else { '0' }
$autoplay = if ($Play) { '1' } else { '0' }
# The shared toolbox accepts a command name or an absolute Linux executable path.
Invoke-Linux env @($windowExecutable, $fixture, $FontDirectory, $atlas, $Mode, $Scenario, $Locale, $motion, $autoplay, $At)
