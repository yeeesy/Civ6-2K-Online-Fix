[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$systemTempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
$fixtureRoot = Join-Path $systemTempRoot (
    'Civ6Fix_VerifyHashes_' + [Guid]::NewGuid().ToString('N'))
$fixtureFull = [IO.Path]::GetFullPath($fixtureRoot)
$safeFixture =
    $fixtureFull.StartsWith($systemTempRoot, [StringComparison]::OrdinalIgnoreCase) -and
    [IO.Path]::GetFileName($fixtureFull).StartsWith(
        'Civ6Fix_VerifyHashes_', [StringComparison]::Ordinal)
if (-not $safeFixture) {
    throw "Refusing to create an unsafe checksum fixture path: $fixtureFull"
}

try {
    New-Item -ItemType Directory -Path $fixtureFull | Out-Null
    $verifier = Join-Path $fixtureFull 'verify_hashes.ps1'
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\verify_hashes.ps1') `
        -Destination $verifier

    $payload = Join-Path $fixtureFull 'payload.txt'
    Set-Content -LiteralPath $payload -Value 'expected payload' -Encoding utf8NoBOM
    $payloadChecksum = (Get-FileHash -LiteralPath $payload -Algorithm SHA256).Hash
    $verifierChecksum = (Get-FileHash -LiteralPath $verifier -Algorithm SHA256).Hash
    Set-Content -LiteralPath (Join-Path $fixtureFull 'SHA256SUMS.txt') `
        -Value @(
            "$payloadChecksum  payload.txt"
            "$verifierChecksum  verify_hashes.ps1"
        ) -Encoding utf8NoBOM

    $baseline = @(& $verifier)
    if ($baseline -notcontains 'SHA-256 verification passed for 2 files.') {
        throw 'The checksum verifier rejected a complete package file set.'
    }
    Write-Output 'PASS: checksum verifier accepts an exact package file set'

    $unlisted = Join-Path $fixtureFull 'unlisted.txt'
    Set-Content -LiteralPath $unlisted `
        -Value 'must not be ignored' -Encoding utf8NoBOM
    $rejectedUnlistedFile = $false
    try {
        & $verifier | Out-Null
    }
    catch {
        $rejectedUnlistedFile = $true
    }
    if (-not $rejectedUnlistedFile) {
        throw 'The checksum verifier accepted an unlisted package file.'
    }
    Write-Output 'PASS: checksum verifier rejects unlisted package files'

    Remove-Item -LiteralPath $unlisted -Force
    $checksumFile = Join-Path $fixtureFull 'SHA256SUMS.txt'
    $validChecksumLines = @(Get-Content -LiteralPath $checksumFile)
    Set-Content -LiteralPath $checksumFile `
        -Value @($validChecksumLines + $validChecksumLines[0]) `
        -Encoding utf8NoBOM
    $rejectedDuplicate = $false
    try {
        & $verifier | Out-Null
    }
    catch {
        $rejectedDuplicate = $_.Exception.Message -like '*Duplicate checksum path*'
    }
    if (-not $rejectedDuplicate) {
        throw 'The checksum verifier did not reject a duplicate checksum path.'
    }
    Write-Output 'PASS: checksum verifier rejects duplicate checksum paths'

    Set-Content -LiteralPath $checksumFile `
        -Value "$payloadChecksum  ..\outside.txt" -Encoding utf8NoBOM
    $rejectedEscape = $false
    try {
        & $verifier | Out-Null
    }
    catch {
        $rejectedEscape = $_.Exception.Message -like '*escaped the package*'
    }
    if (-not $rejectedEscape) {
        throw 'The checksum verifier did not reject a path outside the package.'
    }
    Write-Output 'PASS: checksum verifier rejects paths outside the package'
}
finally {
    if ($safeFixture -and (Test-Path -LiteralPath $fixtureFull)) {
        Remove-Item -LiteralPath $fixtureFull -Recurse -Force
    }
}
