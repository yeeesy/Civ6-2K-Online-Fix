[CmdletBinding()]
param(
    [string]$OutputName,

    [switch]$AllowNonReleaseName,

    [switch]$NoArchive
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$projectRoot = $PSScriptRoot
$buildRoot = Join-Path $projectRoot 'build'
$distRoot = Join-Path $projectRoot 'dist'
$versionSource = Join-Path $projectRoot 'version.json'
if (-not (Test-Path -LiteralPath $versionSource -PathType Leaf)) {
    throw "Version source is missing: $versionSource"
}
$productIdentity = Get-Content -LiteralPath $versionSource -Raw | ConvertFrom-Json
$productVersion = [string]$productIdentity.Version
$windowsVersion = [string]$productIdentity.WindowsVersion
$packageBaseName = [string]$productIdentity.PackageBaseName
$releaseOutputName = "${packageBaseName}_$productVersion"
if ([string]::IsNullOrWhiteSpace($OutputName)) {
    $OutputName = $releaseOutputName
}
if ($OutputName -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,80}$') {
    throw "Invalid package output name: $OutputName"
}
if (-not $AllowNonReleaseName -and $OutputName -ne $releaseOutputName) {
    throw "Release package name must be $releaseOutputName. Use -AllowNonReleaseName only for disposable CI output."
}
$packageRoot = Join-Path $distRoot $OutputName
$archivePath = Join-Path $distRoot "$OutputName.zip"
$archiveChecksumPath = "$archivePath.sha256"

$distFull = [IO.Path]::GetFullPath($distRoot).TrimEnd('\') + '\'
$packageFull = [IO.Path]::GetFullPath($packageRoot)
if (-not $packageFull.StartsWith($distFull, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Package target escaped the dist directory: $packageFull"
}

if (Test-Path -LiteralPath $packageRoot) {
    throw "Package directory already exists; refusing to overwrite: $packageRoot"
}
if (-not $NoArchive -and (Test-Path -LiteralPath $archivePath)) {
    throw "Package archive already exists; refusing to overwrite: $archivePath"
}
if (-not $NoArchive -and (Test-Path -LiteralPath $archiveChecksumPath)) {
    throw "Package archive checksum already exists; refusing to overwrite: $archiveChecksumPath"
}

$appSource = Join-Path $buildRoot 'Civ6_2K_Online_Fix.exe'
$manifestSource = Join-Path $buildRoot 'build_manifest.json'
$packageDocuments = @(
    'README.md'
    'CHANGELOG.md'
    'LICENSE'
    'SECURITY.md'
    'Civ6_2K_Online_Fix.ini.example'
    'verify_hashes.ps1'
)
$sourceRootFiles = @(
    'README.md'
    'CHANGELOG.md'
    'LICENSE'
    'SECURITY.md'
    'CONTRIBUTING.md'
    'Civ6_2K_Online_Fix.ini.example'
    'verify_hashes.ps1'
    'build.ps1'
    'package.ps1'
    'version.json'
    '.gitignore'
    '.editorconfig'
)
$requiredFiles = @($appSource, $manifestSource, (Join-Path $projectRoot 'logs_README.txt'))
$requiredFiles += $packageDocuments | ForEach-Object { Join-Path $projectRoot $_ }
$requiredFiles += $sourceRootFiles | ForEach-Object { Join-Path $projectRoot $_ }
$requiredDirectories = @('src', 'tests', 'docs', '.github') |
    ForEach-Object { Join-Path $projectRoot $_ }

foreach ($path in $requiredFiles) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required package input is missing: $path"
    }
}
foreach ($path in $requiredDirectories) {
    if (-not (Test-Path -LiteralPath $path -PathType Container)) {
        throw "Required source directory is missing: $path"
    }
}

$packageInputs = @($requiredFiles + $requiredDirectories)
foreach ($path in $packageInputs) {
    $item = Get-Item -LiteralPath $path -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Release input must not be a reparse point: $path"
    }
    if ($item.PSIsContainer) {
        $nestedReparsePoint = Get-ChildItem -LiteralPath $path -Recurse -Force |
            Where-Object {
                ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
            } |
            Select-Object -First 1
        if ($null -ne $nestedReparsePoint) {
            throw "Release source tree contains a reparse point: $($nestedReparsePoint.FullName)"
        }
    }
}

$forbiddenSourceSuffixes = @(
    '.dll', '.dmp', '.env', '.exe', '.jsonl', '.key', '.log', '.obj',
    '.p12', '.pdb', '.pem', '.pfx', '.res', '.snk', '.zip'
)
$sourceTreeFiles = @($requiredDirectories | ForEach-Object {
    Get-ChildItem -LiteralPath $_ -File -Recurse -Force
})
$forbiddenSourceFile = $sourceTreeFiles | Where-Object {
    $name = $_.Name.ToLowerInvariant()
    $forbiddenSourceSuffixes | Where-Object { $name.EndsWith($_) } |
        Select-Object -First 1
} | Select-Object -First 1
if ($null -ne $forbiddenSourceFile) {
    throw "Release source tree contains a forbidden generated or sensitive file: $($forbiddenSourceFile.FullName)"
}

