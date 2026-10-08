@echo off
rem Copies the compiled map to the game folder and opens it in ironwail.
rem   play.bat                            -> testmap\gridtest.bsp
rem   play.bat path\to\mymap.map          -> another map (its .bsp, and .lit if any, next to the .map)
rem   set QUAKE=D:\quake                  -> Quake folder (default D:\quake)
rem   set GAME=qbspserver                 -> game folder inside it (default qbspserver; id1 is the fallback)
rem Run server.bat + receive.bat (or qbsp) first. The map has no light unless you run light on it.
setlocal
if not defined QUAKE set QUAKE=D:\quake
if not defined GAME set GAME=qbspserver
set ROOT=%~dp0
set MAP=%~1
if "%MAP%"=="" set MAP=%ROOT%testmap\gridtest.map
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

if not exist "%QUAKE%\%GAME%\maps" mkdir "%QUAKE%\%GAME%\maps"
copy /y "%BSP%" "%QUAKE%\%GAME%\maps\" >nul || exit /b 1
if exist "%LIT%" (copy /y "%LIT%" "%QUAKE%\%GAME%\maps\" >nul) else (del /q "%QUAKE%\%GAME%\maps\%NAME%.lit" 2>nul)
echo [play] %NAME% -^> %QUAKE%\%GAME%\maps\

cd /d "%QUAKE%"
start "" "%QUAKE%\ironwail.exe" -basedir "%QUAKE%" -game %GAME% +vid_fullscreen 0 +vid_width 1280 +vid_height 720 +vid_restart +map %NAME%
