@echo off
rem Packages qbspserver for distribution: dist\qbspserver-<version>-win64.zip (only what it needs to run).
rem   dist.bat [version]    (default: git describe of this repo, e.g. a tag like v0.3.0; "-dirty" with local changes)
rem Build first: build.bat qbspserver, build.bat qbspreceive, build.bat qbsp.
rem Contents: qbspserver.exe, qbspreceive.exe, qbsp.exe (the same compiler, also faster, same output),
rem the TBB DLLs, the MSVC runtime (app-local), README.txt (dist_readme.txt) and the licenses
rem (GPL / Apache require them).
setlocal enabledelayedexpansion
set ROOT=%~dp0
set SRC=%ROOT%ericw-tools
set SERVER=%SRC%\build-windows\qbspserver\Release
set QBSP=%SRC%\build-windows\qbsp\Release

set VERSION=%~1
if "%VERSION%"=="" for /f %%V in ('git -C "%ROOT%." describe --tags --always --dirty') do set VERSION=%%V
set NAME=qbspserver-%VERSION%-win64
set OUT=%ROOT%dist\%NAME%

for %%F in ("%SERVER%\qbspserver.exe" "%SERVER%\qbspreceive.exe" "%QBSP%\qbsp.exe") do (
    if not exist %%F (
        echo [dist] Missing %%~F. Build first: build.bat qbspserver, qbspreceive, qbsp
        exit /b 1
    )
)

rem MSVC runtime from the newest Visual Studio redist folder
set VS=
for /f "usebackq delims=" %%P in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VS=%%P
set CRT=
for /f "delims=" %%D in ('dir /b /ad /o-n "%VS%\VC\Redist\MSVC\1*" 2^>nul') do (
    if not defined CRT for /d %%C in ("%VS%\VC\Redist\MSVC\%%D\x64\Microsoft.VC*.CRT") do set CRT=%%C
)
if not defined CRT (
    echo [dist] MSVC redist not found under "%VS%\VC\Redist\MSVC".
    exit /b 1
)

if exist "%OUT%" rmdir /s /q "%OUT%"
if exist "%OUT%.zip" del /q "%OUT%.zip"
mkdir "%OUT%\licenses" || exit /b 1

for %%F in (qbspserver.exe qbspreceive.exe tbb12.dll tbbmalloc.dll) do (
    copy /y "%SERVER%\%%F" "%OUT%\" >nul || (echo [dist] Missing %SERVER%\%%F & exit /b 1)
)
copy /y "%QBSP%\qbsp.exe" "%OUT%\" >nul || exit /b 1
for %%F in (msvcp140.dll vcruntime140.dll vcruntime140_1.dll) do (
    copy /y "!CRT!\%%F" "%OUT%\" >nul || (echo [dist] Missing "!CRT!\%%F" & exit /b 1)
)
copy /y "%ROOT%dist_readme.txt" "%OUT%\README.txt" >nul || exit /b 1
copy /y "%SRC%\gpl_v3.txt" "%OUT%\licenses\ericw-tools-gpl_v3.txt" >nul || exit /b 1
copy /y "%SRC%\deps\oneapi-tbb-2021.11.0\LICENSE.txt" "%OUT%\licenses\tbb-LICENSE.txt" >nul || exit /b 1

powershell -NoProfile -Command "Compress-Archive -Path '%OUT%' -DestinationPath '%OUT%.zip' -Force" || exit /b 1
rmdir /s /q "%OUT%"

echo [dist] %NAME%  runtime: "!CRT!"
echo [dist] OK -^> dist\%NAME%.zip
