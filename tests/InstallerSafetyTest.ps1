# Focused tests for the loader installer rollback/restore trust boundary.
# All files are synthetic and created under a unique temporary directory.
# The signature checker is mocked only to exercise both trust outcomes deterministically.

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$loaderScript = Join-Path $root 'SMModLoader\installer\modloader.ps1'
. $loaderScript

# Preserve a real Windows signature check as a diagnostic when a system DLL is available.
$realSignatureCheck = (Get-Command Test-MicrosoftSignedDll).ScriptBlock
$systemDll = Join-Path $env:SystemRoot 'System32\kernel32.dll'
if (Test-Path -LiteralPath $systemDll) {
    $realResult = & $realSignatureCheck $systemDll
    Write-Host "Authenticode helper on System32\kernel32.dll: $realResult"
}

$script:TrustedTestPaths = @()

# These small mocks isolate the install/uninstall decision branches from PE metadata
# and certificate availability. The production signature checker remains in modloader.ps1.
function Test-MicrosoftSignedDll([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    $full = [IO.Path]::GetFullPath($Path)
    return ($script:TrustedTestPaths -contains $full)
}

function Get-DllProduct([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    if ([IO.File]::ReadAllText($Path) -like 'LOADER_*') { return 'SM Mod Loader' }
    return $null
}

function Get-DllVersion([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return [version]'0.0' }
    switch ([IO.File]::ReadAllText($Path)) {
        'LOADER_OLD' { return [version]'1.1.0' }
        'LOADER_NEW' { return [version]'1.2.0' }
        default { return [version]'0.0' }
    }
}

function Assert([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "FAILED: $Message" }
    Write-Host "[PASS] $Message" -ForegroundColor Green
}

function Expect-Failure([scriptblock]$Action, [string]$Message) {
    $failed = $false
    try { & $Action } catch { $failed = $true }
    Assert $failed $Message
}

function Reset-Release([string]$Path) {
    if (Test-Path -LiteralPath $Path) { Remove-Item -LiteralPath $Path -Recurse -Force }
    New-Item -ItemType Directory -Path $Path -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $Path 'DLLModules') -Force | Out-Null
}

function Write-TestFile([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, $Text, [Text.Encoding]::ASCII)
}

$work = Join-Path ([IO.Path]::GetTempPath()) ("SMModLoaderSafety-" + [guid]::NewGuid().ToString('N'))
$release = Join-Path $work 'Release'
$loader = Join-Path $work 'SMModLoader.dll'
$vc = Join-Path $release 'vcruntime140_1.dll'
$backup = Join-Path $release 'vcruntime140_1_orig.dll'

try {
    New-Item -ItemType Directory -Path $work -Force | Out-Null
    Write-TestFile $loader 'LOADER_NEW'

    # 1. An existing loader must not be upgraded when its saved runtime is untrusted.
    Reset-Release $release
    Write-TestFile $vc 'LOADER_OLD'
    Write-TestFile $backup 'UNTRUSTED_BACKUP'
    $script:TrustedTestPaths = @()
    Expect-Failure { Install-ModLoader $release $loader } 'upgrade refuses an unsigned runtime backup'
    Assert (([IO.File]::ReadAllText($vc) -eq 'LOADER_OLD') -and
            ([IO.File]::ReadAllText($backup) -eq 'UNTRUSTED_BACKUP')) 'failed upgrade leaves both DLLs unchanged'

    # 2. Uninstall must not remove the working loader before validating its backup.
    Reset-Release $release
    Write-TestFile $vc 'LOADER_OLD'
    Write-TestFile $backup 'UNTRUSTED_BACKUP'
    $script:TrustedTestPaths = @()
    Expect-Failure { Uninstall-ModLoaderIfUnused $release } 'uninstall refuses an unsigned runtime backup'
    Assert (([IO.File]::ReadAllText($vc) -eq 'LOADER_OLD') -and
            ([IO.File]::ReadAllText($backup) -eq 'UNTRUSTED_BACKUP')) 'failed uninstall keeps loader and untrusted backup intact'

    # 3. A verified backup is restored on uninstall.
    Reset-Release $release
    Write-TestFile $vc 'LOADER_OLD'
    Write-TestFile $backup 'MICROSOFT_RUNTIME'
    $script:TrustedTestPaths = @([IO.Path]::GetFullPath($backup))
    Uninstall-ModLoaderIfUnused $release
    Assert (([IO.File]::ReadAllText($vc) -eq 'MICROSOFT_RUNTIME') -and
            -not (Test-Path -LiteralPath $backup)) 'uninstall restores a verified runtime backup'

    # 4. A signed game runtime must not be replaced if an existing backup is untrusted.
    Reset-Release $release
    Write-TestFile $vc 'MICROSOFT_RUNTIME'
    Write-TestFile $backup 'UNTRUSTED_BACKUP'
    $script:TrustedTestPaths = @([IO.Path]::GetFullPath($vc))
    Expect-Failure { Install-ModLoader $release $loader } 'fresh install refuses to overwrite an untrusted backup'
    Assert (([IO.File]::ReadAllText($vc) -eq 'MICROSOFT_RUNTIME') -and
            ([IO.File]::ReadAllText($backup) -eq 'UNTRUSTED_BACKUP')) 'failed fresh install preserves both files'

    # 5. If Steam already restored the runtime, uninstall preserves an unknown backup.
    Reset-Release $release
    Write-TestFile $vc 'MICROSOFT_RUNTIME'
    Write-TestFile $backup 'UNTRUSTED_BACKUP'
    $script:TrustedTestPaths = @([IO.Path]::GetFullPath($vc))
    Uninstall-ModLoaderIfUnused $release
    Assert (([IO.File]::ReadAllText($vc) -eq 'MICROSOFT_RUNTIME') -and
            (Test-Path -LiteralPath $backup)) 'uninstall preserves an untrusted stale backup'

    # 6. Valid backup permits an in-place loader update without changing the backup.
    Reset-Release $release
    Write-TestFile $vc 'LOADER_OLD'
    Write-TestFile $backup 'MICROSOFT_RUNTIME'
    $script:TrustedTestPaths = @([IO.Path]::GetFullPath($backup))
    [void](Install-ModLoader $release $loader)
    Assert (([IO.File]::ReadAllText($vc) -eq 'LOADER_NEW') -and
            ([IO.File]::ReadAllText($backup) -eq 'MICROSOFT_RUNTIME')) 'upgrade accepts a verified backup and preserves it'

    Write-Host 'All installer safety checks passed.' -ForegroundColor Green
}
finally {
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
}
