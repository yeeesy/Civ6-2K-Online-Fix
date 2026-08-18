[CmdletBinding()]
param(
    [switch]$SkipTests,
    [switch]$Analyze
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$projectRoot = $PSScriptRoot
$sourceRoot = Join-Path $projectRoot 'src'
$testRoot = Join-Path $projectRoot 'tests'
$buildRoot = Join-Path $projectRoot 'build'
$generatedRoot = Join-Path $buildRoot 'generated'
$buildFlavor = 'public-verified-dx11-dx12'
$appExecutableName = 'Civ6_2K_Online_Fix.exe'
$versionFile = Join-Path $projectRoot 'version.json'
if (-not (Test-Path -LiteralPath $versionFile -PathType Leaf)) {
    throw "Version source was not found: $versionFile"
}
$productIdentity = Get-Content -LiteralPath $versionFile -Raw | ConvertFrom-Json
$productVersion = [string]$productIdentity.Version
$windowsVersion = [string]$productIdentity.WindowsVersion
$packageBaseName = [string]$productIdentity.PackageBaseName
if ($productVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?$' -or
    $windowsVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$' -or
    $packageBaseName -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,80}$') {
    throw 'version.json contains an invalid product identity.'
}
$windowsVersionParts = @($windowsVersion.Split('.') | ForEach-Object {
    $component = 0
    if (-not [int]::TryParse($_, [ref]$component) -or
        $component -lt 0 -or $component -gt 65535) {
        throw "Invalid Windows version component: $_"
    }
    $component
})
if ($windowsVersionParts.Count -ne 4) {
    throw 'WindowsVersion must contain exactly four numeric components.'
}

New-Item -ItemType Directory -Path $buildRoot,$generatedRoot -Force | Out-Null
$generatedIdentityHeader = Join-Path $generatedRoot 'ProductIdentity.generated.h'
$numericVersion = $windowsVersionParts -join ','
$generatedIdentity = @"
#pragma once

#define CIV6FIX_VERSION_NUMERIC $numericVersion
#define CIV6FIX_VERSION_SEMVER "$productVersion"
#define CIV6FIX_VERSION_SEMVER_WIDE L"$productVersion"

