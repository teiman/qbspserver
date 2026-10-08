@echo off
rem Builds ericw-tools (Release). On Linux, use build.sh.
rem   build.bat            -> everything
rem   build.bat light      -> a single target (light, qbsp, vis, bspinfo...)
rem Embree and TBB are downloaded into ericw-tools\deps on the first run.
setlocal
set ROOT=%~dp0
set BUILD=%ROOT%ericw-tools\build-windows
set DEPS=%ROOT%ericw-tools\deps

rem Embree and TBB (curl and tar come with Windows 10 and later)
if not exist "%DEPS%\embree-4.4.0\lib\cmake" (
    echo [build] Downloading Embree 4.4.0...
    if not exist "%DEPS%\embree-4.4.0" mkdir "%DEPS%\embree-4.4.0"
    curl -fsSL -o "%DEPS%\embree.zip" https://github.com/RenderKit/embree/releases/download/v4.4.0/embree-4.4.0.x64.windows.zip || exit /b 1
    tar -xf "%DEPS%\embree.zip" -C "%DEPS%\embree-4.4.0" || exit /b 1
)
if not exist "%DEPS%\oneapi-tbb-2021.11.0\lib\cmake" (
    echo [build] Downloading oneTBB 2021.11.0...
    curl -fsSL -o "%DEPS%\tbb.zip" https://github.com/uxlfoundation/oneTBB/releases/download/v2021.11.0/oneapi-tbb-2021.11.0-win.zip || exit /b 1
    tar -xf "%DEPS%\tbb.zip" -C "%DEPS%" || exit /b 1
)

rem Vulkan GPU backend for vklight (used unless -cpu) if the Vulkan SDK is installed
set GPU=OFF
if defined VULKAN_SDK (
    set GPU=ON
    set "PATH=%VULKAN_SDK%\Bin;%PATH%"
)

rem No -G: CMake uses the newest installed Visual Studio
if not exist "%BUILD%\CMakeCache.txt" (
    echo [build] Not configured yet. Configuring ^(GPU=%GPU%^)...
    cmake -S "%ROOT%ericw-tools" -B "%BUILD%" -A x64 ^
        -Dembree_DIR="%DEPS%\embree-4.4.0\lib\cmake\embree-4.4.0" ^
        -DTBB_DIR="%DEPS%\oneapi-tbb-2021.11.0\lib\cmake\tbb" ^
        -DLIGHT_ENABLE_VULKAN_GPU=%GPU% ^
        -DENABLE_LIGHTPREVIEW=OFF -DDISABLE_DOCS=ON -DDISABLE_TESTS=ON || exit /b 1
)

set TARGET=
if not "%~1"=="" set TARGET=--target %~1

set T0=%TIME%
cmake --build "%BUILD%" --config Release --parallel %TARGET% -- /v:minimal /nologo
set RC=%ERRORLEVEL%
echo.
if %RC% neq 0 (
    echo [build] FAILED ^(code %RC%^)  start %T0%  end %TIME%
) else (
    echo [build] OK  start %T0%  end %TIME%
)
exit /b %RC%
