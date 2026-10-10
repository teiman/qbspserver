@echo off
rem Lights the compiled map (vis + light), copies it to the game folder and opens it in ironwail.
rem   play.bat                            -> testmap\e1m1.bsp
rem   play.bat path\to\mymap.map          -> another map (its .bsp, and .lit if any, next to the .map)
rem   set QUAKE=D:\quake                  -> Quake folder (default D:\quake)
rem   set GAME=qbspserver                 -> game folder inside it (default qbspserver; id1 is the fallback)
rem   set NOLIGHT=1                       -> skip vis and light (the map shows fully bright)
rem Run server.bat + receive.bat (or qbsp) first.
setlocal
if not defined QUAKE set QUAKE=D:\quake
if not defined GAME set GAME=qbspserver
set ROOT=%~dp0
set MAP=%~1
if "%MAP%"=="" set MAP=%ROOT%testmap\e1m1.map
for %%F in ("%MAP%") do (
    set BSP=%%~dpnF.bsp
    set LIT=%%~dpnF.lit
    set NAME=%%~nF
)

if not exist "%BSP%" (
    echo [play] Missing %BSP%: run receive.bat first
    exit /b 1
)
if not exist "%QUAKE%\ironwail.exe" (
    echo [play] Missing %QUAKE%\ironwail.exe: set QUAKE to your Quake folder
    exit /b 1
)

rem vis and light, when built (build.bat vis / build.bat light): a couple of seconds on e1m1
set VIS=%ROOT%ericw-tools\build-windows\vis\Release\vis.exe
set LIGHT=%ROOT%ericw-tools\build-windows\light\Release\vklight.exe
if not defined NOLIGHT (
    if exist "%VIS%" ("%VIS%" "%BSP%" >nul || echo [play] vis failed, playing without it) else (echo [play] no vis.exe: build.bat vis)
    if exist "%LIGHT%" ("%LIGHT%" "%BSP%" >nul || echo [play] light failed, playing without it) else (echo [play] no vklight.exe: build.bat light)
)

if not exist "%QUAKE%\%GAME%\maps" mkdir "%QUAKE%\%GAME%\maps"
copy /y "%BSP%" "%QUAKE%\%GAME%\maps\" >nul || exit /b 1
if exist "%LIT%" (copy /y "%LIT%" "%QUAKE%\%GAME%\maps\" >nul) else (del /q "%QUAKE%\%GAME%\maps\%NAME%.lit" 2>nul)
echo [play] %NAME% -^> %QUAKE%\%GAME%\maps\

cd /d "%QUAKE%"
start "" "%QUAKE%\ironwail.exe" -basedir "%QUAKE%" -game %GAME% +vid_fullscreen 0 +vid_width 1280 +vid_height 720 +vid_restart +map %NAME%