#ifndef RC_INVOKED
namespace civ6fix {

inline constexpr char kProductVersion[] = CIV6FIX_VERSION_SEMVER;
inline constexpr wchar_t kProductVersionWide[] =
    L"Civ6 2K Online Fix " CIV6FIX_VERSION_SEMVER_WIDE;

}  // namespace civ6fix
#endif
"@
Set-Content -LiteralPath $generatedIdentityHeader -Value $generatedIdentity `
    -Encoding utf8NoBOM

$manifestTemplatePath = Join-Path $sourceRoot 'Civ6_2K_Online_Fix.manifest.in'
if (-not (Test-Path -LiteralPath $manifestTemplatePath -PathType Leaf)) {
    throw "Manifest template was not found: $manifestTemplatePath"
}
$manifestTemplate = Get-Content -LiteralPath $manifestTemplatePath -Raw
$manifestVersionPlaceholder = '@WINDOWS_VERSION@'
if (($manifestTemplate.Split($manifestVersionPlaceholder).Count - 1) -ne 1) {
    throw 'Manifest template must contain exactly one Windows version placeholder.'
}
$generatedManifest = Join-Path $generatedRoot 'ProductManifest.generated.manifest'
$manifestTemplate.Replace($manifestVersionPlaceholder, $windowsVersion) |
    Set-Content -LiteralPath $generatedManifest -Encoding utf8NoBOM

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw "Visual Studio Installer discovery tool was not found: $vswhere"
}
$visualStudioRoot = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
if ([string]::IsNullOrWhiteSpace($visualStudioRoot)) {
    throw 'No Visual Studio installation with the x64 C++ toolchain was found.'
}
$vcVersionFile = Join-Path $visualStudioRoot 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt'
if (-not (Test-Path -LiteralPath $vcVersionFile -PathType Leaf)) {
    throw "MSVC version marker was not found: $vcVersionFile"
}
$msvcVersion = (Get-Content -LiteralPath $vcVersionFile -Raw).Trim()
$msvc = Join-Path $visualStudioRoot "VC\Tools\MSVC\$msvcVersion"

$sdkRegistryPath = 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows Kits\Installed Roots'
$sdk = Get-ItemPropertyValue -LiteralPath $sdkRegistryPath -Name KitsRoot10 -ErrorAction SilentlyContinue
if ([string]::IsNullOrWhiteSpace($sdk)) {
    $sdk = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
}
$sdkIncludeRoot = Join-Path $sdk 'Include'
$sdkVersionItem = Get-ChildItem -LiteralPath $sdkIncludeRoot -Directory |
    Where-Object {
        Test-Path -LiteralPath (Join-Path $_.FullName 'um\Windows.h') -PathType Leaf
    } |
    Sort-Object { [version]$_.Name } -Descending |
    Select-Object -First 1
if ($null -eq $sdkVersionItem) {
    throw "No usable Windows 10/11 SDK was found under: $sdkIncludeRoot"
}
$sdkVersion = $sdkVersionItem.Name
$compiler = Join-Path $msvc 'bin\Hostx64\x64\cl.exe'
$resourceCompiler = Join-Path $sdk "bin\$sdkVersion\x64\rc.exe"
foreach ($tool in @($compiler, $resourceCompiler)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Required build tool was not found: $tool"
    }
}

$commonCompilerArgs = @(
    '/nologo'
    '/std:c++17'
    '/EHsc'
    '/permissive-'
    '/W4'
    '/WX'
    '/MT'
    '/O2'
    '/sdl'
    '/guard:cf'
    '/experimental:deterministic'
    "/pathmap:$projectRoot=."
    '/utf-8'
    '/Zc:__cplusplus'
    '/Zc:preprocessor'
    '/DUNICODE'
    '/D_UNICODE'
    "/I$generatedRoot"
    "/I$sourceRoot"
    "/I$(Join-Path $msvc 'include')"
    "/I$(Join-Path $sdk "Include\$sdkVersion\ucrt")"
    "/I$(Join-Path $sdk "Include\$sdkVersion\shared")"
    "/I$(Join-Path $sdk "Include\$sdkVersion\um")"
)
if ($Analyze) {
    $commonCompilerArgs += @(
        '/analyze'
        # Windows SDK 10.0.26100 winreg.h emits C6553 from its own SAL annotation.
        '/wd6553'
    )
}
$libraryArgs = @(
    "/LIBPATH:$(Join-Path $msvc 'lib\x64')"
    "/LIBPATH:$(Join-Path $sdk "Lib\$sdkVersion\ucrt\x64")"
    "/LIBPATH:$(Join-Path $sdk "Lib\$sdkVersion\um\x64")"
)

$objects = @{}
foreach ($name in @('FixSession', 'SessionPresentation', 'SteamDiscovery', 'Logger', 'Win32FixPlatform', 'AppWindow')) {
    $object = Join-Path $buildRoot "$name.obj"
    $compileArgs = @($commonCompilerArgs) + @(
        '/c'
        "/Fo$object"
        (Join-Path $sourceRoot "$name.cpp")
    )
    & $compiler @compileArgs
    if ($LASTEXITCODE -ne 0) {
        throw "$name compilation failed with exit code $LASTEXITCODE"
    }
    $objects[$name] = $object
}

$testExe = Join-Path $buildRoot 'Civ6FixCoreTests.exe'
$testObject = Join-Path $buildRoot 'Civ6FixCoreTests.obj'
$testArgs = @($commonCompilerArgs) + @(
    "/Fo$testObject"
    "/Fe$testExe"
    (Join-Path $testRoot 'Civ6FixCoreTests.cpp')
    $objects['FixSession']
    $objects['SessionPresentation']
    $objects['SteamDiscovery']
    '/link'
) + $libraryArgs

& $compiler @testArgs
if ($LASTEXITCODE -ne 0) {
    throw "Unit test compilation failed with exit code $LASTEXITCODE"
}

$testCount = 0
if (-not $SkipTests) {
    $testOutput = @(& $testExe 2>&1)
    $testExitCode = $LASTEXITCODE
    $testOutput | ForEach-Object { Write-Output $_ }
    if ($testExitCode -ne 0) {
        throw "Unit tests failed with exit code $testExitCode"
    }
    $testCount = @($testOutput | Where-Object { $_ -like 'PASS:*' }).Count
    foreach ($scriptTest in @(
        'VerifyHashes.Tests.ps1'
        'VersionSource.Tests.ps1'
        'ReleasePolicy.Tests.ps1'
    )) {
        $powershellTestOutput = @(
            & (Join-Path $testRoot $scriptTest) 2>&1
        )
        $powershellTestOutput | ForEach-Object { Write-Output $_ }
        $testCount += @(
            $powershellTestOutput | Where-Object { $_ -like 'PASS:*' }
        ).Count
    }
    if ($testCount -le 0) {
        throw 'Unit tests reported success without any counted assertions.'
    }
}

$resourceFile = Join-Path $buildRoot 'Civ6_2K_Online_Fix.res'
$resourceArgs = @(
    '/nologo'
    '/i'
    $generatedRoot
    '/i'
    (Join-Path $sdk "Include\$sdkVersion\um")
    '/i'
    (Join-Path $sdk "Include\$sdkVersion\shared")
    '/i'
    (Join-Path $sdk "Include\$sdkVersion\ucrt")
    "/fo$resourceFile"
    (Join-Path $sourceRoot 'Civ6_2K_Online_Fix.rc')
)
Push-Location $sourceRoot
try {
    & $resourceCompiler @resourceArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Resource compilation failed with exit code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
}

$appExe = Join-Path $buildRoot $appExecutableName
$appObject = Join-Path $buildRoot 'Civ6_2K_Online_Fix.obj'
$appArgs = @($commonCompilerArgs) + @(
    "/Fo$appObject"
    "/Fe$appExe"
    (Join-Path $sourceRoot 'Civ6_2K_Online_Fix.cpp')
    $objects['FixSession']
    $objects['SessionPresentation']
    $objects['SteamDiscovery']
    $objects['Logger']
    $objects['Win32FixPlatform']
    $objects['AppWindow']
    $resourceFile
    '/link'
    '/SUBSYSTEM:WINDOWS'
    '/INCREMENTAL:NO'
    '/OPT:REF'
    '/OPT:ICF'
    '/BREPRO'
    '/experimental:deterministic'
    '/DYNAMICBASE'
    '/HIGHENTROPYVA'
    '/NXCOMPAT'
    '/guard:cf'
    '/CETCOMPAT'
    'advapi32.lib'
    'bcrypt.lib'
    'comctl32.lib'
    'gdi32.lib'
    'psapi.lib'
    'shell32.lib'
    'user32.lib'
) + $libraryArgs

& $compiler @appArgs
if ($LASTEXITCODE -ne 0) {
    throw "Application compilation failed with exit code $LASTEXITCODE"
}

$appItem = Get-Item -LiteralPath $appExe
$versionInfo = $appItem.VersionInfo
if ($versionInfo.FileVersion -ne $productVersion -or
    $versionInfo.ProductVersion -ne $productVersion) {
    throw "Executable version resource does not match version.json: file=$($versionInfo.FileVersion), product=$($versionInfo.ProductVersion), expected=$productVersion"
}
$manifest = [ordered]@{
    Product = 'Civ6 2K Online Fix'
    Version = $productVersion
    WindowsVersion = $windowsVersion
    PackageBaseName = $packageBaseName
    BuildFlavor = $buildFlavor
    Dx12WriteEnabled = $true
    BuiltAtUtc = (Get-Date).ToUniversalTime().ToString('o')
    Configuration = 'Release'
    Architecture = 'x64'
    Executable = $appItem.Name
    Length = $appItem.Length
    SHA256 = (Get-FileHash -LiteralPath $appExe -Algorithm SHA256).Hash
    BuildProfiles = @(
        [ordered]@{
            Id = 'civ6-win64steam-1.0.12.68-dx11'
            Renderer = 'DX11'
            SupportState = 'Verified'
            SHA256 = 'E7450823CC8E00468CFF7B9D7B97C63140EAE38AE1D774BA4EFA437556C42D63'
        }
        [ordered]@{
            Id = 'civ6-win64steam-1.0.12.68-dx12'
            Renderer = 'DX12'
            SupportState = 'Verified'
            SHA256 = 'C2C3D40B86260A541D8A4D38CB70D50D3406AE1DE374AFC382D1E42BC1342F1E'
        }
    )
    MSVCVersion = $msvcVersion
    WindowsSDKVersion = $sdkVersion
    NativeAnalysis = [bool]$Analyze
    TestsRun = (-not $SkipTests)
    TestCount = $testCount
    ReproducibleBuild = $true
    SourcePathMapping = '.\'
    VersionSource = 'version.json'
    SecurityProperties = @(
        'asInvoker'
        'NXCOMPAT'
        'HIGHENTROPYVA'
        'CFG'
        'CETCOMPAT'
        'StaticMSVCRuntime'
        'DeterministicCompilation'
        'SourcePathMapping'
        'ReproducibleLink'
        'SingleSourceVersion'
    )
}
$manifestPath = Join-Path $buildRoot 'build_manifest.json'
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding utf8NoBOM

$manifest
