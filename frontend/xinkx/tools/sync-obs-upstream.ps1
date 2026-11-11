# Sync upstream OBS into a disposable integration branch.
# Read-only unless -Integrate is passed. Does not push or force-update.
[CmdletBinding()]
param(
    [ValidateSet('master', 'release/33.1', 'release/33.0', 'release/32.2')]
    [string]$Branch = 'master',
    [switch]$Integrate
)

$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path

function Invoke-Git {
    param([string[]]$Arguments)
    & git -C $repository @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "git failed: $($Arguments -join ' ')"
    }
}

$current = (& git -C $repository branch --show-current).Trim()
if ($LASTEXITCODE -ne 0 -or $current -ne 'xinkx/main') {
    throw 'Switch to xinkx/main before checking the upstream integration.'
}
$dirty = (& git -C $repository status --porcelain)
if ($LASTEXITCODE -ne 0 -or $dirty) {
    throw 'Working tree is not clean. Commit or stash local changes first.'
}

$remotes = @(& git -C $repository remote)
if ($remotes -notcontains 'upstream') {
    if ($Integrate) {
        Invoke-Git -Arguments @('remote', 'add', 'upstream', 'https://github.com/obsproject/obs-studio.git')
    } else {
        Write-Host 'Upstream remote not configured. Check mode will fetch via URL.'
    }
}

if ($remotes -contains 'upstream' -or $Integrate) {
    Invoke-Git -Arguments @('fetch', 'upstream', $Branch, '--no-tags')
    $revision = 'FETCH_HEAD'
} else {
    Invoke-Git -Arguments @('fetch', 'https://github.com/obsproject/obs-studio.git', $Branch, '--no-tags')
    $revision = 'FETCH_HEAD'
}

Invoke-Git -Arguments @('log', '-1', '--oneline', $revision)
Invoke-Git -Arguments @('rev-list', '--left-right', '--count', "HEAD...$revision")
if (-not $Integrate) {
    Write-Host 'Dry run only. Add -Integrate to create a review branch and start merge.'
    exit 0
}

$target = 'xinkx/integrate-obs-' + ($Branch -replace '/', '-') + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
Invoke-Git -Arguments @('switch', '-c', $target)
Write-Host "Integrating upstream in $target"
& git -C $repository merge --no-ff --no-commit $revision
if ($LASTEXITCODE -ne 0) {
    Write-Host 'Merge conflicts. Resolve them only on this integration branch; xinkx/main is intact.'
    & git -C $repository status --short
    exit 1
}
Write-Host 'Changes staged for review; NO commit and NO push performed.'
Write-Host 'Build/test, then commit the reviewed merge on this integration branch.'
