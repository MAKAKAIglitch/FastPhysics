param()
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$root = Split-Path -Parent $PSScriptRoot
$header = Get-Content (Join-Path $root 'src\version.h') -Raw
if ($header -notmatch 'FP_VERSION_STRING\s+"([^"]+)"') { throw 'FP_VERSION_STRING not found' }
$version = $Matches[1]
$archivePath = Join-Path $root "dist\FastPhysics-$version-source.zip"
if (-not (Test-Path -LiteralPath $archivePath -PathType Leaf)) { throw "Source archive not found: $archivePath" }
$archive = [System.IO.Compression.ZipFile]::OpenRead($archivePath)
try {
    $names = @($archive.Entries | ForEach-Object { $_.FullName })
    $prefix = "FastPhysics-$version-source/"
    $required = @(
        ($prefix + 'LICENSE'),
        ($prefix + 'README.md'),
        ($prefix + 'docs/HARDENING.md'),
        ($prefix + 'build.bat'),
        ($prefix + 'release.bat'),
        ($prefix + 'package.ps1'),
        ($prefix + 'tests/VerifySourcePackage.ps1'),
        ($prefix + 'tests/SourceSecurityChecks.ps1'),
        ($prefix + 'tests/InstallerSafetyTest.ps1'),
        ($prefix + 'tests/SelfTest.cpp'),
        ($prefix + 'src/FastPhysics.cpp'),
        ($prefix + 'src/fastlcp.cpp'),
        ($prefix + 'SMModLoader/build.bat'),
        ($prefix + 'SMModLoader/src/SMModLoader.cpp'),
        ($prefix + 'SMModLoader/installer/modloader.ps1'),
        ($prefix + 'SMModLoader/tests/LoaderTest.cpp')
    )
    foreach ($entry in $required) { if ($names -notcontains $entry) { throw "Missing source entry: $entry" } }
    $licenseEntry = $archive.GetEntry($prefix + 'LICENSE')
    $reader = [System.IO.StreamReader]::new($licenseEntry.Open())
    try { $licenseText = $reader.ReadToEnd() } finally { $reader.Dispose() }
    if ($licenseText -notmatch 'zlib licence' -or $licenseText -notmatch 'Copyright') {
        throw 'Original zlib licence notice missing or altered in source archive.'
    }
    Write-Host "Source archive verified: $archivePath" -ForegroundColor Green
}
finally { $archive.Dispose() }
