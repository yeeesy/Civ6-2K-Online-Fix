[CmdletBinding()]
param(
    [string]$ChecksumFile = (Join-Path $PSScriptRoot 'SHA256SUMS.txt')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $ChecksumFile -PathType Leaf)) {
    throw "Checksum file not found: $ChecksumFile"
}

$packageRoot = [IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\') + '\'
$checksumFull = [IO.Path]::GetFullPath($ChecksumFile)
if (-not $checksumFull.StartsWith(
        $packageRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Checksum file escaped the package: $checksumFull"
}
$listedFiles = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
$verified = 0
foreach ($line in Get-Content -LiteralPath $ChecksumFile) {
    if ([string]::IsNullOrWhiteSpace($line)) {
        continue
    }
    $match = [regex]::Match($line, '^(?<hash>[A-Fa-f0-9]{64})  (?<path>.+)$')
    if (-not $match.Success) {
        throw "Invalid checksum line: $line"
    }
    $relative = $match.Groups['path'].Value.Replace('/', '\')
    $candidate = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot $relative))
    if (-not $candidate.StartsWith($packageRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Checksum path escaped the package: $relative"
    }
    if ($candidate.Equals($checksumFull, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'SHA256SUMS.txt must not list itself.'
    }
    if (-not $listedFiles.Add($candidate)) {
        throw "Duplicate checksum path: $relative"
    }
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        throw "Package file is missing: $relative"
    }
    $actual = (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash
    if ($actual -ne $match.Groups['hash'].Value.ToUpperInvariant()) {
        throw "SHA-256 mismatch: $relative"
    }
    ++$verified
}

if ($verified -eq 0) {
    throw 'No package files were listed in the checksum file.'
}

$packageEntries = @(Get-ChildItem -LiteralPath $PSScriptRoot -Recurse -Force)
$reparsePoint = $packageEntries | Where-Object {
    ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
} | Select-Object -First 1
if ($null -ne $reparsePoint) {
    throw "Package contains a reparse point: $($reparsePoint.FullName)"
}
$actualFiles = @($packageEntries | Where-Object {
    -not $_.PSIsContainer -and
    -not $_.FullName.Equals(
        $checksumFull, [StringComparison]::OrdinalIgnoreCase)
})
foreach ($file in $actualFiles) {
    if (-not $listedFiles.Contains([IO.Path]::GetFullPath($file.FullName))) {
        $relative = [IO.Path]::GetRelativePath($PSScriptRoot, $file.FullName)
        throw "Package contains an unlisted file: $relative"
    }
}
if ($actualFiles.Count -ne $listedFiles.Count) {
    throw "Checksum file set does not match the package: listed $($listedFiles.Count), found $($actualFiles.Count)."
}

Write-Output "SHA-256 verification passed for $verified files."
