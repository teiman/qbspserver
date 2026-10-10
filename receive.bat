@echo off
rem Asks the running qbspserver (see server.bat) to compile the map, and waits for the result.
rem   receive.bat                         -> testmap\e1m1.map
rem   receive.bat path\to\mymap.map       -> another map (must be the one the server holds)
rem   set QBSPSERVER_PORT=26601           -> another port (the same one as server.bat)
rem Exit code: 0 compiled, 1 compile failed, 2 no server, 3 rejected (the server holds another map)
rem   receive.bat && light mymap && vis mymap
setlocal
set ROOT=%~dp0
set BIN=%ROOT%ericw-tools\build-windows\qbspserver\Release
set MAP=%~1
if "%MAP%"=="" set MAP=%ROOT%testmap\e1m1.map
set PORT_ARG=
if defined QBSPSERVER_PORT set PORT_ARG=-port %QBSPSERVER_PORT%

if not exist "%BIN%\qbspreceive.exe" (
    echo [receive] qbspreceive.exe not found. Build it first: build.bat qbspreceive
    exit /b 2
)

"%BIN%\qbspreceive.exe" %PORT_ARG% "%MAP%"
set RC=%ERRORLEVEL%
if %RC%==0 (echo [receive] OK) else (echo [receive] FAILED ^(code %RC%^))
exit /b %RC%
