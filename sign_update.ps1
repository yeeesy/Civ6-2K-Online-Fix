[CmdletBinding()]
param([Parameter(Mandatory)][string]$Archive)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$identity=Get-Content (Join-Path $PSScriptRoot 'version.json') -Raw | ConvertFrom-Json
$version=[string]$identity.Version
if($version -notmatch '^\d+\.\d+\.\d+$'){throw 'Only stable releases can be signed for automatic updates.'}
$base="Civ6_2K_Online_Fix_$version"
$zip=Get-Item -LiteralPath $Archive
if($zip.Name -cne "$base.zip" -or $zip.Length -gt 32MB){throw 'Unexpected release archive.'}
$out=Join-Path $zip.DirectoryName "$base.update"
if(Test-Path -LiteralPath $out){throw 'Refusing to overwrite release signature.'}
$pin=(Get-Content (Join-Path $PSScriptRoot 'src/update-public-key.xml') -Raw).Trim()
$key=[Security.Cryptography.CngKey]::Open('Civ6_2K_Online_Fix.yeeesy.update-v1',[Security.Cryptography.CngProvider]::MicrosoftSoftwareKeyStorageProvider)
$rsa=[Security.Cryptography.RSACng]::new($key)
try {
    if($key.IsMachineKey -or $key.ExportPolicy -ne [Security.Cryptography.CngExportPolicies]::None -or
       $rsa.KeySize -lt 3072 -or $rsa.ToXmlString($false) -cne $pin){throw 'Pinned signing identity mismatch.'}
    $archiveStream=[IO.File]::OpenRead($zip.FullName)
    try {
        $zipHash=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($archiveStream));$archiveStream.Position=0
        $z=[IO.Compression.ZipArchive]::new($archiveStream,[IO.Compression.ZipArchiveMode]::Read,$true)
        try {
            $e=@($z.Entries | Where-Object {$_.FullName.Replace('\','/') -eq "$base/Civ6_2K_Online_Fix.exe"})
            if($e.Count -ne 1 -or $e[0].Length -le 0 -or $e[0].Length -gt 16MB){throw 'Invalid package executable.'}
            $payload=$e[0].Open()
            try {$exeHash=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($payload))}finally{$payload.Dispose()}
            $build=Get-Content (Join-Path $PSScriptRoot 'build/build_manifest.json') -Raw | ConvertFrom-Json
            if($build.SHA256 -ne $exeHash -or $build.Version -cne $version -or !$build.TestsRun -or !$build.NativeAnalysis){throw 'Archive is not the verified build.'}
            $body=[Text.Encoding]::UTF8.GetBytes((@('CIV6-UPDATE/1','yeeesy/Civ6-2K-Online-Fix',$version,$zip.Length,$zipHash,$e[0].Length,$exeHash) -join [char]10))
        } finally {$z.Dispose()}
    } finally {$archiveStream.Dispose()}
    $sig=$rsa.SignData($body,[Security.Cryptography.HashAlgorithmName]::SHA256,[Security.Cryptography.RSASignaturePadding]::Pkcs1)
    if(!$rsa.VerifyData($body,$sig,[Security.Cryptography.HashAlgorithmName]::SHA256,[Security.Cryptography.RSASignaturePadding]::Pkcs1)){throw 'Self verification failed.'}
    $bytes=[Text.Encoding]::ASCII.GetBytes(([Convert]::ToBase64String($body)+[char]10+[Convert]::ToBase64String($sig)))
    $dest=[IO.File]::Open($out,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
    try{$dest.Write($bytes,0,$bytes.Length);$dest.Flush($true)}finally{$dest.Dispose()}
    Get-Item -LiteralPath $out
} finally {$rsa.Dispose();$key.Dispose()}