$buildManifest = Get-Content -LiteralPath $manifestSource -Raw | ConvertFrom-Json
$dx11Profiles = @($buildManifest.BuildProfiles | Where-Object Renderer -eq 'DX11')
$dx12Profiles = @($buildManifest.BuildProfiles | Where-Object Renderer -eq 'DX12')
if ($buildManifest.Version -ne $productVersion -or
    $buildManifest.WindowsVersion -ne $windowsVersion -or
    $buildManifest.PackageBaseName -ne $packageBaseName -or
    $buildManifest.VersionSource -ne 'version.json' -or
    $buildManifest.BuildFlavor -ne 'public-verified-dx11-dx12' -or
    -not $buildManifest.Dx12WriteEnabled -or
    $dx11Profiles.Count -ne 1 -or
    $dx11Profiles[0].Id -ne 'civ6-win64steam-1.0.12.68-dx11' -or
    $dx11Profiles[0].SupportState -ne 'Verified' -or
    $dx11Profiles[0].SHA256 -ne 'E7450823CC8E00468CFF7B9D7B97C63140EAE38AE1D774BA4EFA437556C42D63' -or
    $dx12Profiles.Count -ne 1 -or
    $dx12Profiles[0].Id -ne 'civ6-win64steam-1.0.12.68-dx12' -or
    $dx12Profiles[0].SupportState -ne 'Verified' -or
    $dx12Profiles[0].SHA256 -ne 'C2C3D40B86260A541D8A4D38CB70D50D3406AE1DE374AFC382D1E42BC1342F1E' -or
    -not $buildManifest.TestsRun -or
    $buildManifest.TestCount -lt 1 -or
    -not $buildManifest.NativeAnalysis -or
    -not $buildManifest.ReproducibleBuild) {
    throw "Release packaging requires the exact DX11/DX12 Verified public profiles, version $productVersion from version.json, executed tests, NativeAnalysis=true, and ReproducibleBuild=true."
}
$builtHash = (Get-FileHash -LiteralPath $appSource -Algorithm SHA256).Hash
if ($builtHash -ne $buildManifest.SHA256) {
    throw 'Executable hash does not match build_manifest.json.'
}

New-Item -ItemType Directory -Path $distRoot -Force | Out-Null
$sourceTarget = Join-Path $packageRoot 'source'
$logsTarget = Join-Path $packageRoot 'logs'
New-Item -ItemType Directory -Path $packageRoot,$sourceTarget,$logsTarget | Out-Null

Copy-Item -LiteralPath $appSource -Destination (Join-Path $packageRoot 'Civ6_2K_Online_Fix.exe')
Copy-Item -LiteralPath $manifestSource -Destination (Join-Path $packageRoot 'build_manifest.json')
foreach ($name in $packageDocuments) {
    Copy-Item -LiteralPath (Join-Path $projectRoot $name) -Destination $packageRoot
}
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs') -Destination $packageRoot -Recurse
Copy-Item -LiteralPath (Join-Path $projectRoot 'logs_README.txt') -Destination (Join-Path $logsTarget 'README.txt')

foreach ($name in $sourceRootFiles) {
    Copy-Item -LiteralPath (Join-Path $projectRoot $name) -Destination $sourceTarget
}
foreach ($name in @('src', 'tests', 'docs', '.github')) {
    Copy-Item -LiteralPath (Join-Path $projectRoot $name) -Destination $sourceTarget -Recurse
}

$hashLines = @()
$filesToHash = @(Get-ChildItem -LiteralPath $packageRoot -File -Recurse |
    Where-Object { $_.Name -ne 'SHA256SUMS.txt' } |
    Sort-Object FullName)
foreach ($file in $filesToHash) {
    $relative = [IO.Path]::GetRelativePath($packageRoot, $file.FullName).Replace('\', '/')
    $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    $hashLines += "$hash  $relative"
}
$hashPath = Join-Path $packageRoot 'SHA256SUMS.txt'
$hashLines | Set-Content -LiteralPath $hashPath -Encoding utf8NoBOM

foreach ($line in $hashLines) {
    $match = [regex]::Match($line, '^(?<hash>[A-F0-9]{64})  (?<path>.+)$')
    if (-not $match.Success) {
        throw "Invalid generated checksum line: $line"
    }
    $relativePath = $match.Groups['path'].Value.Replace('/', '\')
    $filePath = Join-Path $packageRoot $relativePath
    $actual = (Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash
    if ($actual -ne $match.Groups['hash'].Value) {
        throw "Checksum verification failed immediately after packaging: $relativePath"
    }
}

$archiveHash = $null
if (-not $NoArchive) {
    Compress-Archive -LiteralPath $packageRoot -DestinationPath $archivePath -CompressionLevel Optimal
    $archiveHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash
    "$archiveHash  $([IO.Path]::GetFileName($archivePath))" |
        Set-Content -LiteralPath $archiveChecksumPath -Encoding ascii
}

[pscustomobject]@{
    PackageRoot = $packageRoot
    PackageFileCount = (@(Get-ChildItem -LiteralPath $packageRoot -File -Recurse)).Count
    ExecutableSHA256 = $builtHash
    HashList = $hashPath
    Archive = if ($NoArchive) { $null } else { $archivePath }
    ArchiveSHA256 = $archiveHash
    ArchiveChecksum = if ($NoArchive) { $null } else { $archiveChecksumPath }
}
