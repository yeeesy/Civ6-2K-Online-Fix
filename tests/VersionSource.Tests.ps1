[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$versionFile = Join-Path $projectRoot 'version.json'
if (-not (Test-Path -LiteralPath $versionFile -PathType Leaf)) {
    throw 'version.json is missing; release version does not have a single source.'
}
$identity = Get-Content -LiteralPath $versionFile -Raw | ConvertFrom-Json
if ([string]::IsNullOrWhiteSpace($identity.Version) -or
    $identity.Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?$' -or
    [string]::IsNullOrWhiteSpace($identity.WindowsVersion) -or
    $identity.WindowsVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$') {
    throw 'version.json contains an invalid semantic or Windows version.'
}

$functionalConsumers = @(
    (Join-Path $projectRoot 'build.ps1')
    (Join-Path $projectRoot 'package.ps1')
    (Join-Path $projectRoot 'src\Civ6_2K_Online_Fix.rc')
    (Join-Path $projectRoot 'src\Civ6_2K_Online_Fix.manifest.in')
)
foreach ($path in $functionalConsumers) {
    $content = Get-Content -LiteralPath $path -Raw
    if ($content.Contains([string]$identity.Version)) {
        throw "Functional version consumer contains a duplicated semantic version: $path"
    }
}
if (Test-Path -LiteralPath (Join-Path $projectRoot 'src\ProductIdentity.h')) {
    throw 'The checked-in ProductIdentity.h duplicates generated version identity.'
}

$manifestTemplate = Join-Path $projectRoot 'src\Civ6_2K_Online_Fix.manifest.in'
$manifestContent = Get-Content -LiteralPath $manifestTemplate -Raw
if (($manifestContent.Split('@WINDOWS_VERSION@').Count - 1) -ne 1) {
    throw 'Manifest template must contain exactly one generated-version placeholder.'
}
$buildContent = Get-Content -LiteralPath (Join-Path $projectRoot 'build.ps1') -Raw
if (-not $buildContent.Contains("Replace(`$manifestVersionPlaceholder, `$windowsVersion)")) {
    throw 'Build script does not derive the generated manifest from WindowsVersion.'
}

Write-Output 'PASS: release identity has one functional source in version.json'
