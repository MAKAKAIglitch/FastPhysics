# Regression guard for capabilities that this mod does not need.
# Intentional capabilities are local game-memory patching (VirtualProtect),
# same-process plugin loading (LoadLibraryExW), and local worker threads.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

$nativeFiles = @(
    'src/FastPhysics.cpp',
    'src/fastlcp.cpp',
    'SMModLoader/src/SMModLoader.cpp'
)
$scriptFiles = @(
    'installer/Install.bat',
    'installer/Uninstall.bat',
    'installer/files/install.ps1',
    'installer/files/uninstall.ps1',
    'installer/files/common.ps1',
    'SMModLoader/installer/modloader.ps1',
    'build.bat',
    'release.bat',
    'package.ps1'
)

$nativeForbidden = '\b(?:WinHttpOpen|InternetOpenA|InternetOpenW|WSAStartup|URLDownloadToFileA|URLDownloadToFileW|CreateRemoteThread|WriteProcessMemory|NtWriteVirtualMemory|NtCreateThreadEx|OpenProcess|RegSetValueExA|RegSetValueExW|CreateServiceA|CreateServiceW|OpenSCManagerA|OpenSCManagerW|SetWindowsHookExA|SetWindowsHookExW|GetAsyncKeyState)\b'
$scriptForbidden = '\b(?:Invoke-WebRequest|Invoke-RestMethod|Start-BitsTransfer|DownloadFile|DownloadString|Invoke-Expression|Register-ScheduledTask|New-Service|Set-Service|New-ItemProperty|Set-ItemProperty|Set-ExecutionPolicy)\b'

foreach ($relative in $nativeFiles) {
    $path = Join-Path $root $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required source file missing: $relative" }
    $source = Get-Content -LiteralPath $path -Raw
    if ($source -match $nativeForbidden) {
        throw "Unexpected network, remote-process, keyboard-hook or persistence API in $relative"
    }
}

foreach ($relative in $scriptFiles) {
    $path = Join-Path $root $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required script missing: $relative" }
    $source = Get-Content -LiteralPath $path -Raw
    if ($source -match $scriptForbidden) {
        throw "Unexpected downloader, dynamic expression, service or registry-write command in $relative"
    }
}

$fastPhysicsPath = Join-Path $root 'src/FastPhysics.cpp'
$fastPhysicsSource = Get-Content -LiteralPath $fastPhysicsPath -Raw
foreach ($required in @(
    'HashFileSha256',
    'ExpectedGameExeSHA256',
    'game executable SHA-256 pin matched',
    'ValidateLcpSolution',
    'IsWritableRange',
    'InterlockedExchangePointer',
    'g_runtimeDisabled'
)) {
    if (-not $fastPhysicsSource.Contains($required)) {
        throw "The 1.0.7 safety guard is missing: $required"
    }
}
if ($fastPhysicsSource -notmatch 'if \(_stricmp\(actualHash, expectedHash\) != 0\)') {
    throw 'The exact executable SHA-256 mismatch must fail closed.'
}

$loaderPath = Join-Path $root 'SMModLoader/src/SMModLoader.cpp'
$loaderSource = Get-Content -LiteralPath $loaderPath -Raw
foreach ($required in @(
    'LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR',
    'LOAD_LIBRARY_SEARCH_APPLICATION_DIR',
    'LOAD_LIBRARY_SEARCH_SYSTEM32',
    'LoadLibraryExW(path, nullptr, kSafeLoadFlags)'
)) {
    if (-not $loaderSource.Contains($required)) {
        throw "The restricted DLL search policy is missing: $required"
    }
}
if ($loaderSource.Contains('LOAD_WITH_ALTERED_SEARCH_PATH')) {
    throw 'Unsafe legacy DLL search mode was reintroduced.'
}

Write-Host "Source security regression checks passed ($($nativeFiles.Count) native files, $($scriptFiles.Count) scripts)." -ForegroundColor Green
