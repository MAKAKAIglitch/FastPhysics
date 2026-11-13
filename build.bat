@echo off
rem Builds bin\FastPhysics.dll and build\SelfTest.exe.
rem Only src\fastlcp.cpp is compiled with /arch:AVX2; host/dispatch code stays baseline x64.
rem Requires Visual Studio 2022 with the "Desktop development with C++" workload.
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio installer locator was not found: "%VSWHERE%"
    exit /b 1
)

rem Avoid FOR /F command parsing here: the quoted Program Files path was split at "C:\Program".
set "VSDIR="
set "VSWHERE_OUTPUT=%TEMP%\FastPhysics-vswhere-%RANDOM%.txt"
"%VSWHERE%" -latest -products "*" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%VSWHERE_OUTPUT%"
if errorlevel 1 (
    del /q "%VSWHERE_OUTPUT%" >nul 2>&1
    echo Failed to query Visual Studio installation.
    exit /b 1
)
set /p "VSDIR="<"%VSWHERE_OUTPUT%"
del /q "%VSWHERE_OUTPUT%" >nul 2>&1

if not defined VSDIR (
    echo Visual Studio with C++ tools was not found.
    exit /b 1
)
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" (
    echo Visual Studio C++ environment script was not found: "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat"
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

if not exist bin mkdir bin
if not exist build mkdir build

set "CLFLAGS=/nologo /O2 /MT /utf-8 /W4 /permissive- /std:c++17 /EHs-c- /D_HAS_EXCEPTIONS=0 /Brepro /I src"

rc /nologo /fo build\FastPhysics.res src\FastPhysics.rc
if errorlevel 1 exit /b 1

cl %CLFLAGS% /c src\FastPhysics.cpp /Fo"build\FastPhysics.obj"
if errorlevel 1 exit /b 1

cl %CLFLAGS% /arch:AVX2 /c src\fastlcp.cpp /Fo"build\fastlcp.obj"
if errorlevel 1 exit /b 1

link /nologo /DLL /Brepro /OUT:bin\FastPhysics.dll /IMPLIB:build\FastPhysics.lib build\FastPhysics.obj build\fastlcp.obj build\FastPhysics.res bcrypt.lib
if errorlevel 1 exit /b 1

cl %CLFLAGS% /c tests\SelfTest.cpp /Fo"build\SelfTest.obj"
if errorlevel 1 exit /b 1

cl %CLFLAGS% /arch:AVX2 /c src\fastlcp.cpp /Fo"build\fastlcp_test.obj"
if errorlevel 1 exit /b 1

link /nologo /Brepro /OUT:build\SelfTest.exe build\SelfTest.obj build\fastlcp_test.obj bcrypt.lib
if errorlevel 1 exit /b 1

echo.
echo Built: bin\FastPhysics.dll
echo Self-test: build\SelfTest.exe
