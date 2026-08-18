[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$workflowPath = Join-Path $projectRoot '.github\workflows\windows-ci.yml'
$workflow = Get-Content -LiteralPath $workflowPath -Raw
$usesMatches = [regex]::Matches(
    $workflow, '(?m)^\s*uses:\s*(?<target>\S+)\s*(?:#.*)?$')
if ($usesMatches.Count -eq 0) {
    throw 'Windows CI contains no auditable action dependency.'
}
foreach ($match in $usesMatches) {
    $target = $match.Groups['target'].Value
    if ($target.StartsWith('./', [StringComparison]::Ordinal)) {
        continue
    }
    $separator = $target.LastIndexOf('@')
    if ($separator -le 0 -or
        $target.Substring($separator + 1) -notmatch '^[0-9A-Fa-f]{40}$') {
        throw "Third-party GitHub Action is not pinned to a full commit: $target"
    }
}
if ($workflow -match '(?m)^\s*pull_request_target\s*:' -or
    $workflow -notmatch '(?m)^\s*contents:\s*read\s*$' -or
    $workflow -notmatch '(?m)^\s*persist-credentials:\s*false\s*$') {
    throw 'Windows CI must use pull_request, read-only contents, and non-persistent checkout credentials.'
}
Write-Output 'PASS: GitHub Actions dependencies and permissions are fail-closed'

$packageScript = Get-Content -LiteralPath (Join-Path $projectRoot 'package.ps1') -Raw
foreach ($forbiddenRoot in @('HANDOFF.md', "'artifacts'", "'evidence'")) {
    if ($packageScript.Contains($forbiddenRoot)) {
        throw "Release source/package allowlist references a forbidden local root: $forbiddenRoot"
    }
}
foreach ($requiredGuard in @('.jsonl', '.dmp', '.pem', 'ReparsePoint')) {
    if (-not $packageScript.Contains($requiredGuard)) {
        throw "Release package policy is missing a required guard: $requiredGuard"
    }
}
Write-Output 'PASS: release package policy excludes local evidence and sensitive files'

$packageScript = Get-Content -LiteralPath (Join-Path $projectRoot 'package.ps1') -Raw
foreach ($requiredArchiveChecksumRule in @(
    'archiveChecksumPath',
    'GetFileName($archivePath)',
    'Set-Content -LiteralPath $archiveChecksumPath -Encoding ascii'
)) {
    if (-not $packageScript.Contains($requiredArchiveChecksumRule)) {
        throw "Release package does not create its external archive checksum: $requiredArchiveChecksumRule"
    }
}
Write-Output 'PASS: release package emits an external SHA-256 file for the archive'

$ignoreRules = @(Get-Content -LiteralPath (Join-Path $projectRoot '.gitignore'))
foreach ($requiredRule in @(
    '/HANDOFF.md', '/artifacts/', '/evidence/', '/logs/', '*.jsonl', '*.dmp',
    '*.pem', '*.pfx', '*.exe', '*.dll'
)) {
    if ($ignoreRules -notcontains $requiredRule) {
        throw "Git ignore policy is missing: $requiredRule"
    }
}
Write-Output 'PASS: Git defaults exclude handoff, evidence, logs, binaries, and keys'
